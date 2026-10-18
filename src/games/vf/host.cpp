// Virtua Fighter in the desktop host (src/host/game_host.h): its settings (the ring, the cheats,
// Arcade mode's coins), its pictures, the Arcade settings and Cheats pages, and the cheats each frame.
#include "../../host/game_host.h"
#include "art.h"
#include "controls.h"
#include "gameplay.h"
#include "romset.h"
#include <algorithm>
#include <array>
#include <iterator>
#include <optional>

namespace game::vf {
namespace {

std::string on_off(bool b) { return b ? "On" : "Off"; }
int wrap(int v, int n) { return (v % n + n) % n; }

class host final : public game_host
{
public:
	const romset::definition &romset() const override { return ROMSET; }
	const control_profile &controls() const override { return CONTROLS; }

	void load_settings(const settings_map &kv) override
	{
		auto num = [&](const char *k, int &v, int lo, int hi) {
			auto it = kv.find(k);
			if (it != kv.end()) v = std::clamp(atoi(it->second.c_str()), lo, hi);
		};
		auto flag = [&](const char *k, bool &v) { int i = v; num(k, i, 0, 1); v = i; };
		num("ring_size", ring, 0, 1500);
		if (ring && (ring < 1200 || ring % 100)) ring = 0;   // 0: by difficulty
		num("arcade_coins", arcade_coin, -1, 0xffff);
		num("akira_slot", akira_slot, 0, int(std::size(UNUSED_CHARACTERS)) - 1);
		num("infinite_energy", infinite_energy, 0, 3);
		flag("one_hit_ko", one_hit_ko);
		flag("no_time_limit", no_time_limit);
		num("ring_cheat", ring_cheat, 0, 2);
	}

	void save_settings(FILE *f) const override
	{
		fprintf(f, "ring_size = %d\n", ring);
		if (arcade_coin >= 0) fprintf(f, "arcade_coins = %d\n", arcade_coin);
		if (akira_slot) fprintf(f, "akira_slot = %d\n", akira_slot);
		if (infinite_energy) fprintf(f, "infinite_energy = %d\n", infinite_energy);
		if (one_hit_ko) fprintf(f, "one_hit_ko = 1\n");
		if (no_time_limit) fprintf(f, "no_time_limit = 1\n");
		if (ring_cheat) fprintf(f, "ring_cheat = %d\n", ring_cheat);
	}

	void start(host_context &c) override
	{
		ctx = &c;
		vf.emplace(c.machine);
		vf->set_no_time_limit(no_time_limit);   // (Cheats: an instruction of the game's, so before it's recompiled)
	}

	void before_boot() override
	{
		load_screens(*vf, ctx->assets_dir + "/games/vf/backgrounds");   // the title and select's widescreen sides
		load_screens(*vf, ctx->data_dir + "/backgrounds");     // the player's own, which win
		vf->set_ring_width(ring_now());
		vf->set_akira_slot(akira_slot);
		load_secret_icon(*vf, akira_slot, icon_dirs());
	}

	// The play style's coins: PC mode is free play (its FREE PLAY text hidden); Arcade mode has the
	// cabinet's own coin setting, kept in settings.ini while PC mode has free play in its place (it
	// was left at free play, so Arcade mode after PC mode had no credits to put in, and a cabinet's
	// coin slot did nothing). The settings are read at boot: when they change, boot again.
	bool booted() override
	{
		settings_before.reset();   // (a restart applies them)
		const int now = vf->coin_setting();
		if (now < 0) return false;                       // (the game's very first boot writes them)
		if (ctx->pc_mode) {
			if (now != gameplay::FREE_PLAY && now != arcade_coin) { arcade_coin = now; ctx->settings_changed(); }
			return vf->force_free_play();
		}
		// Arcade: its own setting back; free play left by PC mode before one was kept: the game's first
		return vf->set_coin_setting(arcade_coin >= 0 ? arcade_coin : now == gameplay::FREE_PLAY ? 0 : now);
	}

	bool has_play_style() const override { return true; }

	// Arcade mode's setting as it is now (the service menu may have changed it), kept for when PC mode replaces it
	void keep_arcade_settings() override
	{
		const int now = vf->coin_setting();
		if (!ctx->pc_mode && now >= 0 && now != arcade_coin) { arcade_coin = now; ctx->settings_changed(); }
	}

	void play_style_changed() override { if (ctx->pc_mode) vf->hide_free_play_text(); else vf->show_free_play_text(); }

	std::vector<menu_item> menu_items() override
	{
		return { { "Arcade settings", "Difficulty, ring size, rounds, continues and the service menu. Changing them restarts the game.", {}, {},
				   [this] { ctx->push_page(arcade_page()); } } };
	}

	std::vector<menu_item> menu_items_after_controls() override
	{
		return { { "Cheats", "Play as the characters left unused in the game. Changing it restarts the game.", {}, {},
				   [this] { ctx->push_page(cheats_page()); } } };
	}

	// the arcade settings are read as the game starts: restart it if one is different
	restart menu_closed() override
	{
		if (!settings_before) return restart::none;
		const bool changed = *settings_before != settings_now();
		settings_before.reset();
		if (!changed) return restart::none;
		// (the game's code is then new: recompiled before the reset, which leaves the old behind)
		return vf->set_no_time_limit(no_time_limit) ? restart::game_and_code : restart::game;
	}

	void before_frame() override
	{
		if (infinite_energy || one_hit_ko || no_time_limit) vf->fight_cheats(infinite_energy, one_hit_ko, no_time_limit);
	}

	// health (NVRAM b4, b6), the round wins' top bits (182, 202), the fight's state (work RAM 1281, 1282)
	void trace_fields(unsigned (&fields)[6]) const override
	{
		model1 &m = ctx->machine;
		auto nv16 = [&](uint32_t a) { return unsigned(m.nvram()[a] | m.nvram()[a + 1] << 8); };
		const unsigned values[6] = { nv16(0xb4), nv16(0xb6), nv16(0x182) >> 15, nv16(0x202) >> 15,
									 unsigned(m.work_ram()[0x1281]), unsigned(m.work_ram()[0x1282]) };
		std::copy(std::begin(values), std::end(values), fields);
	}

private:
	host_context *ctx = nullptr;
	std::optional<gameplay> vf;

	int ring = 0;              // Arcade settings: the ring's width (0: by difficulty)
	int arcade_coin = -1;      // Arcade mode's coin/credit setting, kept while PC mode makes it free play (-1: not seen yet)
	int akira_slot = 0;        // Cheats: who plays in Akira's place (UNUSED_CHARACTERS; 0 Akira)
	int infinite_energy = 0;   // Cheats: bit 0 player 1, bit 1 player 2
	bool one_hit_ko = false, no_time_limit = false;   // Cheats
	int ring_cheat = 0;        // Cheats: 0 the arcade's, 1 tiny (600), 2 huge (5000: no ring outs)
	int ring_now() const { return ring_cheat == 1 ? 600 : ring_cheat == 2 ? 5000 : ring; }

	std::vector<std::string> icon_dirs() const { return { ctx->data_dir + "/icons", ctx->assets_dir + "/games/vf/select_icons" }; }   // (the player's first)

	// ---- the arcade's own settings (test mode's GAME ASSIGNMENTS): the game reads them as it starts,
	// so closing the menu after changing one restarts it (only if something is different) ----
	using ga = gameplay::assignment;
	static constexpr int N_ASSIGN = 5;
	using settings_values = std::array<int, N_ASSIGN + 4>;
	std::optional<settings_values> settings_before;   // as the Arcade settings or Cheats page opened
	settings_values settings_now()
	{
		settings_values v;
		for (int i = 0; i < N_ASSIGN; i++) v[i] = vf->assignment_value(ga(i));
		v[N_ASSIGN] = ring;
		v[N_ASSIGN + 1] = akira_slot;   // (Cheats: read as a fight loads, so it restarts too)
		v[N_ASSIGN + 2] = ring_cheat;
		v[N_ASSIGN + 3] = no_time_limit;   // (an instruction of the game's: a restart recompiles it)
		return v;
	}
	void changed() { ctx->settings_changed(); }

	menu_page arcade_page()
	{
		if (!settings_before) settings_before = settings_now();
		menu_page p{ "Arcade settings: a change restarts the game when the menu closes", "", {} };   // always in view
		gameplay *g = &*vf;
		auto choice = [&](const char *label, const char *help, ga a, std::function<std::string(int)> name) {
			p.items.push_back({ label, help,
				[g, a, name] { int v = g->assignment_value(a); return v < 0 ? std::string("-") : name(v); },
				[g, a](int d) { int v = g->assignment_value(a); if (v >= 0) g->set_assignment(a, v + d); } });
		};
		static const char *DIFFICULTY[] = { "Easy", "Normal", "Hard", "Hardest" };
		choice("Difficulty", "How hard the computer fights. Harder also gives both fighters less energy, and a smaller ring while Ring size is Default.",
			ga::difficulty, [](int v) { return std::string(DIFFICULTY[v]); });
		static const int RINGS[] = { 0, 1200, 1300, 1400, 1500 };   // the arcade's widths (STAGE WIDTH)
		static const char *RING_NAMES[] = { "Default", "Small", "Medium", "Large", "Extra large" };
		p.items.push_back({ "Ring size",
			"Default follows the difficulty, as on the arcade: Extra large on Easy, down to Small on Hardest. A smaller ring makes ring outs easier.",
			[this] { return std::string(RING_NAMES[std::find(std::begin(RINGS), std::end(RINGS), ring) - std::begin(RINGS)]); },
			[this](int d) {
				int i = int(std::find(std::begin(RINGS), std::end(RINGS), ring) - std::begin(RINGS));
				ring = RINGS[std::clamp(i + d, 0, int(std::size(RINGS)) - 1)];
				changed();
				vf->set_ring_width(ring_now());
			} });
		choice("Rounds to win (vs CPU)", "Rounds needed to win a match against the computer.", ga::match_1p, [](int v) { return std::to_string(v); });
		choice("Rounds to win (vs player)", "Rounds needed to win a match between two players.", ga::match_vs, [](int v) { return std::to_string(v); });
		choice("Continue", "Off ends the game when you lose to the computer.", ga::continues, [](int v) { return on_off(v); });
		choice("Demo sound", "Sound during the demo that plays between games.", ga::advertise_sound, [](int v) { return on_off(v); });
		p.items.push_back({ "Service menu",
			"The arcade's own test menu: input and sound tests, coin settings and bookkeeping. Down moves, Start selects (or 9 and F2 on a keyboard). Choose EXIT to come back.",
			{}, {}, [this] { ctx->service_menu(); } });
		p.items.push_back(ctx->back_item());
		return p;
	}

	// ---- cheats: the unused characters (in Akira's place) ----
	menu_page cheats_page()
	{
		if (!settings_before) settings_before = settings_now();
		menu_page p{ "Cheats: a change restarts the game when the menu closes", "", {} };
		constexpr int N_CHARS = int(std::size(UNUSED_CHARACTERS));
		p.items.push_back({ "Secret character",
			"A character cut from the game, in Akira's spot on the select screen (Akira is away while one is in). "
			"Unfinished: borrowed moves, no throws. The Koskol brothers were to be the final bosses.",
			[this] { return std::string(akira_slot ? UNUSED_CHARACTERS[akira_slot] : "Off"); },
			[this](int d) {
				akira_slot = wrap(akira_slot + d, N_CHARS);
				changed();
				vf->set_akira_slot(akira_slot);
				load_secret_icon(*vf, akira_slot, icon_dirs());
			} });
		static const char *WHO[] = { "Off", "Player 1", "Player 2", "Both" };
		p.items.push_back({ "Infinite energy", "Energy stays full: hits land but never knock out.",
			[this] { return std::string(WHO[infinite_energy]); },
			[this](int d) { infinite_energy = wrap(infinite_energy + d, 4); changed(); } });
		p.items.push_back({ "One-hit KO", "Every fighter (but one with infinite energy) is down to their last bit of energy: any hit wins the round.",
			[this] { return on_off(one_hit_ko); },
			[this](int) { one_hit_ko = !one_hit_ko; changed(); } });
		p.items.push_back({ "No time limit", "The round's clock stays at its start and never runs out: a round ends by knockout or ring out.",
			[this] { return on_off(no_time_limit); },
			[this](int) { no_time_limit = !no_time_limit; changed(); } });
		static const char *RING[] = { "Normal", "Tiny", "Huge" };
		p.items.push_back({ "Ring", "Tiny is half the smallest ring: ring outs everywhere. Huge is far wider than the stage: no ring outs.",
			[this] { return std::string(RING[ring_cheat]); },
			[this](int d) { ring_cheat = wrap(ring_cheat + d, 3); changed(); vf->set_ring_width(ring_now()); } });
		p.items.push_back(ctx->back_item());
		return p;
	}
};

}
}

std::unique_ptr<game_host> make_game_host() { return std::make_unique<game::vf::host>(); }
