#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

class model1;

namespace game::vr {

class tgp_state
{
public:
	struct lookup_result
	{
		float height = 0;
		uint32_t entry = 0;
	};

	void select_track(int track) { m_selected_track = track; }
	int selected_track() const { return m_selected_track; }
	void set_base(float x, float y, float z, float radius) { m_base = { x, y, z, radius }; }
	void set_circle(float x, float y, float radius) { m_circle = { x, y, radius }; }
	void set_collision_box(const std::array<float, 12> &box) { m_collision_box = box; }
	const std::array<float, 12> &collision_box() const { return m_collision_box; }
	float circle_test(float x, float y) const
	{
		x -= m_circle[0];
		y -= m_circle[1];
		return std::sqrt(x*x + y*y) - m_circle[2];
	}
	float vector_length(float x, float y, float z) const
	{
		x -= m_base[0];
		y -= m_base[1];
		z -= m_base[2];
		return std::sqrt(x*x + y*y + z*z) - m_base[3];
	}

	std::array<uint32_t, 12> read_quad(std::span<const uint32_t> data, size_t index) const
	{
		std::array<uint32_t, 12> result{};
		const size_t offset = word(data, 0x20 + m_selected_track) + 16 * index;
		for (size_t i = 0; i < result.size(); i++)
			result[i] = word(data, offset + i);
		return result;
	}

	std::array<uint32_t, 3> read_tri(std::span<const uint32_t> data, size_t index) const
	{
		std::array<uint32_t, 3> result{};
		const size_t offset = word(data, 0x20 + m_selected_track) + 16 * index + 12;
		for (size_t i = 0; i < result.size(); i++)
			result[i] = word(data, offset + i);
		return result;
	}

	uint32_t read_info(std::span<const uint32_t> data, size_t index) const
	{
		return word(data, word(data, 0x20 + m_selected_track) + 16 * index + 15);
	}

	lookup_result lookup(std::span<const uint32_t> data, size_t index, float x, float z,
		float height_query) const
	{
		const size_t index_offset = word(data, 0x10 + m_selected_track) + index;
		const size_t data_offset = word(data, 0x20 + m_selected_track);
		const uint32_t length = word(data, index_offset);
		float best_distance = -1;
		lookup_result result;

		for (uint32_t i = 0; i < length; i++) {
			const uint32_t base_position = word(data, index_offset + 1 + i);
			const size_t position = data_offset + size_t(base_position) * 0x10;
			auto point = [&](size_t n) { return std::bit_cast<float>(word(data, position + n)); };
			const float ax = point(12);
			const float ay = point(14);
			const float az = point(13);
			for (int j = 0; j < 4; j++) {
				float t1, t2;
				const int k = (j + 1) & 3;
				tri_calc_pq(ax, ay, point(3*j), point(3*j+2), point(3*k), point(3*k+2), x, z, &t1, &t2);
				if (t1 >= 0 && t2 >= 0 && t1 + t2 <= 1) {
					const float candidate_height = az + t1 * (point(3*j+1) - az) +
						t2 * (point(3*k+1) - az);
					const float distance = (height_query - candidate_height) *
						(height_query - candidate_height);
					if (best_distance == -1 || distance < best_distance) {
						best_distance = distance;
						result.height = candidate_height;
						result.entry = base_position + i;
					}
				}
			}
		}
		return result;
	}

private:
	static uint32_t word(std::span<const uint32_t> data, size_t index)
	{
		return index < data.size() ? data[index] : 0;
	}

	static void tri_calc_pq(float ax, float ay, float bx, float by, float cx, float cy,
		float px, float py, float *t1, float *t2)
	{
		bx -= ax;
		cx -= ax;
		px -= ax;
		by -= ay;
		cy -= ay;
		py -= ay;
		const float d = bx * cy - by * cx;
		*t1 = (px * cy - py * cx) / d;
		*t2 = (bx * py - by * px) / d;
	}

	int m_selected_track = 0;
	std::array<float, 4> m_base{};
	std::array<float, 3> m_circle{};
	std::array<float, 12> m_collision_box{};
};

// Virtua Racing's track and collision commands from MAME's TGP simulation, working on the
// machine's TGP FIFOs. VR runs its real TGP program (MB86233), so nothing sends these yet; a VR
// HLE command table would, by command number.
class tgp_track_hle
{
public:
	explicit tgp_track_hle(model1 &machine) : m_machine(machine) {}
	// The words command f takes from the input FIFO, or -1 when it isn't one of these.
	static int arguments(uint32_t f);
	// Runs command f with its arguments already in the FIFO; false when it isn't one of these.
	bool run(uint32_t f);
	const tgp_state &state() const { return m_state; }

private:
	struct command { uint32_t number; int arguments; void (tgp_track_hle::*handler)(); };
	static const command commands[];
	static const command *find(uint32_t f);
	void track_select();
	void load_base();
	void track_read_quad();
	void track_read_tri();
	void vlength();
	void track_read_info();
	void colbox_set();
	void colbox_test();
	void track_lookup();
	void col_setcirc();
	void col_testpt();
	void car_move();

	model1 &m_machine;
	tgp_state m_state;
};

}
