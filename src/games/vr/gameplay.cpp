#include "gameplay.h"

namespace game::vr {

namespace {
constexpr uint32_t TRACK_SELECT_CAMERA = 0x00502150;
}

gameplay_callbacks create_gameplay()
{
	gameplay_callbacks callbacks;
	callbacks.wide_scene = [](uint32_t camera, bool) {
		return camera != 0 && camera != TRACK_SELECT_CAMERA;
	};
	return callbacks;
}

}
