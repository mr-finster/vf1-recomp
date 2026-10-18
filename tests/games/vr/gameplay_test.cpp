#include "../../../src/games/vr/gameplay.h"
#include <cassert>

int main()
{
	const auto callbacks = game::vr::create_gameplay();
	assert(callbacks.wide_scene);
	assert(!callbacks.wide_scene(0, false));
	assert(!callbacks.wide_scene(0, true));
	assert(!callbacks.wide_scene(0x00502150, false));
	assert(callbacks.wide_scene(1, false));
	assert(callbacks.wide_scene(0x5020f0, false));
}
