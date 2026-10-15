// The players' controls: keys and pad buttons for each of VF's moves, and which pad each player
// holds. Edited in the menu (Controls), kept in settings.ini.
#pragma once
#include <SDL3/SDL.h>
#include <map>
#include <string>

enum control_action {
	C_UP, C_DOWN, C_LEFT, C_RIGHT, C_GUARD, C_PUNCH, C_KICK,
	C_GUARD_PUNCH, C_PUNCH_KICK, C_GUARD_KICK, C_ALL_THREE, C_START, C_COIN, C_MENU, C_COUNT
};
inline const char *const CONTROL_NAMES[C_COUNT] = {
	"Up", "Down", "Left", "Right", "Guard", "Punch", "Kick",
	"Guard + Punch", "Punch + Kick", "Guard + Kick", "All three", "Start", "Coin", "Menu" };
inline const char *const CONTROL_KEYS[C_COUNT] = {
	"up", "down", "left", "right", "guard", "punch", "kick",
	"guard_punch", "punch_kick", "guard_kick", "all_three", "start", "coin", "menu" };

// Pad controls: an SDL gamepad button, or one of the triggers.
enum { PAD_NONE = -1, PAD_LT = 100, PAD_RT = 101 };

struct player_controls
{
	SDL_Scancode key[C_COUNT];
	int pad[C_COUNT];                  // the d-pad and left stick always move as well
	std::string pad_choice = "auto";   // "auto" (the next free pad), "none", or a pad's GUID

	static player_controls defaults(int player)
	{
		player_controls c;
		static const SDL_Scancode k1[C_COUNT] = {
			SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
			SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C,
			SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_1, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN };
		static const SDL_Scancode k2[C_COUNT] = {
			SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L,
			SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_M,
			SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_2, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN };
		// Saturn layout: A guard, B punch, C kick on the west, south and east buttons
		static const int pad[C_COUNT] = {
			PAD_NONE, PAD_NONE, PAD_NONE, PAD_NONE,
			SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
			SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
			PAD_RT, SDL_GAMEPAD_BUTTON_START, PAD_NONE, SDL_GAMEPAD_BUTTON_BACK };   // (Coin: none; Menu: View tapped)
		for (int a = 0; a < C_COUNT; a++) {
			c.key[a] = (player ? k2 : k1)[a];
			c.pad[a] = pad[a];
		}
		return c;
	}

	void load(const std::map<std::string, std::string> &kv, int player)
	{
		const std::string p = player ? "p2_" : "p1_";
		for (int a = 0; a < C_COUNT; a++) {
			auto k = kv.find(p + "key_" + CONTROL_KEYS[a]);
			if (k != kv.end()) key[a] = k->second == "none" ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(k->second.c_str());
			auto b = kv.find(p + "pad_" + CONTROL_KEYS[a]);
			if (b != kv.end()) pad[a] = pad_from_name(b->second);
		}
		auto c = kv.find(p + "pad");
		if (c != kv.end()) pad_choice = c->second;
	}

	void save(FILE *f, int player) const
	{
		const char *p = player ? "p2_" : "p1_";
		fprintf(f, "%spad = %s\n", p, pad_choice.c_str());
		for (int a = 0; a < C_COUNT; a++)
			fprintf(f, "%skey_%s = %s\n", p, CONTROL_KEYS[a], key[a] ? SDL_GetScancodeName(key[a]) : "none");
		for (int a = C_GUARD; a < C_COUNT; a++)
			fprintf(f, "%spad_%s = %s\n", p, CONTROL_KEYS[a], pad_save_name(pad[a]).c_str());
	}

	static bool pad_down(SDL_Gamepad *g, int code)
	{
		if (!g || code == PAD_NONE) return false;
		if (code == PAD_LT) return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
		if (code == PAD_RT) return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
		return SDL_GetGamepadButton(g, SDL_GamepadButton(code));
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
