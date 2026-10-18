#include "../../../src/games/vr/tgp.h"
#include <cassert>
#include <cmath>
#include <vector>

static void set_float(std::vector<uint32_t> &data, size_t index, float value)
{
	data[index] = std::bit_cast<uint32_t>(value);
}

int main()
{
	std::vector<uint32_t> data(256);
	game::vr::tgp_state state;
	assert(state.selected_track() == 0);
	assert(std::abs(state.vector_length(3, 4, 0) - 5.0f) < 0.0001f);

	state.select_track(7);
	assert(state.selected_track() == 7);

	state.set_base(1, 2, 3, 1);
	assert(std::abs(state.vector_length(1, 2, 4) - 0.0f) < 0.0001f);
	assert(std::abs(state.vector_length(1, 2, 5) - 1.0f) < 0.0001f);

	state.set_circle(1, 2, 2);
	assert(std::abs(state.circle_test(1, 2) + 2.0f) < 0.0001f);
	assert(std::abs(state.circle_test(4, 6) - 3.0f) < 0.0001f);
	const std::array<float, 12> box{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
	state.set_collision_box(box);
	assert(state.collision_box() == box);

	state.select_track(1);
	data[0x21] = 128;
	for (uint32_t i = 0; i < 16; i++)
		data[144 + i] = 0x100 + i;
	const auto quad = state.read_quad(data, 1);
	for (uint32_t i = 0; i < quad.size(); i++)
		assert(quad[i] == 0x100 + i);
	const auto tri = state.read_tri(data, 1);
	assert((tri == std::array<uint32_t, 3>{ 0x10c, 0x10d, 0x10e }));
	assert(state.read_info(data, 1) == 0x10f);

	data[0x11] = 40;
	data[40] = 1;
	data[41] = 0;
	const float vertices[15] = {
		-1, 10, -1, 1, 10, -1, 1, 10, 1, -1, 10, 1, 0, 10, 0
	};
	for (size_t i = 0; i < std::size(vertices); i++)
		set_float(data, 128 + i, vertices[i]);
	const auto result = state.lookup(data, 0, 0.25f, 0.25f, 7.0f);
	assert(std::abs(result.height - 10.0f) < 0.0001f);
	assert(result.entry == 0);

	const auto outside = state.lookup(data, 0, 3.0f, 3.0f, 7.0f);
	assert(outside.height == 0.0f);
	assert(outside.entry == 0);
}
