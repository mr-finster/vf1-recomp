// Desktop host: SDL3 window, keyboard and gamepads, frame pacing at the Model 1's 57.52 Hz,
// and the battery-backed RAM and EEPROM saved next to the regions.
#include "version.h"      // VF_VERSION, VF_DATE (made by the Makefile from git)
#include "regions.h"
#include "romset.h"
#include <filesystem>
#include "png.h"
#include "menu.h"
#include "online.h"
#include "links.h"
#include "text.h"
#include "chime.h"
#include "flags.h"
#include "crt_colours.h"
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
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>         // (power throttling off, below)
// A laptop with two graphics chips (NVIDIA Optimus, AMD switchable graphics): the game on the faster one, not the one
// in the processor, whatever Windows would pick. The drivers look for these two in the program's exports.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
#include <string>
#include <algorithm>
#include <atomic>
#include <array>
#include <optional>
#include <tuple>
#include <cmath>
#include <deque>
#include <map>
#include <vector>
#include <set>
#include <cerrno>
#include <climits>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#ifdef __APPLE__
#include <pthread.h>
#endif

// Every message box goes through here: a test run (VF_HIDDEN) never shows one (nor a file picker),
// the message goes to stderr (log.txt without a console) instead.
// The music's and effects' volumes an online match's own sound board plays at (0-100 each, 8 bits apiece): set by
// the Sound page, taken by the match sound's thread before each frame (the match's board is the other one, unheard;
// the overall volume is the stream's gain)
static std::atomic<uint32_t> g_match_volumes{ 0 };
static uint32_t pack_volumes(int music, int effects) { return uint32_t(music) | uint32_t(effects) << 8; }

// The menus' language (settings.ini's language, Auto the system's: tr), set as soon as settings.ini is read: the
// game's own boxes (choosing the ROM, a start that failed) in it too. Only what the menus and those boxes say, never
// the game, a match or a save; the log stays English
static void use_language(const std::string &key, bool say)
{
	tr::choice c;
	tr::choice_from(key, c);
	tr::set(tr::resolve(c));
	if (say) fprintf(stderr, "the menus' language: %s (Game > Language: %s)\n", tr::code(tr::current()), key.c_str());
}

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
// Sticks: a joystick SDL doesn't know as a pad (a DirectInput arcade stick) is given a fighting stick's layout
//   (give_stick_layout); Controls > Player > Set up buttons makes one from its own presses (settings.ini's
//   pad_mapping); a gamecontrollerdb.txt in the data folder or beside the program is read too (VF3's).
// The PC build (the default; --arcade for the cabinet's behaviour): no boot screen, always free
// play with nothing said about it, and Start simply starts.
bool g_pc_mode = true;
bool g_match_board = false;   // (an online match's board: the host's backup and the match's settings, none of this player's)

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
	static const bool vjoy_log = getenv("VF_TEST_VJOY") != nullptr;   // (tests: player 1's controls as they change)
	if (vjoy_log) {
		static std::string was;
		std::string now;
		static const uint8_t B[] = { PL_UP, PL_DOWN, PL_LEFT, PL_RIGHT, PL_GUARD, PL_PUNCH, PL_KICK };
		static const char *const N[] = { "up", "down", "left", "right", "guard", "punch", "kick" };
		for (int i = 0; i < 7; i++) if (in[1] & B[i]) now += std::string(now.empty() ? "" : " ") + N[i];
		if (in[0] & IN0_START1) now += std::string(now.empty() ? "" : " ") + "start";
		if (now != was) fprintf(stderr, "test: player 1: %s\n", now.empty() ? "-" : now.c_str());
		was = now;
	}
	for (int i = 0; i < 3; i++)
		m.m_inputs[i] = uint8_t(~in[i]);
	return hk;
}

// ---- sticks SDL doesn't know (VF3's) ----
// A joystick SDL has no gamepad mapping for (a DirectInput arcade stick: HORI's Virtua Stick, the Astro City Mini's)
// isn't a gamepad to SDL, and the game reads only gamepads. Each is given a fighting stick's layout by its GUID, a
// mapping as SDL's own database has them (buttons 0-7 as A B X Y LB RB LT RT, 8 View and 9 Start, the hat the d-pad,
// axes 0 and 1 the left stick), so it shows up as a pad and the rest (Controls' Pad and Pad buttons, the menu,
// online) is as for any pad. Controls > Player > Set up buttons makes one of its own from what's pressed, kept in
// settings.ini's pad_mapping lines. A gamecontrollerdb.txt in the data folder or beside the program is read first,
// and the player's own mappings after it: both win over the layout given here.
// The GUIDs given the stick's layout here (Any pad takes the pads SDL knows first, then these)
std::set<std::string> g_stick_guids;

std::string joystick_guid(SDL_JoystickID id)
{
	char buf[64];
	SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), buf, sizeof buf);
	return buf;
}

// a name as a mapping holds it (no commas: they part its fields)
std::string mapping_name(const char *n)
{
	std::string s = n && *n ? n : "Joystick";
	for (char &c : s) if (c == ',') c = ' ';
	return s;
}

// A joystick that isn't a gamepad given the stick's layout (true if it was). Not wheels, flight sticks and the like
// (an axis held at one end would hold a direction), nor one without buttons.
bool give_stick_layout(SDL_JoystickID id)
{
	static std::set<SDL_JoystickID> looked;   // (each joystick once: opening a DirectInput one is slow, and open_pads asks often)
	if (SDL_IsGamepad(id) || !looked.insert(id).second) return false;
	switch (SDL_GetJoystickTypeForID(id)) {
	case SDL_JOYSTICK_TYPE_WHEEL: case SDL_JOYSTICK_TYPE_FLIGHT_STICK: case SDL_JOYSTICK_TYPE_THROTTLE:
	case SDL_JOYSTICK_TYPE_GUITAR: case SDL_JOYSTICK_TYPE_DRUM_KIT: return false;
	default: break;
	}
	SDL_Joystick *j = SDL_OpenJoystick(id);
	if (!j) return false;
	const int nb = SDL_GetNumJoystickButtons(j), nh = SDL_GetNumJoystickHats(j), na = SDL_GetNumJoystickAxes(j);
	const std::string name = mapping_name(SDL_GetJoystickName(j)), guid = joystick_guid(id);
	// (axes 0 and 1 the left stick only if both rest in the middle: a slider or a trigger resting at one end would hold
	// a direction)
	bool centred = na >= 2;
	SDL_UpdateJoysticks();   // (a DirectInput device's axes read once: its initial state isn't always known yet)
	for (int a = 0; a < 2 && centred; a++) {
		Sint16 rest = 0;
		if (SDL_GetJoystickAxisInitialState(j, a, &rest) && std::abs(int(rest)) > 8000) centred = false;
		if (std::abs(int(SDL_GetJoystickAxis(j, a))) > 8000) centred = false;
	}
	SDL_CloseJoystick(j);
	// (a fighting stick: buttons, and a hat, six buttons, or four with a stick's two axes; anything less, as the HID
	// device a keyboard's lights or a media key can show up as, isn't made a pad, or it would take a player)
	if (nb <= 0 || (nh == 0 && nb < 6 && !(nb >= 4 && centred))) {
		if (nb > 0 || nh > 0) fprintf(stderr, "a joystick SDL doesn't know: %s (%s): %d buttons, %d hats, %d axes: not a stick, left alone\n", name.c_str(), guid.c_str(), nb, nh, na);
		return false;
	}
	static const char *const BUTTONS[] = { "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger" };
	std::string m = guid + "," + name + ",";
	for (int b = 0; b < std::min(nb, 8); b++) m += std::string(BUTTONS[b]) + ":b" + std::to_string(b) + ",";
	if (nb >= 10) m += "back:b8,start:b9,";   // (most sticks' Select and Start, after their eight)
	else if (nb == 9) m += "start:b8,";
	if (nh > 0) m += "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,";
	if (centred) m += "leftx:a0,lefty:a1,";
	if (SDL_AddGamepadMapping(m.c_str()) < 0) {
		fprintf(stderr, "a joystick SDL doesn't know: %s (%s): couldn't give it a stick's layout: %s\n", name.c_str(), guid.c_str(), SDL_GetError());
		return false;
	}
	g_stick_guids.insert(guid);
	fprintf(stderr, "a joystick SDL doesn't know: %s (%s): given a stick's layout; set its buttons in Controls\n", name.c_str(), guid.c_str());
	return true;
}

void give_unknown_sticks()
{
	int n = 0;
	SDL_JoystickID *ids = SDL_GetJoysticks(&n);
	for (int i = 0; i < n; i++) give_stick_layout(ids[i]);
	SDL_free(ids);
}

// The player's gamecontrollerdb.txt (SDL's community database's format), in the data folder or beside the program,
// then their own mappings (Set up buttons'), before the pads are opened: so both win over a stick's layout.
void load_pad_mappings(const std::string &data, const std::string &program, const std::vector<std::string> &own)
{
	std::set<std::string> seen;
	for (std::string d : { data, program }) {
		while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
		if (d.empty()) continue;
		const std::string f = d + "/gamecontrollerdb.txt";   // (as UTF-8, for SDL: not std::filesystem's, Windows' code page)
		if (!seen.insert(f).second) continue;
		SDL_PathInfo info;
		if (!SDL_GetPathInfo(f.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) continue;
		const int n = SDL_AddGamepadMappingsFromFile(f.c_str());
		if (n < 0) fprintf(stderr, "pads: couldn't read %s: %s\n", f.c_str(), SDL_GetError());
		else fprintf(stderr, "pads: %d mappings from %s\n", n, f.c_str());
	}
	for (const std::string &m : own)
		if (SDL_AddGamepadMapping(m.c_str()) < 0) fprintf(stderr, "pads: a pad_mapping in settings.ini wasn't taken (%s): %s\n", SDL_GetError(), m.c_str());
		else {
			const size_t a = m.find(','), b = m.find(',', a + 1);
			fprintf(stderr, "pads: the buttons set up in Controls for %s (%s)\n", m.substr(a + 1, b - a - 1).c_str(), m.substr(0, a).c_str());
		}
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

// The GPU's start marker: written in place and flushed (no swap: a rename's not on the disk until the folder is, and
// a crash that takes the machine down in the driver would lose it).
bool write_marker(const std::string &path, const char *text)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	const size_t n = strlen(text);
	const bool ok = fwrite(text, 1, n, f) == n && flush_to_disk(f);
	return fclose(f) == 0 && ok;
}

// The saves written off the game's thread: save_blob flushes each file to the disk (a crash mid-save
// keeps the old one), which on a hard disk waits tens of milliseconds a file, and the autosave ran
// every few seconds in play: a hitch each time (a reporter: "on an SSD barely noticeable, on a HDD
// heavy stutter even at 1x"). put() hands over a copy and returns at once; a writer thread saves
// it, the newest copy of a file winning if two wait. finish() writes what's left and stops.
struct bg_saver {
	std::mutex mx;
	std::condition_variable cv;
	std::map<std::string, std::vector<uint8_t>> jobs;
	bool stop = false;
	std::thread th{ [this] { run(); } };
	void put(const std::string &path, const void *src, size_t n) {
		{ std::lock_guard<std::mutex> l(mx); if (stop) return; jobs[path].assign(static_cast<const uint8_t *>(src), static_cast<const uint8_t *>(src) + n); }
		cv.notify_one();
	}
	void run() {
		std::unique_lock<std::mutex> l(mx);
		for (;;) {
			cv.wait(l, [&] { return stop || !jobs.empty(); });
			if (jobs.empty()) return;   // (stop, nothing left)
			auto todo = std::move(jobs);
			jobs.clear();
			l.unlock();
			for (auto &[path, bytes] : todo)
				if (!save_blob(path, bytes.data(), bytes.size())) fprintf(stderr, "couldn't save %s\n", path.c_str());
			l.lock();
		}
	}
	void finish() {
		{ std::lock_guard<std::mutex> l(mx); stop = true; }
		cv.notify_one();
		if (th.joinable()) th.join();
	}
	~bg_saver() { finish(); }
};

// The menu's choices and the controls, kept in settings.ini next to the saves ("key = value").
struct settings
{
	int scale = 2, aspect = 0, volume = 100, music = 100, effects = 100, aa = 1, filter = 0, ring = 0;
	int crt_colours = 0, brightness = 100, gamma = 100;   // the picture's colours: an arcade monitor's (crt::PRESETS), the player's brightness and gamma (%)
	int renderer3d = 2;     // the 3D's renderer: 0 the CPU, 1 the GPU, 2 Auto (whichever is quicker here)
	int graphics = 0;       // Windows: the GPU's driver, from the next start: 0 Auto (Vulkan first, Direct3D 12 if it won't start), 1 Direct3D 12
	int akira_slot = 0;     // Cheats: who plays in Akira's place (model1::UNUSED_CHARACTERS; 0 Akira)
	bool random_cpu = false;   // Cheats: with Random fighter, the computer's every opponent a random fighter too
	int random_fighter = 0;   // (Cheats: any random fighter, a player's or the computer's: over akira_slot; from random_player and random_cpu)
	std::string random_player[2];   // Cheats: each player's fighter: "" the select as ever, "*" a random one each game, or a name's (model1::set_random_player)
	std::string random_named[2];    // (each player's last name typed: Left / Right come back to it)
	void random_now() { random_fighter = !random_player[0].empty() || !random_player[1].empty() || random_cpu; }
	int infinite_energy = 0;   // Cheats: bit 0 player 1, bit 1 player 2
	bool one_hit_ko = false, no_time_limit = false, kage_unmasked = false;   // Cheats
	int ring_cheat = 0;     // Cheats: 0 the arcade's, 1 tiny (600), 2 huge (5000: no ring outs)
	int ring_now() const { return ring_cheat == 1 ? 600 : ring_cheat == 2 ? 5000 : ring; }
	static constexpr const char *BUILD = VF_VERSION " " VF_DATE " " VF_COMMIT;   // (what auto_3d was learnt by)
	std::string auto_3d;    // Auto's findings, per render scale and antialiasing ("3x1:g,2x0:c")
	int arcade_coin = -1;   // Arcade mode's coin/credit setting, kept while PC mode makes it free play (-1: not seen yet)
	bool english_set = false;   // (the game's language made English once, its first start here: Japanese after is the player's choice)
	bool vs_rounds_set = false;   // (Rounds to win (vs player) 2, the board's own, made 3 once: a 2 after is the player's choice)
	// (and once more, from 2 Nov, for everyone, an install that had the first move too: its own flag. A missing
	// settings.ini has neither, so its NVRAM's 2 moves too; vs_rounds_set still written, for older builds)
	bool vs_rounds_3 = false;
	std::string roms_folder;   // where the player keeps vf.zip (roms_folder = ...; read before the game starts, kept as written)
	bool log = true;   // log.txt written (Game > Log file: read before the log's opened, so from the next start)
	std::string language = "auto";   // Game > Language: auto (the system's), or a language's code (tr::choice_key)
	bool fullscreen = false, timings = false, arcade = false, smooth_bg = false, show_fps = false;
	int window_scale = 0;   // Display > Window size: the window k times the arcade's 384 lines high (4:3 and Fit 512 x k
	                        // wide, wider shapes theirs), each arcade line k of the screen's; 0 Free (as the player sizes it)
	// The window's place and size as last left in a window (VF3's issue #21), in the window's own units (points on a
	// Mac, as SDL_CreateWindow takes them); window_w 0: none yet. Maximised: the size under it kept, maximised again
	int window_w = 0, window_h = 0, window_x = 0, window_y = 0;
	bool window_max = false;
	// Fullscreen as a borderless window over the screen and a line past it (VF3's issue #7: AMD's driver on Windows
	// froze in its own, exclusive fullscreen): the way on Windows unless fullscreen_exclusive = 1 (Display > Fullscreen
	// mode); elsewhere the system's own fullscreen (a Mac's Spaces, the Deck's gamescope) unless fullscreen_window = 1
	bool fs_window = false, fs_exclusive = false;
	OnlineSettings online;   // the Online page's
	player_controls ctl[2] = { player_controls::defaults(0), player_controls::defaults(1) };
	std::vector<std::string> pad_mappings;   // pad_mapping lines: SDL mappings made in Controls > Set up buttons (one a pad)

	void load(const std::string &path)
	{
		FILE *f = fopen(path.c_str(), "r");
		if (!f) return;
		std::map<std::string, std::string> kv;
		char line[1100], key[64], val[1024];
		while (fgets(line, sizeof line, f))
			if (sscanf(line, " %63[a-z0-9_] = %1023[^\r\n]", key, val) == 2) {
				if (!strcmp(key, "pad_mapping")) pad_mappings.push_back(val);   // (several: one a pad)
				else kv[key] = val;
			}
		fclose(f);
		auto num = [&](const char *k, int &v, int lo, int hi) {
			auto it = kv.find(k);
			if (it != kv.end()) v = std::clamp(atoi(it->second.c_str()), lo, hi);
		};
		auto flag = [&](const char *k, bool &v) { int i = v; num(k, i, 0, 1); v = i; };
		num("scale", scale, 1, 6);
		if (auto it = kv.find("online_name"); it != kv.end()) online.name = online_menu::clean_name(it->second);
		if (auto it = kv.find("online_address"); it != kv.end()) online.address = online_menu::clean_address(it->second);
		if (auto it = kv.find("online_lobby"); it != kv.end() && (it->second.rfind("https://", 0) == 0 || it->second.rfind("http://", 0) == 0))
			online.lobby = it->second.substr(0, 200);   // (another lobby than the built-in one: tests, a server of one's own)
		num("online_delay", online.delay, -1, 10);
		num("online_port", online.port, 1024, 65535);
		num("online_connection", online.show_connection, 0, 2);
		flag("online_hide_ip", online.hide_ip);
		num("online_max_ping", online.max_ping, 0, 1000);
		flag("online_chime", online.chime);
		if (auto it = kv.find("online_fighter"); it != kv.end()) { std::string v = it->second; while (!v.empty() && isspace(uint8_t(v.back()))) v.pop_back(); online.fighter = v.substr(0, 16); }
		if (auto it = kv.find("online_room_name"); it != kv.end()) online.room_name = online_menu::clean_room(it->second);
		num("online_room_listed", online.room_who, 0, 2);
		num("online_rooms_show", online.rooms_show, 0, 2);
		flag("online_quick_listed", online.quick_listed);
		num("picture_shape", aspect, 0, 4);
		num("volume", volume, 0, 100);
		num("antialiasing", aa, 0, 3);   // 1 off, 0 Low (the 3D's edges smoothed), 2 and 3 drawn larger
		num("music_volume", music, 0, 100);
		num("effects_volume", effects, 0, 100);
		num("screen_filter", filter, 0, 2);
		num("crt_colours", crt_colours, 0, crt::N_PRESETS - 1);
		num("brightness", brightness, crt::BRIGHT_MIN, crt::BRIGHT_MAX);
		num("gamma", gamma, crt::GAMMA_MIN, crt::GAMMA_MAX);
		num("ring_size", ring, 0, 1500);
		if (ring && (ring < 1200 || ring % 100)) ring = 0;   // 0: by difficulty
		flag("fullscreen", fullscreen);
		flag("fullscreen_window", fs_window);
		flag("fullscreen_exclusive", fs_exclusive);
		flag("frame_timings", timings);
		flag("log", log);
		if (auto it = kv.find("language"); it != kv.end()) {   // (one this build doesn't know: auto)
			tr::choice c;
			language = tr::choice_from(it->second, c) ? it->second : "auto";
		}
		flag("arcade_mode", arcade);
		flag("background_filter", smooth_bg);
		flag("show_fps", show_fps);
		num("window_scale", window_scale, 0, 16);
		num("window_w", window_w, 0, 65535);
		num("window_h", window_h, 0, 65535);
		num("window_x", window_x, -65535, 65535);
		num("window_y", window_y, -65535, 65535);
		flag("window_maximised", window_max);
		if (window_w < 64 || window_h < 48) window_w = window_h = 0;
		num("arcade_coins", arcade_coin, -1, 0xffff);
		num("renderer_3d", renderer3d, 0, 2);
		num("graphics", graphics, 0, 1);
		num("akira_slot", akira_slot, 0, int(std::size(model1::UNUSED_CHARACTERS)) - 1);
		num("random_fighter", random_fighter, 0, 2);   // (0 off, else on: 2 was "Looks + moves", now what on is)
		flag("random_opponents", random_cpu);
		num("infinite_energy", infinite_energy, 0, 3);
		flag("one_hit_ko", one_hit_ko);
		flag("no_time_limit", no_time_limit);
		flag("kage_unmasked", kage_unmasked);
		flag("english_set", english_set);
		flag("vs_rounds_set", vs_rounds_set);
		flag("vs_rounds_3", vs_rounds_3);
		num("ring_cheat", ring_cheat, 0, 2);
		if (auto it = kv.find("auto_3d"); it != kv.end()) auto_3d = it->second;
		// (Auto's findings by another build (another version or commit) forgotten, so each new build looks
		// again: a driver updated since, or a fix here; kept for ever, one slow measure kept a scale on the CPU)
		if (auto it = kv.find("auto_3d_build"); !auto_3d.empty() && (it == kv.end() || it->second != BUILD)) {
			fprintf(stderr, "settings: Auto's findings (%s) by %s forgotten: this build looks again\n", auto_3d.c_str(), it == kv.end() ? "an older build" : it->second.c_str());
			auto_3d.clear();
		}
		auto trimmed = [](std::string v) { while (!v.empty() && isspace(uint8_t(v.back()))) v.pop_back(); return v; };   // (a value edited by hand: "* ")
		for (int p = 0; p < 2; p++)
			if (auto it = kv.find(p ? "random_p2_name" : "random_p1_name"); it != kv.end()) random_named[p] = trimmed(it->second);
		for (int p = 0; p < 2; p++)
			if (auto it = kv.find(p ? "random_p2" : "random_p1"); it != kv.end()) random_player[p] = trimmed(it->second) == "normal" ? std::string() : trimmed(it->second);
			else if (random_fighter) random_player[p] = "*";   // (from before: Random fighter on, both players)
		if (auto it = kv.find("random_name"); it != kv.end() && random_fighter && kv.find("random_p1") == kv.end())
			random_player[0] = random_player[1] = trimmed(it->second);   // (from before: one name for both)
		for (int p = 0; p < 2; p++)   // (a seed played, its name kept for the menu's Seed row too)
			if (random_named[p].empty() && !random_player[p].empty() && random_player[p] != "*" && random_player[p][0] != '#') random_named[p] = random_player[p];
		// (from before: Secret character, for anyone at Akira's place: player 1's fighter now)
		if (akira_slot > 0 && random_player[0].empty() && kv.find("random_p1") == kv.end()) random_player[0] = "#" + std::to_string(akira_slot);
		akira_slot = 0;
		random_now();
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

	// False if it couldn't be written (the caller keeps it to save again; said once in the log)
	bool save(const std::string &path) const
	{
		const std::string tmp = path + ".tmp";
		std::error_code ec;
		if (const auto dir = std::filesystem::path(path).parent_path(); !dir.empty()) std::filesystem::create_directories(dir, ec);
		FILE *f = fopen(tmp.c_str(), "w");
		static bool said = false;
		if (!f) {
			if (!said) fprintf(stderr, "couldn't save the settings to %s: %s\n", path.c_str(), strerror(errno));
			said = true;
			return false;
		}
		fprintf(f, "# Virtua Fighter settings (the in-game menu writes these)\n");
		fprintf(f, "scale = %d\npicture_shape = %d\nfullscreen = %d\nframe_timings = %d\nvolume = %d\narcade_mode = %d\n"
				"background_filter = %d\nmusic_volume = %d\neffects_volume = %d\nantialiasing = %d\nshow_fps = %d\nscreen_filter = %d\nring_size = %d\n", scale, aspect,
				int(fullscreen), int(timings), volume, int(arcade), int(smooth_bg), music, effects, aa, int(show_fps), filter, ring);
		if (!log) fprintf(f, "log = 0\n");
		fprintf(f, "language = %s\n", language.c_str());
		fprintf(f, "window_scale = %d\n", window_scale);
		if (fs_window) fprintf(f, "fullscreen_window = 1\n");
		if (fs_exclusive) fprintf(f, "fullscreen_exclusive = 1\n");
		if (window_w > 0) fprintf(f, "window_w = %d\nwindow_h = %d\nwindow_x = %d\nwindow_y = %d\nwindow_maximised = %d\n", window_w, window_h, window_x, window_y, int(window_max));
		if (arcade_coin >= 0) fprintf(f, "arcade_coins = %d\n", arcade_coin);
		fprintf(f, "renderer_3d = %d\n", renderer3d);
		if (graphics) fprintf(f, "graphics = %d\n", graphics);
		if (akira_slot) fprintf(f, "akira_slot = %d\n", akira_slot);
		for (int p = 0; p < 2; p++) {
			// (Normal written as "normal": an empty value isn't read back, and a player missing came back as Random)
			fprintf(f, "random_p%d = %s\n", p + 1, random_player[p].empty() ? "normal" : random_player[p].c_str());
			if (!random_named[p].empty()) fprintf(f, "random_p%d_name = %s\n", p + 1, random_named[p].c_str());
		}
		if (random_cpu) fprintf(f, "random_opponents = 1\n");
		if (infinite_energy) fprintf(f, "infinite_energy = %d\n", infinite_energy);
		if (one_hit_ko) fprintf(f, "one_hit_ko = 1\n");
		if (no_time_limit) fprintf(f, "no_time_limit = 1\n");
		if (kage_unmasked) fprintf(f, "kage_unmasked = 1\n");
		if (english_set) fprintf(f, "english_set = 1\n");
		if (vs_rounds_set) fprintf(f, "vs_rounds_set = 1\n");
		if (vs_rounds_3) fprintf(f, "vs_rounds_3 = 1\n");
		fprintf(f, "crt_colours = %d\nbrightness = %d\ngamma = %d\n", crt_colours, brightness, gamma);
		if (ring_cheat) fprintf(f, "ring_cheat = %d\n", ring_cheat);
		if (!auto_3d.empty()) fprintf(f, "auto_3d = %s\nauto_3d_build = %s\n", auto_3d.c_str(), BUILD);
		if (!roms_folder.empty()) fprintf(f, "roms_folder = %s\n", roms_folder.c_str());
		fprintf(f, "online_name = %s\nonline_delay = %d\nonline_connection = %d\nonline_port = %d\n", online.name.c_str(), online.delay,
				online.show_connection, online.port);
		if (!online.address.empty()) fprintf(f, "online_address = %s\n", online.address.c_str());
		if (!online.lobby.empty()) fprintf(f, "online_lobby = %s\n", online.lobby.c_str());
		fprintf(f, "online_hide_ip = %d\nonline_max_ping = %d\nonline_chime = %d\n", int(online.hide_ip), online.max_ping, int(online.chime));
		if (!online.fighter.empty()) fprintf(f, "online_fighter = %s\n", online.fighter.c_str());
		if (!online.room_name.empty()) fprintf(f, "online_room_name = %s\n", online.room_name.c_str());
		fprintf(f, "online_room_listed = %d\nonline_rooms_show = %d\nonline_quick_listed = %d\n", online.room_who, online.rooms_show, int(online.quick_listed));
		for (int p = 0; p < 2; p++) ctl[p].save(f, p);
		for (const std::string &m : pad_mappings) fprintf(f, "pad_mapping = %s\n", m.c_str());
		const bool flushed = flush_to_disk(f);
		if (fclose(f) == 0 && flushed && replace_file(path, tmp)) return true;
		std::filesystem::remove(tmp, ec);
		if (!said) fprintf(stderr, "couldn't save the settings to %s\n", path.c_str());
		said = true;
		return false;
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
bool first_launch(const std::string &data, const std::string &keep, const std::string &folder, model1::regions &out, std::string &first_link)
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
	// where a picked set goes: the ROM folder, but never over the player's own set there (one that
	// failed the check goes on failing; the picked one in the data folder's roms, looked in next time)
	fs::path dest = roms;
	if (!have.empty()) {
		romset::result r = romset::load(have);
		if (r.ok) { keep_copy(); out = std::move(r.regions); return true; }
		problem = r.error + "\n\n";
		if (!folder.empty() && fs::path(have[0]).parent_path() == roms) {
			dest = own;
			if (std::vector<std::string> o = zips_in(own); !o.empty()) {
				romset::result r2 = romset::load(o);
				if (r2.ok) { fprintf(stderr, "the ROM folder's set: %s; the game from %s\n", r.error.c_str(), own.string().c_str()); out = std::move(r2.regions); return true; }
			}
		}
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
	const std::string where = tr::fill(tr::ROMS_COPIED_INTO, { { "folder", dest.string() } });
	std::string note = problem + tr::fill(tr::ROMS_CHOOSE, { { "where", where } });
	for (;;) {
		if (!message_box(SDL_MESSAGEBOX_INFORMATION, "Virtua Fighter", note)) return false;
		// (SDL may answer on another thread: done is set last, released, so failed and files are seen)
		struct pick { std::atomic<bool> done{ false }; bool failed = false; std::vector<std::string> files; } chosen;
		static std::string filter_name;   // (kept: the dialog reads it later)
		filter_name = tr::G_ZIP_FILTER;
		static SDL_DialogFileFilter filters[] = { { "", "zip" } };
		filters[0].name = filter_name.c_str();
		SDL_ShowOpenFileDialog([](void *ud, const char *const *list, int) {
			auto *c = static_cast<pick *>(ud);
			if (!list) c->failed = true;                  // no picker here (e.g. Steam Deck Game Mode)
			else for (; *list; list++) c->files.push_back(*list);
			c->done.store(true, std::memory_order_release);
		}, &chosen, nullptr, filters, 1, nullptr, false);
		while (!chosen.done.load(std::memory_order_acquire)) {
			SDL_Event e;
			while (SDL_PollEvent(&e))   // (a link that started the app, a Mac's open-URL event, kept for once the game's up)
				if (e.type == SDL_EVENT_DROP_FILE && e.drop.data && links::is_link(e.drop.data)) first_link = std::string(e.drop.data).substr(0, 64);
			SDL_Delay(10);
		}
		if (chosen.failed) {
			const std::string msg = tr::fill(tr::ROMS_NO_PICKER, { { "folder", dest.string() } });
			std::error_code ec;
			fs::create_directories(dest, ec);
			message_box(SDL_MESSAGEBOX_INFORMATION, "Virtua Fighter", msg);
			return false;
		}
		if (chosen.files.empty()) return false;
		const std::string f = chosen.files[0];
		if (!romset::is_game_set(f)) {
			note = tr::fill(tr::ROMS_NOT_A_SET, { { "file", fs::path(f).filename().string() }, { "where", where } });
			continue;
		}
		std::error_code ec;
		fs::create_directories(dest, ec);
		std::string why;
		if (!copy_whole(f, dest / "vf.zip", why)) {
			note = tr::fill(tr::ROMS_NO_COPY, { { "folder", dest.string() }, { "why", why } });
			continue;
		}
		const fs::path io = fs::path(f).parent_path() / "model1io.zip";    // its neighbour, if any
		if (fs::exists(io, ec)) copy_whole(io, dest / "model1io.zip", why);
		romset::result r = romset::load(zips_in(dest));
		if (r.ok) { keep_copy(); out = std::move(r.regions); return true; }
		note = tr::fill(tr::ROMS_BAD_SET, { { "why", r.error } });   // ({why}: romset's, in English)
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
// Portable mode, as the player asks for it: a file named portable.txt beside the program, or in the
// folder above it (its name as found, else null). Any contents; "portable" and Windows' portable.txt.txt (a New Text Document
// renamed with its extension hidden) count too.
const char *portable_marker(const std::string &folder)
{
	if (folder.empty()) return nullptr;
	// beside the program, or the folder above it: the download unzips into a folder of its own,
	// and a portable.txt put in the folder it was unzipped to ("the main folder") went unseen
	// (2026-10-16, a player's report). Looked for with SDL (UTF-8 names, as on a drive whose
	// folders have accents), not std::filesystem (Windows' code page there).
	std::string up = folder;
	while (!up.empty() && (up.back() == '/' || up.back() == '\\')) up.pop_back();
	const size_t cut = up.find_last_of("/\\");
	up = cut == std::string::npos ? std::string() : up.substr(0, cut + 1);
	static const char *const NAMES[] = { "portable.txt", "portable", "portable.txt.txt" };
	static const char *const BESIDE[] = { "portable.txt beside the program", "portable beside the program", "portable.txt.txt beside the program" };
	static const char *const ABOVE[] = { "portable.txt in the folder above", "portable in the folder above", "portable.txt.txt in the folder above" };
	for (int above = 0; above < 2; above++) {
		const std::string &dir = above ? up : folder;
		if (dir.empty()) continue;
		for (int k = 0; k < 3; k++) {
			SDL_PathInfo info;
			if (SDL_GetPathInfo((dir + NAMES[k]).c_str(), &info) && info.type == SDL_PATHTYPE_FILE) return above ? ABOVE[k] : BESIDE[k];
		}
	}
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

// settings.ini's log line, read before the settings are (the log's opened first): false for log = 0
static bool log_wanted(const std::string &path)
{
	bool on = true;
	if (FILE *f = fopen(path.c_str(), "r")) {
		char line[256];
		while (fgets(line, sizeof line, f)) {
			int v = 1;
			if (sscanf(line, " log = %d", &v) == 1) on = v != 0;
		}
		fclose(f);
	}
	return on;
}
#ifdef _WIN32
static const char *const NULL_DEVICE = "NUL";
#else
static const char *const NULL_DEVICE = "/dev/null";
#endif

int main(int argc, char **argv)
{
#ifdef __APPLE__
	// the game, its drawing and its sound on the performance cores: without it an M-series Mac can put
	// them on the efficiency cores (VF3's d5b1992)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
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
	// Playing by address from the command line (a LAN, a VPN): --host waits for a player, --join ADDRESS joins one;
	// --port N the host's UDP port (7700); as Online > Play by address, started as the game is up
	bool arg_host = false;
	std::string arg_join;
	int arg_port = 0;
	std::string arg_link;   // (a link from the Online page on the web: vf1recomp://vf1/join/CODE, links.h)
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
		else if (a == "--host") arg_host = true;
		else if (a == "--join" && i + 1 < argc) arg_join = argv[++i];
		else if (a.rfind("--join=", 0) == 0) arg_join = a.substr(7);
		else if (a == "--port" && i + 1 < argc) arg_port = std::clamp(atoi(argv[++i]), 1024, 65535);
		else if (a.rfind("--port=", 0) == 0) arg_port = std::clamp(atoi(a.c_str() + 7), 1024, 65535);
		else if (links::is_link(a)) { arg_link = a.substr(0, 64); break; }   // (nothing after a link read: on Windows that would be its own text, past a quote in it)
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
			// a link with the game already running (Windows, Linux): handed to it, and this copy's done, before its
			// log.txt is opened (the running copy's left as it is)
			if (!arg_link.empty() && links::hand_over(dir, arg_link)) return 0;
			// A player's build has no console (a Mac app, a Windows GUI program, a Steam shortcut):
			// its messages go to log.txt in the data folder instead, for when something goes wrong.
			// settings.ini's log = 0 (Game > Log file): no log.txt written, the program's messages thrown away; read
			// here, before the log's opened (the settings themselves are read below)
			if (!log_wanted(dir + "/settings.ini")) {
				if (!freopen(NULL_DEVICE, "w", stderr)) {}
			} else if (!isatty(fileno(stderr))) {
				std::filesystem::create_directories(dir, ec);
				if (!freopen((dir + "/log.txt").c_str(), "w", stderr)) {}
				setvbuf(stderr, nullptr, _IONBF, 0);
			}
			// the log's first line: which build (as the menu's footer), and where it runs
			fprintf(stderr, "Virtua Fighter Recomp %s (%s, online %s) on %s, %d logical processors, %d MB of memory\n", VF_VERSION, *VF_DATE ? VF_DATE : "no date",
					vf1net::online_version(VF_VERSION).c_str(), SDL_GetPlatform(), SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM());
			if (portable) fprintf(stderr, "portable (%s): everything in %s\n", portable, dir.c_str());
			else if (home.find("/AppTranslocation/") != std::string::npos)   // (macOS runs a downloaded app from a hidden copy until it's moved)
				fprintf(stderr, "run from macOS's hidden copy of the app (App Translocation): a portable.txt beside it isn't seen until the app is moved with the Finder\n");
			// the player's ROM folder: the command line's, else VF_ROMS, else settings.ini's
			// (settings.ini read now for its ROM folder, and the language of the boxes that ask for the game)
			settings pre;
			pre.load(dir + "/settings.ini");
			use_language(pre.language, false);
			std::string roms_folder, from;
			if (!arg_roms.empty()) roms_folder = player_folder(arg_roms), from = "--roms";
			else if (const char *e = getenv("VF_ROMS"); e && *e) roms_folder = player_folder(e), from = "VF_ROMS";
			else if (!pre.roms_folder.empty()) roms_folder = player_folder(pre.roms_folder, dir), from = "settings.ini";
			if (!roms_folder.empty()) fprintf(stderr, "ROM folder (%s): %s\n", from.c_str(), roms_folder.c_str());
			else fprintf(stderr, "ROM folder: %s\n", (std::filesystem::path(dir) / "roms").string().c_str());
			std::string first_link;   // (a link that came while the ROM was being chosen: a Mac's, the app started by it)
			if (!first_launch(dir, keep == dir ? std::string() : keep, roms_folder, game, first_link)) return getenv("VF_HIDDEN") ? 1 : 0;   // (a test run without the game: an error)
			if (arg_link.empty() && !first_link.empty()) arg_link = first_link;
		}
	}
	// a link with the game already running (Windows, Linux): handed to it, and this copy's done; else this one's the
	// copy links go to, and this user's system told it opens them (not in tests)
	if (!arg_link.empty() && links::hand_over(dir, arg_link)) { fprintf(stderr, "links: handed %s to the copy running\n", arg_link.c_str()); return 0; }
	links::running(dir);
	if (!getenv("VF_HIDDEN") && !getenv("VF_QUIT_AFTER")) { const std::string said = links::register_scheme(); if (!said.empty()) fprintf(stderr, "%s\n", said.c_str()); }
	const std::string settings_path = dir + "/settings.ini";
	settings cfg;
	bool cfg_dirty = false;      // saved at exit only when the player changed something
	// The first run's render scale: 2x everywhere (the pictures are drawn on the CPU, and a thread
	// count says little about how fast it is); the menu goes higher. Steam sets SteamDeck=1 on the
	// Deck: fullscreen, 16:10.
	cfg.scale = 2;
	if (getenv("SteamDeck")) { cfg.fullscreen = true; cfg.aspect = 2; }
	cfg.load(settings_path);
	use_language(cfg.language, true);   // (the menus', and the game's own boxes': Game > Language)
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
	// already 10,576 x 3,072 and about 600 MB of buffers; more would fail on ordinary machines), and
	// past 32:9 within that same 10,576 pixels across (80:9 at 8x would be 26,456: past a GPU's
	// 16,384 and over 1.5 GB)
	auto aa_fit = [&m](int scale, int aa) {
		const int across = m.frame_w() / std::max(1, m.render_scale());   // the picture's arcade pixels across
		return std::max(1, std::min({ aa, 8 / scale, 10576 / (std::max(1, across) * scale) }));
	};
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
	// the ranking's names for random fighters and secret characters (model1::ranking_frame), beside the backup
	const std::string rank_path = dir + "/ranking_names.txt";
	uint32_t rank_saved = m.ranking_names_version();   // (a name found by its time and character at the load: saved again)
	if (FILE *f = fopen(rank_path.c_str(), "rb")) {
		std::string text;
		char buf[4096];
		for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
		fclose(f);
		m.set_ranking_names(text);
	}
	bg_saver saver;   // (the autosave's writes, off the game's thread)
	bool rank_put = false;   // (names handed to it: the last save at quit writes them whatever, in case its write failed)
	auto save_rank_names = [&](bool now) {
		if (m.ranking_names_version() == rank_saved && !(now && rank_put)) return;
		const std::string text = m.ranking_names();
		if (!now) { saver.put(rank_path, text.data(), text.size()); rank_saved = m.ranking_names_version(); rank_put = true; return; }
		if (save_blob(rank_path, reinterpret_cast<const uint8_t *>(text.data()), text.size())) rank_saved = m.ranking_names_version();
	};
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
	m.set_strands_moved(cfg.random_fighter > 0);   // (Random fighter's hair: the same)
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
			const tr::line why(tr::FAIL_RECOMPILE, { { "file", find_assets() + "/vf_map.txt" } });
			fprintf(stderr, "%s\n", why.en().c_str());
			message_box(SDL_MESSAGEBOX_WARNING, "Virtua Fighter", why.str());
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
	m.set_kage_unmasked(cfg.kage_unmasked);
	// (a player's Random fighter wins over Secret character: both are Akira's place; Random opponents alone doesn't)
	auto akira_now = [&] { return 0; };   // (a secret character is a player's fighter now: Player 1 / 2 fighter)
	// (Random fighter first: its records put back as VF has them, then the Secret character's
	// changes to them, as its portrait head, kept)
	auto apply_players = [&] {   // (each player's setting; a name no longer made: Random)
		for (int p = 0; p < 2; p++)
			if (!m.set_random_player(p, cfg.random_fighter ? cfg.random_player[p] : std::string())) {
				// (a name no seed gives now: shown as Random, not as a Seed it can't play, the name gone)
				if (cfg.random_named[p] == cfg.random_player[p]) cfg.random_named[p].clear();
				cfg.random_player[p] = "*";
				m.set_random_player(p, "*");
				cfg_dirty = true;
			}
	};
	apply_players();
	m.set_random_fighter(cfg.random_fighter > 0, cfg.random_fighter > 0, cfg.random_cpu);
	m.set_akira_slot(akira_now());
	auto icon_dirs = [&] { return std::vector<std::string>{ dir + "/icons", find_assets() + "/icons" }; };   // (the player's first)
	load_secret_icon(m, akira_now(), icon_dirs());
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
	// The game's language English, once: the board comes set to Japanese (Arcade settings' Language
	// changes it after; an install from before it, English the once too)
	auto settle_language = [&]() -> bool {
		if (cfg.english_set || m.assignment(model1::game_assignment::language) < 0) return false;   // (the game's very first boot writes the settings)
		cfg.english_set = true; cfg_dirty = true;
		return m.set_assignment(model1::game_assignment::language, 1);
	};
	// Two players' matches first to 3 rounds, once, as VF3's and SEGA's tournaments (and every online match): the
	// board comes set to 2 (a new player's, an install from before it, and one whose settings.ini is gone alike: its
	// NVRAM kept a 2; Arcade settings' Rounds to win (vs player) changes it after, and a 2 chosen then stays)
	auto settle_vs_rounds = [&]() -> bool {
		if (cfg.vs_rounds_3 || m.assignment(model1::game_assignment::match_vs) < 0) return false;   // (the game's very first boot writes the settings)
		cfg.vs_rounds_set = cfg.vs_rounds_3 = true; cfg_dirty = true;
		if (m.assignment(model1::game_assignment::match_vs) != 2) return false;
		fprintf(stderr, "settings: Rounds to win (vs player) 2 (the board's own) to 3, once\n");
		return m.set_assignment(model1::game_assignment::match_vs, 3);
	};
	auto apply_play_style = [&] { if (g_pc_mode) m.hide_free_play_text(); else m.show_free_play_text(); };
	apply_play_style();
	boot();
	if (const bool coins = settle_coins(), language = settle_language(), rounds = settle_vs_rounds(); coins || language || rounds) boot();   // (all, each its own)

	// Video is a must (with a message if it fails: a player's build has no console); sound and
	// pads aren't: without a sound device the game keeps time by the clock.
	auto fail = [](tr::text what) {   // (the log in English, the box in the menus' language; SDL's words as they are)
		const tr::line msg(what, { { "why", tr::line(std::string(SDL_GetError())) } });
		fprintf(stderr, "%s\n", msg.en().c_str());
		message_box(SDL_MESSAGEBOX_ERROR, "Virtua Fighter", msg.str());
		return 1;
	};
#ifdef _WIN32
	// Windows only lets the program in front hand over the foreground: started from another program,
	// the game could open behind it with the keys going there. SDL's raise can force it.
	SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
	// Power throttling off for the game (as VF2's and VF3's): its threads at the clocks and on the cores the
	// scheduler gives a program that's in use, and its timers as fine as asked for, even minimized. (Windows 11
	// otherwise may put a game it thinks is in the background on the efficiency cores, at their low clocks.)
	{
		using set_t = BOOL(WINAPI *)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
		if (auto set = reinterpret_cast<set_t>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation")))) {
			PROCESS_POWER_THROTTLING_STATE st{};
			st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
			st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
			st.StateMask = 0;
			if (!set(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st)) {   // (Windows 10 before 21H2 has no timer flag)
				st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
				set(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
			}
		}
	}
#endif
	if (!SDL_Init(SDL_INIT_VIDEO)) return fail(tr::FAIL_VIDEO);
	if (getenv("VF_TEST_VJOY")) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   // (tests: a hidden window never has the focus)
	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) fprintf(stderr, "no gamepads: %s\n", SDL_GetError());
	// testing: VF_TEST_VJOY, a virtual stick SDL doesn't know (10 buttons, a hat, 2 axes), pressed as it says (below)
	SDL_Joystick *test_vjoy = nullptr;
	auto attach_vjoy = [&] {
		SDL_VirtualJoystickDesc d;
		SDL_INIT_INTERFACE(&d);
		d.type = SDL_JOYSTICK_TYPE_UNKNOWN;
		d.vendor_id = 0x1234;
		d.product_id = 0x5678;
		d.naxes = 2;
		d.nbuttons = 10;
		d.nhats = 1;
		d.name = "VF1 Test Stick";
		const SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
		test_vjoy = id ? SDL_OpenJoystick(id) : nullptr;
		fprintf(stderr, "test: a virtual stick: %s, %s\n", id ? joystick_guid(id).c_str() : SDL_GetError(),
		        id && SDL_IsGamepad(id) ? "a gamepad to SDL" : "not a gamepad to SDL");
	};
	if (const char *v = getenv("VF_TEST_VJOY"); v && !strchr(v, 'p')) attach_vjoy();   // (p@N: plugged in at pass N instead)
	load_pad_mappings(dir, program_folder(), cfg.pad_mappings);
	give_unknown_sticks();
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) fprintf(stderr, "no sound: %s\n", SDL_GetError());
	// the first window: 1366 x 768, or 16:9 within 90% of a smaller screen's usable area
	int win_w = 1366, win_h = 768;
	// testing: VF_WINDOW=WxH, the window's size in points (a hidden one as large as a player's screen)
	const bool fixed_window = getenv("VF_WINDOW") && sscanf(getenv("VF_WINDOW"), "%dx%d", &win_w, &win_h) == 2;
	bool win_placed = false;   // (the window put where it was last left)
	int win_x = 0, win_y = 0;
	if (!fixed_window) {
		SDL_Rect usable;
		if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {
			const float k = std::min({ 1.0f, usable.w * 0.9f / win_w, usable.h * 0.9f / win_h });
			win_w = int(win_w * k); win_h = int(win_h * k);
		}
		// The window as last left (settings.ini's window_w ...): there if it's still all on a connected screen's usable
		// area; else at that size (no larger than the screen) in the middle of the screen it was most on, or the primary
		if (cfg.window_w > 0) {
			int n = 0;
			SDL_DisplayID *ds = SDL_GetDisplays(&n);
			const SDL_Rect r{ cfg.window_x, cfg.window_y, cfg.window_w, cfg.window_h };
			SDL_Rect best{ 0, 0, 0, 0 };
			long long best_area = -1;
			for (int i = 0; ds && i < n; i++) {
				SDL_Rect u, in;
				if (!SDL_GetDisplayUsableBounds(ds[i], &u) || u.w <= 0 || u.h <= 0) continue;
				const long long a = SDL_GetRectIntersection(&r, &u, &in) ? (long long)in.w * in.h : 0;
				if (r.x >= u.x && r.y >= u.y && r.x + r.w <= u.x + u.w && r.y + r.h <= u.y + u.h) { best = u; best_area = LLONG_MAX; break; }
				if (a > best_area || (best_area <= 0 && ds[i] == SDL_GetPrimaryDisplay())) { best = u; best_area = a; }
			}
			SDL_free(ds);
			if (best.w > 0) {
				win_w = std::min(cfg.window_w, best.w);
				win_h = std::min(cfg.window_h, best.h);
				if (best_area == LLONG_MAX) { win_x = cfg.window_x; win_y = cfg.window_y; }
				else { win_x = best.x + (best.w - win_w) / 2; win_y = best.y + (best.h - win_h) / 2; }
				win_placed = true;
				fprintf(stderr, "the window as last left: %d x %d at %d, %d%s%s\n", win_w, win_h, win_x, win_y,
				        best_area == LLONG_MAX ? "" : " (centred: not all on a screen now)", cfg.window_max ? ", maximised" : "");
			}
		}
	}
	// Fullscreen's way (Config's fs_window): a borderless window over the screen and a line past it, so no driver
	// takes it for a game's own fullscreen (VF3's issue #7: AMD's Vulkan on Windows froze there, every frame waiting),
	// the window's place, size and maximised kept for the way back; or the system's own (SDL_WINDOW_FULLSCREEN)
#ifdef _WIN32
	static constexpr bool FS_BORDERLESS_DEFAULT = true;
#else
	static constexpr bool FS_BORDERLESS_DEFAULT = false;   // (a Mac's green button and Spaces, the Deck's gamescope)
#endif
	auto fs_borderless = [&] { return !cfg.fs_exclusive && (FS_BORDERLESS_DEFAULT || cfg.fs_window); };
	SDL_PropertiesID wp = SDL_CreateProperties();
	SDL_SetStringProperty(wp, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Virtua Fighter");
	SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, win_w);
	SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, win_h);
	if (win_placed) {
		SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_X_NUMBER, win_x);
		SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_Y_NUMBER, win_y);
	}
	SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
		(fullscreen && !fs_borderless() ? SDL_WINDOW_FULLSCREEN : 0) | (win_placed && cfg.window_max ? SDL_WINDOW_MAXIMIZED : 0) |
		(getenv("VF_HIDDEN") ? SDL_WINDOW_HIDDEN : 0));   // testing: never on screen
	SDL_Window *win = SDL_CreateWindowWithProperties(wp);
	bool fs_win_on = false, fs_win_max = false;
	SDL_Rect fs_win_was{ 0, 0, 0, 0 };
	auto is_fullscreen = [&] { return fs_win_on || (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0; };
	SDL_DestroyProperties(wp);
	if (!win) return fail(tr::FAIL_WINDOW);
	SDL_HideCursor();
	// Started from a terminal, the window can open behind it or without the keyboard (the keys go
	// to the terminal): take it, and again once the first picture is up if it still isn't ours.
	if (!getenv("VF_HIDDEN")) SDL_RaiseWindow(win);
	int focus_checks = 600;   // about 5 s: going fullscreen on a Mac moves to a new Space, which takes a while
	SDL_Renderer *ren = nullptr;
	// Why the pictures or the 3D are on the CPU when the setting isn't CPU: said on screen, not only in the log (a
	// player whose GPU couldn't be used just saw a slow game). cpu_why_new: not said yet.
	// (its words, tr: why, what to do, and the scale cut for the run said with it: cpu_notice)
	std::string cpu_why;          // (why, in English: empty none said)
	tr::line cpu_why_line, cpu_todo;
	bool cpu_why_new = false, cpu_why_cut = false;
	// (why: what happened, in a player's words; then what to do. None with a renderer chosen by hand, SDL_RENDER_DRIVER)
#ifdef __APPLE__
	static const tr::text UPDATE = tr::CPU_TODO_UPDATE_MAC;
#else
	static const tr::text UPDATE = tr::CPU_TODO_UPDATE;
#endif
	static const tr::text NEXT = tr::CPU_TODO_NEXT_START;
	auto on_cpu_because = [&](tr::text why, tr::text what_to_do) {
		if (cpu_why.empty() && !getenv("SDL_RENDER_DRIVER")) { cpu_why_line = why; cpu_todo = what_to_do; cpu_why = cpu_why_line.en(); cpu_why_new = true; }
	};
	auto cpu_notice = [&] { return tr::line(cpu_why_cut ? tr::CPU_NOTICE_CUT : tr::CPU_NOTICE, { { "why", cpu_why_line }, { "todo", cpu_todo } }); };
#if VF_GPU3D_SDL
	// The 3D on the GPU wanted (Auto or GPU): SDL's "gpu" renderer (Direct3D 12 on Windows, Vulkan on
	// Linux and the Deck), whose device the 3D's compute shares, drawing straight into the renderer's
	// textures: nothing comes back to the CPU and nothing waits for the GPU (vf1-recomp #11: on a
	// Radeon 780M at 6x each picture's 3D came back, 37 MB, was merged on the CPU and went up again,
	// the GPU idle at its lowest clock in between). Else, or if it won't start or can't put pictures
	// together exactly, SDL's usual one and the 3D on a device of its own, as before. (SDL_RENDER_DRIVER,
	// SDL's own, chooses over this; VF_NO_GPU_RENDERER=1 skips it, tests. A start that closed while
	// starting it leaves gpu-start.crashed, as the 3D's own start does below: SDL's usual one then.)
	const std::string gpu_marker = dir + "/gpu-start.crashed";
	// (a marker holds the build that left it: another build's, a driver or a fix since, the GPU tried again)
	auto marker_by = [&](const std::string &path) {
		std::string by;
		size_t n = 0;
		if (void *d = SDL_LoadFile(path.c_str(), &n)) { by.assign(static_cast<const char *>(d), n); SDL_free(d); }
		return by;
	};
	auto marker_crashed = [&](const std::string &path) {
		if (!SDL_GetPathInfo(path.c_str(), nullptr)) return false;
		const std::string by = marker_by(path);
		if (by == settings::BUILD) { on_cpu_because(tr::CPU_WHY_CLOSED, tr::CPU_TODO_RETRY); return true; }
		fprintf(stderr, "the GPU: %s closed while starting it: this build tries it again\n", by.empty() ? "an older build" : by.c_str());
		return false;
	};
	// (one marker over the whole of the GPU's start: the renderer, the 2D's check, the 3D's device and its first test
	// picture, the Windows retries on the other driver too; taken away once Auto's first measure is done)
	bool marker_mine = false;
	auto leave_marker = [&](const std::string &path) {
		if (marker_mine) return;
		write_marker(path, settings::BUILD);   // (flushed to the disk: a crash that takes the machine down keeps it)
		marker_mine = true;
	};
#ifdef _WIN32
	// (Windows: a start on Vulkan, chosen first, that closed the game: Direct3D 12 first from now on and tried this start,
	// not the CPU. Display > Graphics driver says so and puts Vulkan first again. On Direct3D 12 too: the CPU, as anywhere.)
	if (cfg.renderer3d != 0 && !getenv("SDL_GPU_DRIVER") && cfg.graphics == 0 && gpu3d_prefer_vulkan() &&
		SDL_GetPathInfo(gpu_marker.c_str(), nullptr) && marker_by(gpu_marker) == settings::BUILD) {
		fprintf(stderr, "the GPU: the last start closed while starting it on Vulkan: Direct3D 12 first from now on (Display > Graphics driver)\n");
		cfg.graphics = 1;
		cfg.save(settings_path);   // (now: should Direct3D 12 close the game too, the next start knows)
		SDL_RemovePath(gpu_marker.c_str());
	}
#endif
	// (Windows: Vulkan first on any card, unless Graphics driver is Direct3D 12; SDL_GPU_DRIVER chooses over both)
	const bool vk_first = !getenv("SDL_GPU_DRIVER") && cfg.graphics == 0 && gpu3d_prefer_vulkan();
	bool ren_vk = false;   // (the gpu renderer started on Vulkan, chosen first)
	if (cfg.renderer3d != 0 && !getenv("SDL_RENDER_DRIVER") && !getenv("VF_NO_GPU_RENDERER") && !marker_crashed(gpu_marker)) {
		leave_marker(gpu_marker);
		// (Windows: Vulkan first, Direct3D 12 if it won't start, gpu3d_prefer_vulkan)
		if (vk_first) {
			SDL_SetHint(SDL_HINT_GPU_DRIVER, "vulkan");
			ren = SDL_CreateRenderer(win, "gpu");
			ren_vk = ren != nullptr;
			fprintf(stderr, "SDL's gpu renderer: Vulkan (Windows, first): %s\n", ren ? "started" : SDL_GetError());
			SDL_ResetHint(SDL_HINT_GPU_DRIVER);
		}
		if (!ren) ren = SDL_CreateRenderer(win, "gpu");
		if (!ren) fprintf(stderr, "SDL's gpu renderer: %s (SDL's usual renderer instead)\n", SDL_GetError());
	}
#endif
	if (!ren) ren = SDL_CreateRenderer(win, nullptr);
	if (!ren) return fail(tr::FAIL_GRAPHICS);
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
	// The GPU's picture against the CPU's, as vf2-recomp's check (its #3): judged by how wrong, not
	// how many. Colours (never the unused top byte) off by a little anywhere are a driver's rounding
	// (NVIDIA's on Direct3D 12 scales and blends a hair differently: vf1-recomp #16, an RTX 5080 at
	// 6x and 16:9 sent to the CPU by its first picture, then held to 60 a second); only more than 1%
	// of the picture off by more than 8 a channel is a GPU putting it together wrong.
	struct picture_diff { long long n = 0, big = 0; int most = 0; bool wrong = false; };
	auto compare = [](const std::vector<uint32_t> &g, const uint32_t *c, size_t px, bool loose) {
		picture_diff d;
		for (size_t i = 0; i < px; i++) {
			if (!((g[i] ^ c[i]) & 0xffffff)) continue;
			int off = 0;
			for (int k = 0; k < 24; k += 8) off = std::max(off, std::abs(int((g[i] >> k) & 0xff) - int((c[i] >> k) & 0xff)));
			if (loose && off <= 2) continue;   // (the 3D straight into the renderer's texture: a shadow over the 2D is the GPU's blend)
			d.n++;
			d.most = std::max(d.most, off);
			d.big += off > 8;
		}
		d.wrong = d.big > std::max(64LL, (long long)(px / 100));
		return d;
	};
	{
		const char *rname = SDL_GetRendererName(ren);
		std::string why;
		if (getenv("VF_CPU_2D")) why = "VF_CPU_2D";
		else if (!gc.init(ren)) why = "this renderer can't";
		else if (gc.self_check(why)) gpu_2d = true;
#if VF_GPU3D_SDL
		// (on Vulkan, chosen first, it can't: the gpu renderer on Direct3D 12 tried, as before Vulkan came first)
		if (!gpu_2d && ren_vk && rname && !strcmp(rname, "gpu") && !getenv("VF_CPU_2D")) {
			fprintf(stderr, "SDL's gpu renderer on Vulkan can't put pictures together exactly (%s): trying it on Direct3D 12\n", why.c_str());
			gc.reset();
			SDL_DestroyRenderer(ren);
			SDL_SetHint(SDL_HINT_GPU_DRIVER, "direct3d12");
			ren = SDL_CreateRenderer(win, "gpu");
			SDL_ResetHint(SDL_HINT_GPU_DRIVER);
			if (!ren) ren = SDL_CreateRenderer(win, nullptr);
			if (!ren) return fail(tr::FAIL_GRAPHICS);
			SDL_SetRenderVSync(ren, 1);
			rname = SDL_GetRendererName(ren);
			why.clear();
			if (!gc.init(ren)) why = "this renderer can't";
			else if (gc.self_check(why)) gpu_2d = true;
		}
		if (!gpu_2d && rname && !strcmp(rname, "gpu") && !getenv("VF_CPU_2D")) {   // (SDL's usual one, which could)
			fprintf(stderr, "SDL's gpu renderer can't put pictures together exactly (%s): SDL's usual renderer instead\n", why.c_str());
			gc.reset();
			SDL_DestroyRenderer(ren);
			ren = SDL_CreateRenderer(win, nullptr);
			if (!ren) return fail(tr::FAIL_GRAPHICS);
			SDL_SetRenderVSync(ren, 1);
			rname = SDL_GetRendererName(ren);
			why.clear();
			if (!gc.init(ren)) why = "this renderer can't";
			else if (gc.self_check(why)) gpu_2d = true;
		}
#endif
		if (gpu_2d) fprintf(stderr, "pictures put together on the GPU (renderer: %s)\n", rname ? rname : "?");
		else if (cfg.renderer3d != 0 && !getenv("VF_CPU_2D")) on_cpu_because(tr::CPU_WHY_COMPOSE, UPDATE);   // (the 3D on the CPU by choice: SDL's usual renderer, which may not)
		// Low's edges smoothed there too (SDL's gpu renderer): the 3D then goes straight into its
		// texture with Low as without, nothing back to the CPU (vf1-recomp #16: an RTX 5080 at 6x,
		// Low, its 3D read back each picture for the CPU's smoothing, held at 60)
		if (!gpu_2d) fprintf(stderr, "pictures drawn on the CPU (renderer: %s; %s)\n", rname ? rname : "?", why.c_str());
		else if (gc.init_edge()) { m.set_gpu_edge(true); fprintf(stderr, "Low antialiasing's edges smoothed on the GPU\n"); }
	}
	// The 3D on the GPU (src/gpu/gpu3d.h, Display > 3D renderer): on the device SDL's Metal renderer
	// draws with; off the Mac SDL_GPU's (Direct3D 12 or Vulkan): the device of SDL's "gpu" renderer
	// when that's the renderer (its pictures straight into the renderer's textures), else a device of
	// its own (each picture's 3D back to the CPU); made only when the setting isn't CPU (VF_3D=cpu|gpu|auto
	// over it), its pictures checked against the CPU's before they're trusted. Any other renderer, or
	// no GPU: the CPU's. (Tests: VF_G3_OWN_DEVICE=1 a device of its own anyway, VF_G3_NO_TARGET=1 the
	// pictures back to the CPU anyway, VF_G3_NO_VERIFY=1 none checked.)
	std::unique_ptr<gpu3d> g3;
	bool g3_shared = false;       // (SDL_GPU: on the renderer's own device, drawing into its textures)
	bool g3_next_start = false;   // (SDL_GPU: none made, the setting CPU as the game started)
	bool g3_crashed = false;      // (SDL_GPU: none made, the last start closed while starting it; a choice in the menu tries again)
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
		const bool crashed = cfg.renderer3d != 0 && !marker_mine && marker_crashed(marker);   // (marker_mine: this start's own)
		if (crashed) {
			fprintf(stderr, "the 3D on the CPU: the last start closed while starting the GPU's (%s; a new build tries it again)\n", marker.c_str());
			g3_next_start = g3_crashed = true;   // (the menu's choice: from the next start, the marker gone)
		}
		if (cfg.renderer3d != 0 && !crashed) {
			const uint64_t t0 = SDL_GetTicksNS();
			leave_marker(marker);
			{
				const char *rn = SDL_GetRendererName(ren);
				if (gpu_2d && rn && !strcmp(rn, "gpu") && !getenv("VF_G3_OWN_DEVICE")) g3_shared = (g3 = gpu3d_create_sdlgpu_on(SDL_GetGPURendererDevice(ren))) != nullptr;
			}
			if (!g3 && vk_first) g3 = gpu3d_create_sdlgpu("vulkan");
			if (!g3) g3 = gpu3d_create_sdlgpu();
			fprintf(stderr, "the 3D on the GPU: %s (%.0f ms to start)\n", g3 ? g3->name() : "none here (the CPU's 3D)", (SDL_GetTicksNS() - t0) / 1e6);
			if (!g3) on_cpu_because(tr::CPU_WHY_3D_NOT_HERE, UPDATE);
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
		m.set_gpu3d_target(g3_shared && !getenv("VF_G3_NO_TARGET"));   // (tests: the pictures back to the CPU anyway)
		if (g3_shared) fprintf(stderr, "the 3D on the GPU: on the renderer's device, straight into its textures\n");
		if (const char *c = getenv("VF_GPU3D_CHECK")) m.m_gpu3d_check = std::max(0, atoi(c));
#if VF_GPU3D_SDL
		m.m_gpu3d_verify = getenv("VF_G3_NO_VERIFY") ? 0 : 3;   // (the game's pictures: three checked against the CPU's)
#endif
	}
	// Auto: the GPU whenever its test picture passes (the CPU only after a GPU error, for the run).
	struct auto3d_state {
		bool gpu = false;              // the GPU in use (Auto's choice, or GPU)
		bool broken = false;           // a GPU error: the CPU for the run
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
	// The 3D gone to the CPU in play (a GPU error, or its picture not the CPU's: as VF3's issue #26, 0.5 pictures a
	// second at 3x with 2x antialiasing there): drawn at 1x without antialiasing for this run (cpu_cut, at the pass's
	// end, said with the notice); the settings saved stay the player's (a scale or antialiasing chosen puts them
	// back). Drawing only: the game's the same
	bool cpu_cut = false, cpu_cut_due = false;
	auto apply_3d = [&] { m.set_gpu3d_on(g3 && !a3.broken && a3.gpu); m.m_gpu3d_chosen = cfg.renderer3d == 1; };
	auto renderer_name = [&]() -> tr::line {   // (.str() the menu's, .en() the log's)
		const tr::text setting = cfg.renderer3d == 2 ? tr::AUTO : tr::R3D_GPU;
#if VF_GPU3D_METAL
		if (!g3 || a3.broken) return tr::D_R3D_NO_GPU;
#elif VF_GPU3D_SDL
		if (!g3 && g3_crashed) return tr::D_R3D_CRASHED;
		if (!g3 && g3_next_start && !cfg.renderer3d) return tr::R3D_CPU;   // (none made, the setting CPU: there may well be a GPU)
		if (!g3 && g3_next_start && cfg.renderer3d) return tr::line(tr::D_R3D_NEXT_START, { { "what", setting } });
		// (the setting first, then why it's the CPU now: the GPU won't start here, or it failed in this run)
		if (!cfg.renderer3d && (!g3 || a3.broken)) return tr::R3D_CPU;
		if (!g3) return tr::line(tr::D_R3D_WONT_START, { { "what", setting } });
		if (a3.broken) return tr::line(tr::D_R3D_THIS_RUN, { { "what", setting } });
#else
		if (!g3 || a3.broken) return tr::D_R3D_NO_GPU_BUILD;
#endif
		if (cfg.renderer3d == 2) return tr::line(tr::AUTO_IS, { { "what", a3.gpu ? tr::R3D_GPU : tr::R3D_CPU } });
		return cfg.renderer3d == 1 ? tr::R3D_GPU : tr::R3D_CPU;
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
				// (and Vulkan, chosen first on Windows, failing: Direct3D 12's tried)
				const bool d3d = !strncmp(g3->name(), "direct3d12", 10), vk = vk_first && !strncmp(g3->name(), "vulkan", 6);
				if (attempt == 0 && (d3d || vk) && !getenv("SDL_GPU_DRIVER")) {
					m.set_gpu3d(nullptr);
					m.set_gpu3d_target(false);
					g3_shared = false;
					g3.reset();
					g3 = gpu3d_create_sdlgpu(d3d ? "vulkan" : "direct3d12");
					fprintf(stderr, "the 3D on the GPU: trying %s instead: %s\n", d3d ? "Vulkan" : "Direct3D 12", g3 ? g3->name() : "none here");
					if (g3) { m.set_gpu3d(g3.get()); a3.tested = false; continue; }
					a3.broken = true; a3.gpu = false; apply_3d();   // (g3 gone: the CPU's)
					on_cpu_because(d > 0 ? tr::CPU_WHY_WRONG : tr::CPU_WHY_TEST_FAILED, UPDATE);
					return;
				}
				a3.broken = true; a3.gpu = false; apply_3d();
				fprintf(stderr, "the 3D on the GPU: the CPU for this run\n");
				on_cpu_because(d > 0 ? tr::CPU_WHY_WRONG : tr::CPU_WHY_TEST_FAILED, UPDATE);
				return;
			}
			if (d > 0) fprintf(stderr, "the 3D on the GPU (%s): its test picture: %lld of %lld pixels differ by rounding (%lld drawn on one only, %lld by more than %d, colours by %d at most): keeping the GPU\n",
							   g3->name(), d, px, dd.cover, dd.big, model1::GPU_ROUNDING_COLOUR, dd.most);
			else fprintf(stderr, "the 3D on the GPU (%s): its test picture is the CPU's, pixel for pixel\n", g3->name());
			// (a test picture that failed on the first driver counted a failure: the one that passed starts clean,
			// else auto_tick took it for a failure in play at the first picture and threw a working GPU away)
			a3.failed = m.m_g3.failed;
		}
#endif
		if (cfg.renderer3d != 2) {
			a3.gpu = cfg.renderer3d == 1; apply_3d();
			fprintf(stderr, "3D renderer: the player's setting (%s): the %s\n", why, a3.gpu ? "GPU" : "CPU");
			return;
		}
		// Auto: the GPU, whenever it works (its test picture passed; any error later, the CPU for the
		// run). Measuring the two at the start and every 10 s chose the CPU on fast processors at low
		// scales (the GPU's measure waited for its picture to come back: an overestimate), and each
		// check drew four of the game's pictures on the CPU at the full size: a hitch at high scales
		// (a GPU drawn on the CPU, as in a virtual machine or a container: several times slower than the
		// CPU's own drawing, the CPU; as VF2's Auto)
		a3.gpu = !g3->software();
		auto_remember(a3.gpu);
		fprintf(stderr, "3D renderer: Auto (%s) at %dx, antialiasing %d: the %s\n", why, cfg.scale, cfg.aa, a3.gpu ? "GPU" : "CPU (its GPU is drawn on the CPU)");
		if (!a3.gpu) on_cpu_because(tr::CPU_WHY_NO_DRIVER, tr::CPU_TODO_INSTALL);
		apply_3d();
	};
	// after each picture: its time; Auto's checks
	auto auto_tick = [&] {
		if (!g3) return;
		const model1::gpu3d_stats &st = m.m_g3;
		if (st.failed != a3.failed) {
			a3.failed = st.failed;
			if (!a3.broken) {
				fprintf(stderr, "3D renderer: the GPU failed: the CPU for this run\n");
				if (st.wrong) on_cpu_because(tr::CPU_WHY_WRONG, UPDATE);   // (a game's picture checked: not the CPU's)
				else on_cpu_because(tr::CPU_WHY_STOPPED, NEXT);
				cpu_cut_due = true;
			}
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
				fprintf(stderr, "the 3D: %s", renderer_name().en().c_str());
				for (int k = 1; k >= 0; k--) if (n[k]) fprintf(stderr, ", the %s %.2f ms a picture (%d)", k ? "GPU" : "CPU", sum[k] / n[k], n[k]);
				if (n[1]) fprintf(stderr, " [GPU: recording %.2f ms, on the GPU %.2f ms]", rsum / n[1], gsum / n[1]);
				{
					static uint64_t handed0 = 0;
					if (m.m_g3.handed > handed0) fprintf(stderr, ", %llu straight into the renderer's texture (not waited for)", (unsigned long long)(m.m_g3.handed - handed0));
					handed0 = m.m_g3.handed;
				}
				if (m.m_g3.checked) fprintf(stderr, ", checked %llu, %llu differ", (unsigned long long)m.m_g3.checked, (unsigned long long)m.m_g3.differ);
				fprintf(stderr, "\n");
				since = SDL_GetTicksNS(); sum[0] = sum[1] = gsum = rsum = 0; n[0] = n[1] = 0;
			}
		}
		// (Auto doesn't check the CPU against the GPU as it plays: the GPU, see auto_measure)
	};
	auto_measure("the start");
#if VF_GPU3D_SDL
	if (marker_mine) SDL_RemovePath(gpu_marker.c_str());   // (the GPU's start done, its first test picture too)
#endif
	std::set<int> gpu_failed;           // render scales the GPU couldn't put together (a part too large for it)
	// the pictures put together on the GPU (with antialiasing too: gpu_compose::resolve averages them
	// there, at_gpu_aa), or drawn whole by the CPU
	auto on_gpu = [&] { return gpu_2d && !gpu_failed.count(m.render_scale()); };
	int gpu_recheck = 0;                // a check that failed: the next pictures checked too (up to 2)
	bool gpu_dumped = false;            // a failed check's pictures written beside the log
	int gpu_verified = 0;               // the game's own pictures checked so far (the 1st, 60th and 600th)
	model1::gpu_parts parts;
	// The 3D's textures on the renderer's device (gpu3d::draw_target's), wrapped as the renderer's
	// own, made before a picture that needs them: the picture's size, again when it changes. Any
	// failure: the 3D comes back to the CPU as before, from then on.
	SDL_Texture *g3_tex[model1::G3_TARGETS] = {};
	uint64_t g3_wrapped = 0;
	int g3_tw = 0, g3_th = 0;
	auto g3_unwrap = [&] {
		gc.set_3d_targets(g3_tex, 0);
		for (SDL_Texture *&t : g3_tex) if (t) { SDL_DestroyTexture(t); t = nullptr; }
		g3_wrapped = 0; g3_tw = g3_th = 0;
	};
	auto g3_prepare = [&] {
		if (!g3 || !m.gpu3d_target()) return;
		const int w = m.frame_w(), h = m.frame_h();
		if (g3_tex[0] && g3_tw == w && g3_th == h && g3_wrapped == g3->targets_made()) return;
		g3_unwrap();
		bool ok = g3->make_targets(w, h, model1::G3_TARGETS);
		for (int i = 0; ok && i < model1::G3_TARGETS; i++) {
			SDL_PropertiesID pr = SDL_CreateProperties();
			SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);   // (RGBA8 in memory)
			SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
			SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
			SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
			SDL_SetPointerProperty(pr, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, g3->target_handle(i));
			g3_tex[i] = SDL_CreateTextureWithProperties(ren, pr);
			SDL_DestroyProperties(pr);
			ok = g3_tex[i] != nullptr;
		}
		if (ok) ok = gc.set_3d_targets(g3_tex, model1::G3_TARGETS);
		if (!ok) {
			fprintf(stderr, "the 3D on the GPU: no %dx%d textures on the renderer for it (%s): its pictures come back to the CPU from now on\n", w, h, SDL_GetError());
			g3_unwrap();
			m.set_gpu3d_target(false);
			return;
		}
		g3_wrapped = g3->targets_made(); g3_tw = w; g3_th = h;
	};
	bool show_target = false;           // the picture on show is the GPU's (else tex)
	const int gpu_check = getenv("VF_GPU_CHECK") ? atoi(getenv("VF_GPU_CHECK")) : 0;   // testing: every nth checked
	int gpu_pictures = 0, gpu_checked = 0, gpu_differ = 0;
	int gpu_check_from = 0;             // (the GPU's pictures counted from here for the checks: a new scale or antialiasing checks again)
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
	// the picture's shape: the chosen one, or the window's (between 4:3 and 80:9)
	auto apply_aspect = [&] {
		float a = ASPECTS[aspect_i];
		if (a <= 0) {
			int w = 0, h = 0;
			SDL_GetWindowSizeInPixels(win, &w, &h);
			a = h > 0 ? std::clamp(float(w) / float(h), 4.0f / 3.0f, model1::MAX_ASPECT) : 16.0f / 9.0f;
		}
		if (std::fabs(a - m.aspect()) > 0.001f) {
			m.set_aspect(a);
			// (past 32:9 antialiasing may fit less, or again more, at the new width)
			const int rs = cpu_cut ? 1 : cfg.scale * aa_fit(cfg.scale, cfg.aa);
			if (rs != m.render_scale()) m.set_render_scale(rs);
		}
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
	SDL_Texture *glass = nullptr;
	int glass_w = 0, glass_h = 0, glass_kind = 0;
	// Online play's flags on the HUD, beside each player's name (model1::set_hud_names leaves two cells for one): the
	// countries' flags (assets/ui/flags.png, 48 x 36 each, as they are) drawn over the game's picture into those
	// cells, 12 x 9 of the arcade's pixels in the middle of their 16 x 16, its edges on the window's own pixels (sharp
	// at any size), after the picture and before the screen filter
	SDL_Surface *flag_atlas = nullptr;
	SDL_Texture *flag_tex[2] = { nullptr, nullptr };
	std::string hud_country[2], flag_made[2];   // (each player's, as the lobby has it: "" none; and the one each texture holds)
	auto hud_flag = [&](int p) -> SDL_Texture * {
		const int i = flags::index_of(hud_country[p]);
		if (i < 0) return nullptr;
		if (flag_tex[p] && flag_made[p] == hud_country[p]) return flag_tex[p];
		if (!flag_atlas)
			if (SDL_Surface *f = SDL_LoadSurface((find_assets() + "/ui/flags.png").c_str())) {
				flag_atlas = SDL_ConvertSurface(f, SDL_PIXELFORMAT_ABGR8888);
				SDL_DestroySurface(f);
			}
		if (!flag_atlas) return nullptr;
		const int x0 = i % flags::COLS * flags::CELL_W + 1, y0 = i / flags::COLS * flags::CELL_H + 1;
		if (x0 + flags::W > flag_atlas->w || y0 + flags::H > flag_atlas->h) return nullptr;
		if (flag_tex[p]) SDL_DestroyTexture(flag_tex[p]);
		flag_tex[p] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, flags::W, flags::H);
		if (!flag_tex[p]) return nullptr;
		SDL_UpdateTexture(flag_tex[p], nullptr, static_cast<const uint8_t *>(flag_atlas->pixels) + size_t(y0) * size_t(flag_atlas->pitch) + size_t(x0) * 4, flag_atlas->pitch);
		SDL_SetTextureScaleMode(flag_tex[p], SDL_SCALEMODE_LINEAR);
		SDL_SetTextureBlendMode(flag_tex[p], SDL_BLENDMODE_BLEND);
		flag_made[p] = hud_country[p];
		return flag_tex[p];
	};
	auto draw_hud_flags = [&] {
		if (hud_country[0].empty() && hud_country[1].empty()) return;
		int lw = 0, lh = 0;
		SDL_RendererLogicalPresentation mode;
		SDL_FRect out;
		SDL_GetRenderLogicalPresentation(ren, &lw, &lh, &mode);
		if (lw <= 0 || lh <= 0 || !SDL_GetRenderLogicalPresentationRect(ren, &out) || out.w <= 0) return;
		// (the arcade's pixels to the logical size's, and the logical size's to the window's: each edge put on a whole
		// window pixel, so the flag's never blurred into the box around it)
		const float sx = float(lw) / float(m.arcade_width()), sy = float(lh) / float(model1::SCREEN_H), wx = out.w / float(lw), wy = out.h / float(lh);
		auto snap_x = [&](float v) { return (std::round(out.x + v * sx * wx) - out.x) / wx; };
		auto snap_y = [&](float v) { return (std::round(out.y + v * sy * wy) - out.y) / wy; };
		for (int p = 0; p < 2; p++) {
			int x = 0, y = 0;
			if (!m.hud_flag_at(p, x, y)) continue;
			if (SDL_Texture *t = hud_flag(p)) {
				const float l = snap_x(float(x) + 2), r = snap_x(float(x) + 14), top = snap_y(float(y) + 3.5f), bottom = snap_y(float(y) + 12.5f);   // (its two cells, 16 x 16: 12 x 9 in their middle)
				const SDL_FRect dst{ l, top, r - l, bottom - top };
				SDL_RenderTexture(ren, t, nullptr, &dst);
			}
		}
	};
	// CRT colours, brightness and gamma (crt_colours.h): the game's picture drawn through them as it goes to the window
	crt::Colours crt_paint;
	bool crt_on = false;   // (any of them set: the menu says when this renderer can't)
	auto draw_filter = [&] {
		SDL_FRect pic;
		if (!cfg.filter || !SDL_GetRenderLogicalPresentationRect(ren, &pic)) return;
		// on whole pixels, one of the glass's to each (as VF3's, vf3-recomp #7): the picture's edges can fall between
		// them (a borderless fullscreen's 1920 x 1081, 4:3: 1441.33 wide from 239.33), and the glass stretched by a
		// fraction, sampled to the nearest, drew one column twice in the middle (a line down the screen)
		const float x0 = std::round(pic.x), y0 = std::round(pic.y);
		const SDL_FRect r{ x0, y0, std::round(pic.x + pic.w) - x0, std::round(pic.y + pic.h) - y0 };
		const int w = int(r.w), h = int(r.h);
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
		// over the picture, under the menu: in the window's own pixels (the logical presentation off for it)
		int lw = 0, lh = 0;
		SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
		SDL_GetRenderLogicalPresentation(ren, &lw, &lh, &mode);
		SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
		SDL_RenderTexture(ren, glass, nullptr, &r);
		SDL_SetRenderLogicalPresentation(ren, lw, lh, mode);
	};

	SDL_AudioSpec spec{ SDL_AUDIO_S16, 2, m1audio::OUT_RATE };
	SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	SDL_AudioStream *chime_stream = nullptr;   // (the match found chime's: opened the first time)
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
		give_unknown_sticks();   // (a joystick that came without its own event)
		int n = 0;
		SDL_JoystickID *ids = SDL_GetGamepads(&n);
		std::vector<bool> used(n, false);
		// first the pads chosen by name, then "auto" players take the free ones in order, then a
		// player whose chosen pad isn't there (or shows up differently: a pad in another mode
		// has another GUID) takes the next free one too. The free ones: the pads SDL knows first, then the joysticks
		// given a stick's layout here (one that isn't a pad at all, as a keyboard's lights can be, doesn't take
		// player 1 from a real pad)
		for (int pass = 0; pass < 3; pass++)
			for (int p = 0; p < 2; p++) {
				const std::string &c = cfg.ctl[p].pad_choice;
				if (pads[p] || c == "none") continue;
				const bool chosen = c != "auto";
				if ((pass == 0 && !chosen) || (pass == 1 && chosen)) continue;
				for (int stick = 0; stick < (pass == 0 ? 1 : 2) && !pads[p]; stick++)
					for (int i = 0; i < n && !pads[p]; i++)
						if (!used[i] && (pass == 0 ? guid_of(ids[i]) == c : g_stick_guids.count(guid_of(ids[i])) == size_t(stick))) {
							pads[p] = SDL_OpenGamepad(ids[i]);
							used[i] = true;
						}
			}
		SDL_free(ids);
		if (pads_swapped && pads[0] && pads[1]) std::swap(pads[0], pads[1]);
		for (int p = 0; p < 2; p++)
			if (pads[p]) fprintf(stderr, "player %d: %s\n", p + 1, SDL_GetGamepadName(pads[p]));
		if (!pads[0] && !pads[1]) {   // (said, for a log from a player whose pad does nothing)
			int nj = 0;
			SDL_free(SDL_GetJoysticks(&nj));
			fprintf(stderr, "pads: none (%d gamepads, %d joysticks seen)\n", n, nj);
		}
	};
	open_pads();

	bool running = true, paused = false, show_frame = true;
	pad_state pstate[2];
	// F9: timings in the title bar (and on stderr) every 120 frames; a frame has 17.4 ms
	struct perf_stats { bool show = false; int frames = 0, starved = 0, held = 0, long_frame = 0, long_picture = 0, long_other = 0, ahead = 0; uint64_t emu = 0, emu_max = 0, present = 0, work = 0, shown = 0, since = SDL_GetTicksNS(); };
	perf_stats perf{ cfg.timings || getenv("VF_PERF") != nullptr };
	// No load governor: the render scale and antialiasing are the player's, always, and a picture is
	// made for every refresh of the screen, whatever its rate (it lowered them, or held 60 a second,
	// on windows it judged short: false alarms on fast machines kept them lowered, vf1-recomp #16 #18)
	uint64_t started_at = 0;              // (the first picture: the log's start)
	constexpr uint64_t smooth_gap_ns = 8333333;   // (a screen's rate unknown, or hidden in tests: a picture each 1/120 s at most)
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
				aspect_i ? ASPECT_NAMES[aspect_i] : "fit", renderer_name().en().c_str(), on_gpu() ? "GPU" : "CPU",
				rn ? rn : "?", "each refresh", "none", vsync, hz, pw, ph);
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
	std::optional<std::array<int, 12>> assign_before;
	auto reset_game = [&](bool keep_coins = true) {
		// (never on a match's board: the host's backup booted, the same on both sides; end_match gives this player's
		// back first)
		if (g_match_board) { fprintf(stderr, "(no restart: an online match's board)\n"); return; }
		assign_before.reset();   // a restart applies them
		if (keep_coins) keep_arcade_coins();     // (Arcade: as the service menu may have left them)
		menu.close();
		paused = false;
		// Cheats' fighters, as they are now (Random fighter is applied here, not as the menu changes it)
		apply_players();
		// the cheats' code patches, as they are now (every restart: the menu's Reset game and Play style
		// too, not only a cheat changed); the game's code then new: recompiled before the reset
		bool code = m.set_no_time_limit(cfg.no_time_limit);
		code |= m.set_strands_moved(cfg.random_fighter > 0);
#ifdef VF_NATIVE
		if (code && !interp && !recompile()) {
			// (the old native code would run on the new patches: the interpreter from now on, as at a start)
			fprintf(stderr, "Couldn't recompile the game after the cheats' patches: running it on the slower interpreter instead.\n");
			rec::detach(m.cpu());
			interp = true;
			m.run_cpu = nullptr;
		}
#else
		(void)code;
#endif
		m.set_random_fighter(cfg.random_fighter > 0, cfg.random_fighter > 0, cfg.random_cpu);   // (first: see above)
		m.set_akira_slot(akira_now());
		load_secret_icon(m, akira_now(), icon_dirs());
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);
		SDL_RenderPresent(ren);
		boot();
		if (const bool coins = settle_coins(), language = settle_language(); coins || language) boot();   // (the service menu may have set coins)
		make_texture();
		show_frame = false;
	};
	auto save_settings = [&] { return cfg.save(settings_path); };
	auto set_scale = [&](int v) {
		// (a new picture size or antialiasing: the GPU's pictures checked against the CPU's again, as at the
		// start. Low's edge pass on the GPU, chosen after the start's checks, was never checked: on some
		// NVIDIA cards, Direct3D 12, it left the 3D frozen or gone (vf1-recomp #29); now that's caught and
		// the CPU draws instead)
		gpu_check_from = gpu_pictures;
		gpu_verified = 0;
		gpu_recheck = 0;
		cpu_cut = false;   // (the player's own: the 3D's CPU cut lifted)
		cfg.scale = v; cfg_dirty = true;
		m.set_render_scale(v * aa_fit(v, cfg.aa));
		m.set_edge_aa(cfg.aa == 0);
		m.set_smooth(true);
		sync.reset();                          // a new picture size: show the next frame as it is
		auto_measure("a new scale or antialiasing");
		make_texture(); present_shape(); redraw_if_stopped();
		log_state("the player's scale or antialiasing");
	};
	auto player_scale_to = [&](int v) { set_scale(v); };
	// Display > Window size: the window k times the arcade's 384 lines high, in the window's own pixels (a Mac's
	// Retina window is sized in points: the pixel density's share of them), its width the picture shape's at that
	// height (4:3 and Fit: 512 x k, the 496 x 384 picture on its 4:3 tube; wider shapes the picture's own width, 384
	// times theirs, times k), so the picture fills it and each arcade line is k of the screen's; only the sizes that
	// fit the screen's usable area. In fullscreen it waits for the window. Resized by hand: Free again.
	int win_expect_w = 0, win_expect_h = 0;   // (the window's pixels as Window size made them)
	int win_set_w = 0, win_set_h = 0;         // (and its points)
	bool window_moved = false;                // (moved, resized, maximised: remembered after the pass's events)
	bool window_rescale = false;              // (made again after this pass's events: out of the OS's fullscreen, or
	                                          // moved to a screen of another pixel density)
	auto window_target = [&](int k, int &w, int &h) {
		h = 384 * k;
		w = ASPECTS[aspect_i] <= 0 ? 512 * k : int(std::lround(384 * ASPECTS[aspect_i])) * k;
	};
	auto window_fits = [&](int k) {
		int w, h;
		window_target(k, w, h);
		const float d = std::max(SDL_GetWindowPixelDensity(win), 0.25f);
		SDL_Rect usable;
		if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &usable) || usable.w <= 0 || usable.h <= 0) return k == 1;
		int top = 0, left = 0, bottom = 0, right = 0;
		if (!SDL_GetWindowBordersSize(win, &top, &left, &bottom, &right)) top = left = bottom = right = 0;
		return w / d + left + right <= usable.w && h / d + top + bottom <= usable.h;
	};
	auto window_scale_most = [&] {
		int k = 0;
		while (k < 16 && window_fits(k + 1)) k++;
		return k;
	};
	auto apply_window_scale = [&] {
		if (cfg.window_scale <= 0 || is_fullscreen() || fixed_window) return;
		const int most = window_scale_most();
		if (cfg.window_scale > most) {
			fprintf(stderr, "window size %dx: larger than the screen, %s\n", cfg.window_scale, most ? "the largest that fits" : "Free");
			cfg.window_scale = most;
			cfg_dirty = true;
			if (!most) return;
		}
		int w, h;
		window_target(cfg.window_scale, w, h);
		if (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) { SDL_RestoreWindow(win); SDL_SyncWindow(win); }
		const float d = std::max(SDL_GetWindowPixelDensity(win), 0.25f);
		int pw = int(std::lround(w / d)), ph = int(std::lround(h / d));
		SDL_SetWindowSize(win, pw, ph);
		SDL_SyncWindow(win);
		int gw = 0, gh = 0;
		SDL_GetWindowSizeInPixels(win, &gw, &gh);
		if ((gw != w || gh != h) && gw > 0 && gh > 0) {   // (a fractional density's rounding: a point more or less)
			pw += gw < w ? 1 : gw > w ? -1 : 0;
			ph += gh < h ? 1 : gh > h ? -1 : 0;
			SDL_SetWindowSize(win, pw, ph);
			SDL_SyncWindow(win);
			SDL_GetWindowSizeInPixels(win, &gw, &gh);
		}
		win_expect_w = gw; win_expect_h = gh;
		win_set_w = pw; win_set_h = ph;
		SDL_FlushEvent(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);   // (its own sizes on the way: not the player's resize)
		fprintf(stderr, "window size %dx: %d x %d pixels (%d x %d points, density %.2f)%s\n", cfg.window_scale, gw, gh, pw, ph, d,
		        gw == w && gh == h ? "" : ": not the size asked");
		if (ASPECTS[aspect_i] <= 0) {   // (Fit: the picture's shape the window's new one)
			const float before = m.aspect();
			apply_aspect();
			if (m.aspect() != before) { sync.reset(); make_texture(); present_shape(); redraw_if_stopped(); }
		}
	};
	// the window's place and size kept in the settings as the player leaves them, in a window only (not fullscreen;
	// maximised: said so, the size under it kept); not for VF_WINDOW's tests
	auto remember_window = [&] {
		if (fixed_window || is_fullscreen()) return;
		const SDL_WindowFlags fl = SDL_GetWindowFlags(win);
		if (fl & SDL_WINDOW_MINIMIZED) return;
		const bool mx = (fl & SDL_WINDOW_MAXIMIZED) != 0;
		int w = cfg.window_w, h = cfg.window_h, x = cfg.window_x, y = cfg.window_y;
		if (!mx || w <= 0) { SDL_GetWindowSize(win, &w, &h); SDL_GetWindowPosition(win, &x, &y); }   // (maximised from the first: its size then)
		if (w <= 0 || h <= 0) return;
		if (w == cfg.window_w && h == cfg.window_h && x == cfg.window_x && y == cfg.window_y && mx == cfg.window_max) return;
		cfg.window_w = w; cfg.window_h = h; cfg.window_x = x; cfg.window_y = y; cfg.window_max = mx;
		cfg_dirty = true;
	};
	auto set_window_scale = [&](int k) { cfg.window_scale = std::max(k, 0); cfg_dirty = true; apply_window_scale(); };
	auto set_shape = [&](int v) {
		aspect_i = cfg.aspect = v; cfg_dirty = true;
		sync.reset();
		apply_window_scale();   // (Window size: the new shape's width)
		apply_aspect(); make_texture(); present_shape(); redraw_if_stopped();
	};
	auto set_fullscreen = [&](bool on) {
		cfg.fullscreen = on; cfg_dirty = true;
		SDL_Rect b;
		if (on && !fs_win_on && fs_borderless() && SDL_GetDisplayBounds(SDL_GetDisplayForWindow(win), &b)) {
			if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) { SDL_SetWindowFullscreen(win, false); SDL_SyncWindow(win); }
			fs_win_max = (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) != 0;
			if (fs_win_max) { SDL_RestoreWindow(win); SDL_SyncWindow(win); }   // (the size under it, for the way back)
			SDL_GetWindowPosition(win, &fs_win_was.x, &fs_win_was.y);
			SDL_GetWindowSize(win, &fs_win_was.w, &fs_win_was.h);
			SDL_SetWindowBordered(win, false);
			SDL_SetWindowPosition(win, b.x, b.y);
			SDL_SetWindowSize(win, b.w, b.h + 1);
			fs_win_on = true;
			fprintf(stderr, "fullscreen: a borderless window, %d x %d%s\n", b.w, b.h + 1,
			        cfg.fs_window ? " (fullscreen_window)" : " (fullscreen_exclusive = 1 for the driver's own)");
		} else if (!on && fs_win_on) {
			SDL_SetWindowBordered(win, true);
			if (fs_win_was.w > 0) { SDL_SetWindowSize(win, fs_win_was.w, fs_win_was.h); SDL_SetWindowPosition(win, fs_win_was.x, fs_win_was.y); }
			if (fs_win_max) SDL_MaximizeWindow(win);
			fs_win_on = false;
		} else if (!fs_win_on) SDL_SetWindowFullscreen(win, on);
		SDL_SyncWindow(win);     // macOS animates into fullscreen: wait for it to finish
		if (!on) apply_window_scale();
	};
	// borderless fullscreen from the start: the window (made at its remembered place and size) over the screen
	if (cfg.fullscreen && fs_borderless()) { const bool d = cfg_dirty; set_fullscreen(true); cfg_dirty = d; }
	apply_window_scale();   // (the saved Window size from the start; VF_WINDOW's tests: the size they ask)
	auto on_off = [](bool b) { return std::string(b ? tr::ON : tr::OFF); };
	auto wrap = [](int v, int n) { return (v % n + n) % n; };
	auto back_item = [&] { return menu_item{ tr::BACK, tr::BACK_HELP, {}, {}, [&] { menu.pop(); } }; };
	// Game > Language: the setting, and the menus made again in the language at once (the page it's on kept)
	std::function<void()> menus_relabelled;   // (set below, with the pages)
	auto language_choice = [&] { tr::choice c; tr::choice_from(cfg.language, c); return c; };
	auto set_language = [&](tr::choice c) {
		cfg.language = tr::choice_key(c);
		cfg_dirty = true;
		tr::set(tr::resolve(c));
		fprintf(stderr, "Game > Language: %s (the menus in %s)\n", cfg.language.c_str(), tr::code(tr::current()));
		menu.set_language();
		if (menus_relabelled) menus_relabelled();
	};
	auto display_page = [&] {
		menu_page p{ tr::P_DISPLAY, "", {} };
		p.items.push_back({ tr::D_SCALE, tr::D_SCALE_HELP,
			[&] { return tr::fill(tr::N_X, { { "n", std::to_string(cfg.scale) } }); },
			[&](int d) { player_scale_to(std::clamp(cfg.scale + d, 1, 6)); } });
		p.items.push_back({ tr::D_AA, tr::D_AA_HELP,
			[&] { return cfg.aa == 0 ? std::string(tr::D_AA_LOW) : aa_now() == 1 ? std::string(cfg.aa > 1 ? tr::D_AA_OFF_TOO_LARGE : tr::OFF) : tr::fill(tr::N_X, { { "n", std::to_string(aa_now()) } }); },
			[&](int d) {
				static const int order[] = { 1, 0, 2, 3 };   // Off, Low, 2x, 3x
				int i = 0;
				while (i < 3 && order[i] != cfg.aa) i++;
				cfg.aa = order[std::clamp(i + d, 0, 3)];
				set_scale(cfg.scale);
			} });
		p.items.push_back({ tr::D_RENDERER, tr::D_RENDERER_HELP,
			[&] { return renderer_name().str(); },
			[&](int d) {
				SDL_RemovePath((dir + "/gpu-start.crashed").c_str());   // (the player's choice: a crashed start tried again from the next)
				const bool was_crashed = g3_crashed;
				g3_crashed = false;
				menu.toast("");   // (its advice followed: the notice taken down)
				// (SDL_GPU: from the next start. After a crashed start the first press only tries the GPU again, the
				// setting kept: one press on to CPU would have left it there)
				if (!g3 && g3_next_start) { if (!was_crashed) { cfg.renderer3d = wrap(cfg.renderer3d + d, 3); cfg_dirty = true; } return; }
				if (!g3) { cfg.renderer3d = wrap(cfg.renderer3d + d, 3); cfg_dirty = true; g3_next_start = true; return; }   // (none made: from the next start)
				cfg.renderer3d = wrap(cfg.renderer3d + d, 3); cfg_dirty = true;
				auto_measure("chosen");
				redraw_if_stopped();
				log_state("the player's 3D renderer");
			} });
#ifdef _WIN32
		p.items.push_back({ tr::D_DRIVER, tr::D_DRIVER_HELP,
			[&] { return std::string(cfg.graphics ? "Direct3D 12" : tr::D_DRIVER_AUTO.c_str()); },
			[&](int) {
				cfg.graphics = !cfg.graphics; cfg_dirty = true;
				SDL_RemovePath((dir + "/gpu-start.crashed").c_str());   // (another driver: a crashed start tried again with it)
				g3_crashed = false;
				menu.toast("");
			} });
#endif
		p.items.push_back({ tr::D_SHAPE, tr::D_SHAPE_HELP,
			[&] { return std::string(aspect_i ? ASPECT_NAMES[aspect_i] : tr::D_SHAPE_FIT.c_str()); },
			[&](int d) { set_shape(wrap(aspect_i + d, N_ASPECTS)); } });
		p.items.push_back({ tr::D_BG_FILTER, tr::D_BG_FILTER_HELP,
			[&] { return std::string(cfg.smooth_bg ? tr::D_SMOOTH : tr::OFF); },
			[&](int) { cfg.smooth_bg = !cfg.smooth_bg; cfg_dirty = true; m.set_sky_filter(cfg.smooth_bg); redraw_if_stopped(); } });
		p.items.push_back({ tr::D_SCREEN_FILTER, tr::D_SCREEN_FILTER_HELP,
			[&] { const tr::text names[] = { tr::OFF, tr::D_SCANLINES, tr::D_CRT }; return std::string(names[cfg.filter]); },
			[&](int d) { cfg.filter = wrap(cfg.filter + d, 3); cfg_dirty = true; } });
		p.items.push_back({ tr::D_CRT_COLOURS, tr::D_CRT_COLOURS_HELP,
			[&] {
				const tr::text names[] = { tr::OFF, tr::CRT_SEGA, tr::CRT_SONY, tr::CRT_NTSC_JP, tr::CRT_NTSC_US, tr::CRT_PAL };
				static_assert(sizeof names / sizeof names[0] == crt::N_PRESETS, "a name for each of crt::PRESETS");
				const std::string n = names[cfg.crt_colours];
				return crt_on && crt_paint.failed() ? tr::fill(tr::NOT_WITH_RENDERER, { { "what", n } }) : n;
			},
			[&](int d) { cfg.crt_colours = wrap(cfg.crt_colours + d, crt::N_PRESETS); cfg_dirty = true; } });
		p.items.push_back({ tr::D_BRIGHTNESS, tr::D_BRIGHTNESS_HELP,
			[&] { const std::string n = tr::fill(tr::N_PERCENT, { { "n", std::to_string(cfg.brightness) } }); return crt_on && crt_paint.failed() ? tr::fill(tr::NOT_WITH_RENDERER, { { "what", n } }) : n; },
			[&](int d) { cfg.brightness = std::clamp(cfg.brightness + 5 * d, crt::BRIGHT_MIN, crt::BRIGHT_MAX); cfg_dirty = true; } });
		p.items.push_back({ tr::D_GAMMA, tr::D_GAMMA_HELP,
			[&] { const std::string n = tr::fill(tr::N_PERCENT, { { "n", std::to_string(cfg.gamma) } }); return crt_on && crt_paint.failed() ? tr::fill(tr::NOT_WITH_RENDERER, { { "what", n } }) : n; },
			[&](int d) { cfg.gamma = std::clamp(cfg.gamma + 5 * d, crt::GAMMA_MIN, crt::GAMMA_MAX); cfg_dirty = true; } });
		p.items.push_back({ tr::D_SHOW_FPS, tr::D_SHOW_FPS_HELP,
			[&] { return on_off(cfg.show_fps); },
			[&](int) { cfg.show_fps = !cfg.show_fps; cfg_dirty = true; if (!cfg.show_fps) menu.set_fps(""); } });
		p.items.push_back({ tr::D_WINDOW_SIZE, tr::D_WINDOW_SIZE_HELP,
			[&] {
				if (cfg.window_scale <= 0) return std::string(tr::D_FREE);
				int w, h;
				window_target(cfg.window_scale, w, h);
				return tr::fill(is_fullscreen() ? tr::D_WINDOW_N_IN_WINDOW : tr::D_WINDOW_N, { { "n", std::to_string(cfg.window_scale) }, { "w", std::to_string(w) }, { "h", std::to_string(h) } });
			},
			[&](int d) { set_window_scale(wrap(cfg.window_scale + d, window_scale_most() + 1)); } });
		p.items.push_back({ tr::D_FULLSCREEN, tr::D_FULLSCREEN_HELP,
			[&] { return on_off(is_fullscreen()); },
			[&](int) { set_fullscreen(!is_fullscreen()); } });
#ifdef _WIN32
		p.items.push_back({ tr::D_FS_MODE, tr::D_FS_MODE_HELP,
			[&] { return std::string(fs_borderless() ? tr::D_BORDERLESS : tr::D_STANDARD); },
			[&](int) {
				const bool was = is_fullscreen();
				if (was) set_fullscreen(false);
				cfg.fs_exclusive = fs_borderless(); cfg_dirty = true;
				if (was) set_fullscreen(true);
				menu.refresh();
			} });
#endif
		p.items.push_back(back_item());
		return p;
	};
	auto sound_page = [&] {
		menu_page p{ tr::P_SOUND, "", {} };
		p.items.push_back({ tr::S_VOLUME, tr::S_VOLUME_HELP,
			[&] { return tr::fill(tr::N_PERCENT, { { "n", std::to_string(cfg.volume) } }); },
			[&](int d) {
				cfg.volume = std::clamp(cfg.volume + 10 * d, 0, 100); cfg_dirty = true;
				if (audio && !getenv("VF_HIDDEN")) SDL_SetAudioStreamGain(audio, cfg.volume / 100.0f);
			} });
		p.items.push_back({ tr::S_MUSIC, tr::S_MUSIC_HELP,
			[&] { return tr::fill(tr::N_PERCENT, { { "n", std::to_string(cfg.music) } }); },
			[&](int d) {
				cfg.music = std::clamp(cfg.music + 10 * d, 0, 100); cfg_dirty = true;
				m.set_volumes(cfg.music / 100.0f, cfg.effects / 100.0f);
				g_match_volumes = pack_volumes(cfg.music, cfg.effects);
			} });
		p.items.push_back({ tr::S_EFFECTS, tr::S_EFFECTS_HELP,
			[&] { return tr::fill(tr::N_PERCENT, { { "n", std::to_string(cfg.effects) } }); },
			[&](int d) {
				cfg.effects = std::clamp(cfg.effects + 10 * d, 0, 100); cfg_dirty = true;
				m.set_volumes(cfg.music / 100.0f, cfg.effects / 100.0f);
				g_match_volumes = pack_volumes(cfg.music, cfg.effects);
			} });
		p.items.push_back(back_item());
		return p;
	};
	auto game_page = [&] {
		menu_page p{ tr::P_GAME, "", {} };
		p.items.push_back({ tr::G_LANGUAGE, tr::G_LANGUAGE_HELP,
			[&] {
				const tr::choice c = language_choice();
				return c.automatic() ? tr::fill(tr::LANG_AUTO_IS, { { "lang", tr::info(tr::system()).name } }) : tr::choice_name(c);
			},
			[&](int d) { set_language(tr::step(language_choice(), d)); } });
		p.items.push_back({ tr::G_PLAY_STYLE, tr::G_PLAY_STYLE_HELP,
			[&] { return std::string(cfg.arcade ? tr::G_ARCADE : tr::G_PC); },
			[&](int) {
				keep_arcade_coins();
				cfg.arcade = !cfg.arcade; g_pc_mode = !cfg.arcade; cfg_dirty = true;
				apply_play_style();
				reset_game(false);   // (kept above, in the style it was)
			} });
		p.items.push_back({ tr::G_RESET, tr::G_RESET_HELP,
			{}, {}, [&] { reset_game(); } });
		p.items.push_back({ tr::G_LOG, tr::G_LOG_HELP,
			[&] { return on_off(cfg.log); }, [&](int) { cfg.log = !cfg.log; cfg_dirty = true; }, [&] { cfg.log = !cfg.log; cfg_dirty = true; } });
		p.items.push_back(back_item());
		return p;
	};
	// ---- the arcade's own settings (test mode's GAME ASSIGNMENTS): the game reads them as it starts,
	// so closing the menu after changing one restarts it (only if something is different) ----
	using ga = model1::game_assignment;
	constexpr int N_ASSIGN = 6;
	static_assert(std::tuple_size_v<decltype(assign_before)::value_type> == N_ASSIGN + 6);
	auto assignments_now = [&] {
		std::array<int, N_ASSIGN + 6> v;
		for (int i = 0; i < N_ASSIGN; i++) v[i] = m.assignment(ga(i));
		v[N_ASSIGN] = cfg.ring;
		v[N_ASSIGN + 1] = cfg.akira_slot;   // (Cheats: read as a fight loads, so it restarts too)
		v[N_ASSIGN + 2] = cfg.ring_cheat;
		v[N_ASSIGN + 3] = cfg.no_time_limit;   // (an instruction of the game's: a restart recompiles it)
		v[N_ASSIGN + 4] = (cfg.random_fighter != 0) * 2 + cfg.random_cpu;
		v[N_ASSIGN + 5] = int(std::hash<std::string>{}(cfg.random_player[0] + "/" + cfg.random_player[1]));   // (each player's fighter)
		return v;
	};
	auto arcade_page = [&] {
		if (!assign_before) assign_before = assignments_now();
		menu_page p{ tr::P_ARCADE, "", {} };   // always in view
		auto choice = [&](tr::text label, tr::text help, ga a, std::function<std::string(int)> name) {
			p.items.push_back({ label, help,
				[&m, a, name] { int v = m.assignment(a); return v < 0 ? std::string("-") : name(v); },
				[&m, &cfg, &cfg_dirty, a](int d) {
					int v = m.assignment(a); if (v >= 0) m.set_assignment(a, v + d);
					// (the player's language: English's once-only setting not to overrule it at the restart)
					if (v >= 0 && a == ga::language && !cfg.english_set) cfg.english_set = true, cfg_dirty = true;
					if (v >= 0 && a == ga::match_vs && !cfg.vs_rounds_3) cfg.vs_rounds_set = cfg.vs_rounds_3 = true, cfg_dirty = true;   // (the same, the rounds')
				} });
		};
		static const tr::text LANGUAGES[] = { tr::A_JAPANESE, tr::A_ENGLISH };
		choice(tr::A_LANGUAGE, tr::A_LANGUAGE_HELP, ga::language, [](int v) { return std::string(LANGUAGES[v]); });
		static const tr::text DIFFICULTY[] = { tr::A_EASY, tr::A_NORMAL, tr::A_HARD, tr::A_HARDEST };
		choice(tr::A_DIFFICULTY, tr::A_DIFFICULTY_HELP, ga::difficulty, [](int v) { return std::string(DIFFICULTY[v]); });
		static const int RINGS[] = { 0, 1200, 1300, 1400, 1500 };   // the arcade's widths (STAGE WIDTH)
		static const tr::text RING_NAMES[] = { tr::A_RING_DEFAULT, tr::A_RING_SMALL, tr::A_RING_MEDIUM, tr::A_RING_LARGE, tr::A_RING_XL };
		p.items.push_back({ tr::A_RING_SIZE, tr::A_RING_SIZE_HELP,
			[&] { return std::string(RING_NAMES[std::find(std::begin(RINGS), std::end(RINGS), cfg.ring) - std::begin(RINGS)]); },
			[&](int d) {
				int i = int(std::find(std::begin(RINGS), std::end(RINGS), cfg.ring) - std::begin(RINGS));
				cfg.ring = RINGS[std::clamp(i + d, 0, int(std::size(RINGS)) - 1)];
				cfg_dirty = true;
				m.set_ring_width(cfg.ring_now());
			} });
		choice(tr::A_ROUNDS_CPU, tr::A_ROUNDS_CPU_HELP, ga::match_1p, [](int v) { return std::to_string(v); });
		choice(tr::A_ROUNDS_VS, tr::A_ROUNDS_VS_HELP, ga::match_vs, [](int v) { return std::to_string(v); });
		choice(tr::A_CONTINUE, tr::A_CONTINUE_HELP, ga::continues, [&](int v) { return on_off(v); });
		choice(tr::A_DEMO_SOUND, tr::A_DEMO_SOUND_HELP, ga::advertise_sound, [&](int v) { return on_off(v); });
		p.items.push_back({ tr::A_SERVICE, tr::A_SERVICE_HELP,
			{}, {}, [&] { menu.close(); paused = false; g_test_pending = true; } });
		p.items.push_back(back_item());
		return p;
	};
	// ---- cheats: the unused characters (in Akira's place) ----
	// Each player's fighter's row state: typing a seed in it, what was typed, a mistake; and where the
	// page put the rows (to rebuild it with the same one selected)
	struct seed_entry { bool typing = false, pending = false; std::string typed, error; int secret = 1; };   // (pending: Seed chosen, none typed yet: plays as Random)
	std::shared_ptr<seed_entry> seed_st[2] = { std::make_shared<seed_entry>(), std::make_shared<seed_entry>() };
	int fighter_row[2] = { 0, 0 }, fighter_sub[2] = { -1, -1 };
	std::function<menu_page()> cheats_page;
	// (out here: the rows' functions outlive the page's building)
	auto kind_of = [&](int pl) { const std::string &v = cfg.random_player[pl]; return v.empty() ? 0 : v == "*" ? 1 : v[0] == '#' ? 3 : 2; };
	static const tr::text KINDS[] = { tr::C_NORMAL, tr::C_RANDOM, tr::C_SEED, tr::C_SECRET };
	// (the page made first: where a player's rows land depends on the rows above)
	auto rebuild = [&](int pl, bool sub) { menu_page pg = cheats_page(); menu.replace(std::move(pg), sub && fighter_sub[pl] >= 0 ? fighter_sub[pl] : fighter_row[pl]); };
	constexpr int N_SECRET = int(std::size(model1::UNUSED_CHARACTERS));
	cheats_page = [&] {
		if (!assign_before) assign_before = assignments_now();
		menu_page p{ tr::P_CHEATS_RESTART, "", {} };
		auto heading = [&](tr::text t) { menu_item h{ t, "", {}, {}, {}, true }; h.heading = true; p.items.push_back(h); };
		heading(tr::C_H_FIGHTERS);
		// Each player's fighter (Normal, Random, Seed, Secret character), and under it, only when it
		// needs one, a row of its own: the seed (typed right in it, a mistake said under it) or the
		// secret character. Anything but Normal is picked for that player at the select
		for (int pl = 0; pl < 2; pl++) {
			auto st = seed_st[pl];
			if (kind_of(pl) == 3) st->secret = std::clamp(atoi(cfg.random_player[pl].c_str() + 1), 1, N_SECRET - 1);
			auto entry = std::make_shared<std::function<bool(const SDL_Event &)>>();
			auto set_kind = [&, pl](const std::string &v) { cfg.random_player[pl] = v; if (v != "" && v != "*" && v[0] != '#') cfg.random_named[pl] = v; cfg.random_now(); cfg_dirty = true; };
			auto start_typing = [&, pl, st, entry] {
				st->typing = true; st->typed.clear(); st->error.clear();
				rebuild(pl, true);
				menu.capture(tr::C_SEED_HINT, *entry, [&, pl, st] { st->typing = false; st->error.clear(); rebuild(pl, false); });
			};
			*entry = [&, pl, st, set_kind](const SDL_Event &e) {
				bool enter = false;
				if (e.type == SDL_EVENT_KEY_DOWN) {
					const SDL_Scancode sc = e.key.scancode;
					SDL_Keycode k = e.key.key;   // (the letter on the key, the keyboard's layout: AZERTY's A is the scancode's Q)
					if (k == SDLK_UNKNOWN && sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) k = SDLK_A + (sc - SDL_SCANCODE_A);   // (none given: by its place)
					if (k >= SDLK_A && k <= SDLK_Z) { if (st->typed.size() < 12) st->typed += char('A' + (k - SDLK_A)); }
					else if (sc == SDL_SCANCODE_BACKSPACE) { if (!st->typed.empty()) st->typed.pop_back(); }
					else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) enter = true;
					else return false;
				} else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
					std::string &t = st->typed;
					auto turn = [&](int d) { if (t.empty()) t.push_back('A'); else t.back() = char('A' + ((t.back() - 'A' + d) % 26 + 26) % 26); };
					switch (e.gbutton.button) {
					case SDL_GAMEPAD_BUTTON_EAST: st->typing = false; st->error.clear(); rebuild(pl, false); return true;   // (B: as it was)
					case SDL_GAMEPAD_BUTTON_SOUTH: enter = true; break;
					case SDL_GAMEPAD_BUTTON_DPAD_UP: turn(+1); break;
					case SDL_GAMEPAD_BUTTON_DPAD_DOWN: turn(-1); break;
					case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: if (t.size() < 12) t.push_back(t.empty() ? 'A' : t.back()); break;
					case SDL_GAMEPAD_BUTTON_DPAD_LEFT: if (!t.empty()) t.pop_back(); break;
					default: return false;
					}
				} else
					return false;
				st->error.clear();
				if (enter) {
					if (st->typed.empty()) st->error = tr::C_SEED_EMPTY;
					else if (!m.random_fighter_names().count(st->typed)) st->error = tr::fill(tr::C_SEED_UNKNOWN, { { "seed", st->typed } });
					else { set_kind(st->typed); st->typing = st->pending = false; rebuild(pl, true); return true; }
				}
				menu.refresh();
				return false;
			};
			fighter_row[pl] = int(p.items.size());
			p.items.push_back({ pl ? tr::C_P2_FIGHTER : tr::C_P1_FIGHTER, tr::C_FIGHTER_HELP,
				[&, pl, st] { return std::string(KINDS[st->typing || st->pending ? 2 : kind_of(pl)]); },
				[&, pl, st, set_kind, start_typing](int d) {
					const int k = wrap((st->pending ? 2 : kind_of(pl)) + d, 4);
					st->typing = st->pending = false; st->error.clear();
					if (k == 0) set_kind("");
					else if (k == 1) set_kind("*");
					else if (k == 3) set_kind("#" + std::to_string(st->secret));
					else if (!cfg.random_named[pl].empty() && m.random_fighter_names().count(cfg.random_named[pl])) set_kind(cfg.random_named[pl]);
					else { st->pending = true; set_kind("*"); }   // (no seed yet: Random until one is typed in its row)
					rebuild(pl, false);
				},
				{}, false,
				[&, pl, st, set_kind] { st->typing = st->pending = false; set_kind(""); rebuild(pl, false); } });
			fighter_sub[pl] = -1;
			const int kind = st->typing || st->pending ? 2 : kind_of(pl);
			if (kind == 2) {
				fighter_sub[pl] = int(p.items.size());
				menu_item it{ tr::C_SEED, tr::C_SEED_HELP,
					[&, pl, st] { return st->typing ? st->typed + "_" : st->pending ? std::string(tr::C_SEED_PENDING) : cfg.random_player[pl]; },
					{}, start_typing };
				it.below = [st] { return st->typing ? st->error : std::string(); };
				it.sub = true;
				p.items.push_back(it);
			} else if (kind == 3) {
				fighter_sub[pl] = int(p.items.size());
				static const tr::text ABOUT[] = { tr::C_ABOUT_SIBA, tr::C_ABOUT_SIBA, tr::C_ABOUT_JEFF, tr::C_ABOUT_TAKERU, tr::C_ABOUT_REKKA,
					tr::C_ABOUT_SCORE, tr::C_ABOUT_AEON, tr::C_ABOUT_DURAL };
				menu_item ch{ tr::C_CHARACTER, ABOUT[std::clamp(st->secret, 1, 7)],
					[st] { return online_menu::secret_name(std::clamp(st->secret, 1, int(std::size(model1::UNUSED_CHARACTERS)) - 1)); },
					[&, pl, st, set_kind](int d) {
						st->secret = 1 + wrap(st->secret - 1 + d, N_SECRET - 1);
						set_kind("#" + std::to_string(st->secret));
						rebuild(pl, true);   // (its help: this character's)
					} };
				ch.sub = true;
				p.items.push_back(ch);
			}
		}
		p.items.push_back({ tr::C_RANDOM_CPU, tr::C_RANDOM_CPU_HELP,
			[&] { return on_off(cfg.random_cpu); },
			[&](int) {
				cfg.random_cpu = !cfg.random_cpu;   // (applied as the game restarts)
				cfg.random_now();
				cfg_dirty = true;
			} });
		static const tr::text WHO[] = { tr::OFF, tr::P_PLAYER_1, tr::P_PLAYER_2, tr::C_BOTH };
		heading(tr::C_H_FIGHT);
		p.items.push_back({ tr::C_INFINITE, tr::C_INFINITE_HELP,
			[&] { return std::string(WHO[cfg.infinite_energy]); },
			[&](int d) { cfg.infinite_energy = wrap(cfg.infinite_energy + d, 4); cfg_dirty = true; } });
		p.items.push_back({ tr::C_ONE_HIT, tr::C_ONE_HIT_HELP,
			[&] { return on_off(cfg.one_hit_ko); },
			[&](int) { cfg.one_hit_ko = !cfg.one_hit_ko; cfg_dirty = true; } });
		p.items.push_back({ tr::C_NO_TIME, tr::C_NO_TIME_HELP,
			[&] { return on_off(cfg.no_time_limit); },
			[&](int) { cfg.no_time_limit = !cfg.no_time_limit; cfg_dirty = true; } });
		static const tr::text RING[] = { tr::C_RING_NORMAL, tr::C_RING_TINY, tr::C_RING_HUGE };
		p.items.push_back({ tr::C_RING, tr::C_RING_HELP,
			[&] { return std::string(RING[cfg.ring_cheat]); },
			[&](int d) { cfg.ring_cheat = wrap(cfg.ring_cheat + d, 3); cfg_dirty = true; m.set_ring_width(cfg.ring_now()); } });
		heading(tr::C_H_LOOKS);
		p.items.push_back({ tr::C_KAGE_MASK, tr::C_KAGE_MASK_HELP,
			[&] { return std::string(cfg.kage_unmasked ? tr::C_MASK_OFF : tr::C_MASK_ARCADE); },
			[&](int) { cfg.kage_unmasked = !cfg.kage_unmasked; cfg_dirty = true; m.set_kage_unmasked(cfg.kage_unmasked); } });
		p.items.push_back(back_item());
		return p;
	};
	// ---- controls: per player, keys and pad buttons, and which pad ----
	auto key_name = [](SDL_Scancode k) { return std::string(k ? SDL_GetScancodeName(k) : "-"); };   // (the keys' own names, as they're printed: not translated)
	// The controls' names in the language on (CONTROL_NAMES: the log's and the settings' English), and a pad button's
	auto control_name = [](int a) -> std::string {
		static const tr::text names[C_COUNT] = { tr::CTL_UP, tr::CTL_DOWN, tr::CTL_LEFT, tr::CTL_RIGHT, tr::CTL_GUARD, tr::CTL_PUNCH, tr::CTL_KICK,
			tr::CTL_GUARD_PUNCH, tr::CTL_PUNCH_KICK, tr::CTL_GUARD_KICK, tr::CTL_ALL_THREE, tr::CTL_START, tr::CTL_COIN, tr::CTL_MENU };
		return a >= 0 && a < C_COUNT ? std::string(names[a]) : std::string();
	};
	auto pad_label = [](int code) -> std::string {   // (the buttons' printed names as they are: A, B, LB, Start, View)
		switch (code) {
		case SDL_GAMEPAD_BUTTON_LEFT_STICK: return tr::PAD_LEFT_STICK;
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return tr::PAD_RIGHT_STICK;
		case SDL_GAMEPAD_BUTTON_TOUCHPAD: return tr::PAD_TOUCHPAD;
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return tr::PAD_DPAD_UP;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return tr::PAD_DPAD_DOWN;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return tr::PAD_DPAD_LEFT;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return tr::PAD_DPAD_RIGHT;
		default: return player_controls::pad_label(code);
		}
	};
	auto pad_name = [&](int p) {
		const std::string &c = cfg.ctl[p].pad_choice;
		const char *nm = pads[p] ? SDL_GetGamepadName(pads[p]) : nullptr;
		std::string now = pads[p] ? (nm && *nm ? nm : tr::CTL_GAMEPAD.c_str()) : "";
		if (c == "none") return std::string(tr::CTL_NONE);
		if (c == "auto") return now.empty() ? std::string(tr::CTL_ANY_PAD_NONE) : tr::fill(tr::CTL_ANY_PAD, { { "pad", now } });
		return now.empty() ? std::string(tr::CTL_CHOSEN_PAD_GONE) : now;
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
		menu_page pg{ p ? tr::P_P2_KEYBOARD : tr::P_P1_KEYBOARD, "", {} };
		for (int a = 0; a < C_COUNT; a++)
			pg.items.push_back({ control_name(a), a == C_COIN ? tr::CTL_KEY_HELP_COIN : a == C_MENU ? tr::CTL_KEY_HELP_MENU : tr::CTL_KEY_HELP,
				[&, p, a] { return key_name(cfg.ctl[p].key[a]); }, {},
				[&, p, a] {
					menu.capture(tr::fill(p ? tr::CTL_PRESS_KEY_P2 : tr::CTL_PRESS_KEY_P1, { { "control", control_name(a) } }),
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
		menu_page pg{ p ? tr::P_P2_PAD : tr::P_P1_PAD, "", {} };
		for (int a = C_GUARD; a < C_COUNT; a++)
			pg.items.push_back({ control_name(a), a == C_COIN ? tr::CTL_PADB_HELP_COIN : a == C_MENU ? tr::CTL_PADB_HELP_MENU : tr::CTL_PADB_HELP,
				[&, p, a] { return pad_label(cfg.ctl[p].pad[a]); }, {},
				[&, p, a] {
					menu.capture(tr::fill(a >= C_COIN ? (p ? tr::CTL_PRESS_PAD_P2 : tr::CTL_PRESS_PAD_P1) : (p ? tr::CTL_PRESS_PAD_VIEW_P2 : tr::CTL_PRESS_PAD_VIEW_P1), { { "control", control_name(a) } }),
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
	// Set up buttons (VF3's): each of the game's controls asked for in turn, read from the pad's own joystick (its
	// buttons, hats and axes as they are, not through its mapping), and a mapping made of them for its GUID (as SDL's
	// database's): the directions the d-pad, Guard Punch Kick X A B (the Pad buttons' defaults, set back to them) and
	// Start, Menu on View (the menu's own button, tapped), the buttons left over Y LB RB LT RT and the rest (the
	// combinations: Y guard+punch, RB punch+kick, LB guard+kick, RT all three by default), a pad's left stick kept where
	// it rests in the middle and wasn't pressed for something else. Kept in settings.ini (pad_mapping), so a DirectInput
	// stick's own layout, whatever it is, works from then on. Esc stops; Delete skips one (a stick without it); 10 s
	// with nothing pressed stops too.
	struct setup_step { const char *name, *target; int action; };
	static const setup_step SETUP_STEPS[] = {
		{ "Up", "dpup", -1 }, { "Down", "dpdown", -1 }, { "Left", "dpleft", -1 }, { "Right", "dpright", -1 },
		{ "Guard", "x", C_GUARD }, { "Punch", "a", C_PUNCH }, { "Kick", "b", C_KICK },
		{ "Start", "start", C_START }, { "Menu (View or Select; Start again if it has none)", "back", -1 } };
	constexpr int N_SETUP = int(sizeof SETUP_STEPS / sizeof SETUP_STEPS[0]);
	constexpr int SETUP_START = N_SETUP - 2, SETUP_FIRST_BUTTON = 4;   // (Start's step, Menu after it; Guard's the first button's)
	static_assert(N_SETUP == 9, "SETUP_START, SETUP_FIRST_BUTTON and the Pad buttons' CODE follow SETUP_STEPS");
	struct {
		int player = -1;            // the player whose pad's being set up (-1: none)
		SDL_Joystick *joy = nullptr;
		int step = 0;
		bool wait_release = true;   // (the press before, or the button that chose Set up buttons, let go first)
		uint64_t since = 0;         // (SDL_GetTicks: the prompt's, for its time out)
		bool skip = false;          // (Delete: this one skipped)
		std::vector<std::string> got;   // each prompt's input, as a mapping has it ("b3", "h0.1", "-a1"; empty: skipped)
		std::vector<Sint16> rest;   // the axes as they were at the start (a trigger's rest is one end)
		std::string orig;           // the pad's mapping before (what of it isn't pressed for something else is kept)
	} setup;
	auto setup_end = [&] {
		if (setup.joy) SDL_CloseJoystick(setup.joy);
		setup.joy = nullptr;
		setup.player = -1;
	};
	auto setup_prompt = [&] {
		const int p = setup.player;
		static const tr::text SETUP_NAMES[] = { tr::CTL_UP, tr::CTL_DOWN, tr::CTL_LEFT, tr::CTL_RIGHT, tr::CTL_GUARD, tr::CTL_PUNCH, tr::CTL_KICK,
			tr::CTL_START, tr::SETUP_MENU_STEP };
		static_assert(sizeof SETUP_NAMES / sizeof SETUP_NAMES[0] == N_SETUP, "a name for each of SETUP_STEPS");
		menu.capture(tr::fill(tr::SETUP_PRESS, { { "button", SETUP_NAMES[setup.step] }, { "player", std::to_string(p + 1) }, { "n", std::to_string(setup.step + 1) }, { "of", std::to_string(N_SETUP) } }),
			[&](const SDL_Event &e) {   // (the pad's own events are swallowed meanwhile: what counts is read below)
				if (e.type == SDL_EVENT_KEY_DOWN && (e.key.scancode == SDL_SCANCODE_DELETE || e.key.scancode == SDL_SCANCODE_BACKSPACE)) setup.skip = true;
				return false;
			},
			[&] { setup_end(); fprintf(stderr, "set up buttons: stopped\n"); });
	};
	auto setup_start = [&](int p) {
		if (!pads[p]) { menu.toast(tr::SETUP_NO_PAD); return; }
		setup_end();
		setup.joy = SDL_OpenJoystick(SDL_GetGamepadID(pads[p]));   // (a reference of its own: pads are opened again as they come and go)
		if (!setup.joy) { menu.toast(tr::SETUP_UNREADABLE); return; }
		setup.player = p;
		setup.step = 0;
		setup.wait_release = true;
		setup.skip = false;
		setup.since = SDL_GetTicks();
		setup.got.clear();
		setup.rest.clear();
		if (char *om = SDL_GetGamepadMapping(pads[p])) { setup.orig = om; SDL_free(om); } else setup.orig.clear();
		for (int a = 0; a < SDL_GetNumJoystickAxes(setup.joy); a++) setup.rest.push_back(SDL_GetJoystickAxis(setup.joy, a));
		fprintf(stderr, "set up buttons: player %d, %s (%s): %d buttons, %d hats, %d axes\n", p + 1, SDL_GetJoystickName(setup.joy),
		        joystick_guid(SDL_GetJoystickID(setup.joy)).c_str(), SDL_GetNumJoystickButtons(setup.joy), SDL_GetNumJoystickHats(setup.joy),
		        SDL_GetNumJoystickAxes(setup.joy));
		setup_prompt();
	};
	auto forget_mapping = [&](const std::string &guid) {   // (settings.ini's pad_mapping for that GUID: true if there was one)
		auto &all = cfg.pad_mappings;
		const size_t was = all.size();
		all.erase(std::remove_if(all.begin(), all.end(), [&](const std::string &o) { return o.compare(0, o.find(','), guid) == 0; }), all.end());
		return all.size() != was;
	};
	auto setup_finish = [&] {
		SDL_Joystick *j = setup.joy;
		const int p = setup.player;
		const std::string guid = joystick_guid(SDL_GetJoystickID(j));
		std::string m = guid + "," + mapping_name(SDL_GetJoystickName(j)) + ",";
		std::set<std::string> targets, sources;   // (what's mapped, and from what: an axis without its sign)
		auto source = [](std::string v) {   // (an axis's sign and SDL's ~ for one turned round off)
			if (v.size() > 1 && (v[0] == '+' || v[0] == '-')) v.erase(0, 1);
			if (!v.empty() && v.back() == '~') v.pop_back();
			return v;
		};
		auto add = [&](const std::string &t, const std::string &v) { m += t + ":" + v + ","; targets.insert(t); sources.insert(source(v)); };
		for (size_t i = 0; i < setup.got.size(); i++)
			if (!setup.got[i].empty()) add(SETUP_STEPS[i].target, setup.got[i]);
		// the pad's own mapping where it doesn't clash: a pad's triggers, sticks and the rest kept as they were
		for (size_t at = setup.orig.find(',', setup.orig.find(',') + 1); at != std::string::npos && at + 1 < setup.orig.size();) {
			const size_t end = setup.orig.find(',', at + 1);
			const std::string f = setup.orig.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
			at = end;
			const size_t colon = f.find(':');
			if (colon == std::string::npos || f.compare(0, colon, "platform") == 0 || f.compare(0, colon, "crc") == 0) continue;
			const std::string t = f.substr(0, colon), v = f.substr(colon + 1);
			if (!targets.count(t) && !sources.count(source(v))) add(t, v);
		}
		if (!targets.count("leftx") && !sources.count("a0") && !sources.count("a1") && setup.rest.size() >= 2 &&
		    std::abs(int(setup.rest[0])) < 8000 && std::abs(int(setup.rest[1])) < 8000) {
			add("leftx", "a0");
			add("lefty", "a1");
		}
		// (Y first: VF1 has no Escape, and Y is guard+punch by default)
		static const char *const EXTRA[] = { "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "misc1", "paddle1", "paddle2", "paddle3", "paddle4" };
		size_t x = 0;
		for (int b = 0; b < SDL_GetNumJoystickButtons(j); b++) {
			if (sources.count("b" + std::to_string(b))) continue;
			while (x < std::size(EXTRA) && targets.count(EXTRA[x])) x++;
			if (x == std::size(EXTRA)) break;
			add(EXTRA[x], "b" + std::to_string(b));
		}
		setup_end();
		menu.end_capture();
		// (a mapping given before ours with more say, as Steam's SDL_GAMECONTROLLERCONFIG, keeps its place: not taken)
		const int added = SDL_AddGamepadMapping(m.c_str());
		char *now = added < 0 ? nullptr : SDL_GetGamepadMappingForGUID(SDL_StringToGUID(guid.c_str()));
		const std::string body = m.substr(guid.size() + 1, m.size() - guid.size() - 2);   // (the name and the fields, no last comma)
		const bool taken = now && std::string(now).find(body) != std::string::npos;
		if (now) SDL_free(now);
		if (!taken) {
			fprintf(stderr, "set up buttons: the mapping wasn't taken (%s): %s\n", added < 0 ? SDL_GetError() : "another, from Steam or SDL_GAMECONTROLLERCONFIG, decides", m.c_str());
			menu.toast(added < 0 ? tr::SETUP_FAILED : tr::SETUP_STEAM);
			return;
		}
		fprintf(stderr, "set up buttons: %s\n", m.c_str());
		g_stick_guids.erase(guid);   // (the player's own now: an Any pad like any other)
		forget_mapping(guid);
		cfg.pad_mappings.push_back(m);
		// the Pad buttons back on what was pressed for them (each code taken from whatever else had it), for this player
		// and the other if theirs is the same kind of pad (a mapping is a GUID's: two identical sticks share it)
		static const int CODE[] = { SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_START };
		for (int q = 0; q < 2; q++) {
			if (q != p && !(pads[q] && joystick_guid(SDL_GetGamepadID(pads[q])) == guid)) continue;
			for (int i = SETUP_FIRST_BUTTON; i <= SETUP_START && i < int(setup.got.size()); i++) {
				if (setup.got[i].empty()) continue;
				const int code = CODE[i - SETUP_FIRST_BUTTON];
				for (int b = C_GUARD; b < C_COUNT; b++) if (cfg.ctl[q].pad[b] == code) cfg.ctl[q].pad[b] = PAD_NONE;
				cfg.ctl[q].pad[SETUP_STEPS[i].action] = code;
			}
		}
		cfg_dirty = true;
		menu.refresh();
		menu.toast(pads[1 - p] && joystick_guid(SDL_GetGamepadID(pads[1 - p])) == guid
		           ? tr::fill(tr::SETUP_DONE_BOTH, { { "player", std::to_string(p + 1) }, { "other", std::to_string(2 - p) } })
		           : tr::fill(tr::SETUP_DONE, { { "player", std::to_string(p + 1) } }));
	};
	// each pass of the loop while it's on: the joystick's state read (SDL updates it as the events are polled)
	auto setup_poll = [&] {
		if (setup.player < 0) return;
		SDL_Joystick *j = setup.joy;
		if (!menu.capturing() || !SDL_JoystickConnected(j)) {
			const bool gone = menu.capturing();
			setup_end();
			if (gone) { menu.end_capture(); menu.toast(tr::SETUP_DISCONNECTED); }
			return;
		}
		const int nb = SDL_GetNumJoystickButtons(j), nh = SDL_GetNumJoystickHats(j), na = std::min(SDL_GetNumJoystickAxes(j), int(setup.rest.size()));
		std::string in;   // the first input pressed now, as a mapping has it
		bool any = false;
		for (int b = 0; b < nb; b++)
			if (SDL_GetJoystickButton(j, b)) { any = true; if (in.empty()) in = "b" + std::to_string(b); }
		for (int h = 0; h < nh; h++) {
			const Uint8 v = SDL_GetJoystickHat(j, h);
			if (v == SDL_HAT_CENTERED) continue;
			any = true;
			if (in.empty() && (v == SDL_HAT_UP || v == SDL_HAT_RIGHT || v == SDL_HAT_DOWN || v == SDL_HAT_LEFT))   // (not a diagonal)
				in = "h" + std::to_string(h) + "." + std::to_string(v);
		}
		for (int a = 0; a < na; a++) {
			const int v = SDL_GetJoystickAxis(j, a), d = v - setup.rest[a];
			if (std::abs(d) > 8000) any = true;
			if (in.empty() && std::abs(d) > 16000) in = (d < 0 ? "-a" : "+a") + std::to_string(a);
		}
		const uint64_t now = SDL_GetTicks();
		if (setup.wait_release) {
			if (!any) { setup.wait_release = false; setup.since = now; }
			else if (now - setup.since > 10000) {   // (a switch that reads as held, or an axis off its rest: never let go)
				setup_end();
				menu.end_capture();
				menu.toast(tr::SETUP_HELD);
				fprintf(stderr, "set up buttons: something held for 10 s\n");
			}
			return;
		}
		// (Start again on Menu, its last: skipped, for a pad without one and a player without a keyboard)
		const bool start_again = setup.step > SETUP_START && !in.empty() && in == setup.got[SETUP_START];
		if (setup.skip || start_again) { setup.skip = false; in.clear(); }
		else if (in.empty() || std::find(setup.got.begin(), setup.got.end(), in) != setup.got.end()) {   // (nothing new pressed)
			if (now - setup.since > 10000) {
				if (setup.step > SETUP_START) {   // (Start's had: what was pressed kept, Menu left as it was)
					fprintf(stderr, "set up buttons: Menu not pressed for 10 s: kept as it was\n");
					setup.got.push_back("");
					setup_finish();
					return;
				}
				setup_end();
				menu.end_capture();
				menu.toast(tr::SETUP_TIMEOUT);
				fprintf(stderr, "set up buttons: nothing pressed for 10 s\n");
			}
			return;
		}
		fprintf(stderr, "set up buttons: %s is %s\n", SETUP_STEPS[setup.step].name, in.empty() ? "skipped" : in.c_str());
		setup.got.push_back(in);
		setup.wait_release = true;
		setup.since = now;
		if (++setup.step == N_SETUP) setup_finish();
		else setup_prompt();
	};
	auto player_page = [&](int p) {
		menu_page pg{ p ? tr::P_PLAYER_2 : tr::P_PLAYER_1, "", {} };
		pg.items.push_back({ tr::CTL_PAD, tr::CTL_PAD_HELP,
							 [&, p] { return pad_name(p); }, [&, p](int d) { cycle_pad(p, d); } });
		pg.items.push_back({ tr::CTL_KEYBOARD, tr::CTL_KEYBOARD_HELP, {}, {}, [&, p] { menu.push(keys_page(p)); } });
		pg.items.push_back({ tr::CTL_PAD_BUTTONS, tr::CTL_PAD_BUTTONS_HELP,
							 {}, {}, [&, p] { menu.push(pad_buttons_page(p)); } });
		pg.items.push_back({ tr::CTL_SETUP, tr::CTL_SETUP_HELP,
							 {}, {}, [&, p] { setup_start(p); } });
		pg.items.push_back({ tr::CTL_RESET, tr::CTL_RESET_HELP, {}, {},
							 [&, p] {
								 // (a pad's set-up buttons forgotten: SDL keeps a mapping for the run, so its own is back next start)
								 // (not while the other player's pad is the same kind: the mapping's theirs too)
								 if (pads[p]) {
									 const std::string g = joystick_guid(SDL_GetGamepadID(pads[p]));
									 if (pads[1 - p] && joystick_guid(SDL_GetGamepadID(pads[1 - p])) == g)
										 menu.toast(tr::CTL_RESET_SAME_KIND);
									 else if (forget_mapping(g))
										 menu.toast(tr::CTL_RESET_FORGOTTEN);
								 }
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
		menu_page pg{ tr::P_CONTROLS, "", {} };
		pg.items.push_back({ tr::P_PLAYER_1, tr::CTL_P1_HELP, [&] { return pad_name(0); }, {},
							 [&] { menu.push(player_page(0)); } });
		pg.items.push_back({ tr::P_PLAYER_2, tr::CTL_P2_HELP, [&] { return pad_name(1); }, {},
							 [&] { menu.push(player_page(1)); } });
		pg.items.push_back({ tr::CTL_SWAP, tr::CTL_SWAP_HELP, {}, {},
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
	// ---- Online play (online.h): matches through the session; in a match the board holds the host's backup and the
	// match's settings, and this player's own wait here ----
	static Online online(m, !interp);
	std::vector<uint8_t> own_backup;
	uint8_t own_eeprom[model1::EEPROM_SIZE] = {};
	std::string own_rank_names;
	bool match_on = false, resimmed = false;
	std::string hud_player[2];   // (a match's: the players' names for the HUD, each shown while they're in the game)
	int hud_in_shown = -1;
	bool menu_closed_elsewhere = false;   // (the menu closed by a match starting: its closing's bookkeeping still owed)
	// The match's sound: the board's own sound board, taken off it for the match (the main board then never hears back
	// from it, the same on both sides), on a thread of its own to the real clock, fed the bytes the match's board sends
	// it; the queue kept topped up whatever the frames do (held back for the other side, slowed to keep in time), so it
	// never drops out or changes pitch
	struct match_sound_t {
		std::unique_ptr<m1audio> board;
		std::thread th;
		std::mutex mx;
		std::vector<uint8_t> in;
		std::atomic<bool> quit{ false };
		std::atomic<uint64_t> frames{ 0 }, bytes_in{ 0 };   // (the log's: what it played)
		// (a watcher's frames caught up unseen, for the board to run through unheard as fast as it can, in time while it
		// keeps up (vf1net::UnheardQueue, bounded): after in's first unheard_at bytes; while there are some, put's go
		// after them (heard), so the bytes keep their order)
		vf1net::UnheardQueue unheard;
		size_t unheard_at = 0;
		std::atomic<bool> unheard_on{ false };
		void put(const std::vector<uint8_t> &b)
		{
			std::lock_guard<std::mutex> lk(mx);
			if (unheard_on) unheard.heard(b);
			else in.insert(in.end(), b.begin(), b.end());
			bytes_in += b.size();
		}
		void put_unheard(const std::vector<uint8_t> &b)
		{
			if (!th.joinable()) return;   // (no board playing: none to run them through)
			std::lock_guard<std::mutex> lk(mx);
			if (!unheard_on) unheard_at = in.size();
			unheard.frame(b);
			unheard_on = true;
		}
		void stop()
		{
			if (th.joinable()) {
				quit = true;
				th.join();
				fprintf(stderr, "online: the match's sound board played %llu frames, %llu bytes from the game\n", (unsigned long long)frames.load(), (unsigned long long)bytes_in.load());
			}
			quit = false;
			frames = bytes_in = 0;
		}
	} msound;
	auto match_sound_start = [&] {
		msound.stop();
		if (!audio || !msound.board) return;
		m1audio *bp = msound.board.get();
		bp->reset();
		g_match_volumes = pack_volumes(cfg.music, cfg.effects);
		{ std::lock_guard<std::mutex> lk(msound.mx); msound.in.clear(); msound.unheard.clear(); msound.unheard_on = false; }
		SDL_ClearAudioStream(audio);
		SDL_ResumeAudioStreamDevice(audio);
		const int target = frame_bytes * QUEUED;
		msound.th = std::thread([&, target, bp] {
			m1audio &b = *bp;
			std::vector<uint8_t> bytes;
			std::vector<int16_t> out;
			uint32_t volumes = ~0u;   // (the volumes it plays at: the Sound page's, as they change mid-match)
			constexpr int LINES = 424, BYTE_LINES = 8;   // (a frame's scanlines; a byte on the link every 8, as the i8251 sends them)
			const double line_us = 656 * 1e6 / 16e6;
			// The bytes are MIDI's (9x a note, Ax, Bx: a status byte, then its two data bytes), and a frame's often ends
			// in a message's first byte, the rest in the next frame's. The sound program waits in its interrupt for the
			// rest, its music's sequencer with it: a frame held for the other side (a second or two at a high ping) held
			// every note playing until it came. So a message goes to the board whole: its start kept here till its
			// data's here too (or 3 s, a message that never ends).
			auto data_bytes = [](uint8_t st) { const int k = st >> 4; return k == 0xc || k == 0xd ? 1 : k >= 0x8 && k <= 0xe ? 2 : 0; };
			uint8_t status = 0;   // (the last status byte sent: running status, the data bytes of another of its kind)
			int owed = 0;         // (the data bytes the message sent last still has to come)
			int held = 0;         // (frames in a row with only an unfinished message to send)
			while (!msound.quit) {
				if (msound.unheard_on) {   // (a watcher's frames caught up unseen: run through unheard, the bytes put before them first)
					vf1net::UnheardQueue::Piece piece;
					{
						std::lock_guard<std::mutex> lk(msound.mx);
						const size_t at = std::min(msound.unheard_at, msound.in.size());
						bytes.insert(bytes.end(), msound.in.begin(), msound.in.begin() + ptrdiff_t(at));
						msound.in.erase(msound.in.begin(), msound.in.begin() + ptrdiff_t(at));
						msound.unheard_at = 0;
						if (!msound.unheard.next(piece)) { msound.unheard_on = false; continue; }
					}
					vf1net::run_unheard(b, piece, bytes, status, owed);
					continue;
				}
				if (SDL_GetAudioStreamQueued(audio) >= target) { SDL_DelayNS(1000000); continue; }
				{ std::lock_guard<std::mutex> lk(msound.mx); bytes.insert(bytes.end(), msound.in.begin(), msound.in.end()); msound.in.clear(); }
				if (const uint32_t v = g_match_volumes; v != volumes) { volumes = v; b.set_volumes(float(v & 0xff) / 100.0f, float((v >> 8) & 0xff) / 100.0f); }
				size_t whole = 0;   // (the bytes up to the last whole message)
				{
					uint8_t st = status;
					int need = owed;
					for (size_t i = 0; i < bytes.size(); i++) {
						if (bytes[i] >= 0xf8) { if (!need) whole = i + 1; continue; }   // (a timing byte: one alone, wherever it comes)
						if (bytes[i] & 0x80) {   // (a status byte: a message's start)
							st = bytes[i];
							need = data_bytes(st);
							if (!need) whole = i + 1;
							continue;
						}
						if (!need) need = data_bytes(st);   // (running status: another message of the same kind)
						if (!need || --need == 0) whole = i + 1;   // (data with no status before it: as it is)
					}
				}
				// (only frames with nothing to send count: a message waiting while the ones before it go isn't stuck; past 3 s
				// the rest goes as it is, a frame's worth at a time until it's out)
				held = whole == 0 && !bytes.empty() ? held + 1 : 0;
				if (held > 3 * 58) whole = bytes.size();
				size_t sent = 0;
				for (int line = 0; line < LINES; line++) {
					if (line % BYTE_LINES == 0 && sent < whole) {
						const uint8_t x = bytes[sent++];
						if (x >= 0xf8) {}
						else if (x & 0x80) status = x, owed = data_bytes(x);
						else if (owed || (owed = data_bytes(status))) owed--;
						b.rx_byte(x);
					}
					b.run(line_us, out);
					while (b.tx_pending()) b.tx_byte();   // (its answers: no one listens in a match)
				}
				bytes.erase(bytes.begin(), bytes.begin() + ptrdiff_t(sent));
				msound.frames++;
				static FILE *wav = [] { const char *w = getenv("VF_MATCH_SOUND"); return w ? fopen(w, "wb") : nullptr; }();   // (tests: the raw 16-bit stereo it played)
				if (wav && !out.empty()) { fwrite(out.data(), 2, out.size(), wav); fflush(wav); }
				if (!out.empty()) SDL_PutAudioStreamData(audio, out.data(), int(out.size() * sizeof(int16_t)));
				out.clear();
			}
		});
	};
	online.adapter.run = [&](bool resim) {
		m.set_drawing(!resim);   // (a frame run again after a rollback: the board alone, no picture)
		m.run_frame();
		m.set_drawing(true);
		if (resim) resimmed = true;
	};
	online.adapter.settle = [] {};
	online.adapter.booting = [] { SDL_PumpEvents(); };   // (the boot's seconds: the system doesn't take the game for hung)
	online.adapter.program_changed = [&] {   // (the match's code: the cheats' patches off)
#ifdef VF_NATIVE
		if (!interp && !recompile()) {
			fprintf(stderr, "Couldn't recompile the game for the match: running it on the slower interpreter instead.\n");
			rec::detach(m.cpu());
			interp = true;
			m.run_cpu = nullptr;
		}
#endif
		online.adapter.native = !interp;
	};
	online.adapter.on_begin = [&] {
		if (!g_match_board) {   // (this player's own, given back after the match)
			own_backup = m.nvram();
			memcpy(own_eeprom, m.eeprom(), sizeof own_eeprom);
			own_rank_names = m.ranking_names();
			g_match_board = true;
		}
		if (!msound.board) msound.board = m.take_audio();
		// (a watcher: the match's sound board started now, before its first frame, so the boot's set-up of it is heard
		// (Adapter::mute lets the first frame's bytes through); the frames caught up unseen run through it unheard, in
		// time (Adapter::sound_unheard): the stage's music on when the picture's up, as the players hear it)
		if (online.watching()) match_sound_start();
		m.set_ranking_names("");   // (the ranking's names are each player's own: none on the match's board, whose tiles they'd be drawn into)
		online.adapter.native = !interp;
		paused = false;
		g_test_frames = 0;
		g_test_pending = false;
	};
	online.adapter.sound_out = [&](const std::vector<uint8_t> &b) { msound.put(b); };
	online.adapter.sound_unheard = [&](const std::vector<uint8_t> &b) { msound.put_unheard(b); };
	bool online_ready = false;
	auto online_page = [&] {
		if (!online_ready) {   // (the ROMs' hash for the handshake, once)
			online.identity = { "vf1", vf1net::online_version(VF_VERSION), vf1net::content_hash(m), "" };   // (the online version, not the release's)
			online_ready = true;
		}
		cfg.online.seed_name = cfg.random_named[0];   // (the Cheats page's seed name, offered as a fighter)
		cfg.online.seed_ok = [&](const std::string &n) { return m.random_fighter_names().count(n) > 0; };
		return online_menu::page(online, cfg.online, menu, [&] { cfg_dirty = true; });
	};
	int online_at = 5;   // (the main page's Online row, as main_page last made it: where a match's end opens the Online page)
	auto online_row = [&] { return online_at; };
	auto main_page = [&] {
		menu_page p{ "", "", {} };
		if (online.playing() || g_match_board) {   // (the match goes on under the menu; or it's starting, on its board)
			p.items.push_back({ tr::M_RESUME, tr::M_RESUME_MATCH_HELP, {}, {}, [&] { menu.close(); } });
			online_menu::match_rows(online, cfg.online, menu, [&] { cfg_dirty = true; }, p.items);
			p.items.push_back({ tr::P_DISPLAY, tr::M_DISPLAY_HELP, {}, {}, [&] { menu.push(display_page()); } });
			p.items.push_back({ tr::P_SOUND, tr::M_SOUND_HELP, {}, {}, [&] { menu.push(sound_page()); } });
			p.items.push_back({ tr::P_CONTROLS, tr::M_CONTROLS_MATCH_HELP, {}, {}, [&] { menu.push(controls_page()); } });
			return p;
		}
		p.items.push_back({ tr::M_RESUME, tr::M_RESUME_HELP, {}, {}, [&] { menu.close(); } });
		p.items.push_back({ tr::P_DISPLAY, tr::M_DISPLAY_HELP, {}, {}, [&] { menu.push(display_page()); } });
		p.items.push_back({ tr::P_SOUND, tr::M_SOUND_HELP, {}, {}, [&] { menu.push(sound_page()); } });
		p.items.push_back({ tr::P_GAME, tr::M_GAME_HELP, {}, {}, [&] { menu.push(game_page()); } });
		p.items.push_back({ tr::M_ARCADE, tr::M_ARCADE_HELP, {}, {}, [&] { menu.push(arcade_page()); } });
		online_at = int(p.items.size());
		p.items.push_back({ tr::P_ONLINE, tr::M_ONLINE_HELP, {}, {}, [&] { menu.push(online_page()); } });
		p.items.push_back({ tr::P_CONTROLS, tr::M_CONTROLS_HELP, {}, {}, [&] { menu.push(controls_page()); } });
		p.items.push_back({ tr::P_CHEATS, tr::M_CHEATS_HELP, {}, {}, [&] { menu.push(cheats_page()); } });
		p.items.push_back({ tr::M_QUIT, tr::M_QUIT_HELP, {}, {}, [&] { running = false; } });
		return p;
	};
	auto menu_title = [&] { return std::string(online.watching() ? tr::TITLE_WATCHING : online.playing() ? tr::TITLE_ONLINE_MATCH : tr::TITLE_PAUSED); };
	menus_relabelled = [&] {   // (Game > Language changed: the title, the main page and Game made again in it, Language selected)
		const int row = menu.selected();
		menu.set_title(menu_title());
		menu_page root = main_page();
		int game_row = 0;
		for (size_t i = 0; i < root.items.size(); i++) if (root.items[i].label == tr::P_GAME.c_str()) game_row = int(i);
		menu.open(std::move(root));
		menu.select_row(game_row);
		menu.push(game_page());
		menu.select_row(row);
	};
	auto open_menu = [&] {
		menu.set_title(menu_title());
		if (menu.ready() && !menu.is_open()) menu.open(main_page());
	};
	// The game's clock: game frames due since it started, paced by the wall clock at the arcade's
	// rate, trimmed by up to 1% to keep the sound queue at QUEUED frames. A frame runs when the
	// clock passes it, and smooth motion's picture is for the clock's moment: its fraction is how
	// far into the next frame it is, so the pictures are evenly spaced in the game's time. (The
	// sound queue alone can't say: the device takes it in chunks of a frame or more.)
	double frame_clock = 0, queue_avg = QUEUED * frame_bytes;
	int64_t game_run = 0;                    // frames run by the loop
	uint64_t clock_t = SDL_GetTicksNS();
	// A picture each refresh with smooth motion (from VF3, 73c4b27 and 90d61ae): the clock moves the
	// refreshes each pass took, pulled a tenth of the way to the real clock (more than 2 refreshes off,
	// a real hitch: taken whole). By the real clock each picture showed the game where it was when its
	// pass began, early or late in the refresh as the last pass's work took (the frame run during the
	// present's wait, a picture's drawing), and shown a refresh apart the motion wobbled. Only while the
	// passes keep to the screen's refreshes, a whole number of them apart on average: not with vsync
	// off or not honoured, nor a minimised or covered window; never back. VF_NO_EVEN_PACE=1: as before.
	double even_real = 0, even_shown = 0, pass_avg = 0, dt_avg = 1.0 / 120;
	auto restart_clock = [&] { clock_t = SDL_GetTicksNS(); frame_clock = double(game_run); even_real = even_shown = 0; };
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
	// testing: VF_TEST_VJOY=b3@200,h1@260,+0@300,q@900,...: a virtual stick SDL doesn't know (10 buttons, a hat, 2
	// axes; VF3's VF3_TEST_VJOY): its button 3, its hat up (1, 2 right, 4 down, 8 left), axis 0 to its + end (or -1@300
	// axis 1 to its - end) held from those passes of the loop (8 passes); q@N quits then (a run that saves); p@N
	// plugged in then (not at the start); "1": no presses. Player 1's controls are logged as they change.
	struct vjoy_press { char what; int n; int64_t at; };
	std::vector<vjoy_press> vjoy_presses;
	if (const char *k = getenv("VF_TEST_VJOY"))
		for (std::string all = std::string(k) + ","; !all.empty(); all.erase(0, all.find(',') + 1)) {
			const std::string t = all.substr(0, all.find(','));
			const size_t at = t.find('@');
			if (t.size() < 2 || at == std::string::npos) continue;
			vjoy_presses.push_back({ t[0], atoi(t.c_str() + 1), strtoll(t.c_str() + at + 1, nullptr, 10) });
		}
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
	// The game on a thread of its own while the screen waits (from VF3, 7647ac6): a frame that will be
	// due by the next refresh is run during the present's wait for vsync, its inputs read as it starts;
	// what follows it (the hold, its sound) as the present returns. Only with smooth motion, whose
	// pictures are drawn when shown, on this thread: the frame draws nothing. VF_NO_GAME_THREAD=1: off.
	struct game_thread {
		std::mutex mu;
		std::condition_variable cv;
		std::function<void()> job;
		bool busy = false, quit = false;
		std::thread th;
		void start(std::function<void()> f) { { std::lock_guard<std::mutex> l(mu); job = std::move(f); busy = true; } cv.notify_all(); }
		void wait() { std::unique_lock<std::mutex> l(mu); cv.wait(l, [&] { return !busy; }); }
		game_thread() : th([this] {
#ifdef __APPLE__
			pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // (not the efficiency cores)
#endif
			std::unique_lock<std::mutex> l(mu);
			for (;;) {
				cv.wait(l, [&] { return busy || quit; });
				if (quit) return;
				auto f = std::move(job);
				l.unlock();
				f();
				l.lock();
				busy = false;
				cv.notify_all();
			}
		}) {}
		~game_thread() { { std::lock_guard<std::mutex> l(mu); quit = true; } cv.notify_all(); th.join(); }
	};
	static const bool no_game_thread = getenv("VF_NO_GAME_THREAD") != nullptr;
	game_thread gt;
	bool carried_curtain = false;      // (a curtain from the frame run during the last present)
	bool carried_on_gpu = false;       // (... and put together by the GPU)
	double screen_hz_now = 0;          // (the screen's Hz, looked up each second)
	bool vsync_now = true;             // (vsync forced off, or not taken: present returns at once, so not every pass)
	// A game frame's inputs (false: a hotkey opened the menu, no frame runs) and what follows one: the
	// scene changes' hold, its sound
	auto frame_inputs = [&]() -> bool {
		switch (read_inputs(m, pads, pstate, cfg.ctl)) {
		case hotkey::menu: open_menu(); break;
		case hotkey::scale: player_scale_to(cfg.scale % 4 + 1); break;
		case hotkey::aspect: set_shape((aspect_i + 1) % N_ASPECTS); break;
		case hotkey::none: break;
		}
		if (menu.is_open()) return false;
		if (cfg.infinite_energy || cfg.one_hit_ko || cfg.no_time_limit) m.fight_cheats(cfg.infinite_energy, cfg.one_hit_ko, cfg.no_time_limit);
		return true;
	};
	bool curtain_on_gpu = false;       // a curtain shown during a hold, put together by the GPU
	auto after_frame = [&](bool ff) -> bool {
				game_run++;
				// leaving the service menu: the PC build restarts at once, past the boot screen
				if (g_left_test_menu) { g_left_test_menu = false; if (g_pc_mode) { reset_game(); m.m_audio_out.clear(); return false; } }
				// With smooth motion a frame is only drawn when shown: while a scene change is held,
				// draw it anyway so the hold sees the real picture (else it waits on black forever),
				// though it isn't shown
				// (in parts when the GPU puts the pictures together: only the 3D drawn at the render scale)
				bool probe_parts = false;
				if (m.smooth() && sync.holding()) {
					if (on_gpu()) {
						g3_prepare();
						probe_parts = m.render_parts(double(m.frame_number()), parts, false) == 1;
					}
					else m.render_smooth(double(m.frame_number()), false);
				}
				const bool was_holding = sync.holding();
				const bool show_it = sync.show(m);
				// a curtain shown while the hold goes on: frame() as just drawn, or that picture put
				// together by the GPU
				if (show_it) {
					curtain = sync.holding();
					curtain_on_gpu = probe_parts && sync.holding() &&
									 gc.draw(parts, m.frame_w(), m.frame_h(), m.render_scale() > 1 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST) &&
									 (aa_now() == 1 || gc.resolve(aa_now(), shown_w(), shown_h()));
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
				return true;
	};
	// Fullscreen: F11, Alt+Enter, or F (F only while no player has it as a control; the menu takes
	// keys first, so a seed typed there isn't one)
	auto fullscreen_key = [&](const SDL_KeyboardEvent &k) {
		if (k.scancode == SDL_SCANCODE_RETURN && (k.mod & SDL_KMOD_ALT)) return true;
		if (k.scancode != SDL_SCANCODE_F || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) return false;
		for (const auto &c : cfg.ctl)
			for (SDL_Scancode s : c.key) if (s == SDL_SCANCODE_F) return false;
		return true;
	};
	// ---- Online play's pass: the session's notices, the match's start and end, what's on screen about it ----
	// A step that couldn't run (the other side behind, or keeping in time) takes no input: what was pressed for it
	// goes with the next one tried, so a tap of Start or Coin then isn't lost
	uint16_t step_carry = 0;
	constexpr uint16_t ONE_SHOT = vf1net::START_BUTTON | vf1net::COIN_BUTTON;
	Online::Phase online_was = Online::Phase::off;
	std::string online_code_was;   // (the room code: its page made again when the lobby gives it)
	uint64_t online_refresh_t = 0;
	auto end_match = [&] {   // back to playing on its own, from the boot, with this player's backup and settings
		const bool reopen = match_on || g_match_board;   // (a match, or one that started and never got going)
		if (match_on) fprintf(stderr, "online: the match ended at frame %lld; back to playing alone\n", (long long)game_run);
		match_on = false;
		msound.stop();
		{ const std::string none[2]; m.set_hud_names(none); hud_country[0].clear(); hud_country[1].clear(); }
		if (audio) SDL_ClearAudioStream(audio);
		menu.set_notice("");
		menu.set_banner("");
		menu.hide_versus();
		menu.set_net("", 0);
		step_carry = 0;
		if (g_match_board) {
			m.on_uart_tx = nullptr;
			if (msound.board) m.give_audio(std::move(msound.board));
			m.use_io_standin(false);   // (the real I/O board, when the set has one)
			memcpy(m.nvram().data(), own_backup.data(), std::min(own_backup.size(), m.nvram().size()));
			memcpy(m.eeprom(), own_eeprom, sizeof own_eeprom);
			m.set_ranking_names(own_rank_names);
			g_match_board = false;
		}
		// this player's own again: their cheats, ring, Kage and play style (reset_game applies them, and boots)
		m.set_ring_width(cfg.ring_now());
		m.set_kage_unmasked(cfg.kage_unmasked);
		apply_play_style();
		menu.set_title(tr::TITLE_PAUSED);
		reset_game();
		// back on the Online page: what ended the match, and the same again a press away
		if (reopen && menu.ready()) {
			if (menu.is_open()) menu.close();
			menu.open(main_page());
			menu.select_row(online_row());
			menu.push(online_page());
		}
		if (audio) { if (menu.is_open()) SDL_PauseAudioStreamDevice(audio); else SDL_ResumeAudioStreamDevice(audio); }
	};
	// A fighter on the VS screen (vf1net::fighter_words: RANDOM, SECRET: SIBA, a seed's name or SEED 1234) in the language
	// on: the words translated, the names as they are
	auto fighter_shown = [](const std::string &w) -> std::string {
		if (w == "RANDOM") return tr::VS_FIGHTER_RANDOM;
		if (w.rfind("SECRET: ", 0) == 0) return tr::fill(tr::VS_FIGHTER_SECRET, { { "name", w.substr(8) } });
		if (w.rfind("SEED ", 0) == 0) return tr::fill(tr::VS_FIGHTER_SEED, { { "n", w.substr(5) } });
		return w;
	};
	// The button this player presses for Start, as their pad or keyboard has it (online, Player 1's controls)
	auto start_button = [&]() -> std::string {
		const int b = cfg.ctl[0].pad[C_START];
		if (pads[0] && b != PAD_NONE) {
			if (b != SDL_GAMEPAD_BUTTON_START) {
				std::string n = player_controls::pad_label(b);
				for (char &ch : n) ch = char(toupper(uint8_t(ch)));
				return n;
			}
			const char *nm = SDL_GetGamepadName(pads[0]);
			if (nm && strstr(nm, "Steam Deck")) return "MENU";
			switch (SDL_GetGamepadType(pads[0])) {
			case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return "OPTIONS";
			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "+";
			case SDL_GAMEPAD_TYPE_XBOXONE: return "MENU";
			default: return "START";
			}
		}
		std::string k = SDL_GetScancodeName(cfg.ctl[0].key[C_START]);
		for (char &ch : k) ch = char(toupper(uint8_t(ch)));
		return k.empty() ? std::string("START") : k;
	};
	auto online_pass = [&] {
		online.poll(double(SDL_GetTicksNS()) / 1e6);
		Online::Notice n;
		while (online.next_notice(n)) {
			if (n.kind == Online::Notice::joined || n.kind == Online::Notice::watching) {
				const bool watching = n.kind == Online::Notice::watching;
				if (cfg.online.chime && !watching) chime::play(chime_stream, win, audio && !hidden ? float(cfg.volume) / 100.0f : 0.0f);
				match_on = true;
				if (!watching) match_sound_start();   // (a watcher's started with the match's start: on_begin)
				// the players' names under the energy bars (the names the two sides were sent; a watched match's, as its
				// host sent them; theirs as the HUD can show it)
				for (int p = 0; p < 2; p++) {
					hud_player[p] = online.player_name(p);
					hud_country[p] = online.player_country(p);
				}
				hud_in_shown = -1;   // (set on the HUD below, for the players in the game)
				assign_before.reset();   // (the board's arcade settings are the host's now: no restart for them)
				if (menu.is_open()) { menu.close(); menu_closed_elsewhere = true; }
				restart_clock();
				// who's playing who, over the title screen for a moment (the host is player 1)
				// (with each one's fighter under the name: Random, a secret character, a seed's)
				const std::string ft1 = fighter_shown(vf1net::fighter_words(online.player_extra(0), m)), ft2 = fighter_shown(vf1net::fighter_words(online.player_extra(1), m));
				const std::string host = online.player_name(0), guest = online.player_name(1);
				const int ms = online.ping();
				if (watching)
					menu.show_versus({ tr::VS_PLAYER_1, host, tr::VS_PLAYER_2, guest, tr::WATCHING_BEHIND, tr::WATCHING_HOW_TO_STOP, false,
					                   online.player_country(0), online.player_country(1), ft1, ft2 });
				else
					menu.show_versus({ tr::VS_PLAYER_1_HOST, host, tr::VS_PLAYER_2, guest,
					                   ms > 0 ? tr::fill(online.via_relay() ? tr::VS_RELAY_MS : tr::VS_DIRECT_MS, { { "ms", std::to_string(ms) } }) : std::string(online.via_relay() ? tr::VS_RELAY : tr::VS_DIRECT),
					                   online.slow_local() ? tr::VS_MAC_LOCAL : tr::VS_BOTH_PRESS_START, online.via_relay(),
					                   online.player_country(0), online.player_country(1), ft1, ft2 });
			}
			fprintf(stderr, "online: %s\n", Online::log_masked(n.text.en()).c_str());
		}
		if (match_on) {   // (each player's name and flag on the HUD only while they're in the game: a side the CPU has, the game's own)
			const int in = online.players_in_game();
			if (in != hud_in_shown) {
				hud_in_shown = in;
				fprintf(stderr, "online: the HUD's names: %s\n", in == 3 ? "both players'" : in == 1 ? "player 1's (player 2 the CPU's)" : in == 2 ? "player 2's (player 1 the CPU's)" : "the game's own (no one in the game yet)");
				std::string names[2];
				bool fl[2];
				for (int p = 0; p < 2; p++) {
					names[p] = in >> p & 1 ? hud_player[p] : std::string();
					fl[p] = (in >> p & 1) && flags::index_of(hud_country[p]) >= 0;
				}
				m.set_hud_names(names, fl);
			}
		}
		if ((match_on && !online.playing()) || (g_match_board && !online.playing() && !online.starting())) end_match();   // (ended, left, or never got going)
		static bool said_interrupted = false;
		const bool interrupted = online.playing() && online.interrupted() && (said_interrupted || (online.hold_left() <= 6 && !menu.versus_showing()));
		said_interrupted = interrupted;
		// who's to press Start: at the match's start until both have, and after a match the loser, for a rematch
		int wait = online.playing() && !menu.is_open() && !menu.versus_showing() && !interrupted ? online.start_wait() : 0;
		static uint64_t other_out_since = 0;
		if (wait != 4) other_out_since = 0;
		else if (!other_out_since) other_out_since = SDL_GetTicks();
		else if (SDL_GetTicks() - other_out_since > 8000) wait = 0;
		static int said_wait = 0;
		if (wait != said_wait && online.playing()) {
			static const char *what[5] = { "no one", "this player (the match's start)", "the other (the match's start)", "this player (a rematch)", "the other (a rematch)" };
			fprintf(stderr, "online: to press Start: %s\n", what[wait]);
		}
		said_wait = wait;
		const bool press = wait == 1 || wait == 3;
		menu.set_banner(press ? std::string(tr::BANNER_PRESS) : wait ? tr::fill(tr::BANNER_WAITING_FOR, { { "name", online.opponent() } }) : "",
		                wait == 2 ? std::string(tr::BANNER_TO_PRESS_START) : wait == 3 ? std::string(tr::BANNER_FOR_REMATCH) : wait == 4 ? std::string(tr::BANNER_TO_PRESS_REMATCH) : "", press ? start_button() : "");
		if (interrupted)
			menu.set_notice(tr::fill(tr::BANNER_WAITING_FOR, { { "name", online.opponent() } }), tr::fill(tr::NOTICE_DROPPED, { { "n", std::to_string(online.hold_left()) } }),
			                tr::NOTICE_CARRIES_ON);
		else if (online.starting()) menu.set_notice(tr::NOTICE_STARTING, online.status());
		else if (online.watch_between()) menu.set_notice(tr::MATCH_OVER, online.watch_status());   // (a watched match over: not its last picture, still)
		else menu.set_notice("");
		std::string line;
		if (cfg.online.show_connection && online.waiting() && !online.starting())   // (playing alone meanwhile: still waiting, and where)
			line = online.phase() == Online::Phase::hosting && online.by_code() && !online.is_random()
				? (online.room_code().empty() ? std::string(tr::LINE_HOSTING_A_ROOM) : tr::fill(tr::LINE_HOSTING_ROOM, { { "code", online.room_code() } })) + "   " + online.status()
				: online.phase() == Online::Phase::hosting && !online.by_code()
				? tr::fill(tr::LINE_HOSTING_AT, { { "address", online.local_address().empty() ? std::string(tr::LINE_NO_NETWORK) : online.local_address() } }) + "   " + online.status()
				: online.status();
		static game_menu::net_cells cells;
		static uint64_t cells_at = 0;
		static bool had_cells = false;
		if (!online.playing() || (!cfg.online.show_connection && !online.watching()) || menu.versus_showing()) {
			had_cells = false;
			menu.set_net(menu.versus_showing() ? std::string() : line, 0);
		} else if (!had_cells || SDL_GetTicks() - cells_at >= 250 || online.interrupted() != cells.interrupted) {
			had_cells = online.connection(cells, cfg.online.show_connection == 2 && !online.watching());
			cells_at = SDL_GetTicks();
			if (had_cells) menu.set_net(cells);
			else menu.set_net(std::string(), 0);
		}
		// the Online page as things change: its rows made again with the phase, its status lines a few times a second
		if (menu.is_open() && (online.phase() != online_was || online.room_code() != online_code_was) && menu.top_name() == tr::P_ONLINE.c_str() && !menu.capturing())
			menu.replace(online_page(), -1);
		else if (menu.is_open() && online.phase() != online_was && !menu.capturing())
			menu.refresh();
		else if (menu.is_open() && online.playing() && !menu.capturing() && SDL_GetTicks() - online_refresh_t > 500) {
			online_refresh_t = SDL_GetTicks();
			menu.refresh();
		}
		else if (menu.is_open() && online.waiting() && SDL_GetTicks() - online_refresh_t > 250) {
			online_refresh_t = SDL_GetTicks();
			static std::string said;
			if (const std::string now = online.status(); now != said) { said = now; menu.refresh(); }
		}
		else if (menu.is_open() && online.phase() == Online::Phase::off && menu.top_name() == tr::P_ONLINE.c_str() && !menu.capturing())
			// (the Online screen, the room browser: the rooms asked for every 15 s and the page made again as they change,
			// the room selected kept; how many play at random, as the lobby says. Not a match's end, nor a try that failed)
			online_menu::keep_up(online, cfg.online, menu, online_page);
		online_was = online.phase();
		if (!menu.capturing()) online_code_was = online.room_code();
	};
	// a match goes on under the menu; one being set up holds the board, which is the match's now
	auto game_stopped = [&] { return !online.playing() && (paused || menu.is_open() || online.starting()); };
	while (running) {
		if (!started_at) { started_at = SDL_GetTicksNS(); log_state("the start"); }
		// (tests: a room hosted, or joined with its code, or a random opponent looked for, as the menu would, once the
		// game's past its start)
		static bool by_address_started = false;
		if ((arg_host || !arg_join.empty()) && frames_run >= 60 && !by_address_started) {   // (--host, --join: by address, the Online screen showing it)
			by_address_started = true;
			online_page();   // (the identity, as opening the Online page sets it)
			OnlineSettings os = cfg.online;
			if (os.name.empty()) os.name = "PLAYER";   // (no name asked for: the command line's)
			if (arg_port) os.port = arg_port;
			if (!arg_join.empty()) os.address = cfg.online.address = online_menu::clean_address(arg_join);   // (the Online screen says where)
			tr::line err;
			if (!(arg_host ? online.host(os, err) : online.join(os, err))) fprintf(stderr, "online: %s: %s\n", arg_host ? "--host" : "--join", err.en().c_str());
			else fprintf(stderr, "online: %s from the command line\n", arg_host ? "hosting by address" : ("joining " + os.address).c_str());
			if (menu.ready()) {
				if (menu.is_open()) menu.close();
				menu.open(main_page());
				menu.select_row(online_row());
				menu.push(online_page());
			}
		}
		static const char *test_host_room = getenv("VF1_HOST_ROOM"), *test_join_code = getenv("VF1_JOIN_CODE"), *test_random = getenv("VF1_RANDOM");
		// a link (the command line, a Mac's open-URL event, or handed over by another copy): once the game's up, into
		// the room or the match, as Online > Enter code (only the link's own form: links::parse)
		if (frames_run % 60 == 0) { std::string handed; if (links::take(dir, handed)) arg_link = handed; }
		if (!arg_link.empty() && frames_run >= 60 && menu.ready()) {
			links::Link l;
			const std::string url = arg_link;
			arg_link.clear();
			if (!links::parse(url, l)) fprintf(stderr, "links: not a link this game takes: %s\n", Online::log_masked(url.substr(0, 40)).c_str());
			else if (const tr::line no = online_menu::link_refused(online); !no.empty()) { menu.toast(no.str()); fprintf(stderr, "links: %s %s: not now (%s)\n", l.action.c_str(), l.code.c_str(), no.en().c_str()); }
			else {
				online_page();   // (the identity, as opening the Online page sets it)
				tr::line err;
				const bool ok = online_menu::follow_link(online, cfg.online, menu, [&] { cfg_dirty = true; }, l.action, l.code, [&] {
					if (menu.is_open()) menu.close();
					menu.open(main_page());
					menu.select_row(online_row());
					menu.push(online_page());
				}, err);
				fprintf(stderr, "links: %s %s%s%s\n", l.action.c_str(), l.code.c_str(), ok ? "" : (": " + err.en()).c_str(), ok && cfg.online.name.empty() && l.action != "watch" ? " (a name asked for first)" : "");
				SDL_RaiseWindow(win);
			}
		}
		static const char *test_watch = getenv("VF1_WATCH_CODE");
		if ((test_host_room || test_join_code || test_random || test_watch) && frames_run == 60) {
			online_page();   // (the identity, as opening the Online page sets it)
			tr::line err;
			if (getenv("VF1_HOST_PUBLIC")) cfg.online.room_who = 2;
			if (!(test_watch ? online.watch(cfg.online, test_watch, err) : test_random ? online.find_random(cfg.online, err)
			      : test_join_code ? online.join_room(cfg.online, test_join_code, err) : online.host_room(cfg.online, err)))
				fprintf(stderr, "online: (test) %s\n", err.en().c_str());
		}
		online_pass();
		bool in_texture = false;           // this pass's picture is on the GPU already (or the last one stays)
		curtain = carried_curtain;
		carried_curtain = false;
		bool cut_hold = false;             // this pass's picture stays on purpose (a camera cut's hold)
		curtain_on_gpu = carried_on_gpu;
		carried_on_gpu = false;
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
				k.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, true);   // (as a keyboard's: the seed's letters read it)
				k.key.down = true;
				SDL_PushEvent(&k);
			}
		}
		// testing: VF_EVENTS_AT="2000:p,2050:reset,2060:targets" at those passes presses a key, or
		// sends the renderer's device reset or render targets reset (as Direct3D does); "stall" and
		// "unstall" stop and restart the sound device; "sleep" a minute passing at once (the machine asleep)
		static const char *events_at = getenv("VF_EVENTS_AT");
		for (const char *c = events_at; c && *c; c = strchr(c, ',') ? strchr(c, ',') + 1 : "") {
			char what[32] = "";
			long at = -1;
			if (sscanf(c, "%ld:%31[^,]", &at, what) != 2 || at != pass) continue;
			fprintf(stderr, "test: pass %ld: %s\n", at, what);
			if (!strcmp(what, "sleep")) { clock_t -= 60000000000ull; continue; }   // a minute asleep
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
		if (!vjoy_presses.empty())   // VF_TEST_VJOY: each press held 8 passes
			for (const auto &t : vjoy_presses) {
				const bool on = pass >= t.at && pass < t.at + 8;
				if (pass != t.at && pass != t.at + 8) continue;
				if (!test_vjoy && t.what != 'p' && t.what != 'q') continue;
				if (t.what == 'b') SDL_SetJoystickVirtualButton(test_vjoy, t.n, on);
				else if (t.what == 'h') SDL_SetJoystickVirtualHat(test_vjoy, 0, on ? Uint8(t.n) : SDL_HAT_CENTERED);
				else if (t.what == '+' || t.what == '-') SDL_SetJoystickVirtualAxis(test_vjoy, t.n, on ? (t.what == '+' ? 32767 : -32768) : 0);
				else if (t.what == 'q' && on) { SDL_Event q{}; q.type = SDL_EVENT_QUIT; SDL_PushEvent(&q); }
				else if (t.what == 'p' && on && !test_vjoy) attach_vjoy();
				if (on) fprintf(stderr, "test: pass %lld: the virtual stick: %c%d\n", (long long)pass, t.what, t.n);
			}
		if (window_rescale) { window_rescale = false; apply_window_scale(); menu.refresh(); }   // (the last pass's events asked)
		if (window_moved) { window_moved = false; remember_window(); }
		SDL_Event e;
		const bool menu_was_open = menu.is_open();
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_EVENT_QUIT) running = false;
			// (a Mac's open-URL event, as SDL gives it: a link from the web, the game running or just started by it)
			if (e.type == SDL_EVENT_DROP_FILE && e.drop.data && links::is_link(e.drop.data)) arg_link = std::string(e.drop.data).substr(0, 64);
			if (e.type == SDL_EVENT_WINDOW_MOVED || e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_MAXIMIZED ||
				e.type == SDL_EVENT_WINDOW_RESTORED)
				window_moved = true;
			// (fullscreen by the system's own way, a Mac's green button: remembered as the menu's is)
			if ((e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) && !fs_win_on &&
				cfg.fullscreen != (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN)) {
				cfg.fullscreen = e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
				cfg_dirty = true;
			}
			if (e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN && cfg.window_scale > 0) window_rescale = true;
			// Window size: the window resized by hand (a corner dragged, maximised): Free again
			if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && cfg.window_scale > 0 && !window_rescale && !is_fullscreen() &&
				(e.window.data1 != win_expect_w || e.window.data2 != win_expect_h)) {
				int pw = 0, ph = 0;
				SDL_GetWindowSize(win, &pw, &ph);
				if (pw == win_set_w && ph == win_set_h) window_rescale = true;   // (the same points: another density)
				else {
					fprintf(stderr, "window size: resized to %d x %d pixels: Free\n", e.window.data1, e.window.data2);
					cfg.window_scale = 0;
					cfg_dirty = true;
					menu.refresh();
				}
			}
			if (e.type == SDL_EVENT_JOYSTICK_ADDED) give_unknown_sticks();   // (a gamepad's added event follows for one given a layout)
			else if (e.type == SDL_EVENT_RENDER_TARGETS_RESET || e.type == SDL_EVENT_RENDER_DEVICE_RESET) {
				// Direct3D lost what render targets held (a device reset: every texture). The GPU's
				// parts are made again whatever is on show (their old textures can't be used), and
				// after a device reset the picture's texture and the screen filter's glass too; the
				// picture on show goes up again, drawn whole.
				gc.reset();
				if (e.type == SDL_EVENT_RENDER_DEVICE_RESET) {
					crt_paint.lost();
					for (auto &t : flag_tex) if (t) { SDL_DestroyTexture(t); t = nullptr; }   // (made again when next drawn)
					if (g3_tex[0]) {   // (the 3D's textures went with the device: back to the CPU from now on)
						fprintf(stderr, "the 3D on the GPU: the renderer's device was reset: its pictures come back to the CPU from now on\n");
						g3_unwrap();
						m.set_gpu3d_target(false);
					}
					make_texture();
					if (glass) { SDL_DestroyTexture(glass); glass = nullptr; }
				}
				upload();
			}
			// (F11 and Alt+Enter with the menu open too, unless it's waiting for a key: Alt+Enter was its Enter)
			else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && menu.is_open() && !menu.capturing()
					 && (e.key.scancode == SDL_SCANCODE_F11 || (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))))
				set_fullscreen(!is_fullscreen());
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
				else if (e.key.scancode == SDL_SCANCODE_P && !online.playing()) paused = !paused;   // (a match never pauses)
				else if (e.key.scancode == SDL_SCANCODE_F9) { perf.show = cfg.timings = !perf.show; cfg_dirty = true; if (!perf.show) present_shape(); }
				else if (e.key.scancode == SDL_SCANCODE_F3) reset_game();
				else if (e.key.scancode == SDL_SCANCODE_F5) player_scale_to(cfg.scale % 6 + 1);
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
				} else if (e.key.scancode == SDL_SCANCODE_F11 || fullscreen_key(e.key))
					set_fullscreen(!is_fullscreen());
			}
		}
		setup_poll();
		// the menu pauses the game and its sound (not a match's: it goes on)
		if (menu.is_open() != menu_was_open && audio && !online.playing()) {
			if (menu.is_open()) SDL_PauseAudioStreamDevice(audio);
			else SDL_ResumeAudioStreamDevice(audio);
		}
		if ((menu.is_open() != menu_was_open && !menu.is_open()) || std::exchange(menu_closed_elsewhere, false)) {
			// (not a match just left from the menu: Leave closes it before end_match has shown MATCH OVER, which it then didn't)
			if (online.phase() == Online::Phase::off && !match_on) { online.set_last_match({}); online.forget_failure(); }   // (MATCH OVER, COULDN'T CONNECT: just after, not for ever)
			// settings changed in the menu are saved as it closes (not only at quit: a crash or a
			// force quit would lose them); test runs save nothing
			if (cfg_dirty && quit_after < 0) {
				cfg.scale = m.render_scale() / aa_now();
				cfg.aspect = aspect_i;
				if (save_settings()) cfg_dirty = false;   // (else tried again at the next close, and at quit)
			}
			// the arcade settings are read as the game starts: restart it if one is different
			if (assign_before) {
				const bool changed = *assign_before != assignments_now();
				assign_before.reset();
				if (changed) reset_game();   // (the cheats' code patches with it)
			}
			pstate[0] = pstate[1] = pad_state{};   // the button that closed it isn't a move
			pstate[0].back_was = pstate[1].back_was = true;
			pstate[0].back_combo = pstate[1].back_combo = true;
			pstate[0].start_was = pstate[1].start_was = true;
			for (pad_state &s : pstate) s.menu_was = s.touch_was = s.sticks_was = true;
		}
		if (game_stopped()) restart_clock();   // (no catching up afterwards)
		if (!game_stopped()) {
			// Run the game by its clock (frame_clock): the frames it has passed, however often the
			// screen comes round (a 30 or 15 fps limit runs 2 or 4 frames a pass), and more at once if
			// the sound is about to run dry. Tab held: flat out. The controls are read for each frame
			// the game runs, so their timing is the game's, not the screen's.
			const bool *ks = SDL_GetKeyboardState(nullptr);
			const bool ff = ks[SDL_SCANCODE_TAB] && !online.playing();   // (never in a match)
			const uint64_t now_ns = SDL_GetTicksNS();
			// testing: VF_STEP_HZ=n steps the game's clock 1/n s a pass, whatever the time the pass took (with
			// VF_HIDDEN, VF_HZ high and VF_CAPTURE: every in-between picture, on a machine too slow to keep up)
			static const double fixed_dt = getenv("VF_STEP_HZ") ? 1.0 / atof(getenv("VF_STEP_HZ")) : 0;
			double dt = fixed_dt > 0 ? fixed_dt : double(now_ns - clock_t) / 1e9;
			// A gap over two seconds (the machine asleep, the window dragged, the app in the
			// background) isn't the game's time: the averages below start again rather than take it
			// in (they kept even steps off and the pace wrong for a second or two after waking), and
			// sound queued before it goes (it would play late). It counts as one frame's time, so a window
			// the system only lets run now and then still goes on. (Not a hitch of under two: a start's
			// half-second one took the sound queued.)
			if (fixed_dt == 0 && dt > 2.0) {
				static int told = 0;
				if (told++ < 5) fprintf(stderr, "a %.1f s gap (asleep?): the pacing starts again\n", dt);
				dt = 1.0 / FRAME_HZ;
				frame_clock = double(game_run);
				even_real = even_shown = 0;
				pass_avg = screen_hz_now > 0 ? 1.0 / screen_hz_now : 0;
				dt_avg = 1.0 / 120;
				queue_avg = QUEUED * frame_bytes;
				if (audio) SDL_ClearAudioStream(audio);
			}
			clock_t = now_ns;
			{   // (even steps: above, at frame_clock)
				static const bool no_even = getenv("VF_NO_EVEN_PACE") != nullptr;
				pass_avg += (dt - pass_avg) * 0.05;
				const double per = pass_avg * screen_hz_now, whole = std::round(per);
				const bool unseen_now = SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED);
				if (!no_even && fixed_dt == 0 && m.smooth() && !hidden && !unseen_now && vsync_now && screen_hz_now > 0 &&
					whole >= 1 && std::fabs(per - whole) < 0.12) {
					const double unit = 1.0 / screen_hz_now, step = std::max(1.0, std::round(dt * screen_hz_now)) * unit;
					even_real += dt;
					const double behind = even_real - even_shown;
					dt = std::fabs(behind - step) > 2 * unit ? behind : step + (behind - step) * 0.1;
					dt = std::max(0.0, dt);
					even_shown += dt;
					if (even_shown > 1e6) { even_real -= even_shown; even_shown = 0; }   // (kept small)
				} else even_real = even_shown = 0;
			}
			double rate = FRAME_HZ;
			int low = 0;                         // frames the sound needs at once (it's nearly run dry)
			bool sound_full = false;             // over 300 ms queued: no frames run this pass
			if (audio && !ff && !online.playing()) {   // (a match's sound: its own thread's, to the real clock)
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
			rate *= 1 - online.slow_down();   // (a match: ahead of the other side, a little slower till it's caught up)
			frame_clock += dt * rate;
			m.set_parts_mode(on_gpu());
			int due = int(int64_t(std::floor(frame_clock)) - game_run);
			// A pass that came late (a scene change's work, the machine busy elsewhere) doesn't run
			// the time it lost as frames back to back: that made the game, and smooth motion's
			// pictures, race to catch up (a 50 ms pass ran 3 frames in one, the next picture three
			// frames on). No more frames a pass than the screen's pace runs (one at 120 Hz, two at 60
			// or 50, more under an FPS limit); the rest of the lost time is dropped and the clock
			// carries on from there, its fraction kept, so the pictures go on at their pace. The
			// sound's slack (QUEUED frames) covers it, and the 1% trim makes it up.
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
			if (online.playing()) {
				// A match: each frame through the session, with this player's input (either player's controls play
				// this player's fighter; nothing while the menu's open); a frame that can't run yet (the other side
				// behind, or keeping in time) stays due, and the picture stays
				for (int i = 0; i < due && running && online.playing(); i++) {
					// (read_inputs writes the board's ports: put back, the board's are the session's to set, and its saved
					// state holds them)
					uint8_t ports_were[3];
					memcpy(ports_were, m.m_inputs, sizeof ports_were);
					const hotkey hk = read_inputs(m, pads, pstate, cfg.ctl);
					const uint8_t in0 = m.m_inputs[0], in1 = m.m_inputs[1], in2 = m.m_inputs[2];
					memcpy(m.m_inputs, ports_were, sizeof ports_were);
					const uint16_t bits = menu.is_open() ? 0 : uint16_t(vf1net::pack(in0, in1, 0) | vf1net::pack(in0, in2, 1) | step_carry);
					online.poll(double(SDL_GetTicksNS()) / 1e6);
					if (hk == hotkey::menu) open_menu();
					if (!online.step(bits)) {
						step_carry = uint16_t(bits & ONE_SHOT);
						break;
					}
					step_carry = 0;
					if (std::exchange(resimmed, false)) m.smooth_cut();   // (a rollback: the frames before aren't the ones shown)
					if (!after_frame(false)) break;
				}
			} else
			for (int i = 0; i < due; i++) {
				if (!frame_inputs()) break;
				m.run_frame();
				if (!after_frame(ff)) break;
			}
			frame_clock = std::min(frame_clock, double(game_run) + 0.999);   // (frames not run: the menu opened, a restart)
			if (curtain_on_gpu) { show_target = in_texture = true; tp.kind = 'F'; }
			tp.due = due;
			// testing: VF_TEST_LOAD, a slow start (the governor's tests)
			if (test_load_ms > 0 && due && started_at && SDL_GetTicksNS() < started_at + uint64_t(test_load_s * 1e9))
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
			if (on_gpu()) {
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
			// (a picture every refresh of the screen, vsync its pace, whatever its rate: no limit; the
			// budget, refilled by a clock that drifts against the screen's, skipped one now and then: a
			// refresh's stutter. VF3's d2265b3)
			double &screen_hz = screen_hz_now;
			bool &vsync_on = vsync_now;
			{ static uint64_t hz_t = 0;
			  if (SDL_GetTicksNS() - hz_t > 1000000000ull) {
				  hz_t = SDL_GetTicksNS();
				  if (const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win))) screen_hz = dm->refresh_rate;
				  int v = 0;
				  vsync_on = SDL_GetRenderVSync(ren, &v) && v != 0;
			  } }
			const bool every_refresh = screen_hz > 0 && !hidden && vsync_on;
			// (in a match, each frame as it is, when it's run: a picture between two frames a rollback may yet change showed
			// the change as a ghost, and with a rollback every few frames the motion jittered; as VF2's and VF3's, 57.5 a
			// second online)
			const bool match_frames = online.playing();
			if (m.smooth() && !sync.holding() && !unseen && (match_frames ? due > 0 : (due || every_refresh || smooth_budget >= 1))) {
				smooth_budget = std::max(0.0, smooth_budget - 1);
				const double now = match_frames ? double(m.frame_number()) : double(m.frame_number() - 1) + std::clamp(frame_clock - double(game_run), 0.0, 1.0);
				const uint64_t tr = SDL_GetTicksNS();
				const double t = std::min(now, double(m.frame_number()));
				tp.now = t;
				tp.kind = 'N';
				if (on_gpu()) {
					g3_prepare();
					const int r = m.render_parts(t, parts);
					const uint64_t tc = SDL_GetTicksNS();
					tp.draw = tc - tr;
					if (r == 0) tp.kind = 'C';
					else if (r == 2) tp.kind = 'W';
					const bool composed = r == 1 && gc.draw(parts, m.frame_w(), m.frame_h(), m.render_scale() > 1 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST) &&
										  (aa_now() == 1 || gc.resolve(aa_now(), shown_w(), shown_h()));
					if (r == 1) tp.compose = SDL_GetTicksNS() - tc;
					if (composed) {
						show_target = in_texture = true;
						++gpu_pictures;
						if (gpu_verified < 3 && (gpu_pictures - gpu_check_from == (gpu_verified == 0 ? 1 : gpu_verified == 1 ? 60 : 600) || gpu_recheck)) {
							if (!gpu_recheck) gpu_verified++;
							std::vector<uint32_t> back;
							const size_t px = size_t(m.frame_w()) * m.frame_h();
							const bool read = gc.read(back) && back.size() == px;
							m.redraw();
							picture_diff d = read ? compare(back, m.frame().data(), px, parts.d3_target >= 0) : picture_diff{};
							if (read && d.n && !d.wrong)
								fprintf(stderr, "the GPU's picture %d: %lld of %d x %d pixels differ by rounding (%lld by more than 8, colours by %d at most): kept on the GPU\n",
										gpu_pictures, d.n, m.frame_w(), m.frame_h(), d.big, d.most);
							if (read && d.wrong) {
								// both pictures beside the log, for a report (the first time only)
								if (!gpu_dumped) {
									gpu_dumped = true;
									write_png((dir + "/gpu_check_gpu.png").c_str(), back.data(), m.frame_w(), m.frame_h());
									write_png((dir + "/gpu_check_cpu.png").c_str(), m.frame().data(), m.frame_w(), m.frame_h());
								}
								// one picture off (a moment caught mid-change) isn't the GPU being wrong: the
								// next two are checked too, and only the three together send it to the CPU
								if (gpu_recheck < 2) {
									fprintf(stderr, "the GPU's picture %d differs from the CPU's (%lld of %d x %d pixels, %lld by more than 8, colours by %d at most): checking the next\n",
											gpu_pictures, d.n, m.frame_w(), m.frame_h(), d.big, d.most);
									gpu_recheck++;
									d.wrong = false;
								}
							} else if (read)
								gpu_recheck = 0;
							if (!read || d.wrong) {   // the CPU's picture, from now on
								if (read) fprintf(stderr, "pictures drawn on the CPU from now on: the GPU's picture %d differs from the CPU's (%lld of %d x %d pixels, %lld by more than 8, colours by %d at most)\n",
												  gpu_pictures, d.n, m.frame_w(), m.frame_h(), d.big, d.most);
								else fprintf(stderr, "pictures drawn on the CPU from now on: the GPU's picture %d couldn't be read back\n", gpu_pictures);
								if (cfg.renderer3d != 0) on_cpu_because(read ? tr::CPU_WHY_COMPOSE_WRONG : tr::CPU_WHY_READ_BACK, NEXT);
								gpu_2d = false;
								m.set_gpu_edge(false);
								gc.reset();
								show_target = in_texture = false;
							}
						}
						if (gpu_2d && gpu_check && gpu_pictures % gpu_check == 0) {   // the CPU's picture of the same moment
							std::vector<uint32_t> back;
							if (gc.read(back)) {
								m.redraw();
								const long long n = back.size() == size_t(m.frame_w()) * m.frame_h() ? compare(back, m.frame().data(), back.size(), parts.d3_target >= 0).n : -1;
								gpu_checked++;
								if (n) fprintf(stderr, "gpu check: picture %d: %lld pixels differ (frame %llu, moment %.4f)\n", gpu_pictures, n, (unsigned long long)m.frame_number(), t);
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
						if (cfg.renderer3d != 0) on_cpu_because(tr::CPU_WHY_COMPOSE_SIZE, m.render_scale() > 1 ? tr::CPU_TODO_LOWER_SCALE : NEXT);
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
		const bool show_frame_was_new = show_frame && !game_stopped();
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
			if (show_frame_was_new || (!game_stopped() && (sync.holding() || cut_hold))) shown++;
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
			// (the pictures or the 3D on the CPU with the setting not CPU: why, on screen once)
			// (not over online play: after it, and one up as a match is joined is taken down and said again after)
			if (cpu_cut_due) {
				cpu_cut_due = false;
				if (m.render_scale() > 1 || cfg.aa == 0) {
					cpu_cut = true;
					m.set_render_scale(1);
					m.set_edge_aa(false);
					sync.reset();
					make_texture(); present_shape(); redraw_if_stopped();
					fprintf(stderr, "the 3D on the CPU: drawn at 1x without antialiasing for this run (the settings kept)\n");
					if (cpu_why_new) cpu_why_cut = true;   // (with the notice)
				}
			}
			if (online.phase() != Online::Phase::off && !cpu_why.empty() && menu.toast_showing()) { menu.toast(""); cpu_why_new = true; }
			if (cpu_why_new && online.phase() == Online::Phase::off) {
				cpu_why_new = false;
				menu.toast(cpu_notice().str(), 12);
				fprintf(stderr, "the notice: %s\n", cpu_notice().en().c_str());   // (in the log too: what the player was told)
			}
			if (now - since >= 500000000) {
				menu.set_fps(cfg.show_fps ? std::to_string(int(std::lround(shown * 1e9 / double(now - since)))) + " FPS" : "");
				shown = 0; since = now;
			}
		}
		// otherwise only when there is a new picture; a still one (paused, the menu) is uploaded once,
		// and again only when it's redrawn, not every refresh
		{
			static bool was_stopped = false;
			const bool stopped = game_stopped();
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
		// (Random fighter played alone: the select it makes for itself (a moment) isn't shown: who you
		// got is a surprise when the fight comes in)
		if (!m.random_select_hidden())
			crt_paint.draw(ren, show_target ? (aa_now() > 1 && gc.resolved() ? gc.resolved() : gc.target()) : tex, nullptr, cfg.crt_colours, cfg.brightness, cfg.gamma);   // (CRT colours, brightness, gamma: the picture only)
		crt_on = cfg.crt_colours || cfg.brightness != 100 || cfg.gamma != 100;
		if (!m.random_select_hidden()) draw_hud_flags();
		draw_filter();
		menu.render(true);
		static const int shot_at = getenv("VF_SHOT_AT") ? atoi(getenv("VF_SHOT_AT")) : -1;   // testing: the window at that pass too
		if (static const char *rs = getenv("VF_TEST_RESIZE"); rs) {   // (tests: the window resized as by hand, "WxH@pass", "WxH+X+Y@pass")
			int rw = 0, rh = 0, rx = 0, ry = 0;
			long long at = 0;
			if (sscanf(rs, "%dx%d+%d+%d@%lld", &rw, &rh, &rx, &ry, &at) == 5 && frames_run == at) { SDL_SetWindowSize(win, rw, rh); SDL_SetWindowPosition(win, rx, ry); SDL_SyncWindow(win); }
			else if (sscanf(rs, "%dx%d@%lld", &rw, &rh, &at) == 3 && frames_run == at) { SDL_SetWindowSize(win, rw, rh); SDL_SyncWindow(win); }
		}
		if (getenv("VF_TEST_WINDOW_LOG") && frames_run % 20 == 0) {   // (tests: the window's pixels and Window size)
			int pw = 0, ph = 0, x = 0, y = 0;
			SDL_GetWindowSizeInPixels(win, &pw, &ph);
			SDL_GetWindowPosition(win, &x, &y);
			fprintf(stderr, "test: pass %lld: the window %d x %d pixels at %d, %d, window_scale %d, the picture %.4f wide\n",
			        (long long)frames_run, pw, ph, x, y, cfg.window_scale, m.aspect());
		}
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
		if (!game_stopped() && (sync.holding() || cut_hold)) perf.held++;
		// the next frame, if it'll be due by the next refresh, run while this present waits
		bool ahead = false;
		uint64_t ahead_t = 0;
		if (!no_game_thread && running && m.smooth() && !paused && !menu.is_open() && !sync.holding() && !online.playing() && !g_match_board &&
			!SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB] && frame_clock - double(game_run) >= 0.5 &&
			!(audio && SDL_GetAudioStreamQueued(audio) > frame_bytes * int(FRAME_HZ * 0.3)) &&   // (the game waiting for the sound: none ahead either)
			frame_inputs()) {
			ahead = true;
			ahead_t = SDL_GetTicksNS();
			gt.start([&] { m.run_frame(); });
		}
		SDL_RenderPresent(ren);
		{   // (VF3's pace log: a pass over 1.5 refreshes, by what it ran)
			const double refresh = screen_hz_now > 0 ? 1e9 / screen_hz_now : 8333333.0;
			if (double(SDL_GetTicksNS() - pass_t) > 1.5 * refresh) {
				if (tp.due) perf.long_frame++; else if (tp.kind == 'N' || tp.kind == 'W') perf.long_picture++; else perf.long_other++;
			}
		}
		if (ahead) {
			perf.ahead++;
			gt.wait();
			const uint64_t e = SDL_GetTicksNS() - ahead_t;
			perf.emu += e;
			perf.emu_max = std::max(perf.emu_max, e);
			curtain = false;
			curtain_on_gpu = false;
			after_frame(false);
			carried_curtain = curtain;
			carried_on_gpu = curtain_on_gpu;
		}
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
		// The game's backup (rankings, operator settings) saved within a minute of changing, so a
		// crash, a force quit or the Deck's power button loses at most that (every 3 s before: the
		// arcade's bookkeeping changes it in play, so it was written that often; and at quit as ever)
		if (quit_after < 0 && !g_match_board) {   // (a match's board holds the host's: this player's are given back after it)
			static uint64_t next_check = SDL_GetTicksNS() + 60000000000ull;
			if (SDL_GetTicksNS() >= next_check) {
				next_check = SDL_GetTicksNS() + 60000000000ull;
				const uint64_t h = backup_hash();
				if (h != saved_hash) {
					saver.put(nv_path, m.nvram().data(), m.nvram().size());
					saver.put(ee_path, m.eeprom(), model1::EEPROM_SIZE);
					saved_hash = h;
					save_rank_names(false);
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
			if (trace) fprintf(trace, "# pass %lld long passes: %d with a frame, %d a picture only, %d neither; frames run during the present %d\n",
							   (long long)frames_run, perf.long_frame, perf.long_picture, perf.long_other, perf.ahead);
			perf = perf_stats{ perf.show };
			perf.since = SDL_GetTicksNS();
		}

	}

	if (g3) fprintf(stderr, "the 3D at the end: %llu pictures drawn by the GPU, %llu checked against the CPU's, %llu differ; %llu drawn by the CPU instead, %llu GPU errors\n",
					(unsigned long long)m.m_g3.pictures, (unsigned long long)m.m_g3.checked, (unsigned long long)m.m_g3.differ,
					(unsigned long long)m.m_g3.cpu_fallback, (unsigned long long)m.m_g3.failed);
	if (m.m_g3.handed) fprintf(stderr, "the 3D at the end: %llu of the GPU's pictures straight into the renderer's texture\n", (unsigned long long)m.m_g3.handed);
	g3_unwrap();                         // (the renderer's wraps of its textures go first)
	m.set_gpu3d(nullptr);
	g3.reset();                          // (its device goes before SDL does)
	if (gpu_check) fprintf(stderr, "gpu check: %d pictures put together on the GPU checked, %d differ\n", gpu_checked, gpu_differ);
	gc.release();                        // (its textures and shader go before the renderer)
	crt_paint.release();
	msound.stop();
	online.stop();   // (closed in a match: the other side is told at once, not after a silence)
	Online::flush(1500);   // (and the lobby told: a room closed, a search left)
	links::stopped(dir);
	if (chime_stream) SDL_DestroyAudioStream(chime_stream);
	if (g_match_board) {   // (this player's own backup, not the host's, saved)
		m.use_io_standin(false);
		memcpy(m.nvram().data(), own_backup.data(), std::min(own_backup.size(), m.nvram().size()));
		memcpy(m.eeprom(), own_eeprom, sizeof own_eeprom);
		m.set_ranking_names(own_rank_names);
		g_match_board = false;
	}
	remember_window();
	if (quit_after >= 0 && !getenv("VF_SAVE")) { SDL_Quit(); return 0; }   // test runs leave the saves and settings alone (VF_SAVE: they save, as a player's)
	keep_arcade_coins();
	saver.finish();   // (an autosave still being written, done before the last save: the same files)
	save_blob(nv_path, m.nvram().data(), m.nvram().size());
	save_blob(ee_path, m.eeprom(), model1::EEPROM_SIZE);
	save_rank_names(true);
	if (cfg_dirty) {
		cfg.scale = m.render_scale() / aa_now();
		cfg.aspect = aspect_i;
		save_settings();
	}
	SDL_Quit();
	return 0;
}
