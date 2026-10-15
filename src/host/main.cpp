// Desktop host: SDL3 window, keyboard and gamepads, frame pacing at the Model 1's 57.52 Hz,
// and the battery-backed RAM and EEPROM saved next to the regions.
#include "version.h"      // VF_VERSION, VF_DATE (made by the Makefile from git)
#include "regions.h"
#include "romset.h"
#include <filesystem>
#include "png.h"
#include "menu.h"
#include "backgrounds.h"
#include "scene_sync.h"
#include "gpu_compose.h"
#include "controls.h"
#include "../machine/parallel.h"
#include "../rt/rec_rt.h"
#ifdef VF_NATIVE
#include "../rt/native.h"
#endif
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif
#ifdef _WIN32
#include <SDL3/SDL_main.h>   // WinMain, and argv as UTF-8
#endif
#include <string>
#include <algorithm>
#include <array>
#include <optional>
#include <tuple>
#include <cmath>
#include <map>
#include <vector>
#include <set>
#include <cstring>

// Every message box goes through here: a test run (VF_HIDDEN) never shows one (nor a file picker),
// the message goes to stderr (log.txt without a console) instead.
static bool message_box(SDL_MessageBoxFlags flags, const char *title, const std::string &msg)
{
	if (getenv("VF_HIDDEN")) { fprintf(stderr, "(hidden: no dialog) %s\n", msg.c_str()); return false; }
	return SDL_ShowSimpleMessageBox(flags, title, msg.c_str(), nullptr);
}

namespace {

constexpr double FRAME_HZ = 16000000.0 / (656.0 * 424.0);

// Bits in the I/O board's input bytes (active low). The buttons as the game's own INPUT TEST names
// them: 01 punch, 02 kick, 04 guard.
enum : uint8_t {
	IN0_COIN1 = 0x01, IN0_COIN2 = 0x02, IN0_TEST = 0x04, IN0_SERVICE = 0x08,
	IN0_START1 = 0x10, IN0_START2 = 0x20,
	PL_PUNCH = 0x01, PL_KICK = 0x02, PL_GUARD = 0x04,
	PL_DOWN = 0x10, PL_UP = 0x20, PL_RIGHT = 0x40, PL_LEFT = 0x80,
};

// The cabinet's own buttons (the players' are in controls.h, set in the menu)
struct key_bind { SDL_Scancode key; int port; uint8_t bit; };
const key_bind SYS_KEYS[] = {
	{ SDL_SCANCODE_5, 0, IN0_COIN1 }, { SDL_SCANCODE_6, 0, IN0_COIN2 },
	{ SDL_SCANCODE_F2, 0, IN0_TEST }, { SDL_SCANCODE_9, 0, IN0_SERVICE },
};

// Gamepads (a Steam Deck under Steam Input looks like one), by default (the menu remaps them):
//   d-pad / left stick: move     X guard   A punch   B kick
//   Y guard+punch (throws)   RB punch+kick   LB guard+kick   RT guard+punch+kick
//   Start: starts (with --arcade: only with a credit, as the cabinet's start button)
//   Menu (View tapped, remappable), L3 + R3 or the touchpad click: the menu   View + Start: the menu too
//   View + LB: render scale   View + RB: picture shape   (View is a coin instead only when it's set as Coin)
// The PC build (the default; --arcade for the cabinet's behaviour): no boot screen, always free
// play with nothing said about it, and Start simply starts.
bool g_pc_mode = true;

struct pad_state
{
	bool back_was = false, back_combo = false, start_was = false;
	bool menu_was = false, touch_was = false, sticks_was = false;   // the menu's other buttons (edges)
	int coin_frames = 0, start_delay = -1, start_frames = 0;
};

enum class hotkey { none, menu, scale, aspect };

// The game's test menu, from our menu: the test button pressed for a moment, then, while the test
// menu is up, Down is the service button (move) and Start the test button (select). The game's mode
// is 3 in its test menu but also for a few frames of attract, so the test menu counts as up only
// when the test button was pressed in the last 3 s and the mode has stayed 3 for half a second.
int g_test_frames = 0;
bool g_test_pending = false;   // the menu's Service menu: pressed once the game is past its boot (a restart first)

// Testing: VF_SCRIPT=file plays a vfscript-style script of inputs (wait, press, hold, release, mash,
// pokenv; rec and stop mark what VF_CAPTURE=dir saves: every picture shown, as the player sees it),
// one step per game frame, so the real game loop can be checked at a point deep in a play.
struct script_frame { uint8_t in[3] = { 0, 0, 0 }; std::vector<std::pair<uint32_t, uint8_t>> pokes, work_pokes; };
std::vector<script_frame> g_plan;
size_t g_plan_i = 0, g_rec_from = SIZE_MAX, g_rec_to = SIZE_MAX;
void load_script(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return;
	uint8_t held[3] = { 0, 0, 0 };
	std::vector<std::pair<uint32_t, uint8_t>> pokes, work_pokes;
	auto add = [&](int n, const uint8_t *extra) {
		for (int i = 0; i < n; i++) {
			script_frame fr;
			for (int p = 0; p < 3; p++) fr.in[p] = uint8_t(held[p] | (extra ? extra[p] : 0));
			fr.pokes.swap(pokes);
			fr.work_pokes.swap(work_pokes);
			g_plan.push_back(std::move(fr));
		}
	};
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char cmd[32] = "";
		unsigned a = 0, b = 0, c = 0;
		const int k = sscanf(line, "%31s %x %x %u", cmd, &a, &b, &c);
		const std::string w = cmd;
		if (k < 1 || w[0] == '#') continue;
		if (w == "wait") { unsigned n = 0; sscanf(line, "%*s %u", &n); add(int(n), nullptr); }
		else if (w == "press") { unsigned port, mask, n; sscanf(line, "%*s %u %x %u", &port, &mask, &n); uint8_t e[3] = { 0, 0, 0 }; e[port % 3] = uint8_t(mask); add(int(n), e); add(8, nullptr); }
		else if (w == "hold") { unsigned port, mask; sscanf(line, "%*s %u %x", &port, &mask); held[port % 3] = uint8_t(mask); }
		else if (w == "release") { unsigned port; sscanf(line, "%*s %u", &port); held[port % 3] = 0; }
		else if (w == "mash") {
			unsigned n = 0; sscanf(line, "%*s %u", &n);
			for (unsigned i = 0; i < n; i++) { uint8_t e[3] = { 0, uint8_t((i / 4) % 2 ? 0x02 : ((i / 4) % 3 ? 0x04 : 0x40)), 0 }; add(1, e); }
		}
		else if (w == "fight") {   // fight N SEED: both players play real moves and combos (vfscript's bot)
			unsigned n = 0, seed = 0;
			sscanf(line, "%*s %u %u", &n, &seed);
			uint32_t rng = seed * 2654435761u + 7;
			auto rnd = [&](int k) { rng = rng * 1103515245 + 12345; return int((rng >> 16) % unsigned(k)); };
			enum : uint8_t { P = 0x01, K = 0x02, G = 0x04, D = 0x10, U = 0x20, F = 0x40, B = 0x80 };
			struct step_t { uint8_t m; int frames; };
			static const std::vector<std::vector<step_t>> PLAYS = {
				{ { F, 14 } }, { { F, 2 }, { 0, 2 }, { F, 6 } },
				{ { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 4 }, { K, 3 }, { 0, 10 } },
				{ { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 8 } },
				{ { P, 3 }, { 0, 4 }, { K, 3 }, { 0, 10 } },
				{ { F | P, 3 }, { 0, 6 }, { F | K, 3 }, { 0, 10 } }, { { F | P, 3 }, { 0, 10 } },
				{ { P | G, 4 }, { 0, 20 } }, { { F, 6 }, { P | G, 4 }, { 0, 20 } },
				{ { D | K, 3 }, { 0, 12 } }, { { D | B | K, 3 }, { 0, 16 } },
				{ { U | F, 3 }, { 0, 10 }, { K, 3 }, { 0, 24 } }, { { F | K, 3 }, { 0, 12 } },
				{ { K, 3 }, { 0, 4 }, { K, 3 }, { 0, 10 } }, { { G, 18 } }, { { D | G, 14 } }, { { B, 10 } },
			};
			static const int WEIGHTS[] = { 5, 3, 7, 3, 5, 5, 3, 3, 3, 3, 2, 2, 3, 2, 5, 3, 2 };
			int total = 0;
			for (int wt : WEIGHTS) total += wt;
			struct bot { const std::vector<step_t> *play = nullptr; size_t i = 0; int left = 0; uint8_t cur = 0; } bots[2];
			for (unsigned fno = 0; fno < n; fno++) {
				uint8_t e[3] = { 0, 0, 0 };
				for (int p = 0; p < 2; p++) {
					bot &bt = bots[p];
					if (bt.left <= 0) {
						if (bt.play && ++bt.i < bt.play->size()) { bt.cur = (*bt.play)[bt.i].m; bt.left = (*bt.play)[bt.i].frames; }
						else if (bt.play) { bt.play = nullptr; bt.cur = 0; bt.left = 2 + rnd(7); }
						else {
							int r = rnd(total), k = 0;
							while (r >= WEIGHTS[k]) r -= WEIGHTS[k++];
							bt.play = &PLAYS[k]; bt.i = 0;
							bt.cur = PLAYS[k][0].m; bt.left = PLAYS[k][0].frames + (k == 0 || k >= 14 ? rnd(12) : 0);
						}
					}
					bt.left--;
					uint8_t in = bt.cur & 0x37;
					if (bt.cur & F) in |= p ? 0x80 : 0x40;
					if (bt.cur & B) in |= p ? 0x40 : 0x80;
					e[1 + p] = in;
				}
				add(1, e);
			}
		}
		else if (w == "pokenv") pokes.push_back({ a, uint8_t(b) });
		else if (w == "poke") work_pokes.push_back({ a, uint8_t(b) });   // poke ADDR VALUE (hex): a byte of work RAM (500000-53ffff)
		else if (w == "rec") g_rec_from = g_plan.size();
		else if (w == "stop") g_rec_to = g_plan.size();
	}
	fclose(f);
	fprintf(stderr, "script: %zu frames, recording %zu-%zu\n", g_plan.size(), g_rec_from, g_rec_to);
}
bool g_left_test_menu = false;       // set when it closes (the PC build then restarts past the boot screen)

hotkey read_inputs(model1 &m, SDL_Gamepad *pads[2], pad_state ps[2], const player_controls ctl[2])
{
	static const uint8_t BITS[C_COUNT] = {
		PL_UP, PL_DOWN, PL_LEFT, PL_RIGHT, PL_GUARD, PL_PUNCH, PL_KICK,
		PL_GUARD | PL_PUNCH, PL_PUNCH | PL_KICK, PL_GUARD | PL_KICK, PL_GUARD | PL_PUNCH | PL_KICK, 0, 0 };
	uint8_t in[3] = { 0, 0, 0 };  // pressed bits, inverted at the end
	const bool *ks = SDL_GetKeyboardState(nullptr);
	for (const auto &k : SYS_KEYS)
		if (ks[k.key]) in[k.port] |= k.bit;
	auto key = [&](SDL_Scancode k) { return k != SDL_SCANCODE_UNKNOWN && ks[k]; };
	hotkey hk = hotkey::none;
	static bool key_start_was[2], key_coin_was[2], pad_coin_was[2], key_menu_was[2];
	for (int p = 0; p < 2; p++) {
		pad_state &st = ps[p];
		// Start is the cabinet's start button: in Arcade mode it starts only with a credit (coins
		// come from the Coin binding or the 5 and 6 keys)
		auto press_start = [&] { st.start_delay = 0; };
		// a key set as Menu opens it too (Esc always does)
		const bool kmenu = key(ctl[p].key[C_MENU]);
		if (kmenu && !key_menu_was[p]) hk = hotkey::menu;
		key_menu_was[p] = kmenu;
		for (int a = 0; a < C_START; a++)
			if (key(ctl[p].key[a])) in[1 + p] |= BITS[a];
		const bool kstart = key(ctl[p].key[C_START]);
		if (kstart && !key_start_was[p]) press_start();
		key_start_was[p] = kstart;
		// Coin (a cabinet's coin slot, mapped in the menu): a coin for this player, in Arcade mode
		// (PC mode is free play: the game takes no coins)
		const bool kcoin = key(ctl[p].key[C_COIN]);
		if (kcoin && !key_coin_was[p] && !g_pc_mode) st.coin_frames = 6;
		key_coin_was[p] = kcoin;

		const uint8_t coin_bit = p ? IN0_COIN2 : IN0_COIN1, start_bit = p ? IN0_START2 : IN0_START1;
		if (SDL_Gamepad *g = pads[p]) {
			uint8_t &b = in[1 + p];
			auto btn = [&](SDL_GamepadButton x) { return SDL_GetGamepadButton(g, x); };
			int lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
			const int dead = 12000;
			const bool back = btn(SDL_GAMEPAD_BUTTON_BACK);
			const bool start = player_controls::pad_down(g, ctl[p].pad[C_START]);
			// Coin and Menu on View are its tap (below), so View + LB and the like stay hotkeys
			const int coin_code = ctl[p].pad[C_COIN], menu_code = ctl[p].pad[C_MENU];
			const bool pcoin = coin_code != SDL_GAMEPAD_BUTTON_BACK && player_controls::pad_down(g, coin_code);
			if (pcoin && !pad_coin_was[p] && !g_pc_mode) st.coin_frames = 6;
			pad_coin_was[p] = pcoin;
			const bool pmenu = menu_code != SDL_GAMEPAD_BUTTON_BACK && player_controls::pad_down(g, menu_code);
			if (pmenu && !st.menu_was && !back) hk = hotkey::menu;   // (View held: LB and RB are its hotkeys)
			st.menu_was = pmenu;
			// L3 + R3 and the touchpad click (a PlayStation pad's) always open the menu as well, so a
			// pad without a View button, or with Menu moved, can't be locked out of it
			const bool sticks = btn(SDL_GAMEPAD_BUTTON_LEFT_STICK) && btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
			if (sticks && !st.sticks_was) hk = hotkey::menu;
			st.sticks_was = sticks;
			bool touch_bound = false;
			for (int a = C_GUARD; a < C_COUNT; a++) touch_bound |= ctl[p].pad[a] == SDL_GAMEPAD_BUTTON_TOUCHPAD;
			const bool touch = !touch_bound && btn(SDL_GAMEPAD_BUTTON_TOUCHPAD);
			if (touch && !st.touch_was) hk = hotkey::menu;
			st.touch_was = touch;
			if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -dead) b |= PL_UP;
			if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > dead) b |= PL_DOWN;
			if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -dead) b |= PL_LEFT;
			if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > dead) b |= PL_RIGHT;
			if (back) {
				// View held: hotkeys on Start, LB and RB, not moves
				const bool pstart = btn(SDL_GAMEPAD_BUTTON_START);
				const bool lb = btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), rb = btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
				if (pstart && !st.start_was) { hk = hotkey::menu; st.back_combo = true; }
				static bool lb_was[2], rb_was[2];
				if (lb && !lb_was[p]) { hk = hotkey::scale; st.back_combo = true; }
				if (rb && !rb_was[p]) { hk = hotkey::aspect; st.back_combo = true; }
				lb_was[p] = lb; rb_was[p] = rb;
			} else {
				for (int a = C_GUARD; a < C_START; a++)
					if (player_controls::pad_down(g, ctl[p].pad[a])) b |= BITS[a];
				if (start && !st.start_was) press_start();
			}
			// View tapped on its own: whichever of Menu and Coin it's set as
			if (!back && st.back_was && !st.back_combo) {
				if (menu_code == SDL_GAMEPAD_BUTTON_BACK) hk = hotkey::menu;
				else if (coin_code == SDL_GAMEPAD_BUTTON_BACK && !g_pc_mode) st.coin_frames = 6;
			}
			if (!back) st.back_combo = false;
			st.back_was = back;
			st.start_was = back ? btn(SDL_GAMEPAD_BUTTON_START) : start;
		}
		if (st.coin_frames > 0) { in[0] |= coin_bit; st.coin_frames--; }
		if (st.start_delay >= 0 && st.start_delay-- == 0) st.start_frames = 6;
		if (st.start_frames > 0) { in[0] |= start_bit; st.start_frames--; }
	}
	static bool down_was, start_was, in_test;
	static int service_frames, test_ago = 1 << 20, mode3_frames;
	const bool mode3 = m.game_mode() == 3;
	mode3_frames = mode3 ? mode3_frames + 1 : 0;
	if (!in_test && test_ago < 180 && mode3_frames >= 30) in_test = true;
	if (in_test && !mode3) { in_test = false; g_left_test_menu = true; }
	if (in_test) {
		const bool down = (in[1] | in[2]) & PL_DOWN, start = in[0] & (IN0_START1 | IN0_START2);
		if (down && !down_was) service_frames = 6;
		if (start && !start_was) g_test_frames = 6;
		down_was = down; start_was = start;
	}
	if (service_frames > 0) { in[0] |= IN0_SERVICE; service_frames--; }
	if (g_test_pending && m.past_boot()) { g_test_pending = false; g_test_frames = 10; }
	if (g_test_frames > 0) { in[0] |= IN0_TEST; g_test_frames--; }
	test_ago = (in[0] & IN0_TEST) ? 0 : test_ago + 1;
	if (g_plan_i < g_plan.size()) {
		const script_frame &fr = g_plan[g_plan_i++];
		for (int i = 0; i < 3; i++) in[i] |= fr.in[i];
		for (auto [addr, v] : fr.pokes) m.nvram()[(addr - 0x400000) & 0xffff] = v;
		for (auto [addr, v] : fr.work_pokes) m.work_ram()[(addr - 0x500000) % m.work_ram().size()] = v;
	}
	for (int i = 0; i < 3; i++)
		m.m_inputs[i] = uint8_t(~in[i]);
	return hk;
}

// A save is used only if it's the right size (a short one, from a power cut mid-write in an old
// version, would leave the game's backup half old data).
bool load_blob(const std::string &path, uint8_t *dst, size_t n)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	std::vector<uint8_t> buf(n + 1);
	const size_t got = fread(buf.data(), 1, n + 1, f);
	fclose(f);
	if (got != n) return false;
	memcpy(dst, buf.data(), n);
	return true;
}

// Saves are written beside the old file and then swapped in, so a crash or power-off mid-save
// leaves the previous one whole.
// Writes a file's data through to the disk before it replaces the old one: a rename alone can reach
// the disk first, and a power cut (the Deck's button) would then leave an empty file.
bool flush_to_disk(FILE *f)
{
	if (fflush(f) != 0) return false;
#ifdef _WIN32
	return _commit(_fileno(f)) == 0;
#else
	return fsync(fileno(f)) == 0;
#endif
}

bool replace_file(const std::string &path, const std::string &tmp)
{
	std::error_code ec;
	std::filesystem::rename(tmp, path, ec);
	if (ec) { std::filesystem::remove(tmp, ec); return false; }
	return true;
}

bool save_blob(const std::string &path, const uint8_t *src, size_t n)
{
	const std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) return false;
	const bool ok = fwrite(src, 1, n, f) == n && flush_to_disk(f);
	if (fclose(f) != 0 || !ok) { std::error_code ec; std::filesystem::remove(tmp, ec); return false; }
	return replace_file(path, tmp);
}

// The menu's choices and the controls, kept in settings.ini next to the saves ("key = value").
struct settings
{
	int scale = 2, aspect = 0, volume = 100, music = 100, effects = 100, aa = 1, filter = 0, ring = 0;
	int renderer3d = 2;     // the 3D's renderer: 0 the CPU, 1 the GPU, 2 Auto (whichever is quicker here)
	int akira_slot = 0;     // Cheats: who plays in Akira's place (model1::UNUSED_CHARACTERS; 0 Akira)
	int infinite_energy = 0;   // Cheats: bit 0 player 1, bit 1 player 2
	bool one_hit_ko = false, no_time_limit = false;   // Cheats
	int ring_cheat = 0;     // Cheats: 0 the arcade's, 1 tiny (600), 2 huge (5000: no ring outs)
	int ring_now() const { return ring_cheat == 1 ? 600 : ring_cheat == 2 ? 5000 : ring; }
	std::string auto_3d;    // Auto's findings, per render scale and antialiasing ("3x1:g,2x0:c")
	int arcade_coin = -1;   // Arcade mode's coin/credit setting, kept while PC mode makes it free play (-1: not seen yet)
	std::string roms_folder;   // where the player keeps vf.zip (roms_folder = ...; read before the game starts, kept as written)
	bool fullscreen = false, timings = false, arcade = false, smooth_bg = false, show_fps = false;
	player_controls ctl[2] = { player_controls::defaults(0), player_controls::defaults(1) };

	void load(const std::string &path)
	{
		FILE *f = fopen(path.c_str(), "r");
		if (!f) return;
		std::map<std::string, std::string> kv;
		char line[1100], key[64], val[1024];
		while (fgets(line, sizeof line, f))
			if (sscanf(line, " %63[a-z0-9_] = %1023[^\r\n]", key, val) == 2) kv[key] = val;
		fclose(f);
		auto num = [&](const char *k, int &v, int lo, int hi) {
			auto it = kv.find(k);
			if (it != kv.end()) v = std::clamp(atoi(it->second.c_str()), lo, hi);
		};
		auto flag = [&](const char *k, bool &v) { int i = v; num(k, i, 0, 1); v = i; };
		num("scale", scale, 1, 6);
		num("picture_shape", aspect, 0, 4);
		num("volume", volume, 0, 100);
		num("antialiasing", aa, 0, 3);   // 1 off, 0 Low (the 3D's edges smoothed), 2 and 3 drawn larger
		num("music_volume", music, 0, 100);
		num("effects_volume", effects, 0, 100);
		num("screen_filter", filter, 0, 2);
		num("ring_size", ring, 0, 1500);
		if (ring && (ring < 1200 || ring % 100)) ring = 0;   // 0: by difficulty
		flag("fullscreen", fullscreen);
		flag("frame_timings", timings);
		flag("arcade_mode", arcade);
		flag("background_filter", smooth_bg);
		flag("show_fps", show_fps);
		num("arcade_coins", arcade_coin, -1, 0xffff);
		num("renderer_3d", renderer3d, 0, 2);
		num("akira_slot", akira_slot, 0, int(std::size(model1::UNUSED_CHARACTERS)) - 1);
		num("infinite_energy", infinite_energy, 0, 3);
		flag("one_hit_ko", one_hit_ko);
		flag("no_time_limit", no_time_limit);
		num("ring_cheat", ring_cheat, 0, 2);
		if (auto it = kv.find("auto_3d"); it != kv.end()) auto_3d = it->second;
		if (auto it = kv.find("roms_folder"); it != kv.end()) roms_folder = it->second;
		for (int p = 0; p < 2; p++) ctl[p].load(kv, p);
		// settings from before the Menu button: Arcade mode's View tapped was a coin. It stays one
		// (as Coin, so the menu is L3 + R3, the touchpad, View + Start or Esc), so a cabinet's coin
		// button keeps working after the update
		if (arcade && !kv.count("p1_pad_menu"))
			for (auto &c : ctl)
				if (c.pad[C_COIN] == PAD_NONE && c.pad[C_MENU] == SDL_GAMEPAD_BUTTON_BACK) {
					c.pad[C_COIN] = SDL_GAMEPAD_BUTTON_BACK;
					c.pad[C_MENU] = PAD_NONE;
				}
	}

	void save(const std::string &path) const
	{
		const std::string tmp = path + ".tmp";
		FILE *f = fopen(tmp.c_str(), "w");
		if (!f) return;
		fprintf(f, "# Virtua Fighter settings (the in-game menu writes these)\n");
		fprintf(f, "scale = %d\npicture_shape = %d\nfullscreen = %d\nframe_timings = %d\nvolume = %d\narcade_mode = %d\n"
				"background_filter = %d\nmusic_volume = %d\neffects_volume = %d\nantialiasing = %d\nshow_fps = %d\nscreen_filter = %d\nring_size = %d\n", scale, aspect,
				int(fullscreen), int(timings), volume, int(arcade), int(smooth_bg), music, effects, aa, int(show_fps), filter, ring);
		if (arcade_coin >= 0) fprintf(f, "arcade_coins = %d\n", arcade_coin);
		fprintf(f, "renderer_3d = %d\n", renderer3d);
		if (akira_slot) fprintf(f, "akira_slot = %d\n", akira_slot);
		if (infinite_energy) fprintf(f, "infinite_energy = %d\n", infinite_energy);
		if (one_hit_ko) fprintf(f, "one_hit_ko = 1\n");
		if (no_time_limit) fprintf(f, "no_time_limit = 1\n");
		if (ring_cheat) fprintf(f, "ring_cheat = %d\n", ring_cheat);
		if (!auto_3d.empty()) fprintf(f, "auto_3d = %s\n", auto_3d.c_str());
		if (!roms_folder.empty()) fprintf(f, "roms_folder = %s\n", roms_folder.c_str());
		for (int p = 0; p < 2; p++) ctl[p].save(f, p);
		const bool flushed = flush_to_disk(f);
		if (fclose(f) == 0 && flushed) replace_file(path, tmp);
		else { std::error_code ec; std::filesystem::remove(tmp, ec); }
	}
};

// The player's own game, the first time: there are no ROMs in their data folder yet (or not a
// complete set), so ask for MAME's vf.zip with a file picker, copy it in and check it. A model1io.zip
// beside it (the real I/O board's program) comes along too; without it the stand-in board runs.
// Only those two zips are ever read, so a roms folder shared with other games, or a wrong pick,
// can't get in the way. Returns false if they cancel (or there's no way to pick a file).
// A folder of its own (a "roms" folder next to the program: the Steam Deck's, a portable Windows
// one) is new with each update unpacked somewhere new, so the zips are kept in the player's data
// folder too (`keep`: SDL's pref path) and looked for there when the folder's own roms/ has none:
// an update no longer asks for the game again.
// A file copied whole: to a temporary name, then renamed (std::filesystem::copy_file left an empty
// file in Valve's container on a shared folder, and a cut-short copy mustn't look like a set)
static bool copy_whole(const std::filesystem::path &from, const std::filesystem::path &to, std::string &why)
{
	FILE *in = fopen(from.string().c_str(), "rb");
	if (!in) { why = "can't read " + from.string(); return false; }
	const std::filesystem::path tmp = to.string() + ".tmp";
	FILE *out = fopen(tmp.string().c_str(), "wb");
	if (!out) { fclose(in); why = "can't write " + tmp.string(); return false; }
	std::vector<char> buf(1 << 20);
	bool ok = true;
	for (size_t n; (n = fread(buf.data(), 1, buf.size(), in)) > 0;)
		if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
	ok = ok && !ferror(in);
	fclose(in);
	ok = (fclose(out) == 0) && ok;
	if (ok) ok = replace_file(to.string(), tmp.string());
	else { std::error_code ec; std::filesystem::remove(tmp, ec); }
	if (!ok) why = "couldn't write " + to.string();
	return ok;
}

// The player's ROM folder (--roms, VF_ROMS or settings.ini's roms_folder: `folder`) is looked in
// first and only read: nothing is copied out of it or kept elsewhere. Without the set there, the
// usual places are tried next; a set chosen with the picker then is copied into that folder (where
// the next start looks), not into the data folder.
bool first_launch(const std::string &data, const std::string &keep, const std::string &folder, model1::regions &out)
{
	namespace fs = std::filesystem;
	const fs::path own = fs::path(data) / "roms";
	const fs::path roms = folder.empty() ? own : fs::path(folder);       // (where a picked set is copied)
	const fs::path kept = !folder.empty() || keep.empty() ? own : fs::path(keep) / "roms";
	auto zips_in = [&](const fs::path &dir) {
		std::vector<std::string> z;
		std::error_code ec;
		for (const char *name : { "vf.zip", "model1io.zip" })
			if (fs::is_regular_file(dir / name, ec)) z.push_back((dir / name).string());
		return z;
	};
	auto zips_here = [&] { return zips_in(roms); };
	// a copy kept for the next update (when it isn't there already, or is another set)
	auto keep_copy = [&] {
		if (kept == roms || !folder.empty()) return;
		std::error_code ec;
		fs::create_directories(kept, ec);
		for (const char *name : { "vf.zip", "model1io.zip" }) {
			if (!fs::is_regular_file(roms / name, ec)) continue;
			if (fs::is_regular_file(kept / name, ec) && fs::file_size(kept / name, ec) == fs::file_size(roms / name, ec)) continue;
			std::string why;
			if (!copy_whole(roms / name, kept / name, why)) fprintf(stderr, "couldn't keep a copy of %s: %s\n", name, why.c_str());
		}
	};
	std::vector<std::string> have = zips_here();
	std::string problem;
	if (!folder.empty()) {
		if (have.empty()) {
			std::error_code ec;
			fprintf(stderr, "ROM folder: no vf.zip in %s%s: looking in %s next\n", roms.string().c_str(),
					fs::is_directory(roms, ec) ? "" : " (no such folder)", own.string().c_str());
			if (!(have = zips_in(own)).empty()) fprintf(stderr, "the game from %s\n", own.string().c_str());
		} else fprintf(stderr, "the game from the ROM folder %s\n", roms.string().c_str());
	}
	if (!have.empty()) {
		romset::result r = romset::load(have);
		if (r.ok) { keep_copy(); out = std::move(r.regions); return true; }
		problem = r.error + "\n\n";
	} else if (kept != roms) {
		// an update in a new folder: the set kept from before
		std::vector<std::string> before = zips_in(kept);
		if (!before.empty()) {
			romset::result r = romset::load(before);
			if (r.ok) { fprintf(stderr, "the game from %s\n", kept.string().c_str()); out = std::move(r.regions); return true; }
			fprintf(stderr, "the game kept in %s: %s\n", kept.string().c_str(), r.error.c_str());
		}
	}
	if (getenv("VF_HIDDEN")) { fprintf(stderr, "%sno game found in %s (hidden: no picker)\n", problem.c_str(), roms.string().c_str()); return false; }   // testing
	SDL_Init(SDL_INIT_VIDEO);
	const std::string where = "It's copied into " + roms.string() + ", so you only do this once.";
	std::string note = problem + "Choose your copy of Virtua Fighter: MAME's \"vf\" set (vf.zip).\n\n" + where;
	for (;;) {
		if (!message_box(SDL_MESSAGEBOX_INFORMATION, "Virtua Fighter", note)) return false;
		struct pick { bool done = false, failed = false; std::vector<std::string> files; } chosen;
		static const SDL_DialogFileFilter filters[] = { { "MAME ROM sets (zip)", "zip" } };
		SDL_ShowOpenFileDialog([](void *ud, const char *const *list, int) {
			auto *c = static_cast<pick *>(ud);
			if (!list) c->failed = true;                  // no picker here (e.g. Steam Deck Game Mode)
			else for (; *list; list++) c->files.push_back(*list);
			c->done = true;
		}, &chosen, nullptr, filters, 1, nullptr, false);
		while (!chosen.done) { SDL_Event e; while (SDL_PollEvent(&e)) {} SDL_Delay(10); }
		if (chosen.failed) {
			const std::string msg = "There's no file picker here. Put your vf.zip in this folder, then start the game again:\n\n"
									+ roms.string();
			std::error_code ec;
			fs::create_directories(roms, ec);
			message_box(SDL_MESSAGEBOX_INFORMATION, "Virtua Fighter", msg);
			return false;
		}
		if (chosen.files.empty()) return false;
		const std::string f = chosen.files[0];
		if (!romset::is_game_set(f)) {
			note = fs::path(f).filename().string() + " isn't MAME's Virtua Fighter set.\n\nChoose your vf.zip.\n\n" + where;
			continue;
		}
		std::error_code ec;
		fs::create_directories(roms, ec);
		std::string why;
		if (!copy_whole(f, roms / "vf.zip", why)) {
			note = "Couldn't copy it into " + roms.string() + ": " + why + "\n\nChoose your vf.zip.";
			continue;
		}
		const fs::path io = fs::path(f).parent_path() / "model1io.zip";    // its neighbour, if any
		if (fs::exists(io, ec)) copy_whole(io, roms / "model1io.zip", why);
		romset::result r = romset::load(zips_here());
		if (r.ok) { keep_copy(); out = std::move(r.regions); return true; }
		note = r.error + "\n\nChoose MAME's \"vf\" set (vf.zip).";
	}
}

// A folder as the player writes it: "~" (or "~/...") is their home folder; quotes around it are
// dropped; a relative one is taken from `base` (settings.ini's folder) or the current folder.
std::string player_folder(std::string f, const std::string &base = std::string())
{
	while (!f.empty() && (f.back() == ' ' || f.back() == '\t')) f.pop_back();
	if (f.size() >= 2 && (f[0] == '"' || f[0] == '\'') && f.back() == f[0]) f = f.substr(1, f.size() - 2);
	if (f.empty()) return f;
	if (f[0] == '~' && (f.size() == 1 || f[1] == '/' || f[1] == '\\')) {
		const char *h = getenv("HOME");
		if (!h || !*h) h = getenv("USERPROFILE");
		if (h && *h) f = std::string(h) + f.substr(1);
	}
	std::filesystem::path p(f);
	std::error_code ec;
	if (p.is_relative()) p = base.empty() ? std::filesystem::absolute(p, ec) : std::filesystem::path(base) / p;
	p = p.lexically_normal();
	if (!p.has_filename() && p.has_relative_path()) p = p.parent_path();   // (no separator at the end)
	return p.string();
}

// The program's folder, with a separator at the end: a Mac app's is the folder the .app is in (SDL's
// base path in an app is its Contents/Resources)
std::string program_folder()
{
	const char *base = SDL_GetBasePath();
	std::string b = base ? base : "";
	const std::string in_app = ".app/Contents/Resources/";
	if (b.size() > in_app.size() && b.compare(b.size() - in_app.size(), in_app.size(), in_app) == 0) {
		b.resize(b.size() - in_app.size());   // (".../Virtua Fighter Recomp")
		b.resize(b.find_last_of('/') + 1);
	}
	return b;
}
// Portable mode, as the player asks for it: a file named portable.txt beside the program (its name
// as found, else null). Any contents; "portable" and Windows' portable.txt.txt (a New Text Document
// renamed with its extension hidden) count too.
const char *portable_marker(const std::string &folder)
{
	if (folder.empty()) return nullptr;
	std::error_code ec;
	for (const char *name : { "portable.txt", "portable", "portable.txt.txt" })
		if (std::filesystem::is_regular_file(folder + name, ec)) return name;
	return nullptr;
}

// The menu's files: next to the program in a package, else the source tree's assets/.
std::string find_assets()
{
	const char *base = SDL_GetBasePath();
	std::string b = base ? base : "";
	for (std::string d : { b + "assets", b + "../../assets", std::string("assets") })
		if (FILE *f = fopen((d + "/ui/menu.rml").c_str(), "rb")) { fclose(f); return d; }
	return "assets";
}

} // namespace

int main(int argc, char **argv)
{
	// test runs (VF_HIDDEN): a background process on macOS, so no Dock icon appears for each one
	if (getenv("VF_HIDDEN")) SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
	std::string dir = "build/vf";
	// a packaged build keeps its data next to the program
	if (const char *base = SDL_GetBasePath()) {
		std::string d = std::string(base) + "data";
		if (FILE *f = fopen((d + "/maincpu.bin").c_str(), "rb")) { fclose(f); dir = d; }
	}
	// picture shapes F6 / View+RB cycle through
	// 0 = follow the window's shape; the rest are fixed
	const float ASPECTS[] = { 0.0f, 4.0f / 3.0f, 16.0f / 10.0f, 16.0f / 9.0f, 64.0f / 27.0f };
	const char *ASPECT_NAMES[] = { "auto", "4:3", "16:10", "16:9", "21:9" };
	const int N_ASPECTS = 5;
	// the command line wins over settings.ini, which is found in the data directory
	int arg_scale = 0, arg_aspect = -1, arg_full = -1;
	bool interp = false, arg_arcade = false;
	std::string arg_roms;   // --roms <folder>: where the player's vf.zip is (over VF_ROMS and settings.ini's roms_folder)
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--scale" && i + 1 < argc) arg_scale = atoi(argv[++i]);
		else if (a == "--roms" && i + 1 < argc) arg_roms = argv[++i];
		else if (a.rfind("--roms=", 0) == 0) arg_roms = a.substr(7);
		else if (a == "--interp") interp = true;      // MAME's V60 interpreter instead of the recompiled code
		else if (a == "--arcade") arg_arcade = true;  // boot screen, credits and coins as on the cabinet
		else if (a == "--aspect" && i + 1 < argc) {
			std::string v = argv[++i];
			for (int k = 0; k < N_ASPECTS; k++) if (v == ASPECT_NAMES[k]) arg_aspect = k;
		} else if (a == "--fullscreen") arg_full = 1;
		else if (a == "--window") arg_full = 0;
		else if (a.rfind("-psn_", 0) == 0) {}   // (an old macOS's Finder adds its process number)
		else if (a.rfind("--", 0) == 0) { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
		else {
			// a regions folder named on the command line: one that's there (a stray word, say a value
			// after an option that takes none, was taken as the folder)
			std::error_code ec;
			if (!std::filesystem::is_directory(a, ec)) { fprintf(stderr, "%s: no such folder (options start with --)\n", a.c_str()); return 2; }
			dir = a;
		}
	}
	// Where the game comes from: a regions folder (our own builds, tools/romset.py: build/vf by
	// default), else the player's zips in their own data folder, which also holds the settings,
	// saves and backgrounds/.
	if (const char *sc = getenv("VF_SCRIPT")) load_script(sc);
	model1::regions game;
	const char *portable = nullptr;   // (portable mode: the marker's name)
	{
		std::error_code ec;
		if (std::filesystem::exists(dir + "/maincpu.bin", ec)) {
			game = load_regions(dir);
		} else {
			// portable mode (a file named portable.txt beside the program, or beside a Mac's .app)
			// keeps everything in the program's folder and nothing anywhere else: not even the
			// user data folder made (SDL_GetPrefPath makes it), nor a copy of the zips kept there.
			// Else a "roms" folder next to the program (the Steam Deck package) keeps the rest
			// there, the zips also kept in the player's data folder; else the player's data folder.
			const char *base = SDL_GetBasePath();
			const std::string here = base ? std::string(base) : std::string();
			const std::string home = program_folder();
			portable = portable_marker(home);
			char *pref = portable ? nullptr : SDL_GetPrefPath(nullptr, "Virtua Fighter Recomp");
			std::string keep = pref ? pref : "";                                 // (the zips kept across updates)
			if (getenv("VF_DATA")) keep.clear();                                 // (testing: the player's own left alone)
			if (const char *k = getenv("VF_KEEP"); k && !portable) keep = k;     // testing: another user's data folder
			if (getenv("VF_DATA")) dir = getenv("VF_DATA");                     // testing
			else if (portable) dir = home;
			else if (!here.empty() && std::filesystem::is_directory(here + "roms", ec)) dir = here;
			else dir = pref ? pref : ".";
			SDL_free(pref);
			while (!keep.empty() && keep.back() == '/') keep.pop_back();
			while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\') && dir[dir.size() - 2] != ':') dir.pop_back();   // (but a drive's root)
			// A player's build has no console (a Mac app, a Windows GUI program, a Steam shortcut):
			// its messages go to log.txt in the data folder instead, for when something goes wrong.
			if (!isatty(fileno(stderr))) {
				std::filesystem::create_directories(dir, ec);
				freopen((dir + "/log.txt").c_str(), "w", stderr);
				setvbuf(stderr, nullptr, _IONBF, 0);
			}
			// the log's first line: which build (as the menu's footer), and where it runs
			fprintf(stderr, "Virtua Fighter Recomp %s (%s) on %s, %d logical processors, %d MB of memory\n", VF_VERSION, *VF_DATE ? VF_DATE : "no date",
					SDL_GetPlatform(), SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM());
			if (portable) fprintf(stderr, "portable (%s beside the program): everything in %s\n", portable, dir.c_str());
			else if (home.find("/AppTranslocation/") != std::string::npos)   // (macOS runs a downloaded app from a hidden copy until it's moved)
				fprintf(stderr, "run from macOS's hidden copy of the app (App Translocation): a portable.txt beside it isn't seen until the app is moved with the Finder\n");
			// the player's ROM folder: the command line's, else VF_ROMS, else settings.ini's
			std::string roms_folder, from;
			if (!arg_roms.empty()) roms_folder = player_folder(arg_roms), from = "--roms";
			else if (const char *e = getenv("VF_ROMS"); e && *e) roms_folder = player_folder(e), from = "VF_ROMS";
			else {
				settings pre;
				pre.load(dir + "/settings.ini");
				if (!pre.roms_folder.empty()) roms_folder = player_folder(pre.roms_folder, dir), from = "settings.ini";
			}
			if (!roms_folder.empty()) fprintf(stderr, "ROM folder (%s): %s\n", from.c_str(), roms_folder.c_str());
			else fprintf(stderr, "ROM folder: %s\n", (std::filesystem::path(dir) / "roms").string().c_str());
			if (!first_launch(dir, keep == dir ? std::string() : keep, roms_folder, game)) return getenv("VF_HIDDEN") ? 1 : 0;   // (a test run without the game: an error)
		}
	}
	const std::string settings_path = dir + "/settings.ini";
	settings cfg;
	bool cfg_dirty = false;      // saved at exit only when the player changed something
	// The first run's render scale: 2x everywhere (the pictures are drawn on the CPU, and a thread
	// count says little about how fast it is); the menu goes higher. Steam sets SteamDeck=1 on the
	// Deck: fullscreen, 16:10.
	cfg.scale = 2;
	if (getenv("SteamDeck")) { cfg.fullscreen = true; cfg.aspect = 2; }
	cfg.load(settings_path);
	if (arg_scale) cfg.scale = std::clamp(arg_scale, 1, 6);
	if (arg_aspect >= 0) cfg.aspect = arg_aspect;
	if (arg_full >= 0) cfg.fullscreen = arg_full;
	if (arg_arcade) cfg.arcade = true;
	g_pc_mode = !cfg.arcade;
	// the 3D's renderer: VF_3D=cpu|gpu|auto (tests) over the setting
	if (const char *r = getenv("VF_3D")) cfg.renderer3d = !strcmp(r, "cpu") ? 0 : !strcmp(r, "gpu") ? 1 : 2;
#if VF_GPU3D_METAL
	gpu3d_metal_prewarm();   // (its shaders compiled while the game boots: ready if the menu or Auto picks it)
#endif
	int aspect_i = cfg.aspect;
	const bool fullscreen = cfg.fullscreen;
	const int scale = cfg.scale;
	model1 m(std::move(game));
	m.enable_sound();
	// antialiasing draws larger and averages down: within 8x the arcade's size in all (at 32:9 that is
	// already 10,576 x 3,072 and about 600 MB of buffers; more would fail on ordinary machines)
	auto aa_fit = [](int scale, int aa) { return std::max(1, std::min(aa, 8 / scale)); };
	m.set_render_scale(scale * aa_fit(scale, cfg.aa));   // antialiasing: drawn larger, averaged down
	m.set_edge_aa(cfg.aa == 0);                           // or Low: the edges smoothed
	m.set_smooth(true);   // always: a picture for each refresh of the screen, between the arcade's 57.5 frames a second
	m.set_sky_filter(cfg.smooth_bg);
	m.set_aspect(ASPECTS[aspect_i] > 0 ? ASPECTS[aspect_i] : 16.0f / 9.0f);
	// the real I/O board's 93C46 and the stand-in's image are laid out differently
	const std::string nv_path = dir + "/nvram.sav",
		ee_path = dir + (m.real_ioboard() ? "/eeprom_93c46.sav" : "/eeprom.sav");
	load_blob(nv_path, m.nvram().data(), m.nvram().size());
	load_blob(ee_path, m.eeprom(), model1::EEPROM_SIZE);
	// the backup as loaded: the autosave in the loop writes it once it differs (free play set at
	// boot, the game's first settings, an arcade setting changed in the first seconds)
	auto backup_hash = [&] {
		uint64_t h = 1469598103934665603ull;
		for (uint8_t b : m.nvram()) h = (h ^ b) * 1099511628211ull;
		for (size_t i = 0; i < model1::EEPROM_SIZE; i++) h = (h ^ m.eeprom()[i]) * 1099511628211ull;
		return h;
	};
	uint64_t saved_hash = backup_hash();
	if (!interp) m.run_cpu = [](int n) { rec::run(n); };
	m.set_no_time_limit(cfg.no_time_limit);   // (Cheats: an instruction of the game's, so before it's recompiled)
#ifdef VF_NATIVE
	// The game's code, recompiled to native code from this ROM, now (tens of milliseconds); the
	// address map says where the code is. Without it the game runs on the interpreter.
	auto recompile = [&] {
		rec::hints map;
		std::vector<uint8_t> mem = m.maincpu_rom();
		mem.resize(0x2000000);
		rec::native_stats st;
		const bool ok = map.read(find_assets() + "/vf_map.txt") && rec::build_native(mem, map, &st);
		if (ok)
			fprintf(stderr, "recompiled %zu functions (%zu instructions) to %zu KB of native code in %.0f ms\n",
					st.functions, st.instructions, st.code_bytes / 1024, st.ms);
		return ok;
	};
	if (!interp) {
		rec::attach(m.cpu());
		if (!recompile()) {
			// Still playable, just slower: say so (the log is the only console a packaged build has).
			const std::string why = "Couldn't recompile the game (" + find_assets() + "/vf_map.txt missing or unusable, "
									"or no executable memory): running it on the slower interpreter instead.";
			fprintf(stderr, "%s\n", why.c_str());
			message_box(SDL_MESSAGEBOX_WARNING, "Virtua Fighter", why);
			rec::detach(m.cpu());
			interp = true;
			m.run_cpu = nullptr;
		}
	}
#endif
	scene_sync sync;                     // clean scene changes (the menu's Display page)
	sync.enabled = true;                 // always: it is a fix, not a choice
	// Reset; the PC build then runs through the boot screen (about 3.5 s of settings text) unseen.
	auto boot = [&] {
		m.reset();
		if (!interp) rec::attach(m.cpu());   // a fresh game stack
		std::fill_n(&m.work_ram()[0x17f0], 4, 0);   // work RAM survives a reset: forget the last camera (past_boot)
		if (!g_pc_mode) { sync.start(); return; }
		m.set_drawing(false);
		for (int i = 0; i < 900 && !m.past_boot(); i++) m.run_frame();
		m.set_drawing(true);
		m.m_audio_out.clear();
		sync.start();
	};
	load_backgrounds(m, find_assets() + "/backgrounds");   // the title and select's widescreen sides
	load_backgrounds(m, dir + "/backgrounds");   // the player's own (hi-res stage skies...), which win
	m.set_ring_width(cfg.ring_now());
	m.set_akira_slot(cfg.akira_slot);
	auto icon_dirs = [&] { return std::vector<std::string>{ dir + "/icons", find_assets() + "/icons" }; };   // (the player's first)
	load_secret_icon(m, cfg.akira_slot, icon_dirs());
	// The play style's coins: PC mode is free play (its FREE PLAY text hidden); Arcade mode has the
	// cabinet's own coin setting, kept in settings.ini while PC mode has free play in its place (it
	// was left at free play, so Arcade mode after PC mode had no credits to put in, and a cabinet's
	// coin slot did nothing). The settings are read at boot: when they change, boot again.
	auto settle_coins = [&]() -> bool {
		const int now = m.coin_setting();
		if (now < 0) return false;                       // (the game's very first boot writes them)
		if (g_pc_mode) {
			if (now != model1::FREE_PLAY && now != cfg.arcade_coin) { cfg.arcade_coin = now; cfg_dirty = true; }
			return m.force_free_play();
		}
		// Arcade: its own setting back; free play left by PC mode before one was kept: the game's first
		return m.set_coin_setting(cfg.arcade_coin >= 0 ? cfg.arcade_coin : now == model1::FREE_PLAY ? 0 : now);
	};
	// Arcade mode's setting as it is now (the service menu may have changed it), kept for when PC mode replaces it
	auto keep_arcade_coins = [&] {
		const int now = m.coin_setting();
		if (!g_pc_mode && now >= 0 && now != cfg.arcade_coin) { cfg.arcade_coin = now; cfg_dirty = true; }
	};
	auto apply_play_style = [&] { if (g_pc_mode) m.hide_free_play_text(); else m.show_free_play_text(); };
	apply_play_style();
	boot();
	if (settle_coins()) boot();

	// Video is a must (with a message if it fails: a player's build has no console); sound and
	// pads aren't: without a sound device the game keeps time by the clock.
	auto fail = [](const char *what) {
		const std::string msg = std::string(what) + ": " + SDL_GetError();
		fprintf(stderr, "%s\n", msg.c_str());
		message_box(SDL_MESSAGEBOX_ERROR, "Virtua Fighter", msg);
		return 1;
	};
#ifdef _WIN32
	// Windows only lets the program in front hand over the foreground: started from another program,
	// the game could open behind it with the keys going there. SDL's raise can force it.
	SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
#endif
	if (!SDL_Init(SDL_INIT_VIDEO)) return fail("Couldn't start the video");
	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) fprintf(stderr, "no gamepads: %s\n", SDL_GetError());
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) fprintf(stderr, "no sound: %s\n", SDL_GetError());
	// the first window: 1366 x 768, or 16:9 within 90% of a smaller screen's usable area
	int win_w = 1366, win_h = 768;
	// testing: VF_WINDOW=WxH, the window's size in points (a hidden one as large as a player's screen)
	const bool fixed_window = getenv("VF_WINDOW") && sscanf(getenv("VF_WINDOW"), "%dx%d", &win_w, &win_h) == 2;
	{
		SDL_Rect usable;
		if (!fixed_window && SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {
			const float k = std::min({ 1.0f, usable.w * 0.9f / win_w, usable.h * 0.9f / win_h });
			win_w = int(win_w * k); win_h = int(win_h * k);
		}
	}
	SDL_Window *win = SDL_CreateWindow("Virtua Fighter", win_w, win_h,
									   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
									   (fullscreen ? SDL_WINDOW_FULLSCREEN : 0) |
									   (getenv("VF_HIDDEN") ? SDL_WINDOW_HIDDEN : 0));   // testing: never on screen
	if (!win) return fail("Couldn't open the window");
	SDL_HideCursor();
	// Started from a terminal, the window can open behind it or without the keyboard (the keys go
	// to the terminal): take it, and again once the first picture is up if it still isn't ours.
	if (!getenv("VF_HIDDEN")) SDL_RaiseWindow(win);
	int focus_checks = 600;   // about 5 s: going fullscreen on a Mac moves to a new Space, which takes a while
	SDL_Renderer *ren = SDL_CreateRenderer(win, nullptr);
	if (!ren) return fail("Couldn't start the graphics");
	SDL_SetRenderVSync(ren, 1);          // one picture per refresh of the screen (smooth motion fills them)
	SDL_Texture *tex = nullptr;
	auto aa_now = [&] { return m.render_scale() / std::max(1, cfg.scale); };   // the antialiasing in use
	// Antialiasing: the picture is drawn cfg.aa times larger each way and each aa x aa block
	// averaged into one pixel of what is shown.
	auto shown_w = [&] { return m.frame_w() / aa_now(); };
	auto shown_h = [&] { return m.frame_h() / aa_now(); };
	std::vector<uint32_t> aa_buf;
	// the antialiased picture into dst (stride in pixels): a buffer, or the texture's own memory
	auto aa_into = [&](uint32_t *dst, size_t stride) {
		const int k = aa_now();
		const int w = shown_w(), h = shown_h(), sw = m.frame_w();
		const uint32_t *src = m.frame().data();
		// red and blue added together in one word (0x00ff00ff lanes), green in another: 9 pixels
		// fit (9 x 255 < 4096), and the average is a multiply and a shift, not a divide
		parallel_rows(h, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++) {
				uint32_t *d = dst + size_t(y) * stride;
				const uint32_t *r0 = src + size_t(y * k) * sw;
				if (k == 2) {
					const uint32_t *r1 = r0 + sw;
					for (int x = 0; x < w; x++) {
						const uint32_t a0 = r0[2 * x], a1 = r0[2 * x + 1], b0 = r1[2 * x], b1 = r1[2 * x + 1];
						const uint32_t rb = (a0 & 0xff00ff) + (a1 & 0xff00ff) + (b0 & 0xff00ff) + (b1 & 0xff00ff);
						const uint32_t g = (a0 & 0xff00) + (a1 & 0xff00) + (b0 & 0xff00) + (b1 & 0xff00);
						d[x] = ((rb >> 2) & 0xff00ff) | ((g >> 2) & 0xff00);
					}
				} else if (k == 3) {
					const uint32_t *r1 = r0 + sw, *r2 = r1 + sw;
					for (int x = 0; x < w; x++) {
						uint32_t rb = 0, g = 0;
						for (int i = 0; i < 3; i++) {
							const uint32_t p = r0[3 * x + i], q = r1[3 * x + i], t = r2[3 * x + i];
							rb += (p & 0xff00ff) + (q & 0xff00ff) + (t & 0xff00ff);
							g += (p & 0xff00) + (q & 0xff00) + (t & 0xff00);
						}
						// / 9 as * 7282 >> 16 (exact for sums up to 2295)
						const uint32_t r = ((rb >> 16) * 7282) >> 16, bl = ((rb & 0xffff) * 7282) >> 16, gg = ((g >> 8) * 7282) >> 16;
						d[x] = r << 16 | gg << 8 | bl;
					}
				} else {
					for (int x = 0; x < w; x++) {
						uint32_t r = 0, g = 0, b = 0;
						for (int j = 0; j < k; j++) {
							const uint32_t *p = r0 + size_t(j) * sw + x * k;
							for (int i = 0; i < k; i++) { r += (p[i] >> 16) & 0xff; g += (p[i] >> 8) & 0xff; b += p[i] & 0xff; }
						}
						const uint32_t n = k * k;
						d[x] = (r / n) << 16 | (g / n) << 8 | (b / n);
					}
				}
			}
		});
	};
	auto shown_frame = [&]() -> const uint32_t * {
		if (aa_now() == 1) return m.frame().data();
		aa_buf.resize(size_t(shown_w()) * shown_h());
		aa_into(aa_buf.data(), shown_w());
		return aa_buf.data();
	};
	// the picture into the texture: antialiasing averages straight into its memory (no extra copy)
	// The 2D enlarged and put together with the 3D by the GPU (gpu_compose.h), unless antialiasing
	// averages the picture down first, or the renderer can't (VF_CPU_2D=1: the CPU does it all, testing).
	// At startup the GPU puts a few known pictures together and the CPU the same ones: any difference,
	// or a feature this renderer lacks, and the CPU draws every picture whole for the session. The
	// first few of the game's own pictures are checked too. log.txt says which it is.
	gpu_compose gc;
	bool gpu_2d = false;
	{
		const char *rname = SDL_GetRendererName(ren);
		std::string why;
		if (getenv("VF_CPU_2D")) why = "VF_CPU_2D";
		else if (!gc.init(ren)) why = "this renderer can't";
		else if (gc.self_check(why)) gpu_2d = true;
		if (gpu_2d) fprintf(stderr, "pictures put together on the GPU (renderer: %s)\n", rname ? rname : "?");
		else fprintf(stderr, "pictures drawn on the CPU (renderer: %s; %s)\n", rname ? rname : "?", why.c_str());
	}
	// The 3D on the GPU (src/gpu/gpu3d.h, Display > 3D renderer): on the device SDL's Metal renderer
	// draws with; off the Mac SDL_GPU's (Direct3D 12 or Vulkan), a device of its own, made only when
	// the setting isn't CPU (VF_3D=cpu|gpu|auto over it), its pictures checked against the CPU's
	// before they're trusted. Any other renderer, or no GPU: the CPU's.
	std::unique_ptr<gpu3d> g3;
	bool g3_next_start = false;   // (SDL_GPU: none made, the setting CPU as the game started)
#if VF_GPU3D_METAL
	{
		const char *rname = SDL_GetRendererName(ren);
		if (rname && !strcmp(rname, "metal")) g3 = gpu3d_create_metal_layer(SDL_GetRenderMetalLayer(ren));
		if (!g3) fprintf(stderr, "the 3D on the CPU: no GPU for it here (renderer: %s)\n", rname ? rname : "?");
	}
#elif VF_GPU3D_SDL
	{
		// (2026-10-15, as vf2-recomp's for its #6: an old card's driver (an HD 7800 with CRT Emudriver)
		// closed the game inside the GPU's start, with no message, at every start. A marker is left
		// while it starts: there next time, it crashed then, so the CPU's 3D, said so. Deleting
		// gpu-start.crashed tries again.)
		const std::string marker = dir + "/gpu-start.crashed";
		const bool crashed = cfg.renderer3d != 0 && SDL_GetPathInfo(marker.c_str(), nullptr);
		if (crashed) fprintf(stderr, "the 3D on the CPU: the last start closed while starting the GPU's (%s)\n", marker.c_str());
		if (cfg.renderer3d != 0 && !crashed) {
			const uint64_t t0 = SDL_GetTicksNS();
			if (SDL_IOStream *io = SDL_IOFromFile(marker.c_str(), "w")) SDL_CloseIO(io);
			g3 = gpu3d_create_sdlgpu();
			SDL_RemovePath(marker.c_str());
			fprintf(stderr, "the 3D on the GPU: %s (%.0f ms to start)\n", g3 ? g3->name() : "none here (the CPU's 3D)", (SDL_GetTicksNS() - t0) / 1e6);
		} else if (!crashed) {
			g3_next_start = true;
			fprintf(stderr, "the 3D on the CPU (the setting): the GPU's from the next start if chosen\n");
		}
	}
#else
	fprintf(stderr, "the 3D on the CPU: no GPU 3D in this build\n");
#endif
	if (g3) {
		if (const char *r = getenv("GPU3D_REPEAT")) g3->test_repeat = std::max(1, atoi(r));   // tests: a slow GPU
		m.set_gpu3d(g3.get());
		if (const char *c = getenv("VF_GPU3D_CHECK")) m.m_gpu3d_check = std::max(0, atoi(c));
#if VF_GPU3D_SDL
		m.m_gpu3d_verify = 3;   // (the game's pictures: three checked against the CPU's)
#endif
	}
	// Auto: the GPU, unless clearly slower with the frame at risk (auto_gpu below). Measured before the first picture, unseen (a made-up fight's worth of polygons, a few
	// times each way), and again whenever the scale or antialiasing changes; then checked on the
	// game's own pictures every 10 s, four pictures drawn the other way (the first not counted) (the two ways' pictures are
	// the same pixels, so a switch shows nothing). What it found is kept in settings.ini per scale
	// and antialiasing, the start's guess until it has measured. Any GPU error: the CPU for the run.
	struct auto3d_state {
		bool gpu = false;              // the GPU in use (Auto's choice, or GPU)
		bool broken = false;           // a GPU error: the CPU for the run
		std::vector<double> c, g;      // the last pictures' times each way (ms)
		int probe = 0;                 // pictures still to draw the other way
		uint64_t next = 0;             // the next check (ns)
		uint64_t failed = 0;
		bool tested = false;           // SDL_GPU: its test picture checked against the CPU's
	} a3;
	auto auto_key = [&] { return std::to_string(cfg.scale) + "x" + std::to_string(cfg.aa); };
	auto auto_remember = [&](bool gpu) {
		const std::string k = auto_key() + ":";
		std::string out;
		size_t i = 0;
		while (i < cfg.auto_3d.size()) {
			size_t j = cfg.auto_3d.find(',', i);
			if (j == std::string::npos) j = cfg.auto_3d.size();
			const std::string e = cfg.auto_3d.substr(i, j - i);
			if (!e.empty() && e.compare(0, k.size(), k)) out += (out.empty() ? "" : ",") + e;
			i = j + 1;
		}
		cfg.auto_3d = out + (out.empty() ? "" : ",") + k + (gpu ? "g" : "c");
	};
	auto auto_recalled = [&]() -> int {   // -1 none, 0 the CPU, 1 the GPU
		const std::string k = auto_key() + ":";
		const size_t at = ("," + cfg.auto_3d).find("," + k);
		if (at == std::string::npos) return -1;
		const size_t v = at + k.size();
		return v < cfg.auto_3d.size() ? (cfg.auto_3d[v] == 'g') : -1;
	};
	// Auto prefers the GPU: the CPU only when the GPU is clearly slower in a way that matters, over
	// 1.5x the CPU's time and over half a frame (8 ms of the 16.7), so the frame's time is at risk
	// (a GPU a little slower than a fast Mac's CPU, well inside the frame, is still the GPU). Back
	// to the GPU once it's within 1.3x or under 6 ms.
	auto auto_gpu = [](bool was, double c, double g) {
		return was ? !(g > 1.5 * c && g > 8.0) : (g <= 1.3 * c || g < 6.0);
	};
	auto apply_3d = [&] { m.set_gpu3d_on(g3 && !a3.broken && a3.gpu); m.m_gpu3d_chosen = cfg.renderer3d == 1; };
	auto renderer_name = [&]() -> std::string {
#if VF_GPU3D_METAL
		if (!g3 || a3.broken) return "CPU (no GPU here)";
#elif VF_GPU3D_SDL
		if (!g3 && g3_next_start && cfg.renderer3d) return cfg.renderer3d == 2 ? "Auto (from the next start)" : "GPU (from the next start)";
		if (!g3 || a3.broken) return "CPU (no GPU here)";
#else
		if (!g3 || a3.broken) return "CPU (no GPU 3D in this build)";
#endif
		if (cfg.renderer3d == 2) return a3.gpu ? "Auto (GPU)" : "Auto (CPU)";
		return cfg.renderer3d == 1 ? "GPU" : "CPU";
	};
	// Auto's measure, before the first picture and at each new scale or antialiasing
	auto auto_measure = [&](const char *why) {
		if (!g3 || a3.broken) {
			a3.gpu = false; apply_3d();
			fprintf(stderr, "3D renderer: %s (%s): the CPU (no GPU 3D here)\n", cfg.renderer3d == 2 ? "Auto" : cfg.renderer3d ? "GPU" : "CPU", why);
			return;
		}
#if VF_GPU3D_SDL
		// (SDL_GPU: its picture of the test scene checked against the CPU's before it's used)
		// (Direct3D 12's test failing, Vulkan's tried at once: SDL_GPU's other on Windows, where some
		// older cards draw nothing on one and are fine on the other (vf2-recomp #3: a GTX 970); the
		// 3D has a device of its own, so the screen's renderer is untouched. SDL_GPU_DRIVER, SDL's
		// own, chooses over this.)
		for (int attempt = 0; cfg.renderer3d != 0 && !a3.tested; attempt++) {
			a3.tested = true;
			double c = 0, g = 0;
			long long d = -1;
			m.m_gpu3d_chosen = cfg.renderer3d == 1;
			const long long px = (long long)m.frame_w() * m.frame_h();
			const auto &dd = m.m_bench_diff;
			if (!m.bench3d(1, c, g, &d) || d < 0 || !m.gpu_diff_ok(dd, px)) {
				if (d > 0) fprintf(stderr, "the 3D on the GPU (%s): its test picture isn't the CPU's (%lld of %lld pixels differ, %lld drawn on one only, %lld by more than %d)\n",
								   g3->name(), d, px, dd.cover, dd.big, model1::GPU_ROUNDING_COLOUR);
				else fprintf(stderr, "the 3D on the GPU (%s): its test picture failed\n", g3->name());
				if (attempt == 0 && !strncmp(g3->name(), "direct3d12", 10) && !getenv("SDL_GPU_DRIVER")) {
					m.set_gpu3d(nullptr);
					g3.reset();
					g3 = gpu3d_create_sdlgpu("vulkan");
					fprintf(stderr, "the 3D on the GPU: trying Vulkan instead: %s\n", g3 ? g3->name() : "none here");
					if (g3) { m.set_gpu3d(g3.get()); a3.tested = false; continue; }
					a3.broken = true; a3.gpu = false; apply_3d();   // (g3 gone: the CPU's)
					return;
				}
				a3.broken = true; a3.gpu = false; apply_3d();
				fprintf(stderr, "the 3D on the GPU: the CPU for this run\n");
				return;
			}
			if (d > 0) fprintf(stderr, "the 3D on the GPU (%s): its test picture: %lld of %lld pixels differ by rounding (%lld drawn on one only, %lld by more than %d, colours by %d at most): keeping the GPU\n",
							   g3->name(), d, px, dd.cover, dd.big, model1::GPU_ROUNDING_COLOUR, dd.most);
			else fprintf(stderr, "the 3D on the GPU (%s): its test picture is the CPU's, pixel for pixel\n", g3->name());
		}
#endif
		if (cfg.renderer3d != 2) {
			a3.gpu = cfg.renderer3d == 1; apply_3d();
			fprintf(stderr, "3D renderer: the player's setting (%s): the %s\n", why, a3.gpu ? "GPU" : "CPU");
			return;
		}
		const int recalled = auto_recalled();
		const uint64_t t0 = SDL_GetTicksNS();
		double c = 0, g = 0;
		if (!m.bench3d(5, c, g)) {
			a3.broken = true;
			fprintf(stderr, "3D renderer: Auto: the GPU failed its measure: the CPU for this run\n");
		} else {
			if (const char *k = getenv("GPU3D_SLOW_MS")) g += atof(k);   // tests: a slow GPU (each picture this much longer)
			a3.gpu = auto_gpu(true, c, g);
			auto_remember(a3.gpu);
			fprintf(stderr, "3D renderer: Auto (%s) at %dx, antialiasing %d: the CPU %.2f ms, the GPU %.2f ms a picture: the %s (%.0f ms measuring; last time: %s)\n",
					why, cfg.scale, cfg.aa, c, g, a3.gpu ? "GPU" : "CPU", (SDL_GetTicksNS() - t0) / 1e6,
					recalled < 0 ? "none" : recalled ? "the GPU" : "the CPU");
		}
		a3.c.clear(); a3.g.clear(); a3.probe = 0;
		a3.next = SDL_GetTicksNS() + 10000000000ull;
		apply_3d();
	};
	// after each picture: its time; Auto's checks
	auto auto_tick = [&] {
		if (!g3) return;
		const model1::gpu3d_stats &st = m.m_g3;
		if (st.failed != a3.failed) {
			a3.failed = st.failed;
			if (!a3.broken) fprintf(stderr, "3D renderer: the GPU failed: the CPU for this run\n");
			a3.broken = true;
			a3.gpu = false;
			apply_3d();
			return;
		}
		if (st.ms <= 0.01) return;   // (no 3D)
		{   // every 10 s, to the log: the 3D's time a picture
			static uint64_t since = SDL_GetTicksNS();
			static double sum[2] = {}, gsum = 0, rsum = 0; static int n[2] = {};
			sum[st.gpu] += st.ms; n[st.gpu]++;
			if (st.gpu) { gsum += st.gpu_ms; rsum += st.rec_ms; }
			if (SDL_GetTicksNS() - since >= 10000000000ull) {
				fprintf(stderr, "the 3D: %s", renderer_name().c_str());
				for (int k = 1; k >= 0; k--) if (n[k]) fprintf(stderr, ", the %s %.2f ms a picture (%d)", k ? "GPU" : "CPU", sum[k] / n[k], n[k]);
				if (n[1]) fprintf(stderr, " [GPU: recording %.2f ms, on the GPU %.2f ms]", rsum / n[1], gsum / n[1]);
				if (m.m_g3.checked) fprintf(stderr, ", checked %llu, %llu differ", (unsigned long long)m.m_g3.checked, (unsigned long long)m.m_g3.differ);
				fprintf(stderr, "\n");
				since = SDL_GetTicksNS(); sum[0] = sum[1] = gsum = rsum = 0; n[0] = n[1] = 0;
			}
		}
		const double ms = st.ms + (st.gpu ? std::max(0.0, getenv("GPU3D_SLOW_MS") ? atof(getenv("GPU3D_SLOW_MS")) : 0.0) : 0.0);
		auto &v = st.gpu ? a3.g : a3.c;
		if (a3.probe != 4) {   // (a check's first picture the other way is left out: a GPU idle a while is slow to wake)
			v.push_back(ms);
			if (v.size() > 9) v.erase(v.begin());
		}
		if (cfg.renderer3d != 2 || a3.broken) return;
		if (a3.probe > 0) {
			if (--a3.probe == 0) {
				auto med = [](std::vector<double> x) { std::sort(x.begin(), x.end()); return x[x.size() / 2]; };
				const bool was = a3.gpu;
				if (!a3.c.empty() && !a3.g.empty()) {
					const double c = med(a3.c), g = med(a3.g);
					a3.gpu = auto_gpu(was, c, g);   // (the GPU unless clearly slower and the frame at risk)
					if (a3.gpu != was) {
						auto_remember(a3.gpu);
						fprintf(stderr, "3D renderer: Auto's check: the CPU %.2f ms, the GPU %.2f ms a picture: now the %s\n", c, g, a3.gpu ? "GPU" : "CPU");
					} else if (getenv("VF_LOG_AUTO3D"))
						fprintf(stderr, "3D renderer: Auto's check: the CPU %.2f ms, the GPU %.2f ms a picture: the %s still\n", c, g, a3.gpu ? "GPU" : "CPU");
				}
				a3.next = SDL_GetTicksNS() + 10000000000ull;
			}
			m.set_gpu3d_on(a3.probe > 0 ? !a3.gpu : a3.gpu);
			return;
		}
		if (SDL_GetTicksNS() >= a3.next) { a3.probe = 4; m.set_gpu3d_on(!a3.gpu); }
	};
	auto_measure("the start");
	std::set<int> gpu_failed;           // render scales the GPU couldn't put together (a part too large for it)
	int gpu_verified = 0;               // the game's own pictures checked so far (the 1st, 60th and 600th)
	model1::gpu_parts parts;
	bool show_target = false;           // the picture on show is the GPU's (else tex)
	const int gpu_check = getenv("VF_GPU_CHECK") ? atoi(getenv("VF_GPU_CHECK")) : 0;   // testing: every nth checked
	int gpu_pictures = 0, gpu_checked = 0, gpu_differ = 0;
	// the picture on screen, for a file: put together by the GPU, it's drawn whole again first
	auto saved_frame = [&] { if (m.frame_unshown()) m.redraw(); return shown_frame(); };
	bool curtain = false;                // this pass: the frame to show is a curtain shown during a held scene change
	auto upload = [&] {
		show_target = false;
		// frame() put together in parts holds only the 3D, or a held scene's unshown picture: the
		// picture on show, drawn whole, is what goes up (but a curtain shown during a hold is
		// frame() as it is: redrawn, it was the scene before the hold, at the cost of a whole picture)
		if (m.frame_unshown() && !curtain) m.redraw();
		void *px; int pitch;
		if (aa_now() > 1 && SDL_LockTexture(tex, nullptr, &px, &pitch)) {
			aa_into(static_cast<uint32_t *>(px), size_t(pitch) / 4);
			SDL_UnlockTexture(tex);
		} else SDL_UpdateTexture(tex, nullptr, shown_frame(), shown_w() * 4);
	};
	bool still_dirty = true;   // paused or in the menu: the picture changed since it was last uploaded
	auto make_texture = [&] {
		still_dirty = true;
		gc.reset();
		show_target = false;
		if (tex) SDL_DestroyTexture(tex);
		// ARGB8888 is the GPU's own order (BGRA in memory): no conversion on upload. The top byte
		// isn't alpha to us, so no blending.
		tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, shown_w(), shown_h());
		// Too big for this graphics card (some take 4096 or 8192 pixels at most): a lower render scale.
		while (!tex && cfg.scale > 1) {
			fprintf(stderr, "a %dx%d picture is too large here (%s): trying %dx\n", shown_w(), shown_h(), SDL_GetError(), cfg.scale - 1);
			cfg.scale--;
			m.set_render_scale(cfg.scale * aa_fit(cfg.scale, cfg.aa));
			tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, shown_w(), shown_h());
		}
		SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
		std::vector<uint32_t> black(size_t(shown_w()) * shown_h(), 0);
		SDL_UpdateTexture(tex, nullptr, black.data(), shown_w() * 4);
		SDL_SetTextureScaleMode(tex, m.render_scale() > 1 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
	};
	make_texture();
	// the picture's shape: the chosen one, or the window's (between 4:3 and 32:9)
	auto apply_aspect = [&] {
		float a = ASPECTS[aspect_i];
		if (a <= 0) {
			int w = 0, h = 0;
			SDL_GetWindowSizeInPixels(win, &w, &h);
			a = h > 0 ? std::clamp(float(w) / float(h), 4.0f / 3.0f, 32.0f / 9.0f) : 16.0f / 9.0f;
		}
		if (std::fabs(a - m.aspect()) > 0.001f)
			m.set_aspect(a);
	};
	// 496x384 fills a 4:3 tube on the cabinet; widescreen widens the same picture
	auto present_shape = [&] {
		char title[96];
		snprintf(title, sizeof title, "Virtua Fighter  %dx  %s", cfg.scale, ASPECT_NAMES[aspect_i]);
		SDL_SetRenderLogicalPresentation(ren, int(std::lround(384 * m.aspect())), 384, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		SDL_SetWindowTitle(win, title);
	};
	// the saved shape from the start (else the picture fills the window, stretched, until F6)
	apply_aspect(); make_texture(); present_shape();
	// Screen filters: a picture of the monitor's glass multiplied over the game's, at the window's own
	// pixels (made again when the picture's size changes). Scanlines darkens between the arcade's 384
	// lines; CRT adds an aperture grille (red, green and blue stripes) and slightly darker corners.
	const char *FILTER_NAMES[] = { "Off", "Scanlines", "CRT" };
	SDL_Texture *glass = nullptr;
	int glass_w = 0, glass_h = 0, glass_kind = 0;
	auto draw_filter = [&] {
		SDL_FRect r;
		if (!cfg.filter || !SDL_GetRenderLogicalPresentationRect(ren, &r)) return;
		const int w = int(std::lround(r.w)), h = int(std::lround(r.h));
		if (w < 1 || h < 1) return;
		if (!glass || w != glass_w || h != glass_h || cfg.filter != glass_kind) {
			if (glass) SDL_DestroyTexture(glass);
			glass = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
			if (!glass) return;
			glass_w = w; glass_h = h; glass_kind = cfg.filter;
			const bool crt = cfg.filter == 2;
			// the lines want a couple of pixels each to show: they fade in from 1.5 to 3 per line
			const float per_line = h / 384.0f;
			const float depth = (crt ? 0.45f : 0.6f) * std::clamp((per_line - 1.5f) / 1.5f, 0.0f, 1.0f);
			// The lines are a whole number of screen pixels apart: the picture's lines to the nearest,
			// so every line looks the same. Sampled at a fraction of a line apart (the Deck's 800-pixel
			// screen: 2.08 pixels a line), the pattern beat into uneven bands every dozen lines. At a
			// whole number of pixels a line this is the picture's own lines, as before; two pixels a
			// line: a bright row and a dark one.
			const int period = std::max(1, int(std::lround(per_line)));
			std::vector<float> row(h);
			for (int y = 0; y < h; y++) {
				const float f = period == 2 ? float(y % 2) * 0.5f : (float(y % period) + 0.5f) / float(period);
				row[y] = 1 - depth * (1 - std::sin(f * 3.14159265f));   // 1 mid-line, darker at its edges
			}
			auto glass_at = [&](int x, int y, float rgb[3]) {
				rgb[0] = rgb[1] = rgb[2] = row[y];
				if (!crt) return;
				for (int c = 0; c < 3; c++) if (c != x % 3) rgb[c] *= 0.78f;   // the grille: one colour per pixel column
				const float dx = (x + 0.5f) / w * 2 - 1, dy = (y + 0.5f) / h * 2 - 1;
				const float d = std::min(1.0f, (dx * dx + dy * dy) / 2);
				for (int c = 0; c < 3; c++) rgb[c] *= 1 - 0.22f * d * d;      // the corners
			};
			// Multiplying only darkens: drawn as twice the product instead (dst * glass * 2), the glass
			// also brightens the middle of each line, so the picture keeps about its brightness.
			const SDL_BlendMode times2 = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_DST_COLOR, SDL_BLENDFACTOR_SRC_COLOR,
				SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
			const bool doubled = SDL_SetTextureBlendMode(glass, times2);
			if (!doubled) SDL_SetTextureBlendMode(glass, SDL_BLENDMODE_MOD);   // a renderer without it: darker
			float gain = 1;
			if (doubled) {
				double sum = 0; int n = 0;
				for (int y = 0; y < h; y += 3)
					for (int x = 0; x < w; x += 7) { float v[3]; glass_at(x, y, v); sum += v[0] + v[1] + v[2]; n += 3; }
				gain = std::min(1.4f, float(n / sum));
			}
			const float scale = 255 * gain / (doubled ? 2 : 1);
			std::vector<uint32_t> px(size_t(w) * h);
			parallel_rows(h, [&](int y0, int y1) {   // on every core: a 4K window's is ~20 ms on one
				for (int y = y0; y < y1; y++)
					for (int x = 0; x < w; x++) {
						float v[3];
						glass_at(x, y, v);
						uint32_t c[3];
						for (int i = 0; i < 3; i++) c[i] = uint32_t(std::min(255.0f, v[i] * scale + 0.5f));
						px[size_t(y) * w + x] = 0xff000000u | c[0] << 16 | c[1] << 8 | c[2];
					}
			}, 4);
			SDL_UpdateTexture(glass, nullptr, px.data(), w * 4);
			SDL_SetTextureScaleMode(glass, SDL_SCALEMODE_NEAREST);
		}
		SDL_RenderTexture(ren, glass, nullptr, nullptr);   // over the picture (the logical area), under the menu
	};

	SDL_AudioSpec spec{ SDL_AUDIO_S16, 2, m1audio::OUT_RATE };
	SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (audio) {
		SDL_SetAudioStreamGain(audio, getenv("VF_HIDDEN") ? 0.0f : cfg.volume / 100.0f);   // tests: silent
		m.set_volumes(cfg.music / 100.0f, cfg.effects / 100.0f);
		SDL_ResumeAudioStreamDevice(audio);
	} else fprintf(stderr, "no audio: %s\n", SDL_GetError());
	const int frame_bytes = int(m1audio::OUT_RATE / FRAME_HZ) * 4;
	constexpr int QUEUED = 3;            // frames of sound kept queued: the slack for a slow or late screen

	SDL_Gamepad *pads[2] = { nullptr, nullptr };
	bool pads_swapped = false;           // two pads that look alike (same GUID): swapped by hand
	auto guid_of = [](SDL_JoystickID id) {
		char g[64];
		SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), g, sizeof g);
		return std::string(g);
	};
	// each player's pad: the one chosen (by its GUID) if it is there, else the next free one
	// ("auto"), or none
	auto open_pads = [&] {
		for (auto &pd : pads) if (pd) { SDL_CloseGamepad(pd); pd = nullptr; }
		int n = 0;
		SDL_JoystickID *ids = SDL_GetGamepads(&n);
		std::vector<bool> used(n, false);
		// first the pads chosen by name, then "auto" players take the free ones in order, then a
		// player whose chosen pad isn't there (or shows up differently: a pad in another mode
		// has another GUID) takes the next free one too
		for (int pass = 0; pass < 3; pass++)
			for (int p = 0; p < 2; p++) {
				const std::string &c = cfg.ctl[p].pad_choice;
				if (pads[p] || c == "none") continue;
				const bool chosen = c != "auto";
				if ((pass == 0 && !chosen) || (pass == 1 && chosen)) continue;
				for (int i = 0; i < n && !pads[p]; i++)
					if (!used[i] && (pass != 0 || guid_of(ids[i]) == c)) { pads[p] = SDL_OpenGamepad(ids[i]); used[i] = true; }
			}
		SDL_free(ids);
		if (pads_swapped && pads[0] && pads[1]) std::swap(pads[0], pads[1]);
	};
	open_pads();

	bool running = true, paused = false, show_frame = true;
	pad_state pstate[2];
	// F9: timings in the title bar (and on stderr) every 120 frames; a frame has 17.4 ms
	struct perf_stats { bool show = false; int frames = 0, starved = 0, held = 0; uint64_t emu = 0, emu_max = 0, present = 0, work = 0, shown = 0, since = SDL_GetTicksNS(); };
	perf_stats perf{ cfg.timings || getenv("VF_PERF") != nullptr };
	// The load governor (a CPU that can't keep up): when, for about 6 s, the pictures shown fall short
	// of the screen's refresh while busy, the sound runs dry, or running the game, drawing and uploading
	// (not the wait for the screen) take over 97% of the time: first draw no more than 60 smooth
	// pictures a second (not one per refresh of a 120 or 144 Hz screen), then drop antialiasing, then
	// the render scale a step. Nothing is judged in the first 10 s on show (the start is far busier
	// than the rest: on a Steam Deck VF2's went from 3x to 1x then and stayed), and after 10 s of calm
	// (all the pictures, busy under 55%) what it lowered goes back up a step towards the player's
	// (not again in this run if that step is lowered once more). The player's own change starts it
	// afresh. Not in tests (VF_GOVERN: in tests too; VF_TEST_LOAD=ms:s, each pass ms longer a frame
	// at 3x, by the scale's square, for the first s seconds: a slow start).
	int player_scale = -1, player_aa = -1;   // the player's own, while the governor has lowered them
	uint64_t smooth_gap_ns = 8333333;    // smooth pictures: at most one per this, on average (120/s)
	int over_windows = 0;
	uint64_t gov_from = 0;                // judged from (10 s after the first picture)
	int gov_calm = 0;                     // calm windows in a row
	bool gov_raised = false, gov_no_raise = false;
	double test_load_ms = 0, test_load_s = 0;
	if (const char *tl = getenv("VF_TEST_LOAD")) sscanf(tl, "%lf:%lf", &test_load_ms, &test_load_s);
	// The render state in use, to the log: at the start and at each change (the player's or the governor's)
	auto log_state = [&](const char *why) {
		int vsync = 0, pw = 0, ph = 0;
		SDL_GetRenderVSync(ren, &vsync);
		SDL_GetWindowSizeInPixels(win, &pw, &ph);
		float hz = 0;
		if (const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win))) hz = dm->refresh_rate;
		const char *rn = SDL_GetRendererName(ren);
		fprintf(stderr, "render state (%s): %dx (drawn at %dx), antialiasing %s, %s, the 3D: %s, the 2D on the %s (%s), smooth pictures %s, governor %s, vsync %d, screen %.2f Hz, window %d x %d\n",
				why, cfg.scale, m.render_scale(), cfg.aa == 0 ? "Low" : aa_now() > 1 ? (aa_now() == 2 ? "2x" : "3x") : "off",
				aspect_i ? ASPECT_NAMES[aspect_i] : "fit", renderer_name().c_str(), gpu_2d && aa_now() == 1 && !gpu_failed.count(m.render_scale()) ? "GPU" : "CPU",
				rn ? rn : "?", smooth_gap_ns < 16666666 ? "each refresh" : "60 a second",
				((!getenv("VF_HIDDEN") && !getenv("VF_QUIT_AFTER")) || getenv("VF_GOVERN")) ? "on" : "off", vsync, hz, pw, ph);
	};

	// ---- the menu (Esc, or View on a pad) ----
	game_menu menu;
	if (!menu.init(win, ren, find_assets()))
		fprintf(stderr, "menu: couldn't load assets/ui (the game runs without it)\n");
	menu.set_footer(std::string("Virtua Fighter Recomp ") + VF_VERSION + (*VF_DATE ? std::string(" \u00b7 ") + VF_DATE : ""));
	// a new scale or shape reallocates the picture: draw it again at once when the game is stopped
	auto redraw_if_stopped = [&] { if (paused || menu.is_open()) { m.set_stopped(true); m.redraw(); m.set_stopped(false); still_dirty = true; } };
	// Reset (the menu, F3): the menu closes, a paused game unpauses, and the screen goes black at
	// once while the boot runs unseen (a few seconds on a slow machine), not the last picture held.
	// the arcade settings as their page opened (and the ring): see arcade_page
	std::optional<std::array<int, 9>> assign_before;
	auto reset_game = [&](bool keep_coins = true) {
		assign_before.reset();   // a restart applies them
		if (keep_coins) keep_arcade_coins();     // (Arcade: as the service menu may have left them)
		menu.close();
		paused = false;
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);
		SDL_RenderPresent(ren);
		boot();
		if (settle_coins()) boot();   // (the service menu may have set coins)
		make_texture();
		show_frame = false;
	};
	// the settings as the player chose them (not what the governor lowered them to for now)
	auto save_settings = [&] {
		settings out = cfg;
		if (player_scale >= 0) { out.scale = player_scale; out.aa = player_aa; }
		out.save(settings_path);
	};
	auto set_scale = [&](int v, bool by_player = true) {
		cfg.scale = v; cfg_dirty = true;
		// the player's own choice starts the governor afresh (its 60-a-second limit included)
		if (by_player) { smooth_gap_ns = 8333333; over_windows = 0; player_scale = player_aa = -1; gov_calm = 0; gov_raised = gov_no_raise = false; }
		m.set_render_scale(v * aa_fit(v, cfg.aa));
		m.set_edge_aa(cfg.aa == 0);
		m.set_smooth(true);
		sync.reset();                          // a new picture size: show the next frame as it is
		auto_measure("a new scale or antialiasing");
		make_texture(); present_shape(); redraw_if_stopped();
		log_state(by_player ? "the player's scale or antialiasing" : "the governor's");
	};
	auto set_shape = [&](int v) {
		aspect_i = cfg.aspect = v; cfg_dirty = true;
		sync.reset();
		apply_aspect(); make_texture(); present_shape(); redraw_if_stopped();
	};
	auto set_fullscreen = [&](bool on) {
		cfg.fullscreen = on; cfg_dirty = true;
		SDL_SetWindowFullscreen(win, on);
		SDL_SyncWindow(win);     // macOS animates into fullscreen: wait for it to finish
	};
	auto on_off = [](bool b) { return std::string(b ? "On" : "Off"); };
	auto wrap = [](int v, int n) { return (v % n + n) % n; };
	auto back_item = [&] { return menu_item{ "Back", "Back to the previous page.", {}, {}, [&] { menu.pop(); } }; };
	auto display_page = [&] {
		menu_page p{ "Display", "", {} };
		p.items.push_back({ "Render scale",
			"How sharp the 3D is. 1x is the original arcade resolution; higher is sharper but needs a faster computer. If yours can't keep up, the game lowers it for you. (F5)",
			[&] { return std::to_string(cfg.scale) + "x"; },
			[&](int d) { set_scale(std::clamp(cfg.scale + d, 1, 6)); } });
		p.items.push_back({ "Antialiasing",
			"Smooths jagged edges on the 3D. Low smooths the edges for little cost; 2x and 3x draw the 3D larger and look cleanest, but take a lot more power (lower the render scale if the game slows down).",
			[&] { return cfg.aa == 0 ? std::string("Low") : aa_now() == 1 ? std::string(cfg.aa > 1 ? "Off (too large here)" : "Off") : std::to_string(aa_now()) + "x"; },
			[&](int d) {
				static const int order[] = { 1, 0, 2, 3 };   // Off, Low, 2x, 3x
				int i = 0;
				while (i < 3 && order[i] != cfg.aa) i++;
				cfg.aa = order[std::clamp(i + d, 0, 3)];
				set_scale(cfg.scale);
			} });
		p.items.push_back({ "3D renderer",
			"Which draws the 3D: the CPU, or the graphics card (GPU), with exactly the same picture. Auto uses the GPU unless it is too slow on this computer, checking again as you play.",
			[&] { return renderer_name(); },
			[&](int d) {
				if (!g3 && g3_next_start) { cfg.renderer3d = wrap(cfg.renderer3d + d, 3); cfg_dirty = true; return; }   // (SDL_GPU: from the next start)
				if (!g3) return;
				cfg.renderer3d = wrap(cfg.renderer3d + d, 3); cfg_dirty = true;
				auto_measure("chosen");
				redraw_if_stopped();
				log_state("the player's 3D renderer");
			} });
		p.items.push_back({ "Picture shape",
			"Fit the window fills any screen shape. The other shapes add black bars to keep their shape; 4:3 is the original arcade screen. (F6)",
			[&] { return std::string(aspect_i ? ASPECT_NAMES[aspect_i] : "Fit the window"); },
			[&](int d) { set_shape(wrap(aspect_i + d, N_ASPECTS)); } });
		p.items.push_back({ "Background filter",
			"Smooth softens the stage backgrounds so they don't look blocky when enlarged. Off keeps their original pixels. Only matters above 1x.",
			[&] { return std::string(cfg.smooth_bg ? "Smooth" : "Off"); },
			[&](int) { cfg.smooth_bg = !cfg.smooth_bg; cfg_dirty = true; m.set_sky_filter(cfg.smooth_bg); redraw_if_stopped(); } });
		p.items.push_back({ "Screen filter",
			"Scanlines adds the dark lines between the arcade monitor's rows. CRT adds those, the monitor's red, green and blue stripes, and slightly darker corners. The lines need a large window or fullscreen to show.",
			[&] { return std::string(FILTER_NAMES[cfg.filter]); },
			[&](int d) { cfg.filter = wrap(cfg.filter + d, 3); cfg_dirty = true; } });
		p.items.push_back({ "Show FPS", "Shows the frame rate in the top right corner.",
			[&] { return on_off(cfg.show_fps); },
			[&](int) { cfg.show_fps = !cfg.show_fps; cfg_dirty = true; if (!cfg.show_fps) menu.set_fps(""); } });
		p.items.push_back({ "Fullscreen", "Play fullscreen or in a window. (F11)",
			[&] { return on_off(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN); },
			[&](int) { set_fullscreen(!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)); } });
		p.items.push_back(back_item());
		return p;
	};
	auto sound_page = [&] {
		menu_page p{ "Sound", "", {} };
		p.items.push_back({ "Volume", "The overall volume.",
			[&] { return std::to_string(cfg.volume) + "%"; },
			[&](int d) {
				cfg.volume = std::clamp(cfg.volume + 10 * d, 0, 100); cfg_dirty = true;
				if (audio && !getenv("VF_HIDDEN")) SDL_SetAudioStreamGain(audio, cfg.volume / 100.0f);
			} });
		p.items.push_back({ "Music", "The music's volume, within the overall volume.",
			[&] { return std::to_string(cfg.music) + "%"; },
			[&](int d) {
				cfg.music = std::clamp(cfg.music + 10 * d, 0, 100); cfg_dirty = true;
				m.set_volumes(cfg.music / 100.0f, cfg.effects / 100.0f);
			} });
		p.items.push_back({ "Effects and voices", "Hits, voices and the announcer, within the overall volume.",
			[&] { return std::to_string(cfg.effects) + "%"; },
			[&](int d) {
				cfg.effects = std::clamp(cfg.effects + 10 * d, 0, 100); cfg_dirty = true;
				m.set_volumes(cfg.music / 100.0f, cfg.effects / 100.0f);
			} });
		p.items.push_back(back_item());
		return p;
	};
	auto game_page = [&] {
		menu_page p{ "Game", "", {} };
		p.items.push_back({ "Play style",
			"PC skips the boot screen and is always free play. Arcade has the boot screen, coins and credits, like the real cabinet: Start needs a credit, and Coin adds one (set it in Controls: a key, or a pad button such as View; its coin settings are kept for it). Changing it restarts the game.",
			[&] { return std::string(cfg.arcade ? "Arcade" : "PC"); },
			[&](int) {
				keep_arcade_coins();
				cfg.arcade = !cfg.arcade; g_pc_mode = !cfg.arcade; cfg_dirty = true;
				apply_play_style();
				reset_game(false);   // (kept above, in the style it was)
			} });
		p.items.push_back({ "Reset game",
			"Restarts the game, like switching the arcade off and on. Rankings and settings are kept. (F3)",
			{}, {}, [&] { reset_game(); } });
		p.items.push_back(back_item());
		return p;
	};
	// ---- the arcade's own settings (test mode's GAME ASSIGNMENTS): the game reads them as it starts,
	// so closing the menu after changing one restarts it (only if something is different) ----
	using ga = model1::game_assignment;
	constexpr int N_ASSIGN = 5;
	static_assert(std::tuple_size_v<decltype(assign_before)::value_type> == N_ASSIGN + 4);
	auto assignments_now = [&] {
		std::array<int, N_ASSIGN + 4> v;
		for (int i = 0; i < N_ASSIGN; i++) v[i] = m.assignment(ga(i));
		v[N_ASSIGN] = cfg.ring;
		v[N_ASSIGN + 1] = cfg.akira_slot;   // (Cheats: read as a fight loads, so it restarts too)
		v[N_ASSIGN + 2] = cfg.ring_cheat;
		v[N_ASSIGN + 3] = cfg.no_time_limit;   // (an instruction of the game's: a restart recompiles it)
		return v;
	};
	auto arcade_page = [&] {
		if (!assign_before) assign_before = assignments_now();
		menu_page p{ "Arcade settings: a change restarts the game when the menu closes", "", {} };   // always in view
		auto choice = [&](const char *label, const char *help, ga a, std::function<std::string(int)> name) {
			p.items.push_back({ label, help,
				[&m, a, name] { int v = m.assignment(a); return v < 0 ? std::string("-") : name(v); },
				[&m, a](int d) { int v = m.assignment(a); if (v >= 0) m.set_assignment(a, v + d); } });
		};
		static const char *DIFFICULTY[] = { "Easy", "Normal", "Hard", "Hardest" };
		choice("Difficulty", "How hard the computer fights. Harder also gives both fighters less energy, and a smaller ring while Ring size is Default.",
			ga::difficulty, [](int v) { return std::string(DIFFICULTY[v]); });
		static const int RINGS[] = { 0, 1200, 1300, 1400, 1500 };   // the arcade's widths (STAGE WIDTH)
		static const char *RING_NAMES[] = { "Default", "Small", "Medium", "Large", "Extra large" };
		p.items.push_back({ "Ring size",
			"Default follows the difficulty, as on the arcade: Extra large on Easy, down to Small on Hardest. A smaller ring makes ring outs easier.",
			[&] { return std::string(RING_NAMES[std::find(std::begin(RINGS), std::end(RINGS), cfg.ring) - std::begin(RINGS)]); },
			[&](int d) {
				int i = int(std::find(std::begin(RINGS), std::end(RINGS), cfg.ring) - std::begin(RINGS));
				cfg.ring = RINGS[std::clamp(i + d, 0, int(std::size(RINGS)) - 1)];
				cfg_dirty = true;
				m.set_ring_width(cfg.ring_now());
			} });
		choice("Rounds to win (vs CPU)", "Rounds needed to win a match against the computer.", ga::match_1p, [](int v) { return std::to_string(v); });
		choice("Rounds to win (vs player)", "Rounds needed to win a match between two players.", ga::match_vs, [](int v) { return std::to_string(v); });
		choice("Continue", "Off ends the game when you lose to the computer.", ga::continues, [&](int v) { return on_off(v); });
		choice("Demo sound", "Sound during the demo that plays between games.", ga::advertise_sound, [&](int v) { return on_off(v); });
		p.items.push_back({ "Service menu",
			"The arcade's own test menu: input and sound tests, coin settings and bookkeeping. Down moves, Start selects (or 9 and F2 on a keyboard). Choose EXIT to come back.",
			{}, {}, [&] { menu.close(); paused = false; g_test_pending = true; } });
		p.items.push_back(back_item());
		return p;
	};
	// ---- cheats: the unused characters (in Akira's place) ----
	auto cheats_page = [&] {
		if (!assign_before) assign_before = assignments_now();
		menu_page p{ "Cheats: a change restarts the game when the menu closes", "", {} };
		constexpr int N_CHARS = int(std::size(model1::UNUSED_CHARACTERS));
		p.items.push_back({ "Secret character",
			"A character cut from the game, in Akira's spot on the select screen (Akira is away while one is in). "
			"Unfinished: borrowed moves, no throws. The Koskol brothers were to be the final bosses.",
			[&] { return std::string(cfg.akira_slot ? model1::UNUSED_CHARACTERS[cfg.akira_slot] : "Off"); },
			[&](int d) {
				cfg.akira_slot = wrap(cfg.akira_slot + d, N_CHARS);
				cfg_dirty = true;
				m.set_akira_slot(cfg.akira_slot);
				load_secret_icon(m, cfg.akira_slot, icon_dirs());
			} });
		static const char *WHO[] = { "Off", "Player 1", "Player 2", "Both" };
		p.items.push_back({ "Infinite energy", "Energy stays full: hits land but never knock out.",
			[&] { return std::string(WHO[cfg.infinite_energy]); },
			[&](int d) { cfg.infinite_energy = wrap(cfg.infinite_energy + d, 4); cfg_dirty = true; } });
		p.items.push_back({ "One-hit KO", "Every fighter (but one with infinite energy) is down to their last bit of energy: any hit wins the round.",
			[&] { return on_off(cfg.one_hit_ko); },
			[&](int) { cfg.one_hit_ko = !cfg.one_hit_ko; cfg_dirty = true; } });
		p.items.push_back({ "No time limit", "The round's clock stays at its start and never runs out: a round ends by knockout or ring out.",
			[&] { return on_off(cfg.no_time_limit); },
			[&](int) { cfg.no_time_limit = !cfg.no_time_limit; cfg_dirty = true; } });
		static const char *RING[] = { "Normal", "Tiny", "Huge" };
		p.items.push_back({ "Ring", "Tiny is half the smallest ring: ring outs everywhere. Huge is far wider than the stage: no ring outs.",
			[&] { return std::string(RING[cfg.ring_cheat]); },
			[&](int d) { cfg.ring_cheat = wrap(cfg.ring_cheat + d, 3); cfg_dirty = true; m.set_ring_width(cfg.ring_now()); } });
		p.items.push_back(back_item());
		return p;
	};
	// ---- controls: per player, keys and pad buttons, and which pad ----
	auto key_name = [](SDL_Scancode k) { return std::string(k ? SDL_GetScancodeName(k) : "-"); };
	auto pad_name = [&](int p) {
		const std::string &c = cfg.ctl[p].pad_choice;
		const char *nm = pads[p] ? SDL_GetGamepadName(pads[p]) : nullptr;
		std::string now = pads[p] ? (nm && *nm ? nm : "Gamepad") : "";
		if (c == "none") return std::string("None");
		if (c == "auto") return now.empty() ? std::string("Any pad (none connected)") : "Any pad: " + now;
		return now.empty() ? std::string("Chosen pad (not connected)") : now;
	};
	// the pad choices: any free pad, each connected pad, none
	auto cycle_pad = [&](int p, int d) {
		std::vector<std::string> opts = { "auto" };
		int n = 0;
		SDL_JoystickID *ids = SDL_GetGamepads(&n);
		for (int i = 0; i < n; i++) opts.push_back(guid_of(ids[i]));
		SDL_free(ids);
		opts.push_back("none");
		auto it = std::find(opts.begin(), opts.end(), cfg.ctl[p].pad_choice);
		int i = it == opts.end() ? 0 : int(it - opts.begin());
		cfg.ctl[p].pad_choice = opts[wrap(i + d, int(opts.size()))];
		// a pad chosen by one player is no longer the other's
		if (cfg.ctl[p].pad_choice != "auto" && cfg.ctl[p].pad_choice != "none" && cfg.ctl[1 - p].pad_choice == cfg.ctl[p].pad_choice)
			cfg.ctl[1 - p].pad_choice = "auto";
		cfg_dirty = true;
		open_pads();
	};
	auto keys_page = [&](int p) {
		menu_page pg{ p ? "Player 2 keyboard" : "Player 1 keyboard", "", {} };
		for (int a = 0; a < C_COUNT; a++)
			pg.items.push_back({ CONTROL_NAMES[a], std::string(a == C_COIN ? "A coin for this player in Arcade mode (a cabinet's coin slot). "
													: a == C_MENU ? "Opens this menu (as Esc always does): for a cabinet's button. " : "") +
					"Select, then press the key to use. Delete clears it.",
				[&, p, a] { return key_name(cfg.ctl[p].key[a]); }, {},
				[&, p, a] {
					menu.capture(std::string("Press a key for ") + CONTROL_NAMES[a] + (p ? " (player 2)" : " (player 1)") +
								 ". Backspace or Delete clears it, Esc keeps it.",
						[&, p, a](const SDL_Event &e) {
							if (e.type != SDL_EVENT_KEY_DOWN) return false;
							const SDL_Scancode k = e.key.scancode == SDL_SCANCODE_BACKSPACE || e.key.scancode == SDL_SCANCODE_DELETE ? SDL_SCANCODE_UNKNOWN : e.key.scancode;
							// the game's own keys (pause, reset, the menu's shortcuts, the cabinet's coin and
							// test buttons) stay theirs: wait for another key
							static const SDL_Scancode RESERVED[] = { SDL_SCANCODE_P, SDL_SCANCODE_TAB, SDL_SCANCODE_F3,
								SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F9, SDL_SCANCODE_F11, SDL_SCANCODE_F12 };
							for (SDL_Scancode r : RESERVED) if (k == r) return false;
							for (const auto &sk : SYS_KEYS) if (k == sk.key) return false;
							// a key does one thing: take it off whatever had it
							if (k) for (auto &c : cfg.ctl) for (auto &x : c.key) if (x == k) x = SDL_SCANCODE_UNKNOWN;
							cfg.ctl[p].key[a] = k;
							cfg_dirty = true;
							return true;
						});
				}, false, [&, p, a] { cfg.ctl[p].key[a] = SDL_SCANCODE_UNKNOWN; cfg_dirty = true; } });
		pg.items.push_back(back_item());
		return pg;
	};
	auto pad_buttons_page = [&](int p) {
		menu_page pg{ p ? "Player 2 pad buttons" : "Player 1 pad buttons", "", {} };
		for (int a = C_GUARD; a < C_COUNT; a++)
			pg.items.push_back({ CONTROL_NAMES[a], std::string(a == C_COIN ? "A coin for this player in Arcade mode (a cabinet's coin slot). "
												   : a == C_MENU ? "Opens this menu. L3 + R3 and the touchpad click (when it isn't set to something else) always do too, and Esc on a keyboard. " : "") +
												   "Select, then press the button (or trigger) to use. Y (or Delete) clears it. "
												   "The d-pad and left stick always move.",
				[&, p, a] { return player_controls::pad_label(cfg.ctl[p].pad[a]); }, {},
				[&, p, a] {
					menu.capture(std::string("Press a pad button for ") + CONTROL_NAMES[a] + (p ? " (player 2)" : " (player 1)") +
								 (a >= C_COIN ? ". Esc keeps it; Delete clears it." : ". View or Esc keeps it; Delete clears it."),
						[&, p, a](const SDL_Event &e) {
							int code = PAD_NONE;
							if (e.type == SDL_EVENT_KEY_DOWN) {
								if (e.key.scancode != SDL_SCANCODE_DELETE && e.key.scancode != SDL_SCANCODE_BACKSPACE) return false;
								cfg.ctl[p].pad[a] = PAD_NONE;
								cfg_dirty = true;
								return true;
							}
							if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
								// View: keep it (View is for the menu's hotkeys, not moves), unless it's for Coin or Menu
								if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK && a < C_COIN) return true;
								code = e.gbutton.button;
							} else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 16000 &&
									   (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
								code = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PAD_LT : PAD_RT;
							else return false;
							for (int b = C_GUARD; b < C_COUNT; b++) if (cfg.ctl[p].pad[b] == code) cfg.ctl[p].pad[b] = PAD_NONE;
							cfg.ctl[p].pad[a] = code;
							cfg_dirty = true;
							return true;
						});
				}, false, [&, p, a] { cfg.ctl[p].pad[a] = PAD_NONE; cfg_dirty = true; } });
		pg.items.push_back(back_item());
		return pg;
	};
	auto player_page = [&](int p) {
		menu_page pg{ p ? "Player 2" : "Player 1", "", {} };
		pg.items.push_back({ "Pad", "Which controller this player uses: any free one, a particular one, or none.",
							 [&, p] { return pad_name(p); }, [&, p](int d) { cycle_pad(p, d); } });
		pg.items.push_back({ "Keyboard", "Change this player's keys.", {}, {}, [&, p] { menu.push(keys_page(p)); } });
		pg.items.push_back({ "Pad buttons", "Change which buttons guard, punch, kick, do the combinations, start, add a coin and open the menu.",
							 {}, {}, [&, p] { menu.push(pad_buttons_page(p)); } });
		pg.items.push_back({ "Reset to defaults", "Put this player's keys and buttons back as they were.", {}, {},
							 [&, p] {
								 cfg.ctl[p] = player_controls::defaults(p);
								 // the default keys may be the other player's now: take them off there
								 for (auto k : cfg.ctl[p].key)
									 if (k) for (auto &x : cfg.ctl[1 - p].key) if (x == k) x = SDL_SCANCODE_UNKNOWN;
								 cfg_dirty = true;
								 open_pads();
							 } });
		pg.items.push_back(back_item());
		return pg;
	};
	auto controls_page = [&] {
		menu_page pg{ "Controls", "", {} };
		pg.items.push_back({ "Player 1", "Player 1's controller, keys and buttons.", [&] { return pad_name(0); }, {},
							 [&] { menu.push(player_page(0)); } });
		pg.items.push_back({ "Player 2", "Player 2's controller, keys and buttons.", [&] { return pad_name(1); }, {},
							 [&] { menu.push(player_page(1)); } });
		pg.items.push_back({ "Swap pads", "Player 1 and player 2 swap controllers.", {}, {},
							 [&] {
								 auto choice = [&](int p) { return pads[p] ? guid_of(SDL_GetGamepadID(pads[p])) : std::string("none"); };
								 std::string a = choice(0), b = choice(1);
								 if (a == b) {          // alike (same model, Steam Input): swap who holds which
									 pads_swapped = !pads_swapped;
									 std::swap(pads[0], pads[1]);
									 return;
								 }
								 cfg.ctl[0].pad_choice = b;
								 cfg.ctl[1].pad_choice = a;
								 cfg_dirty = true;
								 open_pads();
							 } });
		pg.items.push_back(back_item());
		return pg;
	};
	auto main_page = [&] {
		menu_page p{ "", "", {} };
		p.items.push_back({ "Resume", "Back to the game.", {}, {}, [&] { menu.close(); } });
		p.items.push_back({ "Display", "Sharpness, antialiasing, screen shape, background and screen filters, frame rate and fullscreen.",
							{}, {}, [&] { menu.push(display_page()); } });
		p.items.push_back({ "Sound", "The overall, music and effects volumes.", {}, {}, [&] { menu.push(sound_page()); } });
		p.items.push_back({ "Game", "Play style and reset.", {}, {}, [&] { menu.push(game_page()); } });
		p.items.push_back({ "Arcade settings", "Difficulty, ring size, rounds, continues and the service menu. Changing them restarts the game.", {}, {},
							[&] { menu.push(arcade_page()); } });
		p.items.push_back({ "Controls", "Each player's controller, keys and buttons.", {}, {}, [&] { menu.push(controls_page()); } });
		p.items.push_back({ "Cheats", "Play as the characters left unused in the game. Changing it restarts the game.", {}, {},
							[&] { menu.push(cheats_page()); } });
		p.items.push_back({ "Quit", "Saves and closes the game.", {}, {}, [&] { running = false; } });
		return p;
	};
	auto open_menu = [&] { if (menu.ready() && !menu.is_open()) menu.open(main_page()); };
	// The game's clock: game frames due since it started, paced by the wall clock at the arcade's
	// rate, trimmed by up to 1% to keep the sound queue at QUEUED frames. A frame runs when the
	// clock passes it, and smooth motion's picture is for the clock's moment: its fraction is how
	// far into the next frame it is, so the pictures are evenly spaced in the game's time. (The
	// sound queue alone can't say: the device takes it in chunks of a frame or more.)
	double frame_clock = 0, queue_avg = QUEUED * frame_bytes;
	int64_t game_run = 0;                    // frames run by the loop
	uint64_t clock_t = SDL_GetTicksNS();
	auto restart_clock = [&] { clock_t = SDL_GetTicksNS(); frame_clock = double(game_run); };
	int shot = 0;
	// testing: VF_QUIT_AFTER=n runs n frames, saves build/host_last.png (the game's frame) and
	// build/host_window.png (the window, menu and all) and quits; VF_MENU_KEYS="esc,down,enter"
	// presses those keys 6 frames apart from frame 30
	const int quit_after = getenv("VF_QUIT_AFTER") ? atoi(getenv("VF_QUIT_AFTER")) : -1;
	int64_t frames_run = 0;            // passes of the loop (test hooks)
	const bool hidden = getenv("VF_HIDDEN") != nullptr;
	std::vector<std::string> test_keys;
	if (const char *k = getenv("VF_MENU_KEYS"))
		for (std::string t, all = std::string(k) + ","; !all.empty(); all.erase(0, all.find(',') + 1))
			if (!(t = all.substr(0, all.find(','))).empty()) test_keys.push_back(t);
	// testing: VF_TRACE=file writes a line a pass (a refresh of the screen): when it started, the game
	// frames run and their time, what was shown (N a new smooth picture, W one drawn whole, C a cut's
	// hold keeping the last, H a scene change held, S none due, X stopped, F a frame shown without
	// smooth motion) and its moment, the times to draw, put together on the GPU, upload and present,
	// and the one-off work in it (tile maps decoded whole, layers drawn, stage skies built and scaled,
	// backdrops padded, GPU textures made, pixels uploaded)
	static FILE *trace = getenv("VF_TRACE") ? fopen(getenv("VF_TRACE"), "w") : nullptr;
	struct trace_pass { char kind = 'S'; int due = 0; double now = -1; uint64_t emu = 0, draw = 0, compose = 0, upload = 0, present = 0; } tp;
	const uint64_t trace_t0 = SDL_GetTicksNS();
	if (trace) fprintf(trace, "# pass t_ms due frame emu draw compose upload present kind now hold tiles_full tiles_ms layers layers_ms sky_build sky_build_ms sky_scale sky_scale_ms padded tex_made up_kpx stopped hp1 hp2 replay live r1 r2 quads mode alpha list0 list1 px py pz edge_ms\n");
	while (running) {
		if (!gov_from) { gov_from = SDL_GetTicksNS() + 10000000000ull; log_state("the start"); }
		bool in_texture = false;           // this pass's picture is on the GPU already (or the last one stays)
		curtain = false;
		bool cut_hold = false;             // this pass's picture stays on purpose (a camera cut's hold)
		bool curtain_on_gpu = false;       // a curtain shown during a hold, put together by the GPU
		const uint64_t pass_t = SDL_GetTicksNS();
		tp = trace_pass{};
		const int64_t pass = frames_run++;   // counted in every run: VF_MENU_KEYS and VF_SHOT_AT work without VF_QUIT_AFTER
		if (quit_after >= 0 && pass >= quit_after) {
			write_png("build/host_last.png", saved_frame(), shown_w(), shown_h());
			break;
		}
		if (frames_run >= 30 && (frames_run - 30) % 6 == 0 && size_t((frames_run - 30) / 6) < test_keys.size()) {
			static const std::map<std::string, SDL_Scancode> names = {
				{ "esc", SDL_SCANCODE_ESCAPE }, { "up", SDL_SCANCODE_UP }, { "down", SDL_SCANCODE_DOWN },
				{ "left", SDL_SCANCODE_LEFT }, { "right", SDL_SCANCODE_RIGHT }, { "enter", SDL_SCANCODE_RETURN } };
			const std::string &name = test_keys[(frames_run - 30) / 6];
			auto it = names.find(name);
			const SDL_Scancode sc = it != names.end() ? it->second : SDL_GetScancodeFromName(name.c_str());
			if (sc != SDL_SCANCODE_UNKNOWN) {
				SDL_Event k{};
				k.type = SDL_EVENT_KEY_DOWN;
				k.key.scancode = sc;
				k.key.down = true;
				SDL_PushEvent(&k);
			}
		}
		// testing: VF_EVENTS_AT="2000:p,2050:reset,2060:targets" at those passes presses a key, or
		// sends the renderer's device reset or render targets reset (as Direct3D does); "stall" and
		// "unstall" stop and restart the sound device
		static const char *events_at = getenv("VF_EVENTS_AT");
		for (const char *c = events_at; c && *c; c = strchr(c, ',') ? strchr(c, ',') + 1 : "") {
			char what[32] = "";
			long at = -1;
			if (sscanf(c, "%ld:%31[^,]", &at, what) != 2 || at != pass) continue;
			fprintf(stderr, "test: pass %ld: %s\n", at, what);
			if (!strcmp(what, "stall") || !strcmp(what, "unstall")) {   // the sound device stops taking data
				if (audio) { if (what[0] == 's') SDL_PauseAudioStreamDevice(audio); else SDL_ResumeAudioStreamDevice(audio); }
				continue;
			}
			SDL_Event t{};
			if (!strcmp(what, "reset")) t.type = SDL_EVENT_RENDER_DEVICE_RESET;
			else if (!strcmp(what, "targets")) t.type = SDL_EVENT_RENDER_TARGETS_RESET;
			else {
				t.type = SDL_EVENT_KEY_DOWN;
				t.key.scancode = !strcmp(what, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(what);
				t.key.down = true;
			}
			SDL_PushEvent(&t);
		}
		SDL_Event e;
		const bool menu_was_open = menu.is_open();
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_EVENT_QUIT) running = false;
			else if (e.type == SDL_EVENT_RENDER_TARGETS_RESET || e.type == SDL_EVENT_RENDER_DEVICE_RESET) {
				// Direct3D lost what render targets held (a device reset: every texture). The GPU's
				// parts are made again whatever is on show (their old textures can't be used), and
				// after a device reset the picture's texture and the screen filter's glass too; the
				// picture on show goes up again, drawn whole.
				gc.reset();
				if (e.type == SDL_EVENT_RENDER_DEVICE_RESET) {
					make_texture();
					if (glass) { SDL_DestroyTexture(glass); glass = nullptr; }
				}
				upload();
			}
			else if (menu.handle_event(e)) continue;
			else if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) open_pads();
			else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && ASPECTS[aspect_i] <= 0) {
				float before = m.aspect();
				apply_aspect();
				if (m.aspect() != before) { sync.reset(); make_texture(); present_shape(); redraw_if_stopped(); }
			}
			else if (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
				menu.refresh();
				if (SDL_GetKeyboardFocus() != win) SDL_RaiseWindow(win);   // (the move to a new Space can lose it)
			}
			else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
				if (e.key.scancode == SDL_SCANCODE_ESCAPE) open_menu();
				else if (e.key.scancode == SDL_SCANCODE_P) paused = !paused;
				else if (e.key.scancode == SDL_SCANCODE_F9) { perf.show = cfg.timings = !perf.show; cfg_dirty = true; if (!perf.show) present_shape(); }
				else if (e.key.scancode == SDL_SCANCODE_F3) reset_game();
				else if (e.key.scancode == SDL_SCANCODE_F5) set_scale(cfg.scale % 6 + 1);
				else if (e.key.scancode == SDL_SCANCODE_F6) set_shape((aspect_i + 1) % N_ASPECTS);
				else if (e.key.scancode == SDL_SCANCODE_F12) {
					// F12: the picture as drawn, into Screenshots in the data folder (numbered, never overwritten)
					std::error_code ec;
					const std::string shots = dir + "/Screenshots";
					std::filesystem::create_directories(shots, ec);
					char name[64];
					do snprintf(name, sizeof name, "/vf_%04d.png", ++shot);
					while (std::filesystem::exists(shots + name, ec) && shot < 9999);
					write_png((shots + name).c_str(), saved_frame(), shown_w(), shown_h());
				} else if (e.key.scancode == SDL_SCANCODE_F11)
					set_fullscreen(!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
			}
		}
		// the menu pauses the game and its sound
		if (menu.is_open() != menu_was_open && audio) {
			if (menu.is_open()) SDL_PauseAudioStreamDevice(audio);
			else SDL_ResumeAudioStreamDevice(audio);
		}
		if (menu.is_open() != menu_was_open && !menu.is_open()) {
			// settings changed in the menu are saved as it closes (not only at quit: a crash or a
			// force quit would lose them); test runs save nothing
			if (cfg_dirty && quit_after < 0) {
				cfg.scale = m.render_scale() / aa_now();
				cfg.aspect = aspect_i;
				save_settings();
				cfg_dirty = false;
			}
			// the arcade settings are read as the game starts: restart it if one is different
			if (assign_before) {
				const bool changed = *assign_before != assignments_now();
				assign_before.reset();
				if (changed) {
					// (the game's code is then new: recompiled before the reset, which leaves the old behind)
					const bool code = m.set_no_time_limit(cfg.no_time_limit);
#ifdef VF_NATIVE
					if (code && !interp) recompile();
#else
					(void)code;
#endif
					reset_game();
				}
			}
			pstate[0] = pstate[1] = pad_state{};   // the button that closed it isn't a move
			pstate[0].back_was = pstate[1].back_was = true;
			pstate[0].back_combo = pstate[1].back_combo = true;
			pstate[0].start_was = pstate[1].start_was = true;
			for (pad_state &s : pstate) s.menu_was = s.touch_was = s.sticks_was = true;
		}
		if (paused || menu.is_open()) restart_clock();   // (no catching up afterwards)
		if (!paused && !menu.is_open()) {
			// Run the game by its clock (frame_clock): the frames it has passed, however often the
			// screen comes round (a 30 or 15 fps limit runs 2 or 4 frames a pass), and more at once if
			// the sound is about to run dry. Tab held: flat out. The controls are read for each frame
			// the game runs, so their timing is the game's, not the screen's.
			const bool *ks = SDL_GetKeyboardState(nullptr);
			const bool ff = ks[SDL_SCANCODE_TAB];
			const uint64_t now_ns = SDL_GetTicksNS();
			const double dt = double(now_ns - clock_t) / 1e9;
			clock_t = now_ns;
			double rate = FRAME_HZ;
			int low = 0;                         // frames the sound needs at once (it's nearly run dry)
			bool sound_full = false;             // over 300 ms queued: no frames run this pass
			if (audio && !ff) {
				const int q = SDL_GetAudioStreamQueued(audio);
				// a sound device that stopped taking data without going away, or takes it slower than
				// the 1% trim can follow (Wine's on a Mac: 5%): the queue would grow without end, the
				// sound ever further behind the picture. Over 300 ms queued, the game waits for the
				// sound (as v1.1.6 always did), so the two stay together.
				if (q > frame_bytes * int(FRAME_HZ * 0.3)) {
					static int told = 0;
					if (told++ == 0) fprintf(stderr, "sound: %d ms queued (the device is slow or stalled): the game waits for it\n", int(q * 1000.0 / (frame_bytes * FRAME_HZ)));
					sound_full = true;
				}
				queue_avg += (q - queue_avg) * std::min(1.0, dt / 0.5);
				rate *= 1 - std::clamp((queue_avg / frame_bytes - QUEUED) * 0.005, -0.01, 0.01);
				if (q < frame_bytes * 3 / 2) low = int(std::ceil(QUEUED - double(q) / frame_bytes));
			}
			frame_clock += dt * rate;
			m.set_parts_mode(gpu_2d && aa_now() == 1 && !gpu_failed.count(m.render_scale()));
			int due = int(int64_t(std::floor(frame_clock)) - game_run);
			// A pass that came late (a scene change's work, the machine busy elsewhere) doesn't run
			// the time it lost as frames back to back: that made the game, and smooth motion's
			// pictures, race to catch up (a 50 ms pass ran 3 frames in one, the next picture three
			// frames on). No more frames a pass than the screen's pace runs (one at 120 Hz, two at 60
			// or 50, more under an FPS limit); the rest of the lost time is dropped and the clock
			// carries on from there, its fraction kept, so the pictures go on at their pace. The
			// sound's slack (QUEUED frames) covers it, and the 1% trim makes it up.
			static double dt_avg = 1.0 / 120;
			dt_avg += (std::min(dt, 0.1) - dt_avg) * 0.05;
			const int pace = std::max(1, int(std::ceil(dt_avg * rate + 0.25)));
			if (due > pace) {
				frame_clock = double(game_run + pace) + (frame_clock - std::floor(frame_clock));
				due = pace;
			}
			due = std::max(due, std::min(low, pace + 1));   // (the sound about to run dry: a frame more at most)
			if (due > 0) frame_clock = std::max(frame_clock, double(game_run + due));
			due = std::max(due, 0);
			if (ff) { due = 8; frame_clock = double(game_run + due); }
			else if (sound_full) due = 0;   // (the clock stops at the next frame, below)
			const uint64_t t0 = SDL_GetTicksNS();
			for (int i = 0; i < due; i++) {
				switch (read_inputs(m, pads, pstate, cfg.ctl)) {
				case hotkey::menu: open_menu(); break;
				case hotkey::scale: set_scale(cfg.scale % 4 + 1); break;
				case hotkey::aspect: set_shape((aspect_i + 1) % N_ASPECTS); break;
				case hotkey::none: break;
				}
				if (menu.is_open()) break;
				if (cfg.infinite_energy || cfg.one_hit_ko || cfg.no_time_limit) m.fight_cheats(cfg.infinite_energy, cfg.one_hit_ko, cfg.no_time_limit);
				m.run_frame();
				game_run++;
				// leaving the service menu: the PC build restarts at once, past the boot screen
				if (g_left_test_menu) { g_left_test_menu = false; if (g_pc_mode) { reset_game(); break; } }
				// With smooth motion a frame is only drawn when shown: while a scene change is held,
				// draw it anyway so the hold sees the real picture (else it waits on black forever),
				// though it isn't shown
				// (in parts when the GPU puts the pictures together: only the 3D drawn at the render scale)
				bool probe_parts = false;
				if (m.smooth() && sync.holding()) {
					if (gpu_2d && aa_now() == 1 && !gpu_failed.count(m.render_scale()))
						probe_parts = m.render_parts(double(m.frame_number()), parts, false) == 1;
					else m.render_smooth(double(m.frame_number()), false);
				}
				const bool was_holding = sync.holding();
				const bool show_it = sync.show(m);
				// a curtain shown while the hold goes on: frame() as just drawn, or that picture put
				// together by the GPU
				if (show_it) {
					curtain = sync.holding();
					curtain_on_gpu = probe_parts && sync.holding() &&
									 gc.draw(parts, m.frame_w(), m.frame_h(), m.render_scale() > 1 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
				}
				show_frame = show_it || show_frame;
				// a new scene: nothing to blend from, unless both lists kept are its own (it was built
				// after frames with no 3D, which forget the old scene's): then it moves from its first
				// picture on, instead of standing still until a third list came
				static uint64_t hold_from = 0;
				if (!was_holding && sync.holding()) hold_from = m.frame_number();
				if (was_holding && !sync.holding()) {
					float sa; int older, newer;
					m.smooth_state(sa, older, newer);
					if (older < 0 || uint64_t(older) + 1 < hold_from) m.smooth_cut();
				}
				if (audio && !ff) {
					if (SDL_GetAudioStreamQueued(audio) == 0) perf.starved++;   // the sound ran dry: a crackle
					SDL_PutAudioStreamData(audio, m.m_audio_out.data(), int(m.m_audio_out.size() * 2));
				}
				m.m_audio_out.clear();
			}
			frame_clock = std::min(frame_clock, double(game_run) + 0.999);   // (frames not run: the menu opened, a restart)
			if (curtain_on_gpu) { show_target = in_texture = true; tp.kind = 'F'; }
			tp.due = due;
			// testing: VF_TEST_LOAD, a slow start (the governor's tests)
			if (test_load_ms > 0 && due && gov_from && SDL_GetTicksNS() < gov_from - 10000000000ull + uint64_t(test_load_s * 1e9))
				for (const uint64_t until = SDL_GetTicksNS() + uint64_t(test_load_ms * 1e6 * due * cfg.scale * cfg.scale / 9); SDL_GetTicksNS() < until;) {}
			if (due) tp.emu = SDL_GetTicksNS() - t0;
			if (due) {
				perf.emu += SDL_GetTicksNS() - t0;
				perf.work += SDL_GetTicksNS() - t0;
				perf.emu_max = std::max(perf.emu_max, SDL_GetTicksNS() - t0);
			}
			// smooth motion: the picture for this moment of the screen, between the last two game
			// frames. The sound's playback is the steady clock: the newest frame minus what's still queued.
			// At most 120 of these a second (a 144 or 240 Hz screen needn't redraw every refresh),
			// and none while the window is minimised or hidden.
			// The limit is a budget, not a minimum gap: a gap of 8 ms skipped about a third of a 120 Hz
			// screen's refreshes (8.3 ms apart, give or take), so it showed 70-odd pictures, not 120.
			// a stage's sky scaled on its threads and done, not yet in use: a band of it to the GPU
			if (gpu_2d && aa_now() == 1 && !gpu_failed.count(m.render_scale())) {
				model1::gpu_image pending;
				if (m.sky_pending(pending)) {
					const uint64_t tq = SDL_GetTicksNS();
					gc.preload(pending);
					tp.upload += SDL_GetTicksNS() - tq;
				}
			}
			static double smooth_budget = 1;
			static uint64_t budget_t = SDL_GetTicksNS();
			{ const uint64_t bt = SDL_GetTicksNS();
			  smooth_budget = std::min(2.0, smooth_budget + double(bt - budget_t) / double(smooth_gap_ns));
			  budget_t = bt; }
			const bool unseen = SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED);
			if (m.smooth() && !sync.holding() && !unseen && (due || smooth_budget >= 1)) {
				smooth_budget = std::max(0.0, smooth_budget - 1);
				const double now = double(m.frame_number() - 1) + std::clamp(frame_clock - double(game_run), 0.0, 1.0);
				const uint64_t tr = SDL_GetTicksNS();
				const double t = std::min(now, double(m.frame_number()));
				tp.now = t;
				tp.kind = 'N';
				if (gpu_2d && aa_now() == 1 && !gpu_failed.count(m.render_scale())) {
					const int r = m.render_parts(t, parts);
					const uint64_t tc = SDL_GetTicksNS();
					tp.draw = tc - tr;
					if (r == 0) tp.kind = 'C';
					else if (r == 2) tp.kind = 'W';
					const bool composed = r == 1 && gc.draw(parts, m.frame_w(), m.frame_h(), m.render_scale() > 1 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
					if (r == 1) tp.compose = SDL_GetTicksNS() - tc;
					if (composed) {
						show_target = in_texture = true;
						++gpu_pictures;
						if (gpu_verified < 3 && gpu_pictures == (gpu_verified == 0 ? 1 : gpu_verified == 1 ? 60 : 600)) {
							gpu_verified++;
							std::vector<uint32_t> back;
							bool same = gc.read(back) && back.size() == size_t(m.frame_w()) * m.frame_h();
							m.redraw();
							for (size_t i = 0; same && i < back.size(); i++) same = back[i] == m.frame().data()[i];
							if (!same) {   // the CPU's picture, from now on
								fprintf(stderr, "pictures drawn on the CPU from now on: the GPU's picture %d differs from the CPU's\n", gpu_pictures);
								gpu_2d = false;
								gc.reset();
								show_target = in_texture = false;
							}
						}
						if (gpu_2d && gpu_check && gpu_pictures % gpu_check == 0) {   // the CPU's picture of the same moment
							std::vector<uint32_t> back;
							if (gc.read(back)) {
								m.redraw();
								int n = 0;
								for (size_t i = 0; i < back.size(); i++) n += back[i] != m.frame().data()[i];
								gpu_checked++;
								if (n) fprintf(stderr, "gpu check: picture %d: %d pixels differ (frame %llu, moment %.4f)\n", gpu_pictures, n, (unsigned long long)m.frame_number(), t);
								if (n && getenv("VF_GPU_CHECK_DUMP")) {   // testing: both pictures
									char path[512];
									snprintf(path, sizeof path, "%s/gpu_%d.png", getenv("VF_GPU_CHECK_DUMP"), gpu_pictures);
									write_png(path, back.data(), m.frame_w(), m.frame_h());
									snprintf(path, sizeof path, "%s/cpu_%d.png", getenv("VF_GPU_CHECK_DUMP"), gpu_pictures);
									write_png(path, m.frame().data(), m.frame_w(), m.frame_h());
								}
								gpu_differ += n != 0;
							}
						}
					} else if (r == 0) in_texture = cut_hold = true;   // (a cut's hold: the picture on show stays)
					else if (r == 1) {                        // the GPU failed: drawn whole, and at this scale from now on
						fprintf(stderr, "pictures drawn on the CPU at %dx: the GPU couldn't put them together (%s)\n", m.render_scale(), SDL_GetError());
						gpu_failed.insert(m.render_scale());
						gc.reset();
						m.redraw();
					}
				} else {
					if (!m.render_smooth(t)) { tp.kind = 'C'; cut_hold = true; }
					tp.draw = SDL_GetTicksNS() - tr;
				}
				perf.present += SDL_GetTicksNS() - tr;
				perf.work += SDL_GetTicksNS() - tr;
				show_frame = true;
				auto_tick();
			}
		}
		const uint64_t t1 = SDL_GetTicksNS();
		const bool show_frame_was_new = show_frame && !paused && !menu.is_open();
		if (show_frame && !in_texture) {
			const uint64_t tu = SDL_GetTicksNS();
			upload();
			perf.work += SDL_GetTicksNS() - tu;
			tp.upload = SDL_GetTicksNS() - tu;
			if (tp.kind == 'S') tp.kind = 'F';
		}
		// the FPS (Display > Show FPS): new pictures shown, counted over half a second
		{
			static int shown = 0;
			static uint64_t since = SDL_GetTicksNS();
			if (show_frame_was_new) perf.shown++;
			// the counter: pictures shown a second, where a picture held on purpose (a scene change
			// held until it's whole, a camera cut's hold) counts as shown: the screen kept its pace,
			// and counting it as missed showed a drop to 80-odd at every scene change
			if (show_frame_was_new || (!paused && !menu.is_open() && (sync.holding() || cut_hold))) shown++;
			static const char *cap = getenv("VF_CAPTURE");
			static int captured = 0;
			if (cap && show_frame_was_new && g_plan_i >= g_rec_from && g_plan_i < g_rec_to) {
				char path[512];
				snprintf(path, sizeof path, "%s/%05d.png", cap, captured++);
				write_png(path, saved_frame(), shown_w(), shown_h());
				fprintf(stderr, "CAP %d frame %zu\n", captured - 1, g_plan_i);
			}
			if (cap && g_rec_to != SIZE_MAX && g_plan_i >= g_rec_to) running = false;
			const uint64_t now = SDL_GetTicksNS();
			if (now - since >= 500000000) {
				menu.set_fps(cfg.show_fps ? std::to_string(int(std::lround(shown * 1e9 / double(now - since)))) + " FPS" : "");
				shown = 0; since = now;
			}
		}
		// otherwise only when there is a new picture; a still one (paused, the menu) is uploaded once,
		// and again only when it's redrawn, not every refresh
		{
			static bool was_stopped = false;
			const bool stopped = paused || menu.is_open();
			if (stopped != was_stopped) still_dirty = true;
			// stopped in a held scene change: the picture to keep is the one on screen, not the
			// scene being built that was drawn to look at
			if (stopped && !was_stopped && m.frame_unshown()) m.redraw();
			was_stopped = stopped;
			show_frame = stopped && still_dirty;
			if (show_frame) still_dirty = false;
		}
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);
		SDL_RenderTexture(ren, show_target ? gc.target() : tex, nullptr, nullptr);
		draw_filter();
		menu.render();
		static const int shot_at = getenv("VF_SHOT_AT") ? atoi(getenv("VF_SHOT_AT")) : -1;   // testing: the window at that pass too
		if ((quit_after >= 0 && frames_run == quit_after) || (shot_at >= 0 && frames_run == shot_at)) {
			int lw, lh;
			SDL_RendererLogicalPresentation mode;
			SDL_GetRenderLogicalPresentation(ren, &lw, &lh, &mode);
			SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
			if (SDL_Surface *shot = SDL_RenderReadPixels(ren, nullptr)) {
				if (SDL_Surface *x = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_XRGB8888)) {
					std::vector<uint32_t> px(size_t(x->w) * x->h);
					for (int y = 0; y < x->h; y++) memcpy(&px[size_t(y) * x->w], (uint8_t *)x->pixels + y * x->pitch, x->w * 4);
					write_png(frames_run == shot_at ? "build/host_window_at.png" : "build/host_window.png", px.data(), x->w, x->h);
					SDL_DestroySurface(x);
				}
				SDL_DestroySurface(shot);
			}
			SDL_SetRenderLogicalPresentation(ren, lw, lh, mode);
		}
		// a pass whose picture stays on purpose (a held scene change, a camera cut's hold): not one
		// the governor counts as missed
		if (!paused && !menu.is_open() && (sync.holding() || cut_hold)) perf.held++;
		SDL_RenderPresent(ren);
		if (trace) {
			const bool stopped = paused || menu.is_open();
			if (stopped) tp.kind = 'X';
			else if (sync.holding() && tp.kind == 'S') tp.kind = 'H';
			tp.present = SDL_GetTicksNS() - t1;
			model1::work_trace &w = m.m_work;
			auto nv16 = [&](uint32_t a) { return unsigned(m.nvram()[a] | m.nvram()[a + 1] << 8); };
			float sa, place[3]; int s0, s1;
			m.smooth_state(sa, s0, s1, place);
			fprintf(trace, "%lld %.3f %d %llu %.3f %.3f %.3f %.3f %.3f %c %.4f %d %d %.3f %d %.3f %d %.3f %d %.3f %d %d %llu %d %u %u %u %u %u %u %d %u %.4f %d %d %.3f %.3f %.3f %.3f\n",
					(long long)pass, (pass_t - trace_t0) / 1e6, tp.due, (unsigned long long)m.frame_number(), tp.emu / 1e6, tp.draw / 1e6,
					tp.compose / 1e6, tp.upload / 1e6, tp.present / 1e6, tp.kind, tp.now, int(sync.holding()), w.tiles_full, w.tiles_ns / 1e6,
					w.layers, w.layers_ns / 1e6, w.sky_build, w.sky_build_ns / 1e6, w.sky_scale, w.sky_scale_ns / 1e6, w.padded, gc.made,
					(unsigned long long)(gc.uploaded / 1000), int(stopped), nv16(0xb4), nv16(0xb6), nv16(0x182) >> 15, nv16(0x202) >> 15,
					unsigned(m.work_ram()[0x1281]), unsigned(m.work_ram()[0x1282]), int(m.frame_quads()), unsigned(m.game_mode()), sa, s0, s1, place[0], place[1], place[2], w.edge_ns / 1e6);
			w = model1::work_trace{};
			gc.made = 0; gc.uploaded = 0;
		}
		// Without vsync (forced off, or a minimised window) present returns at once: don't spin a core.
		{ static uint64_t pass = SDL_GetTicksNS(); const uint64_t t = SDL_GetTicksNS();
		  if (!hidden && t - pass < 2000000) SDL_DelayNS(2000000 - (t - pass));
		  pass = SDL_GetTicksNS(); }
		if (hidden) {                // tests: a hidden window may not wait for the screen; pace as 120 Hz (VF_HZ=n: n Hz)
			static uint64_t next = SDL_GetTicksNS();
			static const uint64_t period = uint64_t(1e9 / (getenv("VF_HZ") ? atof(getenv("VF_HZ")) : 120.0));
			next += period;
			const uint64_t t = SDL_GetTicksNS();
			if (next > t) SDL_DelayNS(next - t); else next = t;
		}
		if (focus_checks > 0 && !hidden) {
			const bool ours = SDL_GetKeyboardFocus() == win;
			if (--focus_checks % 30 == 0 && !ours) SDL_RaiseWindow(win);
			if (focus_checks == 0 || ours) {
				static bool told = false;
				if (!told) { told = true; fprintf(stderr, "keyboard: %s\n", ours ? "ours" : "not ours (click the window)"); }
				if (ours) focus_checks = 0;
			}
		}
		perf.present += SDL_GetTicksNS() - t1;
		// The game's backup (rankings, operator settings) saved within a few seconds of changing, so
		// a crash, a force quit or the Deck's power button doesn't lose it.
		if (quit_after < 0) {
			static uint64_t next_check = SDL_GetTicksNS() + 3000000000ull;
			if (SDL_GetTicksNS() >= next_check) {
				next_check = SDL_GetTicksNS() + 3000000000ull;
				const uint64_t h = backup_hash();
				if (h != saved_hash) {
					save_blob(nv_path, m.nvram().data(), m.nvram().size());
					save_blob(ee_path, m.eeprom(), model1::EEPROM_SIZE);
					saved_hash = h;
				}
			}
		}
		if (++perf.frames == 120) {
			if (perf.show) {
				char t[200];
				const double secs = double(SDL_GetTicksNS() - perf.since) / 1e9;
				snprintf(t, sizeof t, "Virtua Fighter  %dx  %.0f passes/s  %.0f shown/s  load %.0f%%  game %.1f ms/frame (max %.1f)  picture %.1f ms  sound dropouts %d",
						 cfg.scale, 120 / secs, perf.shown / secs, perf.work / (secs * 1e7), perf.emu / (secs * FRAME_HZ) / 1e6, perf.emu_max / 1e6,
						 perf.present / 120e6, perf.starved);
				SDL_SetWindowTitle(win, t);
				fprintf(stderr, "%s\n", t);
			}
			if (((!hidden && quit_after < 0) || getenv("VF_GOVERN")) && !paused && !menu.is_open()) {
				const double secs = double(SDL_GetTicksNS() - perf.since) / 1e9;
				const double busy = double(perf.work) / (secs * 1e9);   // CPU work, not the wait for the screen
				// or short of pictures: fewer than 90% of what it aims for (the screen's refresh, 120 at
				// most, or 60 once limited) while busy; a missed refresh is spent waiting, not working
				float hz = 60;
				if (const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win)))
					if (dm->refresh_rate > 0) hz = dm->refresh_rate;
				if (hidden && getenv("VF_HZ")) hz = float(atof(getenv("VF_HZ")));
				// what it aims for leaves out the passes that hold a picture on purpose: a scene change
				// (up to 45 frames) or a camera cut isn't the machine falling behind
				const double aim = std::min<double>(hz, smooth_gap_ns < 16666666 ? 120 : 60) * (1.0 - std::min(1.0, perf.held / 120.0));
				const double fps = perf.shown / secs;
				const bool short_of = m.smooth() && fps < aim * 0.9 && busy > 0.6;
				// only what the player would notice: too few pictures, the sound running dry, or no
				// time left at all (a busy CPU that still keeps up is left alone)
				const bool dropouts = perf.starved > 0 && busy > 0.6;
				// only steady play counts: a window with a scene change or a camera cut held in it (and
				// the work around them) neither counts toward stepping down nor clears the count
				const bool judged = SDL_GetTicksNS() >= gov_from;      // (not in the first 10 s on show)
				if (perf.held == 0 && judged) over_windows = (short_of || dropouts || busy > 0.97) ? over_windows + 1 : 0;
				// calm: every picture there, the sound fed, busy under 55%
				const bool calm = !short_of && !dropouts && busy < 0.55 && (!m.smooth() || fps >= aim * 0.9);
				if (perf.held == 0 && judged) gov_calm = calm ? gov_calm + 1 : 0;
				if (trace) fprintf(trace, "# pass %lld governor window: busy %.0f%% shown %.0f/s aim %.0f starved %d over %d\n", (long long)frames_run, busy * 100, fps, aim, perf.starved, over_windows);
				if (over_windows >= 3) {
					over_windows = 0;
					gov_calm = 0;
					if (gov_raised) gov_no_raise = true;     // (a step back up that didn't hold: no more this run)
					if (trace) fprintf(trace, "# pass %lld governor acts\n", (long long)frames_run);
					if (player_scale < 0) { player_scale = cfg.scale; player_aa = cfg.aa; }
					// (60 a second only on a screen faster than 60 Hz: at 60 or less a picture each
					// refresh is no more, and the budget would leave refreshes without one)
					if (smooth_gap_ns < 16666666 && hz > 65) {
						smooth_gap_ns = 16666666;
						fprintf(stderr, "load %.0f%%, %.0f FPS: smooth pictures limited to 60 a second\n", busy * 100, fps);
						log_state("the governor's");
					} else if (cfg.aa != 1) {
						cfg.aa = 1; set_scale(cfg.scale, false);
						fprintf(stderr, "load %.0f%%, %.0f FPS: antialiasing off\n", busy * 100, fps);
					} else if (cfg.scale > 1) {
						set_scale(cfg.scale - 1, false);
						fprintf(stderr, "load %.0f%%, %.0f FPS: render scale lowered to %dx\n", busy * 100, fps, cfg.scale);
					}
				}
				// back up a step towards the player's after 10 s of calm (10 windows)
				else if (gov_calm >= 10 && !gov_no_raise && player_scale >= 0 && (cfg.scale < player_scale || cfg.aa != player_aa || smooth_gap_ns > 8333333)) {
					gov_calm = 0;
					gov_raised = true;
					if (cfg.scale == player_scale && cfg.aa == player_aa) {   // (the last step: a picture each refresh again)
						smooth_gap_ns = 8333333;
						fprintf(stderr, "load %.0f%%, %.0f FPS: smooth pictures each refresh again\n", busy * 100, fps);
						log_state("the governor's");
					} else if (cfg.scale < player_scale) {
						fprintf(stderr, "load %.0f%%, %.0f FPS: render scale back up to %dx\n", busy * 100, fps, cfg.scale + 1);
						set_scale(cfg.scale + 1, false);
					} else {
						fprintf(stderr, "load %.0f%%, %.0f FPS: antialiasing back on\n", busy * 100, fps);
						cfg.aa = player_aa;
						set_scale(cfg.scale, false);
					}
				}
			}
			perf = perf_stats{ perf.show };
			perf.since = SDL_GetTicksNS();
		}

	}

	if (g3) fprintf(stderr, "the 3D at the end: %llu pictures drawn by the GPU, %llu checked against the CPU's, %llu differ; %llu drawn by the CPU instead, %llu GPU errors\n",
					(unsigned long long)m.m_g3.pictures, (unsigned long long)m.m_g3.checked, (unsigned long long)m.m_g3.differ,
					(unsigned long long)m.m_g3.cpu_fallback, (unsigned long long)m.m_g3.failed);
	m.set_gpu3d(nullptr);
	g3.reset();                          // (its device goes before SDL does)
	if (gpu_check) fprintf(stderr, "gpu check: %d pictures put together on the GPU checked, %d differ\n", gpu_checked, gpu_differ);
	gc.reset();                          // (its textures go before the renderer)
	if (quit_after >= 0 && !getenv("VF_SAVE")) { SDL_Quit(); return 0; }   // test runs leave the saves and settings alone (VF_SAVE: they save, as a player's)
	keep_arcade_coins();
	save_blob(nv_path, m.nvram().data(), m.nvram().size());
	save_blob(ee_path, m.eeprom(), model1::EEPROM_SIZE);
	if (cfg_dirty) {
		cfg.scale = m.render_scale() / aa_now();
		cfg.aspect = aspect_i;
		save_settings();
	}
	SDL_Quit();
	return 0;
}
