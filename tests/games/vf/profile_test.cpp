#include "../../../src/games/vf/controls.h"
#include "../../../src/games/vf/profile.h"
#include <algorithm>
#include <cassert>
#include <string>

int main()
{
	const auto &vf = game::get(game::vf::ID);
	assert(vf.title.name == "vf");
	assert(game::find("vf") == game::vf::ID && !game::find("xx"));
	assert(vf.display_name == "Virtua Fighter");
	assert(vf.romset == "vf");

	const auto &vf_io = game::get_io_profile(game::vf::ID);
	assert(vf_io.digital_controls[8] == "Punch");
	assert(vf_io.analog_neutral[0] == 0xff);
	assert(vf_io.host_axis_defaults[0] == game::host_axis::none);

	// a fighting game's controls: no pedals, a start button per player
	using namespace game::vf;
	assert(CONTROLS.visible(C_GUARD) && CONTROLS.visible(C_ALL_THREE));
	assert(std::string(CONTROLS.key_name(C_PUNCH_KICK)) == "punch_kick");
	assert(!CONTROLS.visible(C_BUTTON_8) && !CONTROLS.visible(C_BUTTON_9));
	assert(std::string(game::vf::CONTROLS.key_name(C_UP)) == "up");
	assert(game::vf::CONTROLS.start_bits[0] != game::vf::CONTROLS.start_bits[1]);
	// every pad action the page shows is one the game has
	for (int a : game::vf::CONTROLS.pad_actions) assert(game::vf::CONTROLS.visible(a));
	const auto &pad = CONTROLS.pad_actions;
	assert(std::find(pad.begin(), pad.end(), C_UP) == pad.end());
}
