// The players' controls: keys and pad buttons for each game's moves, analog axes, and chosen pads.
// Edited in the menu (Controls), kept in settings.ini. Each game says what its controls are (a
// control_profile: src/games/<game>/controls.h).
#pragma once
#include <SDL3/SDL.h>
#include "../games/game.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The host's own controls (the directions, which the d-pad and left stick also give, and the
// cabinet's Start, Coin and Menu), and button slots each game names for its own (its labels and
// settings.ini names: src/games/<game>/controls.h, as game::vf::C_PUNCH = C_BUTTON_2).
enum control_action {
	C_UP, C_DOWN, C_LEFT, C_RIGHT,
	C_BUTTON_1, C_BUTTON_2, C_BUTTON_3, C_BUTTON_4, C_BUTTON_5, C_BUTTON_6, C_BUTTON_7, C_BUTTON_8, C_BUTTON_9,
	C_START, C_COIN, C_MENU, C_COUNT
};
inline const char *const CONTROL_NAMES[C_COUNT] = {
	"Up", "Down", "Left", "Right",
	"Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6", "Button 7", "Button 8", "Button 9",
	"Start", "Coin", "Menu" };
inline const char *const CONTROL_KEYS[C_COUNT] = {
	"up", "down", "left", "right",
	"button_1", "button_2", "button_3", "button_4", "button_5", "button_6", "button_7", "button_8", "button_9",
	"start", "coin", "menu" };

// Pad controls: an SDL gamepad button, or one of the triggers.
enum { PAD_NONE = -1, PAD_LT = 100, PAD_RT = 101 };

// What a game makes of the controls (src/games/<game>/controls.h): the shared host reads the
// keyboard and pads through it, and the Controls menu shows it.
struct control_profile
{
	game::id title;                                   // (its I/O profile: the analog axes' defaults)
	std::array<const char *, C_COUNT> labels{};       // the menu's names; none: the game has no such control
	std::array<const char *, C_COUNT> names{};        // settings.ini's key_<name> and pad_<name>; none: CONTROL_KEYS
	std::array<const char *, C_COUNT> key_names{};    // key_<name> when it isn't name()
	std::array<uint8_t, C_COUNT> bits{};              // the player's input byte's bits it presses (active high)
	std::array<uint8_t, 2> start_bits{};              // each player's start button in the system byte
	std::array<std::array<SDL_Scancode, C_COUNT>, 2> keys{};   // the defaults, per player
	std::array<int, C_COUNT> pad;                     // (PAD_NONE until set)
	std::vector<int> pad_actions;                     // the pad buttons page's, in order (those with bits play)
	// a key renamed in settings.ini: read from its old name when the new one isn't there, `was` then `now`
	struct renamed_key { int action; const char *old_name; SDL_Scancode was, now; };
	std::vector<renamed_key> renamed_keys;
	// keys on the I/O board's analog channels (player 1): `low`'s key gives 0, `high`'s 255
	// (-1: none); `dpad`: the d-pad's left and right as well
	struct analog_key { int channel, low, high; bool dpad; };
	std::vector<analog_key> analog_keys;
	const char *pad_page_help = "";                   // after each pad button's help
	const char *pad_buttons_help = "";                // the player page's Pad buttons
	const char *analog_help = "";                     // each analog axis

	control_profile() { pad.fill(PAD_NONE); }
	// a control the game has: its menu label (none: CONTROL_NAMES) and settings name (none: CONTROL_KEYS)
	void add(int action, const char *label = nullptr, const char *name = nullptr)
	{
		labels[action] = label ? label : CONTROL_NAMES[action];
		names[action] = name;
	}
	bool visible(int action) const { return labels[action] != nullptr; }
	const char *label(int action) const { return labels[action] ? labels[action] : CONTROL_NAMES[action]; }
	const char *name(int action) const { return names[action] ? names[action] : CONTROL_KEYS[action]; }
	const char *key_name(int action) const { return key_names[action] ? key_names[action] : name(action); }
};

struct player_controls
{
	SDL_Scancode key[C_COUNT] = {};
	int pad[C_COUNT] = {};
	std::array<game::host_axis, 8> analog_axis{
		game::host_axis::none, game::host_axis::none, game::host_axis::none, game::host_axis::none,
		game::host_axis::none, game::host_axis::none, game::host_axis::none, game::host_axis::none
	};
	std::string pad_choice = "auto";   // "auto" (the next free pad), "none", or a pad's GUID

	static player_controls defaults(int player, const control_profile &profile)
	{
		player_controls c;
		for (int a = 0; a < C_COUNT; a++) {
			c.key[a] = profile.keys[player ? 1 : 0][a];
			c.pad[a] = profile.pad[a];
		}
		c.analog_axis = game::get_io_profile(profile.title).host_axis_defaults;
		return c;
	}

	void load(const std::map<std::string, std::string> &kv, int player, const control_profile &profile)
	{
		const std::string p = player ? "p2_" : "p1_";
		for (int a = 0; a < C_COUNT; a++) {
			if (!profile.visible(a)) continue;
			auto k = kv.find(p + "key_" + profile.key_name(a));
			const control_profile::renamed_key *renamed = nullptr;
			if (k == kv.end())
				for (const auto &r : profile.renamed_keys)
					if (r.action == a && (k = kv.find(p + "key_" + r.old_name)) != kv.end()) { renamed = &r; break; }
			if (k != kv.end()) {
				key[a] = k->second == "none" ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(k->second.c_str());
				if (renamed && key[a] == renamed->was) key[a] = renamed->now;
			}
			auto b = kv.find(p + "pad_" + profile.name(a));
			if (b != kv.end()) pad[a] = pad_from_name(b->second);
		}
		for (size_t channel = 0; channel < analog_axis.size(); channel++) {
			auto a = kv.find(p + "analog_" + std::to_string(channel));
			if (a != kv.end()) analog_axis[channel] = axis_from_name(a->second);
		}
		auto c = kv.find(p + "pad");
		if (c != kv.end()) pad_choice = c->second;
	}

	void save(FILE *f, int player, const control_profile &profile) const
	{
		const char *p = player ? "p2_" : "p1_";
		fprintf(f, "%spad = %s\n", p, pad_choice.c_str());
		for (int a = 0; a < C_COUNT; a++)
			if (profile.visible(a))
				fprintf(f, "%skey_%s = %s\n", p, profile.key_name(a), key[a] ? SDL_GetScancodeName(key[a]) : "none");
		for (int a = 0; a < C_COUNT; a++)
			if (profile.visible(a))
				fprintf(f, "%spad_%s = %s\n", p, profile.name(a), pad_save_name(pad[a]).c_str());
		for (size_t channel = 0; channel < analog_axis.size(); channel++)
			fprintf(f, "%sanalog_%zu = %s\n", p, channel, axis_save_name(analog_axis[channel]));
	}

	static bool pad_down(SDL_Gamepad *g, int code)
	{
		if (!g || code == PAD_NONE) return false;
		if (code == PAD_LT) return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
		if (code == PAD_RT) return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
		return SDL_GetGamepadButton(g, SDL_GamepadButton(code));
	}

	static const char *axis_label(game::host_axis axis)
	{
		switch (axis) {
		case game::host_axis::left_x: return "Left stick X";
		case game::host_axis::left_y: return "Left stick Y";
		case game::host_axis::right_x: return "Right stick X";
		case game::host_axis::right_y: return "Right stick Y";
		case game::host_axis::left_trigger: return "Left trigger";
		case game::host_axis::right_trigger: return "Right trigger";
		default: return "-";
		}
	}

	static game::host_axis axis_from_event(SDL_GamepadAxis axis)
	{
		switch (axis) {
		case SDL_GAMEPAD_AXIS_LEFTX: return game::host_axis::left_x;
		case SDL_GAMEPAD_AXIS_LEFTY: return game::host_axis::left_y;
		case SDL_GAMEPAD_AXIS_RIGHTX: return game::host_axis::right_x;
		case SDL_GAMEPAD_AXIS_RIGHTY: return game::host_axis::right_y;
		case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return game::host_axis::left_trigger;
		case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return game::host_axis::right_trigger;
		default: return game::host_axis::none;
		}
	}

	static int axis_value(SDL_Gamepad *g, game::host_axis axis)
	{
		if (!g) return 0;
		switch (axis) {
		case game::host_axis::left_x: return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
		case game::host_axis::left_y: return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
		case game::host_axis::right_x: return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
		case game::host_axis::right_y: return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY);
		case game::host_axis::left_trigger: return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
		case game::host_axis::right_trigger: return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
		default: return 0;
		}
	}

	static uint8_t analog_value(int raw, game::analog_kind kind, uint8_t neutral)
	{
		if (kind == game::analog_kind::pedal) {
			const int value = std::clamp(raw, 0, 32767);
			return uint8_t(neutral + (value * (255 - neutral) + 16383) / 32767);
		}
		const int value = std::clamp(raw, -32768, 32767) + 32768;
		return uint8_t((value * 255 + 32767) / 65535);
	}

	// For people: the Xbox-style names most pads (and the Deck) print.
	static std::string pad_label(int code)
	{
		switch (code) {
		case PAD_NONE: return "-";
		case PAD_LT: return "LT";
		case PAD_RT: return "RT";
		case SDL_GAMEPAD_BUTTON_SOUTH: return "A";
		case SDL_GAMEPAD_BUTTON_EAST: return "B";
		case SDL_GAMEPAD_BUTTON_WEST: return "X";
		case SDL_GAMEPAD_BUTTON_NORTH: return "Y";
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
		case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Left stick press";
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Right stick press";
		case SDL_GAMEPAD_BUTTON_START: return "Start";
		case SDL_GAMEPAD_BUTTON_BACK: return "View";
		case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
		case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad click";
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
		default: {
			const char *s = SDL_GetGamepadStringForButton(SDL_GamepadButton(code));
			return s ? s : "?";
		}
		}
	}

private:
	static const char *axis_save_name(game::host_axis axis)
	{
		switch (axis) {
		case game::host_axis::left_x: return "left_x";
		case game::host_axis::left_y: return "left_y";
		case game::host_axis::right_x: return "right_x";
		case game::host_axis::right_y: return "right_y";
		case game::host_axis::left_trigger: return "left_trigger";
		case game::host_axis::right_trigger: return "right_trigger";
		default: return "none";
		}
	}
	static game::host_axis axis_from_name(const std::string &name)
	{
		for (game::host_axis axis : { game::host_axis::left_x, game::host_axis::left_y,
									  game::host_axis::right_x, game::host_axis::right_y,
									  game::host_axis::left_trigger, game::host_axis::right_trigger })
			if (name == axis_save_name(axis)) return axis;
		return game::host_axis::none;
	}
	static std::string pad_save_name(int code)
	{
		if (code == PAD_NONE) return "none";
		if (code == PAD_LT) return "lefttrigger";
		if (code == PAD_RT) return "righttrigger";
		const char *s = SDL_GetGamepadStringForButton(SDL_GamepadButton(code));
		return s ? s : "none";
	}
	static int pad_from_name(const std::string &n)
	{
		if (n == "lefttrigger") return PAD_LT;
		if (n == "righttrigger") return PAD_RT;
		SDL_GamepadButton b = SDL_GetGamepadButtonFromString(n.c_str());
		return b == SDL_GAMEPAD_BUTTON_INVALID ? PAD_NONE : int(b);
	}
};
