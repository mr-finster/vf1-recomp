#include "profile.h"

namespace game::vf {

const profile PROFILE{
	ID, "Virtua Fighter", "vf", "games/vf/vf_map.txt",
	"run tools/romset.py on your vf.zip",
	0x17f0, 0x3002, 3
};

const io_profile IO_PROFILE{
	{
		"Coin 1", "Coin 2", "Test", "Service", "Start 1", "Start 2", "", "",
		"Punch", "Kick", "Guard", "", "Down", "Up", "Right", "Left",
		"Punch", "Kick", "Guard", "", "Down", "Up", "Right", "Left"
	},
	{},
	{},
	{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
	{ host_axis::none, host_axis::none, host_axis::none, host_axis::none,
	  host_axis::none, host_axis::none, host_axis::none, host_axis::none },
	{ analog_kind::centered, analog_kind::centered, analog_kind::centered, analog_kind::centered,
	  analog_kind::centered, analog_kind::centered, analog_kind::centered, analog_kind::centered }
};

}
