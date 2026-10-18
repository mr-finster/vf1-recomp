#include "../../../src/games/game.h"
#include "../../../src/games/vr/controls.h"
#include <string>
#include <cassert>

int main()
{
	const auto &vr = game::get(game::vr::ID);
	assert(vr.title.name == "vr");
	assert(game::find("vr") == game::vr::ID);
	assert(vr.display_name == "Virtua Racing");
	assert(vr.romset == "vr");
	assert(vr.recompiler_map == "games/vr/vr_map.txt");
	assert(vr.mode_ram == 0x3002 && vr.test_menu_mode == 18);   // 13 in attract, 18 in the test menu

	const auto &vr_io = game::get_io_profile(game::vr::ID);
	assert(vr_io.digital_controls[12] == "Shift Down");
	assert(vr_io.digital_controls[13] == "Shift Up");
	assert(vr_io.analog_controls[0] == "Wheel");
	assert(vr_io.analog_controls[1] == "Accelerator");
	assert(vr_io.analog_controls[2] == "Brake");
	assert(vr_io.analog_environment[0] == "VR_WHEEL");
	assert(vr_io.analog_environment[1] == "VR_ACCEL");
	assert(vr_io.analog_environment[2] == "VR_BRAKE");
	assert(vr_io.analog_neutral[0] == 0x80);
	assert(vr_io.analog_neutral[1] == 0x30);
	assert(vr_io.analog_neutral[2] == 0x30);
	assert(vr_io.host_axis_defaults[0] == game::host_axis::left_x);
	assert(vr_io.host_axis_defaults[1] == game::host_axis::right_trigger);
	assert(vr_io.host_axis_defaults[2] == game::host_axis::left_trigger);
	assert(vr_io.analog_kinds[0] == game::analog_kind::centered);
	assert(vr_io.analog_kinds[1] == game::analog_kind::pedal);
	assert(vr_io.analog_kinds[2] == game::analog_kind::pedal);

	const auto controls = player_controls::defaults(0, game::vr::CONTROLS);
	assert(controls.key[C_LEFT] == SDL_SCANCODE_LEFT);
	assert(controls.key[C_RIGHT] == SDL_SCANCODE_RIGHT);
	assert(controls.key[game::vr::C_ACCELERATOR] == SDL_SCANCODE_UP);
	assert(controls.key[game::vr::C_BRAKE] == SDL_SCANCODE_DOWN);
	assert(controls.key[C_UP] == SDL_SCANCODE_X);
	assert(controls.key[C_DOWN] == SDL_SCANCODE_Z);
	assert(controls.key[game::vr::C_ACCELERATOR] != controls.key[C_RIGHT]);
	assert(controls.pad[C_BUTTON_3] == PAD_NONE);
	assert(controls.analog_axis[1] == game::host_axis::right_trigger);
	assert(game::vr::CONTROLS.visible(game::vr::C_ACCELERATOR));
	assert(std::string(game::vr::CONTROLS.key_name(game::vr::C_BRAKE)) == "brake");
	assert(!game::vr::CONTROLS.visible(C_BUTTON_3));   // (two of the button slots: the pedals)
	assert(std::string(game::vr::CONTROLS.name(C_UP)) == "up");   // (pad_up, as before the shift's keys)
	assert(std::string(game::vr::CONTROLS.label(C_UP)) == "Shift Up");
	assert(std::string(game::vr::CONTROLS.key_name(C_UP)) == "shift_up");
	assert(game::vr::CONTROLS.start_bits[0] == game::vr::CONTROLS.start_bits[1]);
	// every pad action the page shows is one the game has
	for (int a : game::vr::CONTROLS.pad_actions) assert(game::vr::CONTROLS.visible(a));
}
