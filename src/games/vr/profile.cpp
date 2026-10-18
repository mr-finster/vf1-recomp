#include "profile.h"

namespace game::vr {

const profile PROFILE{
	ID, "Virtua Racing", "vr", "games/vr/vr_map.txt",
	"run `make vr` with your roms/vr.zip, or tools/romset.py --game vr",
	0x17f0, 0x3002, 18
};

const io_profile IO_PROFILE{
	{
		"Coin 1", "Coin 2", "Test", "Service", "Start", "VR1 (Red)", "VR2 (Blue)", "VR3 (Yellow)",
		"VR4 (Green)", "", "", "", "Shift Down", "Shift Up", "", "",
		"", "", "", "", "", "", "", ""
	},
	{ "Wheel", "Accelerator", "Brake", "", "", "", "", "" },
	{ "VR_WHEEL", "VR_ACCEL", "VR_BRAKE", "", "", "", "", "" },
	{ 0x80, 0x30, 0x30, 0xff, 0xff, 0xff, 0xff, 0xff },
	{ host_axis::left_x, host_axis::right_trigger, host_axis::left_trigger,
	  host_axis::none, host_axis::none, host_axis::none, host_axis::none, host_axis::none },
	{ analog_kind::centered, analog_kind::pedal, analog_kind::pedal }
};

}
