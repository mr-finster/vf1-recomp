// Virtua Racing's data the shared video code works from (game::video_traits).
#include "gameplay.h"

namespace game::vr {

// The 3D blended between its 30 game frames a second, each part paired by what the game says it
// is (a car's wheels change model as they turn and the trackside reuses its models along the
// track, so pairing parts by place in the list put tyres between wheels). Its routine at FF8500
// writes a matrix command for a car's part (body, livery, wheels) with the car's record in R19,
// FF85D8 the pit crew's with theirs in R25; the records lie at 400D00 on, 0x280 apart, and any
// other value is the still world (the track, R19 pointing in ROM; the SEGA logo too, which is drawn
// without the camera: blend_by_records tells it by its place). The camera it drew with is at 501B00. No scenery split for the shadow fix, no hi-res skies.
const video_traits VIDEO{ .smooth_frames = true,
	.records = draw_records{ { { 0xff8500, 19 }, { 0xff85d8, 25 } }, 0x400000, 0x410000, 0x501b00 } };

}
