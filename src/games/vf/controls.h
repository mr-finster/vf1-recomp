// Virtua Fighter's controls for the desktop host (src/host/controls.h): a stick and three buttons
// per player, the throws and combinations on buttons of their own.
#pragma once
#include "../../host/controls.h"
#include "profile.h"

namespace game::vf {

// The player's input byte as the game's own INPUT TEST names the buttons (active high here)
enum : uint8_t {
	PL_PUNCH = 0x01, PL_KICK = 0x02, PL_GUARD = 0x04,
	PL_DOWN = 0x10, PL_UP = 0x20, PL_RIGHT = 0x40, PL_LEFT = 0x80,
};

// its buttons in the shared slots (src/host/controls.h)
enum : int { C_GUARD = C_BUTTON_1, C_PUNCH, C_KICK, C_GUARD_PUNCH, C_PUNCH_KICK, C_GUARD_KICK, C_ALL_THREE };

inline const control_profile CONTROLS = [] {
	control_profile c;
	c.title = ID;
	for (int a : { C_UP, C_DOWN, C_LEFT, C_RIGHT }) c.add(a);
	c.add(C_GUARD, "Guard", "guard");
	c.add(C_PUNCH, "Punch", "punch");
	c.add(C_KICK, "Kick", "kick");
	c.add(C_GUARD_PUNCH, "Guard + Punch", "guard_punch");
	c.add(C_PUNCH_KICK, "Punch + Kick", "punch_kick");
	c.add(C_GUARD_KICK, "Guard + Kick", "guard_kick");
	c.add(C_ALL_THREE, "All three", "all_three");
	for (int a : { C_START, C_COIN, C_MENU }) c.add(a);
	c.bits[C_UP] = PL_UP; c.bits[C_DOWN] = PL_DOWN; c.bits[C_LEFT] = PL_LEFT; c.bits[C_RIGHT] = PL_RIGHT;
	c.bits[C_GUARD] = PL_GUARD; c.bits[C_PUNCH] = PL_PUNCH; c.bits[C_KICK] = PL_KICK;
	c.bits[C_GUARD_PUNCH] = PL_GUARD | PL_PUNCH;
	c.bits[C_PUNCH_KICK] = PL_PUNCH | PL_KICK;
	c.bits[C_GUARD_KICK] = PL_GUARD | PL_KICK;
	c.bits[C_ALL_THREE] = PL_GUARD | PL_PUNCH | PL_KICK;
	c.start_bits = { 0x10, 0x20 };
	auto &k1 = c.keys[0], &k2 = c.keys[1];
	k1[C_UP] = SDL_SCANCODE_UP; k1[C_DOWN] = SDL_SCANCODE_DOWN; k1[C_LEFT] = SDL_SCANCODE_LEFT; k1[C_RIGHT] = SDL_SCANCODE_RIGHT;
	k1[C_GUARD] = SDL_SCANCODE_Z; k1[C_PUNCH] = SDL_SCANCODE_X; k1[C_KICK] = SDL_SCANCODE_C; k1[C_START] = SDL_SCANCODE_1;
	k2[C_UP] = SDL_SCANCODE_I; k2[C_DOWN] = SDL_SCANCODE_K; k2[C_LEFT] = SDL_SCANCODE_J; k2[C_RIGHT] = SDL_SCANCODE_L;
	k2[C_GUARD] = SDL_SCANCODE_B; k2[C_PUNCH] = SDL_SCANCODE_N; k2[C_KICK] = SDL_SCANCODE_M; k2[C_START] = SDL_SCANCODE_2;
	// Saturn layout: A guard, B punch, C kick on the west, south and east buttons
	c.pad[C_GUARD] = SDL_GAMEPAD_BUTTON_WEST;
	c.pad[C_PUNCH] = SDL_GAMEPAD_BUTTON_SOUTH;
	c.pad[C_KICK] = SDL_GAMEPAD_BUTTON_EAST;
	c.pad[C_GUARD_PUNCH] = SDL_GAMEPAD_BUTTON_NORTH;
	c.pad[C_PUNCH_KICK] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
	c.pad[C_GUARD_KICK] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
	c.pad[C_ALL_THREE] = PAD_RT;
	c.pad[C_START] = SDL_GAMEPAD_BUTTON_START;
	c.pad[C_MENU] = SDL_GAMEPAD_BUTTON_BACK;   // (Coin: none; Menu: View tapped)
	// (the d-pad and left stick always move)
	c.pad_actions = { C_GUARD, C_PUNCH, C_KICK, C_GUARD_PUNCH, C_PUNCH_KICK, C_GUARD_KICK, C_ALL_THREE, C_START, C_COIN, C_MENU };
	c.pad_page_help = " The d-pad and left stick always move.";
	c.pad_buttons_help = "Change which buttons guard, punch, kick, do the combinations, start, add a coin and open the menu.";
	c.analog_help = "Select, then move a stick axis or squeeze a trigger. Delete clears the binding.";
	return c;
}();

}
