// Clean scene changes. When a game changes scene, its pieces arrive on different frames (worked
// out on Virtua Fighter): the 3D goes first (a few frames of sky alone), the new 2D layers are
// written over a frame or two, and the new scene's 3D builds up (the floor, then the fighters a
// game frame later, with the sky jumping into place). This holds the picture only while it would
// be wrong, for as little as it can:
//   - frames with no 3D at all (the scene taken down, a curtain, a 2D screen) are shown as they
//     come, as on the arcade;
//   - when 3D arrives after a frame with none, it is held until the polygon count has settled
//     (compared with two frames before: a game's logic may run at 30 Hz, as VF's does), two
//     frames as a rule;
//   - a cut between stages (the ending's replays) brings the new stage's sky a few frames before
//     its 3D, with the old stage's 3D still up in between: a sky that turns into another stage's
//     holds too, and while the sky has changed but the polygons are still the old scene's (within
//     an eighth of its count), the hold goes on, for up to 10 frames (the gap is 5-7);
//   - nothing is held past 12 frames (it was 45: 783 ms before a 2D screen).
// It only decides which picture to show: the game runs on untouched.
#pragma once
#include "../machine/model1.h"
#include <cstdlib>

class scene_sync
{
public:
	bool enabled = true;

	void reset() { m_holding = false; m_held = 0; m_q1 = m_q2 = 0; m_sky = -2; m_sky_before = -2; m_q_before = m_q_shown = 0; }
	// A scene is starting from nothing (after a boot or reset): hold until it is whole.
	void start() { reset(); m_holding = true; m_held = 1; }

	// After a frame has run: true to show it, false to keep showing the last one shown.
	bool show(model1 &m)
	{
		const int q = m.frame_quads(), q1 = m_q1, q2 = m_q2;
		m_q2 = m_q1;
		m_q1 = q;
		if (!enabled) { m_holding = false; return true; }
		// the stage sky: a sky map that isn't one (black, a screen between) counts as no sky (-1),
		// and that change too is a cut when the 3D stays up
		const int sky = m.sky_identity(), sky_was = m_sky;
		m_sky = sky;
		auto start_hold = [&] { m_holding = true; m_held = 1; m_sky_before = sky_was; m_q_before = m_q_shown; };
		if (!m_holding) {
			// (the ending's replays) a stage cut that keeps the old 3D up; or the 3D arriving after a
			// frame with none: the new scene is built over two game frames (the floor, then the
			// fighters), so it's held until its polygon count has settled. Frames with no 3D at all
			// (the scene taken down, a curtain, a 2D screen) are shown as they come: holding them
			// froze the picture for 7-13 frames at every scene change, 45 before a 2D screen.
			const bool cut = q > 0 && sky_was != -2 && sky != sky_was;
			const bool arriving = q > 0 && q1 == 0;
			if (cut || arriving) { start_hold(); return false; }
			m_q_shown = q;
			return true;
		}
		if (q == 0) { m_holding = false; m_q_shown = 0; return true; }   // nothing in 3D again: shown as it is
		const bool settled = q > 0 && q2 > 0 && std::abs(q - q2) * 4 <= q;
		// a new stage's sky over the old stage's 3D (it comes a few frames before the new 3D): wait on
		const bool stale = (sky < 0 || sky != m_sky_before) && m_q_before > 0 && std::abs(q - m_q_before) * 8 <= m_q_before;
		// (the new 3D can have as many polygons as the old: after 10 frames a settled picture is taken anyway)
		if ((settled && (!stale || m_held >= 10)) || ++m_held > 12) { m_holding = false; m_q_shown = q; return true; }
		if (settled) ++m_held;
		return false;
	}

	bool holding() const { return m_holding; }

private:
	bool m_holding = false;
	int m_held = 0, m_q1 = 0, m_q2 = 0;
	int m_sky = -2, m_sky_before = -2;   // the stage sky last frame (-2: not seen yet), and before the hold
	int m_q_shown = 0, m_q_before = 0;   // the polygons in the last frame shown, and in the one before the hold

	static bool is_blank(const model1 &m) { return m.frame_blank(); }   // (drawn whole or in parts)
};
