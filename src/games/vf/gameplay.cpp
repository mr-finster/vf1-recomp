#include "../../machine/model1.h"
#include "gameplay.h"
#include "profile.h"
#include <chrono>
#include <zlib.h>

using game::vf::random_cast;
using game::vf::RECORD_SIZE;
using game::vf::MOVE_COUNT;

namespace {
template <class F>
bool edit_vf_settings(model1 &machine, F edit)
{
	uint16_t *board_words = machine.eeprom_words();
	uint8_t *standin = board_words ? nullptr : machine.eeprom();
	uint16_t words[64];
	if (standin)
		for (int i = 0; i < 64; i++) words[i] = uint16_t(standin[2 * i] << 8 | standin[2 * i + 1]);
	uint16_t *e = board_words ? board_words : words;
	if (e[0] != 0x5345 || e[1] != 0x4741 || !edit(e)) return false;
	uint16_t crc = 0;
	for (int i = 8; i < 64; i++)
		for (uint8_t byte : { uint8_t(e[i] >> 8), uint8_t(e[i]) }) {
			crc ^= uint16_t(byte << 8);
			for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
		}
	e[4] = uint16_t(crc << 8 | crc >> 8);
	if (standin)
		for (int i = 0; i < 64; i++) { standin[2 * i] = uint8_t(words[i] >> 8); standin[2 * i + 1] = uint8_t(words[i]); }
	return true;
}

struct assignment_field { int word, shift, lo, hi; };
constexpr assignment_field ASSIGNMENTS[] = {
	{ 24, 8, 2, 5 },   // MATCH COUNT (1P): rounds to win
	{ 24, 0, 2, 5 },   // MATCH COUNT (VS)
	{ 16, 8, 0, 3 },   // DIFFICULTY: EASY, NORMAL, HARD, HARDEST
	{ 29, 8, 0, 1 },   // ADVERTISE SOUND
	{ 27, 0, 0, 1 },   // CONTINUE
};

struct screen_record { std::string_view name; uint32_t hash; };
// Hashes of the tile maps for VF's title/select screens; select has multiple player layouts.
constexpr screen_record SCREEN_RECORDS[] = {
	{ "title", 0x6e31a62b },
	{ "select", 0xfa5cc994 }, { "select", 0x282467f8 }, { "select", 0xa7c660d4 },
	{ "select", 0x46ffddc5 }, { "select", 0x14d0cd96 }, { "select", 0x0bc13456 }
};

// VF's changes to the running game, on the machine's public interface and VF's own state
class runtime
{
public:
	runtime(model1 &machine, game::vf::gameplay_state &state) : m(machine), vf(state) {}
	explicit runtime(model1 &machine)
		: runtime(machine, *std::static_pointer_cast<game::vf::gameplay_state>(machine.game_state())) {}
	void frame_begin();
	void frame_end();
	bool set_ring_width(int width);
	void set_akira_slot(int which);
	bool set_akira_icon(const uint32_t *xrgb);
	void fight_cheats(int infinite, bool one_hit, bool no_time);
	bool set_no_time_limit(bool on);
	void set_random_fighter(bool on, bool moves, bool cpu);
	int hide_free_play_text();
	void show_free_play_text();

private:
	void secret_icon_frame();
	random_cast &random_fighter_cast();
	uint32_t random_next(uint32_t n);
	void random_fighter_make(uint32_t rec);
	void random_fighter_strands(bool restore);
	void random_fighter_roll();
	void random_fighter_frame();
	void akira_slot_heads(uint32_t record);
	bool patch_program(uint32_t addr, size_t n, uint32_t crc, const uint8_t *to);

	model1 &m;
	game::vf::gameplay_state &vf;
};
}

int game::vf::gameplay::coin_setting()
{
	if (m_machine.title() != ID) return -1;
	int value = -1;
	edit_vf_settings(m_machine, [&](uint16_t *e) {
		value = (e[8] & 0xff00) | (e[10] >> 8);
		return false;
	});
	return value;
}

bool game::vf::gameplay::set_coin_setting(int value)
{
	if (m_machine.title() != ID) return false;
	return edit_vf_settings(m_machine, [&](uint16_t *e) {
		if (((e[8] & 0xff00) | (e[10] >> 8)) == value) return false;
		e[8] = uint16_t((e[8] & 0x00ff) | (value & 0xff00));
		e[10] = uint16_t((e[10] & 0x00ff) | ((value & 0xff) << 8));
		return true;
	});
}

int game::vf::gameplay::assignment_value(assignment which)
{
	const int index = int(which);
	if (m_machine.title() != ID || index < 0 || index >= int(std::size(ASSIGNMENTS))) return -1;
	const assignment_field &field = ASSIGNMENTS[index];
	int value = -1;
	edit_vf_settings(m_machine, [&](uint16_t *e) {
		value = std::clamp((e[field.word] >> field.shift) & 0xff, field.lo, field.hi);
		return false;
	});
	return value;
}

bool game::vf::gameplay::set_assignment(assignment which, int value)
{
	const int index = int(which);
	if (m_machine.title() != ID || index < 0 || index >= int(std::size(ASSIGNMENTS))) return false;
	const assignment_field &field = ASSIGNMENTS[index];
	value = std::clamp(value, field.lo, field.hi);
	return edit_vf_settings(m_machine, [&](uint16_t *e) {
		if (((e[field.word] >> field.shift) & 0xff) == value) return false;
		e[field.word] = uint16_t((e[field.word] & ~(0xff << field.shift)) | value << field.shift);
		return true;
	});
}

bool game::vf::gameplay::set_ring_width(int width)
{
	return m_machine.title() == ID && runtime(m_machine).set_ring_width(width);
}

void game::vf::gameplay::set_akira_slot(int which)
{
	if (m_machine.title() == ID) runtime(m_machine).set_akira_slot(which);
}

bool game::vf::gameplay::set_akira_icon(const uint32_t *xrgb)
{
	return m_machine.title() == ID && runtime(m_machine).set_akira_icon(xrgb);
}

void game::vf::gameplay::fight_cheats(int infinite, bool one_hit, bool no_time)
{
	if (m_machine.title() == ID) runtime(m_machine).fight_cheats(infinite, one_hit, no_time);
}

bool game::vf::gameplay::set_no_time_limit(bool on)
{
	return m_machine.title() == ID && runtime(m_machine).set_no_time_limit(on);
}

void game::vf::gameplay::set_random_fighter(bool on, bool moves, bool cpu)
{
	if (m_machine.title() == ID) runtime(m_machine).set_random_fighter(on, moves, cpu);
}

int game::vf::gameplay::hide_free_play_text()
{
	return m_machine.title() == ID ? runtime(m_machine).hide_free_play_text() : 0;
}

void game::vf::gameplay::show_free_play_text()
{
	if (m_machine.title() == ID) runtime(m_machine).show_free_play_text();
}

std::vector<std::string> game::vf::gameplay::screen_names() const
{
	if (m_machine.title() != ID) return {};
	std::vector<std::string> names;
	for (const auto &record : SCREEN_RECORDS)
		if (std::find(names.begin(), names.end(), record.name) == names.end()) names.emplace_back(record.name);
	return names;
}

bool game::vf::gameplay::load_screen(const std::string &name, std::vector<uint32_t> xrgb, int w, int h)
{
	return m_machine.title() == ID && m_machine.load_screen(name, std::move(xrgb), w, h);
}

void runtime::frame_begin()
{
	auto &state = vf;
	if (!state.random_fighter || state.pick_at < 0) return;
	const int64_t f = int64_t(m.frame_number()) - state.pick_at, ph = f % 40;
	if (ph >= 6 && ph <= 8 && m.nvram().size() > 0x7600)
		for (uint32_t pl = 0; pl < 2; pl++)
			for (uint32_t a : { 0x749cu, 0x74b0u, 0x74b3u }) m.nvram()[a + pl * 0x80] = 0;
	if (ph >= 10 && ph < 16) m.m_inputs[1] &= ~0x02;
	if (ph >= 30 && ph < 36) m.m_inputs[2] &= ~0x02;
}

void runtime::frame_end()
{
	auto &state = vf;
	if (state.icon.on) secret_icon_frame();
	if (state.random_fighter) random_fighter_frame();
}

game::gameplay_callbacks game::vf::create_gameplay(model1 &machine)
{
	game::gameplay_callbacks callbacks;
	auto state = std::make_shared<gameplay_state>();
	callbacks.state = state;
	callbacks.frame_begin = [&machine, state] { runtime(machine, *state).frame_begin(); };
	callbacks.frame_end = [&machine, state] { runtime(machine, *state).frame_end(); };
	callbacks.backdrop_tile = [state](uint32_t o) -> std::optional<uint16_t> {
		const auto &icon = state->icon;
		if (icon.active && o >= icon.at && o < icon.at + 8 * 64 && (o - icon.at) % 64 < 7)
			return icon.keep_map[((o - icon.at) / 64) * 7 + (o - icon.at) % 64];
		return std::nullopt;
	};
	callbacks.screen_hashes = [](std::string_view name) {
		std::vector<uint32_t> hashes;
		for (const auto &record : SCREEN_RECORDS)
			if (record.name == name) hashes.push_back(record.hash);
		return hashes;
	};
	callbacks.screen_name = [](uint32_t hash) -> std::optional<std::string_view> {
		for (const auto &record : SCREEN_RECORDS)
			if (record.hash == hash) return record.name;
		return std::nullopt;
	};
	// Widescreen is for the fight. The camera VF last sent to the display list (profile camera_ram,
	// 5017f0): the fight's (also used by the attract demo, the win and continue screens) is at
	// 5020f0; the title and the player select use others and stay 4:3. The pointer is cleared
	// every other frame (forcing a resend), so 0 keeps the last choice.
	callbacks.wide_scene = [](uint32_t camera, bool previous) {
		if (!camera) return previous;
		return camera == 0x5020f0;
	};
	return callbacks;
}

bool runtime::patch_program(uint32_t addr, size_t n, uint32_t crc, const uint8_t *to)
{
	auto &rom = m.program_rom();
	if (addr + n > rom.size() || ::crc32(0, &rom[addr], uInt(n)) != crc) return false;
	memcpy(&rom[addr], to, n);
	return true;
}

// The ring's width for EASY..HARDEST in 20s (1500, 1400, 1300, 1200), read as each match starts;
// the energy for each (16-bit) follows it.
bool runtime::set_ring_width(int width)
{
	constexpr uint32_t TABLE = 0xfcc9f6;
	static const uint8_t ARCADE[4] = { 75, 70, 65, 60 }, ENERGY[8] = { 0xdc, 0, 0xc8, 0, 0xb4, 0, 0xa0, 0 };
	auto &rom = m.program_rom();
	if (TABLE + 12 > rom.size() || memcmp(&rom[TABLE + 4], ENERGY, 8)) return false;
	for (int i = 0; i < 4; i++)
		if (rom[TABLE + i] < 25 || rom[TABLE + i] > 250) return false;   // not VF's table (or ours: Cheats' rings, 600 and 5000)
	for (int i = 0; i < 4; i++) rom[TABLE + i] = width ? uint8_t(width / 20) : ARCADE[i];
	return true;
}

// Akira's entries (from mountainmanjed's MAME cheats, tcrf.net/Virtua_Fighter_(Arcade)): his two
// slots in the table of characters (fd2e8f: 26 pointers into an array of 0x176-byte records), his
// select portrait, his name on the select screen for each side, and his bio. An unused character
// takes as many as it has; the rest stay Akira'vf.
// The strands that swing (hair, braids, Akira's headband ribbon) are listed by slot, not by record,
// for the select's portrait (fd2b33, read at fce947) and for the fight (fda917, read at ffc4b6, by
// the player's slot): a slot number and its strands' data, to ff. A character in Akira's slots
// would wear his ribbon, so his entries there get a slot no player has (0x19, the last boss's).
void runtime::set_akira_slot(int which)
{
	enum { MODEL1, MODEL2, PORTRAIT, NAME_P1, NAME_P2, BIO, RIBBON1, RIBBON2, RIBBON3, RIBBON4, N };
	static const struct { uint32_t addr; int size; } AT[N] = {
		{ 0xfd2ee7, 2 }, { 0xfd2ebb, 2 }, { 0xfd2a73, 4 }, { 0xfd2a93, 2 }, { 0xfd2ab3, 2 }, { 0xfd2ad3, 2 },
		{ 0xfd2b60, 1 }, { 0xfd2b5b, 1 }, { 0xfda944, 1 }, { 0xfda93f, 1 } };
	constexpr int NO = 0x19;   // (no ribbon)
	static const int64_t TO[std::size(game::vf::UNUSED_CHARACTERS)][N] = {     // -1: Akira's own
		{ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
		{ 0x33af, 0x4843, 0x001d1a8f, 0x0b37, 0x14f7, 0xd773, NO, NO, NO, NO },     // Majido (Siba)
		{ 0x3c7b, 0x4ca5, -1, 0x09ff, 0x11eb, 0xc95b, NO, NO, NO, NO },             // Jeff (TCRF's has Majido's icon: a made one instead)
		{ 0x3df9, 0x4e23, -1, -1, -1, 0xb437, -1, -1, -1, -1 },                     // Takeru, the early Akira (his ribbon)
		// the Koskol brothers, one each, in both of Akira's slots: TCRF's cheats put two in at once
		// (Rekka + Score, Aeon + Dural), one per slot
		{ 0x510f, 0x510f, -1, -1, -1, -1, NO, NO, NO, NO },                         // Rekka
		{ 0x5285, 0x5285, -1, -1, -1, -1, NO, NO, NO, NO },                         // Score
		{ 0x53fb, 0x53fb, -1, -1, -1, -1, NO, NO, NO, NO },                         // Aeon
		{ 0x369b, 0x369b, -1, -1, -1, -1, NO, NO, NO, NO },                         // Dural (TCRF's second with Aeon)
	};
	auto &rom = m.program_rom();
	if (AT[0].addr + 4 > rom.size()) return;
	static uint8_t akira[N][4];
	static bool kept = false, ribbon = false;
	if (!kept) {
		for (int i = 0; i < N; i++) memcpy(akira[i], &rom[AT[i].addr], AT[i].size);
		ribbon = akira[RIBBON1][0] == 0x16 && akira[RIBBON2][0] == 0x0b &&   // (VF's lists as expected)
				 akira[RIBBON3][0] == 0x16 && akira[RIBBON4][0] == 0x0b;
		kept = true;
	}
	which = std::clamp(which, 0, int(std::size(game::vf::UNUSED_CHARACTERS)) - 1);
	for (int i = 0; i < N; i++) {
		uint8_t *p = &rom[AT[i].addr];
		if (TO[which][i] < 0 || (i >= RIBBON1 && !ribbon)) memcpy(p, akira[i], AT[i].size);
		else for (int b = 0; b < AT[i].size; b++) p[b] = uint8_t(TO[which][i] >> (8 * b));   // (little-endian)
	}
	akira_slot_heads(TO[which][MODEL1] < 0 ? 0 : 0xfd0000 | uint32_t(TO[which][MODEL1]));
	// Dural is the game's own in Akira's place, not her record in Akira's slots: the fight looks a
	// fighter up by slot (the select's character, 0-8, through fd2f1b for player 1 and fd2f24 for
	// player 2: Akira's slots 0x16 and 0x0b, Dural's 5), and the game checks for her by slot, as
	// for the metal clank of a hit on her (ffb8d8: 9e0f7f), which a record in Akira's slots never
	// made. Akira's entry is her slot instead.
	static const uint32_t SLOT_TABLES[2] = { 0xfd2f1b, 0xfd2f24 };
	static const uint8_t AKIRA_SLOT[2] = { 0x16, 0x0b };
	for (int t = 0; t < 2; t++) {
		uint8_t *e = &rom[SLOT_TABLES[t]];
		if (SLOT_TABLES[t] + 9 > rom.size() || e[8] != 5 || (e[0] != AKIRA_SLOT[t] && e[0] != 5)) continue;   // (not VF's)
		e[0] = which == 7 ? 5 : AKIRA_SLOT[t];
	}
	// Siba's pounce (Up + Punch at a downed opponent): his moves table (fc78c4, his record's +4)
	// has move 259 there, a placeholder's record (fc2722, five other moves': a jump with no jump in
	// it), so the game planned a jump of 0 + 2 frames, the whole way in 2, and he flew on at that
	// speed for his pounce's 26: across the ring, often out of it. Lau's pounce (381) in its place,
	// as his table has it (Siba's moves are mostly Lau's).
	constexpr uint32_t SIBA_POUNCE = 0xfc78c4 + 130 * 4;
	if (SIBA_POUNCE + 4 <= rom.size() && (rom[SIBA_POUNCE + 1] == 0x01) && (rom[SIBA_POUNCE] == 0x03 || rom[SIBA_POUNCE] == 0x7d))
		rom[SIBA_POUNCE] = which == 1 ? 0x7d : 0x03;   // (381 = 0x17d; 259 = 0x103)
	// The select's camera for Akira's spot (fd278b: x, y, z, then 3 angles, a camera per place in
	// the row): each head sits differently in its own frame (its origin, how it's turned), so each
	// has its own: the eyes at the cast's height (a little below the middle), the face across the
	// middle, the head the cast's size, the neck running off the bottom. Dural's head (the fight's)
	// is turned and tilted down besides: hers turns it to face as the rest. (Found by rendering
	// each against the cast'vf.)
	struct cam { float x, y, z; int16_t a, b, c; };
	static const cam CAMS[std::size(game::vf::UNUSED_CHARACTERS)] = {
		{ 0, 0, 0, 0, 0, 0 },                                   // Akira: his own
		{ -0.028f, -0.075f, 1.0f, 17400, 32000, -23549 },        // Siba
		{ -0.019f, -0.103f, 1.0f, 17400, 32000, -23549 },        // Jeff
		{ -0.025f, -0.1f, 0.9f, 17400, 32000, -23549 },          // Takeru
		{ -0.003f, -0.085f, 0.95f, 17400, 32000, -23549 },       // Rekka
		{ -0.003f, -0.085f, 0.95f, 17400, 32000, -23549 },       // Score
		{ -0.018f, -0.0995f, 0.95f, 17400, 32000, -23549 },      // Aeon
		{ -0.025f, -0.08f, 0.88f, 15400, 32000, -22800 },        // Dural
	};
	constexpr uint32_t CAMERA = 0xfd278b;
	static uint8_t akira_cam[18];
	static bool cam_kept = false;
	if (!cam_kept) { memcpy(akira_cam, &rom[CAMERA], sizeof akira_cam); cam_kept = true; }
	memcpy(&rom[CAMERA], akira_cam, sizeof akira_cam);
	if (which) {
		const cam &c = CAMS[which];
		const float f[3] = { c.x, c.y, c.z };
		const int16_t h[3] = { c.a, c.b, c.c };
		memcpy(&rom[CAMERA], f, sizeof f);       // (little-endian, as VF's)
		memcpy(&rom[CAMERA + 12], h, sizeof h);
	}
}

// A select portrait for the characters without one (Dural and the Koskol brothers), made from
// their own head. A record's +72 is its portrait: 9 parts (lists of objects: the face, the eyes and
// mouth with their frames), at +24 the face's animations by number (ff06ac), at +3c the select's
// blinking. The fight draws a head from it too (ff06ff), or without one the record's +16, a single
// object, as those characters have. So the portrait here is that object alone as its first part
// (a list of it, whatever frame is asked for), no animations, Akira's blinking for the select's
// (it only moves parts it hasn't): in the fight the same head as before, on the select their face.
// It goes in the main CPU's empty f80000-fbffff (no ROM there, nothing reads it). Only the
// character in Akira's place has one; the rest stay as the game made them.
void runtime::akira_slot_heads(uint32_t record)
{
	static const uint32_t NO_PORTRAIT[] = { 0xfd369b, 0xfd510f, 0xfd5285, 0xfd53fb };   // Dural, Rekka, Score, Aeon
	constexpr uint32_t AT = 0xf80000, LIST = AT + 0x80, BLINK_OF = 0xfd3f6f;   // (Akira's record)
	auto &rom = m.program_rom();
	if (LIST + 0x80 > rom.size()) return;
	auto rd = [&](uint32_t a) { return uint32_t(rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | rom[a + 3] << 24); };
	auto wr = [&](uint32_t a, uint32_t v) { for (int b = 0; b < 4; b++) rom[a + b] = uint8_t(v >> (8 * b)); };
	static bool kept = false, ours[std::size(NO_PORTRAIT)];
	if (!kept) {
		for (size_t i = 0; i < std::size(NO_PORTRAIT); i++) ours[i] = rd(NO_PORTRAIT[i] + 0x72) == 0;   // (VF's: none)
		kept = true;
	}
	const uint32_t blink = rd(rd(BLINK_OF + 0x72) + 0x3c);
	bool made = false;
	for (size_t i = 0; i < std::size(NO_PORTRAIT); i++) {
		if (!ours[i]) continue;
		const bool here = NO_PORTRAIT[i] == record && blink >> 16 == 0xfd;
		wr(NO_PORTRAIT[i] + 0x72, here ? AT : 0);
		if (!here) continue;
		std::fill(&rom[AT], &rom[LIST + 0x80], 0);
		wr(AT, LIST);                                     // the first part: the head
		wr(AT + 0x3c, blink);
		const uint32_t head = rd(record + 0x16);
		for (uint32_t k = 0; k < 0x80; k += 4) wr(LIST + k, head);   // (every frame the same)
		made = true;
	}
	if (!made) std::fill(&rom[AT], &rom[LIST + 0x80], 0xff);   // (as it was: no ROM)
}

// Cheats' random fighter. A fighter's record (0x176 bytes) holds its body parts' models: torso
// +12, hips +32, the near arm +1a/+1e, the far one +26/+2a, the front leg +36/+3a/+3e, the back
// one +42/+46/+4a, the hands' poses +5e, +62 and +76 on (64 of them); and at +72 the list of its
// head's models (the blinks), which the select's portrait draws too. The cast share a skeleton,
// so any part sits on any body: the animation stays Akira's (or his place's), the parts are
// anyone'vf. Akira's records: fd4f99 (player 1's costume) and fd3f6f (player 2's).
void runtime::set_random_fighter(bool on, bool moves, bool cpu)
{
	vf.random_fighter = on;
	vf.random_moves = on && moves;
	vf.random_cpu = on && cpu;
	vf.select_seen = false;
	random_fighter_roll();
}

// The records random fighters are made from and into: every record the slots point at, Akira's
// two (fd4f99 player 1's costume, fd3f6f player 2's) and the Koskol brothers'. Their own data is
// kept: the parts always come from the cast as they are, and Off puts them all back.
random_cast &runtime::random_fighter_cast()
{
	static constexpr uint32_t SLOTS = 0xfd2e8f, KOSKOLS[3] = { 0xfd510f, 0xfd5285, 0xfd53fb };
	random_cast &c = vf.random_cast;
	if (c.made) return c;
	c.made = true;
	auto &rom = m.program_rom();
	if (SLOTS + 26 * 4 > rom.size()) return c;
	auto rd = [&](uint32_t a) { return uint32_t(rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | uint32_t(rom[a + 3]) << 24); };
	auto model = [&](uint32_t r, uint32_t o) { return (rd(r + o) >> 16) == 0x2d; };
	auto add = [&](uint32_t r) {
		if ((r >> 16) != 0xfd || r + RECORD_SIZE > rom.size() || c.orig.count(r)) return;
		if (!model(r, 0x12) || !model(r, 0x1a) || !model(r, 0x36)) return;
		c.orig[r].assign(&rom[r], &rom[r] + RECORD_SIZE);
		c.recs.push_back(r);
		if ((rd(r + 0x72) >> 16) == 0xfd) c.heads.push_back(r);
		const uint32_t t = rd(r + 4);
		if ((t >> 16) == 0xfc && t + MOVE_COUNT * 4 <= rom.size() && std::find(c.tables.begin(), c.tables.end(), t) == c.tables.end()) c.tables.push_back(t);
	};
	// Akira's two by name: the slots may hold a secret character in his place just now (Cheats'
	// Secret character, set before Random fighter was turned on), and this list is made once
	add(0xfd4f99);
	add(0xfd3f6f);
	for (int i = 0; i < 26; i++) add(rd(SLOTS + 4 * i));
	for (uint32_t r : KOSKOLS) add(r);
	return c;
}

uint32_t runtime::random_next(uint32_t n)
{
	uint32_t &x = vf.random_state;   // xorshift, seeded from the frame and the clock
	if (!x) x = uint32_t(m.frame_number() * 2654435761u) ^ uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x9e3779b9u;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	return n ? x % n : 0;
}

// One record made a random fighter: each group of parts (a pair of limbs as one) from a random
// fighter's own, a random head (+72, the select's portrait too), and with random moves a moves
// table of its own (in the ROM's empty space, f81000 on: 0x300 bytes a record), each command's
// entry from a random fighter's table. The moves are numbered alike for everyone (Siba's are
// mostly Lau's) and the cast share a skeleton, so any part and move fits anyone.
void runtime::random_fighter_make(uint32_t rec)
{
	random_cast &c = random_fighter_cast();
	auto &rom = m.program_rom();
	auto it = c.orig.find(rec);
	if (it == c.orig.end() || c.recs.empty() || c.heads.empty()) return;
	auto orig = [&](uint32_t r, uint32_t o) { const auto &b = c.orig.at(r); return uint32_t(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24); };
	auto wr = [&](uint32_t a, uint32_t v) { for (int b = 0; b < 4; b++) rom[a + b] = uint8_t(v >> (8 * b)); };
	auto model = [&](uint32_t r, uint32_t o) { return (orig(r, o) >> 16) == 0x2d; };
	memcpy(&rom[rec], it->second.data(), RECORD_SIZE);
	static const std::vector<std::vector<uint32_t>> GROUPS = [] {
		std::vector<std::vector<uint32_t>> g = { { 0x12 }, { 0x32 }, { 0x1a, 0x26 }, { 0x1e, 0x2a }, { 0x36, 0x42 }, { 0x3a, 0x46 }, { 0x3e, 0x4a }, { 0x5e, 0x62 } };
		for (uint32_t o = 0x76; o < 0x176; o += 4) g.back().push_back(o);   // (the hands' poses: one hand set)
		return g;
	}();
	for (const auto &g : GROUPS) {
		const uint32_t src = c.recs[random_next(uint32_t(c.recs.size()))];
		for (uint32_t o : g)
			if (model(src, o) && model(rec, o)) wr(rec + o, orig(src, o));
	}
	if ((orig(rec, 0x72) >> 16) == 0xfd) wr(rec + 0x72, orig(c.heads[random_next(uint32_t(c.heads.size()))], 0x72));
	if (vf.random_moves && !c.tables.empty()) {
		const uint32_t at = 0xf81000 + 0x300 * uint32_t(std::find(c.recs.begin(), c.recs.end(), rec) - c.recs.begin());
		if (at + MOVE_COUNT * 4 > 0xfc0000 || at + MOVE_COUNT * 4 > rom.size()) return;
		for (uint32_t k = 0; k < MOVE_COUNT; k++) {
			const uint32_t t = c.tables[random_next(uint32_t(c.tables.size()))];
			memcpy(&rom[at + 4 * k], &rom[t + 4 * k], 4);
		}
		wr(rec + 4, at);
	}
}

// All of the cast as they are, then Akira's two made random (Akira's place), and with random
// opponents every other record too (the computer's next one is made afresh as it comes in:
// random_fighter_frame).
void runtime::random_fighter_roll()
{
	static constexpr uint32_t AKIRA[2] = { 0xfd4f99, 0xfd3f6f };
	random_cast &c = random_fighter_cast();
	auto &rom = m.program_rom();
	for (const auto &[r, b] : c.orig) memcpy(&rom[r], b.data(), RECORD_SIZE);
	if (!vf.random_fighter) { random_fighter_strands(true); return; }
	for (uint32_t r : AKIRA) random_fighter_make(r);
	if (vf.random_cpu)
		for (uint32_t r : c.recs)
			if (r != AKIRA[0] && r != AKIRA[1]) random_fighter_make(r);
}

// The strands that swing (Sarah's ponytail, Pai's and Lau's braids, Akira's headband ribbon) are
// listed by the player's slot, not by record (fda917: a slot byte and a pointer to its strands,
// 5 bytes each, to ff; read for each fighter at ffc4b6 by its block's slot, -4c). With random
// heads they'd hang where the slot's own head was: the list is made afresh for the two players
// each frame, each slot with the strands of the head its record wears now (none for a head
// without), and put back as VF has it when Random fighter goes off.
void runtime::random_fighter_strands(bool restore)
{
	static constexpr uint32_t LIST = 0xfda917, SLOTS = 0xfd2e8f;
	auto &rom = m.program_rom();
	if (LIST + 80 > rom.size() || m.nvram().size() < 0x1f00) return;
	if (vf.strands_orig.empty()) {
		size_t n = 0;
		while (n < 75 && rom[LIST + n] != 0xff) n += 5;
		if (n >= 75) return;   // (not VF's list)
		vf.strands_orig.assign(&rom[LIST], &rom[LIST] + n + 1);
	}
	auto put = [&](const std::vector<uint8_t> &b) { if (memcmp(&rom[LIST], b.data(), b.size())) memcpy(&rom[LIST], b.data(), b.size()); };
	if (restore || !vf.random_fighter) { put(vf.strands_orig); return; }
	random_cast &c = random_fighter_cast();
	auto rd = [&](const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); };
	auto orig_strands = [&](uint8_t slot) -> uint32_t {   // a slot's own, or 0
		for (size_t i = 0; i + 5 <= vf.strands_orig.size(); i += 5)
			if (vf.strands_orig[i] == slot) return rd(&vf.strands_orig[i + 1]);
		return 0;
	};
	// a head list -> the strands of the slot whose own record wears it
	auto strands_of_head = [&](uint32_t head, uint8_t slot) -> uint32_t {
		for (uint32_t s = 0; s < 26; s++) {
			const uint32_t r = rd(&rom[SLOTS + 4 * s]);
			auto it = c.orig.find(r);
			if (it != c.orig.end() && rd(&it->second[0x72]) == head) return orig_strands(uint8_t(s));
		}
		for (const auto &[r, b] : c.orig)   // (Akira's two, not in the slots while a secret character is)
			if (rd(&b[0x72]) == head) return orig_strands(r == 0xfd4f99 ? 0x16 : r == 0xfd3f6f ? 0x0b : 0xff);
		return orig_strands(slot);
	};
	std::vector<uint8_t> list;
	for (int pl = 0; pl < 2; pl++) {
		const uint32_t block = 0x700 + 0x1800 * pl;
		const uint8_t slot = m.nvram()[block - 0x4c];
		const uint32_t rec = rd(&m.nvram()[block - 0x50]);
		if ((rec >> 16) != 0xfd || rec + RECORD_SIZE > rom.size()) continue;
		const uint32_t head = rd(&rom[rec + 0x72]);
		const uint32_t st = (head >> 16) == 0xfd ? strands_of_head(head, slot) : orig_strands(slot);
		bool dup = false;
		for (size_t i = 0; i < list.size(); i += 5) dup |= list[i] == slot;
		if (!st || dup) continue;
		list.push_back(slot);
		for (int b = 0; b < 4; b++) list.push_back(uint8_t(st >> (8 * b)));
	}
	list.push_back(0xff);
	if (list.size() <= vf.strands_orig.size()) put(list);
}

// A new player select: a new fighter (the select's own Akira icon, b080, comes into the tiles)
void runtime::random_fighter_frame()
{
	bool select = false;
	for (uint32_t i = 0; i < 0x4000 - 8 * 64 && !select; i++)
		select = m.tile_word(i) == 0xb080 && m.tile_word(i + 1) == 0xb081 && m.tile_word(i + 64) == 0xb087;
	if (select && !vf.select_seen) { random_fighter_roll(); vf.pick_at = int64_t(m.frame_number()); }
	random_fighter_strands(false);
	// Random opponents: each player's fighter is in its block (player 1's at 4006b0, player 2's
	// 0x1800 on), set as a match loads: a new one there (the computer's next opponent) is made
	// afresh, so even the same character twice comes in new.
	if (vf.random_cpu && m.nvram().size() > 0x1eb4)
		for (int pl = 0; pl < 2; pl++) {
			const uint32_t o = 0x6b0 + 0x1800 * pl;
			const uint32_t r = uint32_t(m.nvram()[o] | m.nvram()[o + 1] << 8 | m.nvram()[o + 2] << 16 | uint32_t(m.nvram()[o + 3]) << 24);
			if (r != vf.random_seen[pl]) {
				vf.random_seen[pl] = r;
				if (r != 0xfd4f99 && r != 0xfd3f6f && !select) random_fighter_make(r);
			}
		}
	if (!select) vf.pick_at = -1;
	vf.select_seen = select;
}

// The select's icons for the characters without one (all but Siba, whose is in the ROM): pictures
// made from their heads (tools/games/vf/select_icons), shown in Akira's icon's tiles while one is in his place.
// His icon (map at 2ff5ec: 7 x 8 tiles, 8 x 8 pixels at 4 bits, colour 0x61, its own) is drawn on the
// select as entries b080-b0b1, two of them used more than once (the left edge's b087, and b0a5):
// those cells get tiles of their own, b0b2-b0b7 (unused there). While the icon is on screen its
// tiles, colours and map are ours, VF's own kept and put back as it goes; when the game writes
// them again, what it wrote is kept instead.
const char *const game::vf::SECRET_ICON_NAMES[] = { nullptr, nullptr, "jeff", "takeru", "rekka", "score", "aeon", "dural" };

namespace {
constexpr int ICON_W = 7, ICON_H = 8, ICON_COLOUR = 0x61;
constexpr uint16_t ICON_FIRST = 0xb080;
// VF's map of the icon, and ours (the repeated cells moved to b0b2-b0b7)
uint16_t icon_entry(int x, int y, bool ours)
{
	static const uint8_t VF[ICON_H][ICON_W] = {
		{ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 }, { 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d },
		{ 0x07, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13 }, { 0x07, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19 },
		{ 0x07, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f }, { 0x07, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25 },
		{ 0x07, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x25 }, { 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31 } };
	if (ours && x == 0 && y >= 2 && y <= 6) return ICON_FIRST + 0x32 + (y - 2);
	if (ours && x == 6 && y == 6) return ICON_FIRST + 0x37;
	return ICON_FIRST + VF[y][x];
}
}

bool runtime::set_akira_icon(const uint32_t *xrgb)
{
	if (vf.icon.active) { vf.icon.on = false; secret_icon_frame(); }   // (VF's own back first)
	if (!xrgb) { vf.icon.on = false; return true; }
	constexpr int W = ICON_W * 8, H = ICON_H * 8;
	std::vector<uint32_t> cols;
	std::vector<uint8_t> idx(W * H);
	for (int i = 0; i < W * H; i++) {
		const uint32_t c = xrgb[i] & 0xffffff;
		auto it = std::find(cols.begin(), cols.end(), c);
		if (it == cols.end()) { if (cols.size() == 15) return false; cols.push_back(c); it = cols.end() - 1; }
		idx[i] = uint8_t(1 + (it - cols.begin()));   // (0: see-through, unused)
	}
	vf.icon.pal.assign(16, 0x8000);
	for (size_t i = 0; i < cols.size(); i++) {
		const uint32_t c = cols[i];
		vf.icon.pal[1 + i] = uint16_t(0x8000 | ((c & 0xff) >> 3) << 10 | ((c >> 8 & 0xff) >> 3) << 5 | (c >> 16 & 0xff) >> 3);   // (xBGR 555)
	}
	vf.icon.map.resize(ICON_W * ICON_H);
	vf.icon.chars.assign(0x38 * 16, 0);
	for (int ty = 0; ty < ICON_H; ty++)
		for (int tx = 0; tx < ICON_W; tx++) {
			const uint16_t e = icon_entry(tx, ty, true);
			vf.icon.map[ty * ICON_W + tx] = e;
			uint16_t *c = &vf.icon.chars[(e - ICON_FIRST) * 16];
			for (int y = 0; y < 8; y++) {
				uint32_t row = 0;
				for (int x = 0; x < 8; x++) row |= uint32_t(idx[(ty * 8 + y) * W + tx * 8 + x]) << (28 - 4 * x);
				c[y * 2] = uint16_t(row >> 16); c[y * 2 + 1] = uint16_t(row);
			}
		}
	vf.icon.on = true;
	return true;
}

void runtime::secret_icon_frame()
{
	// where the icon is: its first row, VF's or ours
	uint32_t at = ~0u;
	if (vf.icon.on)
		for (uint32_t i = 0; i < 0x4000 - ICON_H * 64 && at == ~0u; i++) {
			if (m.tile_word(i) != ICON_FIRST) continue;
			bool row = true;
			for (int x = 1; x < ICON_W && row; x++) row = m.tile_word(i + x) == ICON_FIRST + x;
			for (int y = 1; y < ICON_H && row; y++) row = (m.tile_word(i + y * 64 + 1) & 0xff80) == ICON_FIRST;
			if (row) at = i;
		}
	const uint32_t code0 = ICON_FIRST & 0x3fff, pal0 = ICON_COLOUR * 16;
	if (vf.icon.active && at != vf.icon.at) {   // gone (or moved): VF's own back
		for (size_t i = 0; i < vf.icon.chars.size(); i++)
			if (m.char_word(code0 * 16 + i) == vf.icon.chars[i]) m.char_write(code0 * 16 + i, vf.icon.keep_chars[i]);
		for (int i = 1; i < 16; i++)
			if (m.palette_word(pal0 + i) == vf.icon.pal[i]) m.palette_write(pal0 + i, vf.icon.keep_pal[i]);
		for (int y = 0; y < ICON_H; y++)
			for (int x = 0; x < ICON_W; x++) {
				const uint32_t o = vf.icon.at + y * 64 + x;
				if (m.tile_word(o) == vf.icon.map[y * ICON_W + x]) m.tile_write(o, vf.icon.keep_map[y * ICON_W + x]);
			}
		vf.icon.active = false;
	}
	if (at == ~0u) return;
	if (!vf.icon.active) {   // VF's as it is now
		vf.icon.keep_chars.resize(vf.icon.chars.size());
		for (size_t i = 0; i < vf.icon.chars.size(); i++) vf.icon.keep_chars[i] = m.char_word(code0 * 16 + i);
		vf.icon.keep_pal.resize(16);
		for (int i = 0; i < 16; i++) vf.icon.keep_pal[i] = m.palette_word(pal0 + i);
		vf.icon.keep_map.resize(ICON_W * ICON_H);
		for (int y = 0; y < ICON_H; y++)
			for (int x = 0; x < ICON_W; x++) vf.icon.keep_map[y * ICON_W + x] = m.tile_word(at + y * 64 + x);
		vf.icon.at = at;
		vf.icon.active = true;
	}
	for (size_t i = 0; i < vf.icon.chars.size(); i++) {
		const uint16_t now = m.char_word(code0 * 16 + i);
		if (now != vf.icon.chars[i]) { vf.icon.keep_chars[i] = now; m.char_write(code0 * 16 + i, vf.icon.chars[i]); }
	}
	for (int i = 1; i < 16; i++) {
		const uint16_t now = m.palette_word(pal0 + i);
		if (now != vf.icon.pal[i]) { vf.icon.keep_pal[i] = now; m.palette_write(pal0 + i, vf.icon.pal[i]); }
	}
	for (int y = 0; y < ICON_H; y++)
		for (int x = 0; x < ICON_W; x++) {
			const uint32_t o = at + y * 64 + x;
			const uint16_t now = m.tile_word(o), ours = vf.icon.map[y * ICON_W + x];
			if (now != ours) { vf.icon.keep_map[y * ICON_W + x] = now; m.tile_write(o, ours); }
		}
}

// No time limit: the round's clock is counted down at fe2190 (dec.h 501285[PC]), and the picture
// is drawn after, so a clock put back each frame still showed a tick off its start (29.96). That
// instruction becomes test.h of the same (the opcode d2 to f2, the same length): the clock stays
// at its start. It's the game's code, so the change needs a recompile (or the interpreter).
bool runtime::set_no_time_limit(bool on)
{
	constexpr uint32_t AT = 0xfe2190;
	static const uint8_t REST[5] = { 0xf2, 0xf5, 0xf0, 0x51, 0xff };   // (its operand)
	auto &rom = m.program_rom();
	if (AT + 6 > rom.size() || memcmp(&rom[AT + 1], REST, 5) || (rom[AT] != 0xd2 && rom[AT] != 0xf2)) return false;
	const uint8_t op = on ? 0xf2 : 0xd2;
	if (rom[AT] == op) return false;
	rom[AT] = op;
	return true;
}

// The fighters' energy (ffc4a0: player n's at [50130c] + n * 0x1800, the energy -2e from there,
// what hits take off; the full amount for the round at 501186).
void runtime::fight_cheats(int infinite, bool one_hit, bool no_time)
{
	// a 16- or 32-bit value at a main CPU address in either RAM: the fighters' data is in the
	// battery-backed one (seen at 400700), the round's numbers in work RAM
	auto at = [&](uint32_t a, int n) -> uint8_t * {
		if (a >= 0x500000 && a + n - 0x500000 <= m.work_ram().size()) return &m.work_ram()[a - 0x500000];
		if (a >= 0x400000 && a + n - 0x400000 <= m.nvram().size()) return &m.nvram()[a - 0x400000];
		return nullptr;
	};
	auto rd16 = [&](uint32_t a) { const uint8_t *p = at(a, 2); return p ? int16_t(p[0] | p[1] << 8) : int16_t(0); };
	auto rd32 = [&](uint32_t a) { const uint8_t *p = at(a, 4); return p ? uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24) : 0u; };
	auto wr16 = [&](uint32_t a, int16_t v) { if (uint8_t *p = at(a, 2)) { p[0] = uint8_t(v); p[1] = uint8_t(uint16_t(v) >> 8); } };
	// (the fields are below the pointer: player 2's at +0x1800, down to -0x80)
	const uint32_t base = rd32(0x50130c);
	const bool ok = at(base - 0x80, 0x80) && at(base + 0x1800 - 0x80, 0x80);
	static int told = 0;
	if (told < 2 && (ok ? told == 1 || told == 0 : told == 0)) {   // (log.txt: what the cheats found, once each way)
		fprintf(stderr, "cheats: players at %06x%s, energy %d %d of %d, clock %d\n", base, ok ? "" : " (not set yet)",
				ok ? rd16(base - 0x2e) : 0, ok ? rd16(base + 0x1800 - 0x2e) : 0, rd16(0x501186), rd16(0x501285));
		told = ok ? 2 : 1;
	}
	if (!ok) return;
	// The round's clock (501285): 0x384 as a round starts (fe2152), counted down while neither
	// fighter is down (fe2190, which set_no_time_limit stops: their -80's bit 0x1b), time up below
	// 0. Other screens count with it too, so this is only once a round has started it. At the end
	// of a round the game files its replay by the clock (fe2617: a round over while it's still
	// above 0x357, its first moments, goes another way), so as a fighter goes down it's let go at
	// 0x356, a round's time like any other. (Put back each frame too, should the code be VF's own.)
	if (no_time) {
		const int t = rd16(0x501285);
		const bool down = (rd32(base - 0x80) | rd32(base + 0x1800 - 0x80)) >> 0x1b & 1;
		if (t == 0x384 && !down) vf.clock_held = true;
		if (vf.clock_held && (down || t <= 0 || t > 0x384)) {
			vf.clock_held = false;
			if (down && t > 0x356) wr16(0x501285, 0x356);
		}
		if (vf.clock_held && t < 0x384) wr16(0x501285, 0x384);
	} else vf.clock_held = false;
	if (!infinite && !one_hit) return;
	const int16_t full = rd16(0x501186);
	if (full <= 0 || full > 1000) return;
	for (int p = 0; p < 2; p++) {
		const uint32_t a = base + p * 0x1800 - 0x2e;
		int16_t e = rd16(a);
		if (infinite >> p & 1) e = full;
		else if (one_hit && e > 1) e = 1;
		else continue;
		wr16(a, e);
	}
}

static std::vector<std::pair<uint32_t, std::vector<uint8_t>>> s_free_play_text;   // the bytes hide_free_play_text replaced

void runtime::show_free_play_text()
{
	for (const auto &[addr, bytes] : s_free_play_text) memcpy(&m.program_rom()[addr], bytes.data(), bytes.size());
	s_free_play_text.clear();
}

int runtime::hide_free_play_text()
{
	if (!s_free_play_text.empty()) return 0;
	auto keep = [&](uint32_t addr, size_t n) {
		auto &rom = m.program_rom();
		if (addr + n <= rom.size()) s_free_play_text.push_back({ addr, std::vector<uint8_t>(&rom[addr], &rom[addr] + n) });
	};
	keep(0x13c01c2, 40);
	keep(0xfd080f, 9);
	int found = 0;
	// The italic FREE PLAY (attract, title): a 14x2 tile image drawn by the routine at ff39a3
	// from a record (base tile, flags, rows, columns, then a byte per tile). Base tile 0x0020,
	// the blank the text layer is cleared with, and every byte 0: blank cells.
	uint8_t blank_image[12 + 28] = { 0x20, 0x00, 0, 0, 2, 0, 0, 0, 14, 0, 0, 0 };
	found += patch_program(0x13c01c2, sizeof blank_image, 0x71d41cda, blank_image);
	// The fight's "FREE PLAY" (top right, between PUSH START BUTTON): a padded string.
	const uint8_t spaces[9] = { ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
	found += patch_program(0xfd080f, sizeof spaces, 0x9dbd988f, spaces);
	return found;
}
