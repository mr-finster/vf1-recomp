// Virtua Racing's scripted play: the tools' run (attract, coins, a race steering from side to side,
// shifting), with an "alternate" scenario that picks the other course, and the checkpoints a run
// must reach (the course selection, the grid, the race, the results).
#include "../script.h"
#include "../../machine/model1.h"

namespace game::vr {

namespace {

class play final : public auto_play
{
public:
	play(bool alternate, bool checks) : m_alternate(alternate), m_checks(checks) {}

	script_inputs frame(int f) override
	{
		script_inputs s;
		const int phase = f;
		if (phase >= 3000 && phase < 3006) s.pressed[0] |= 0x01;   // coin
		if (phase >= 3060 && phase < 3066) s.pressed[0] |= 0x01;   // coin
		if (phase >= 3120 && phase < 3126) s.pressed[0] |= 0x10;   // start
		if (m_alternate && phase >= 3068 && phase < 3130) s.analog[0] = 0xe0;   // the wheel picks the other course
		if (phase >= 3130) {
			const int driving_frame = phase - 3130;
			const int wheel_phase = driving_frame % (m_alternate ? 900 : 480);
			s.analog[0] = m_alternate
				? (phase < 3340 ? 0xe0 : (wheel_phase < 300 ? 0x20 : wheel_phase < 600 ? 0xe0 : 0x80))
				: (wheel_phase < 240
					? 0x20 + wheel_phase * 0xc0 / 239
					: 0xe0 - (wheel_phase - 240) * 0xc0 / 239);
			s.analog[1] = m_alternate                                   // accelerator
				? (driving_frame % 600 < 480 ? 0x7f : 0x30)
				: (driving_frame % 900 < 720 ? 0x7f : 0x30);
			s.analog[2] = m_alternate                                   // brake
				? (driving_frame % 600 >= 480 ? 0x7f : 0x30)
				: (driving_frame % 900 >= 720 ? 0x7f : 0x30);
			const int shift_period = m_alternate ? 240 : 180;
			const int shift_phase = driving_frame % shift_period;
			if (shift_phase < 6) s.pressed[1] |= 0x10;                  // shift down
			else if (shift_phase >= shift_period / 2 && shift_phase < shift_period / 2 + 6)
				s.pressed[1] |= 0x20;                                     // shift up
		}
		return s;
	}

	std::string check(int f, model1 &m) override
	{
		if (!m_checks) return {};
		// the course's label on the grid: red for the first course, cyan for the other
		auto course_label_colors = [&]() {
			std::array<int, 2> counts{};
			for (int y = 350; y < 380; y++)
				for (int x = 400; x < model1::SCREEN_W; x++) {
					const uint32_t pixel = m.frame().data()[y * model1::SCREEN_W + x];
					const int r = (pixel >> 16) & 0xff, g = (pixel >> 8) & 0xff, b = pixel & 0xff;
					if (r > 160 && g < 80 && b < 80) counts[0]++;
					if (b > 160 && r < 80 && g > 100 && g < 230) counts[1]++;
				}
			return counts;
		};
		auto reached = [&](const char *name, bool &recorded) {
			if (!recorded) {
				const auto [red, cyan] = course_label_colors();
				printf("VR scenario reached: %s (frame %d, course-label red/cyan=%d/%d)\n", name, f, red, cyan);
				recorded = true;
			}
		};
		const uint8_t mode = m.game_mode();
		const uint32_t camera = m.camera();
		const int quads = m.frame_quads();
		if (!m_track_select && f >= 3000 && mode == 25 && camera == 0x00502150 && quads <= 10)
			reached("track selection", m_track_select);
		if (m_track_select && !m_race_grid && mode == 29 && camera == 0x00502000 && quads > 1000) {
			reached("race grid", m_race_grid);
			const auto [red, cyan] = course_label_colors();
			if ((m_alternate && red <= cyan) || (!m_alternate && cyan <= red)) {
				char text[160];
				snprintf(text, sizeof text, "VR selected the wrong course at frame %d (red/cyan label pixels=%d/%d)", f, red, cyan);
				return text;
			}
		}
		if (m_race_grid && !m_racing && mode == 9 && camera == 0x00502000 && quads > 1000)
			reached("active race", m_racing);
		if (m_racing && !m_results && mode == 29 && camera == 0x005022d0 && quads >= 1000 && quads < 2000)
			reached("course results", m_results);
		return {};
	}

	std::string finish() override
	{
		if (m_checks && !(m_track_select && m_race_grid && m_racing && m_results))
			return "VR scenario suite did not reach every required checkpoint";
		return {};
	}

	int min_frames() const override { return m_checks ? 20000 : 0; }

private:
	bool m_alternate, m_checks;
	bool m_track_select = false, m_race_grid = false, m_racing = false, m_results = false;
};

}

std::unique_ptr<auto_play> make_auto_play(const std::string &scenario, uint32_t, bool checks)
{
	if (!scenario.empty() && scenario != "alternate") return nullptr;
	return std::make_unique<play>(scenario == "alternate", checks);
}

}
