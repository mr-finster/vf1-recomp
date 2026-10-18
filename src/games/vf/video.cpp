// Virtua Fighter's data the shared video code works from (game::video_traits).
#include "gameplay.h"

namespace game::vf {

const video_traits VIDEO{
	// the logic runs at 30 Hz: the 3D is blended between its frames (model1_interp.cpp)
	.smooth_frames = true,
	// the stages' models come first in the polygon data, the fighters' from 80000 on: a fighter's
	// shadow shades the floor, never a fighter
	.scenery_polys_end = 0x80000,
	// The records' places in the program ROM (all in the bank at 1100000), by the export's names.
	// Wolf's stage has a top row (in ROMX at 2e7aa4): the dithered blend from the sky's colour into
	// the clouds. Drawn from the panorama's own top row instead, the clouds started at a hard line
	// above 1x.
	.skies = {
		{ "beach_clouds", 0x110002c }, { "canyon_mesas", 0x11117a8 }, { "night_city", 0x1134370 },
		{ "lake_mountain", 0x114c878 }, { "desert_dunes", 0x11655b4 }, { "karst_mountains", 0x116af60 },
		{ "dusk_clouds", 0x118a0e8 },
		{ "sunset_clouds", 0x11190b8, 0x2e7aa4 },   // Wolf's stage
	},
	.sky_tile_map = 2, .sky_first_row = 6,   // streamed into tile map 2 from row 6 (704300)
	// The sky scroll routine (ff49a1, at the start of vblank) works from the camera: across, its
	// heading (508004); up and down, 508006; each mixed with the last game frame's (508008,
	// 50800a). 500501 bit 0: the frame between two of the game's.
	.sky_scroll = sky_camera{ 0x508004, 0x508006, 0x508008, 0x50800a, 0x500501 },
	// The name entry: the wheel's letters N-Z, the symbols and BACK take their colours from the
	// block at 94000, END from 94040, which the game fills with FFFF at boot and never sets: MAME
	// (and we, before) drew them black. Cabinet captures (the user's, 2026-10-13: "Congratulations!"
	// 26 and 24) show them blue and END red, front and back alike; the palette's pens 0x372 (blue)
	// and 0x373 (red) are those colours and nothing else uses them. Only the wheel's letter models
	// (380a36-381c2d), which no other scene draws.
	.unset_texture_pens = {
		{ 0x380a36, 0x381c2d, 0x94000, 0x372 },
		{ 0x380a36, 0x381c2d, 0x94040, 0x373 },
	},
};

}
