// Smooth motion above 1x. VF's game logic runs at 30 Hz: it builds a new display list every
// second frame, so the picture moves 30 times a second whatever the screen can show. In smooth
// mode each new list is kept (with the sky's scroll), and the picture is drawn for any moment
// between the last two: every object's matrix (its place, turn and the camera's) is blended
// with the same object's in the list before, and the sky's scroll likewise. The game itself runs
// untouched; the picture is one game frame (two video frames) behind it.
#include "model1.h"
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// An object in a list: its polygons' address, and which time that address comes in the list.
uint64_t object_key(uint32_t poly_adr, uint32_t nth) { return uint64_t(poly_adr) << 20 | nth; }

} // namespace

void model1::set_smooth(bool on)
{
	if (on == m_smooth) return;
	m_smooth = on;
	m_logic[0] = m_logic[1] = m_logic_prev = logic_frame{};
	m_shown = false;
}

// A list's matrix slots: each matrix command (0xb) and the objects drawn with it. Swapped models
// (a hand, a face) keep their slot, so slots are what is matched between two lists.
void model1::index_objects(logic_frame &lf)
{
	lf.slots.clear();
	lf.slots.push_back({});                        // objects before the first matrix
	const uint16_t *l = lf.list.data();
	auto rdi = [&](int a) { return uint32_t(l[a & 0x7fff] | l[(a + 1) & 0x7fff] << 16); };
	auto rdf = [&](int a) { uint32_t v = rdi(a); float f; memcpy(&f, &v, 4); return f; };
	int off = 0;
	float light[3] = { 0, 0, 0 };
	bool has_light = false;
	for (int guard = 0; guard < 0x10000; guard++) {
		switch (rdi(off)) {
		case 0: off += 2; break;
		case 1: case 0x41:
			lf.slots.back().polys.push_back(rdi(off + 4));
			lf.objects++;
			off += 8;
			break;
		case 2: {                                  // as skip_direct
			off += 18;
			for (;;) {
				const uint32_t t = rdi(off + 2) & 3;
				if (!t) break;
				off += t == 2 ? 12 : 20;
			}
			off += 4;
			break;
		}
		case 3: off += 16; break;
		case 4: off += 6 + (rdi(off + 4) + 1) * 2; break;
		case 5: case 6: off += 6 + rdi(off + 4) * 2; break;
		case 7: case 8: off += 4; break;
		case 9: case 0xc: off += 6; break;
		case 0xa:                                  // the light vector, as set_light_direction keeps it
			{
				const glm::vec3 v = view_t::light_vector(rdf(off + 2), rdf(off + 4), rdf(off + 6));
				light[0] = v.x; light[1] = v.y; light[2] = v.z;
				has_light = true;
			}
			off += 8;
			break;
		case 0xb: {
			slot sl;
			for (int i = 0; i < 12; i++) sl.mat[i] = rdf(off + 2 + 2 * i);
			memcpy(sl.light, light, sizeof light);
			sl.has_light = has_light;
			lf.slots.push_back(sl);
			off += 26;
			break;
		}
		default: return;
		}
	}
}

// Instead of drawing the frame: keep its list when it is a new one. The object count stands in
// for the polygon count (the clean scene changes go by it); a frame with nothing in 3D is drawn,
// so a black curtain can be told from a scene being built.
void model1::record_frame()
{
	// No new list this frame (the game draws its 3D every other frame at times, as on the name
	// entry's turning ring): the board keeps showing the last one, so the picture carries on.
	int objects = m_logic[1].list.empty() ? 0 : m_logic[1].objects;
	if ((m_listctl[1] & 0x1f) == 0x1f) {
		const uint16_t *shown = picture_list();
		logic_frame lf;
		lf.list.assign(shown, shown + 0x8000);
		uint64_t h = 1469598103934665603ull;
		for (uint16_t w : lf.list) h = (h ^ w) * 1099511628211ull;
		lf.hash = h;
		lf.frame = int(m_frame);
		lf.hscr = m_tiles.tile_r(0x5002);
		lf.vscr = m_tiles.tile_r(0x5006);
		index_objects(lf);
		// (Random fighter's pose easing, off: the game blends its moves into their stance itself now
		// that it finds the right one (STANCE_LOOKUP), and eased on top a jump attack ramped up out
		// of the old pose. VF_POSE_EASE: testing, as it was)
		static const bool pose_ease = getenv("VF_POSE_EASE") != nullptr;
		if (pose_ease) pose_slots(lf);
		if (static const bool jl = getenv("VF_JOINTLOG") != nullptr; jl) joint_log(lf);   // TESTONLY
		objects = lf.objects;
		static const bool log = getenv("VF_LOG_SMOOTH") != nullptr;
		if (lf.hash != m_logic[1].hash) {
			// A list with nothing in it moved (the camera, the fighters), a list after one where
			// they all moved: the game missed an update (the attract's throws close in), the next
			// list catching up two lists' move. Blended, the picture stood a game frame (4 refreshes
			// at 120 Hz) then went on at twice the speed: a judder at the close-ups (render_smooth).
			const logic_frame &z = m_logic[0], &a = m_logic[1];
			lf.move = list_move(a, lf);
			// (nearly nothing moved, the skips 1-3% of the move before: an ordinary move, not a cut's
			// or a fast swing's, after which the camera often stands)
			lf.still = lf.move >= 0 && a.move > 0.01 && a.move < 0.3 && lf.move < 0.05 * a.move && !z.list.empty() && lf.frame - a.frame == a.frame - z.frame;
			lf.carried = lf.still ? a.move : 0;
			m_logic_prev = std::move(m_logic[0]);
			m_logic[0] = std::move(m_logic[1]);
			m_logic[1] = std::move(lf);
			if (m_random_fighter && pose_ease) pose_transition(m_logic[0], m_logic[1]);
		}
	}
	// the sky's scroll, every frame, with the fractions the game dropped: the smooth sky goes by these
	const uint16_t h = m_tiles.tile_r(0x5002), v = m_tiles.tile_r(0x5006);
	sky_sample &sm = m_sky_hist[m_frame % SKY_HIST];
	sm = { int(m_frame), h, v, float(h & 0x1ff) + m_sky_frac_h, float(v & 0x3ff) + m_sky_frac_v };
	sky_settled(sm.lands_h, sm.lands_v);
	// and the next frame's, when the game's routine wrote this one: it runs again at the next vblank
	// from what is in its RAM now (the game moves the camera just after the routine, not before)
	if (m_sky_written == m_frame && m_sky_read_h && m_sky_read_v) {
		const sky_scroll next = sky_scroll_from_ram(!m_sky_between);
		sm.nx = next.hx;
		sm.ny = next.vy;
		sm.next = true;
	}
	m_render_done = 0;            // the list's uploads are made at the next vblank, as when frames are skipped
	m_last_quads = objects;
	if (objects == 0) {
		// nothing in 3D (a curtain, a 2D screen): the next scene starts afresh, not blended from this
		m_logic[0] = m_logic[1] = m_logic_prev = logic_frame{};
		// (when the host puts pictures together on the GPU: drawn in parts, so the 2D screens and
		// curtains don't cost a whole picture drawn at the render scale and uploaded every frame)
		if (m_parts_mode) render_parts(double(m_frame), m_own_parts, false);
		else {
			screen_update(m_bitmap, m_bitmap.cliprect());
			m_unshown = true;
			m_frame_parts = false;
		}
	}
}

// How far the parts moved between two lists of the same parts (each slot's first model the same,
// in order): the median of their moves (a fast limb or two don't count), else -1.
double model1::list_move(const logic_frame &a, const logic_frame &b)
{
	if (a.slots.size() != b.slots.size() || a.slots.empty()) return -1;
	std::vector<double> d;
	for (size_t i = 0; i < a.slots.size(); i++) {
		const slot &sa = a.slots[i], &sb = b.slots[i];
		if (sa.polys.empty() != sb.polys.empty() || (!sa.polys.empty() && sa.polys[0] != sb.polys[0])) return -1;
		if (sa.polys.empty()) continue;
		const double dx = sb.mat[9] - sa.mat[9], dy = sb.mat[10] - sa.mat[10], dz = sb.mat[11] - sa.mat[11];
		d.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
	}
	if (d.empty()) return -1;
	std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
	return d[d.size() / 2];
}

void model1::smooth_cut()
{
	m_logic[0] = m_logic[1];
	m_logic_prev = logic_frame{};
}

// The picture for a moment `now`, in frames (the newest frame's number plus how far into it).
bool model1::render_smooth(double now, bool shown)
{
	m_draw_time = now - 2;     // the picture is a game frame (two video frames) behind
	// what redraw() needs to draw it again: the moment and the frame it's drawn in
	auto drawn = [&] {
		m_unshown = !shown || m_parts;                  // (in parts: frame() holds only the 3D)
		m_frame_parts = m_parts != nullptr;
		if (shown) { m_shown = true; m_shown_now = now; m_shown_frame = m_frame; }
	};
	if (m_logic[1].list.empty()) { screen_update(m_bitmap, m_bitmap.cliprect()); drawn(); return true; }
	const logic_frame &a = m_logic[0], &b = m_logic[1];
	double alpha = 1;
	if (!a.list.empty() && b.frame > a.frame) {
		// the picture is a span behind (the newer list is in hand when the older one's moment has
		// passed), and never less than a game frame: two lists a frame apart (a scene's first, as it
		// is built) went at a frame behind, and the picture stood still a frame when the next list
		// came two later
		const double span = b.frame - a.frame, behind = std::max(span, 2.0);
		alpha = std::clamp((now - behind - a.frame) / span, 0.0, 1.0);
	}
	m_alpha = float(alpha);
	if (!m_redrawing) m_last_alpha = m_alpha;
	m_replaying = true;
	m_smooth_stats[0] = m_smooth_stats[1] = m_smooth_stats[2] = 0;
	blend_slots(a, b, alpha);   // (Smooth motion off too: its cuts' history kept)
	if (m_arcade_frames) {
		// Smooth motion off: a list as the game made it, never blended: the older till the newer's own
		// moment, so the 3D moves 30 times a second as on the board; the sky each frame's own scroll
		m_alpha = 1;
		if (!m_redrawing) m_last_alpha = alpha < 1 ? 0.f : 1.f;
		m_replay_older = alpha < 1;
		m_replay_extra = false;
		m_sky_settling_now = false;
		if (m_blend_cut && b.frame != m_cut_frame) m_cut_frame = b.frame;   // (the cuts kept as with it on: switched back on just after one, its sky settles as ever)
	} else {
		// A cut can't be blended: the older view stays until the newer one's moment (as the blend
		// would), so the cut lands when the game's sky, written two frames later, changes too. It
		// stood still meanwhile, a game frame (4 refreshes at 120 Hz) at every camera cut; now it
		// carries on from the list before it as it was moving, easing to half speed at the cut, three
		// quarters of a list on at most (straight on, a fast camera swung into views the game never
		// showed).
		// An update the game skipped (a list with nothing moved, record_frame) carries on the same
		// way, the move before it eased out over it (half a list's move), and the list after starts
		// from there, catching up the rest: no stop, no lurch.
		const bool cut = m_blend_cut;
		if (!cut && a.still && a.frame == m_still_carried && alpha < 1 && b.move > 0) {
			alpha += (1 - alpha) * std::min(0.5, 0.5 * a.carried / b.move);
			blend_slots(a, b, alpha);
			m_alpha = float(alpha);
			if (!m_redrawing) m_last_alpha = m_alpha;
		}
		// (and carries on through it until the next list is in, a frame late at times)
		// (Into a cut, three quarters of a list on, slowing to half speed as the cut lands: eased to a
		// stop, the last refreshes before every cut stood still)
		m_replay_older = (cut && alpha < 1) || b.still;
		m_replay_extra = false;
		if (m_replay_older && !m_logic_prev.list.empty() && b.frame - a.frame == a.frame - m_logic_prev.frame) {
			const double c = std::min(alpha, 1.0);
			blend_slots(m_logic_prev, a, 1 + (cut ? c - 0.25 * c * c : 0.5 * (1 - (1 - c) * (1 - c))));
			m_replay_extra = !m_blend_cut;
			if (!cut && m_replay_extra) m_still_carried = b.frame;   // (the list after it catches up from there)
		}
		m_blend_cut = cut;
		if (cut && b.frame != m_cut_frame) m_cut_frame = b.frame;
		if (m_random_fighter && !cut && !m_replay_older && getenv("VF_POSE_EASE")) pose_apply(b);   // (testing: record_frame)
		// After a cut the game moves the sky into place over a few frames (at most 4): the picture
		// before the cut used to stay until it stopped jumping, a picture held for 2-7 refreshes at
		// every replay's camera cut; now the new view shows at once, its sky where it settles
		// (smooth_sky_shift).
		// (the camera's RAM has the new view from the cut's frame on; the scroll the game writes
		// follows it a frame later, mixed with the old view's, and lands one or two frames after that)
		// Where it lands is too far to move the sky to (the tile layer moves 24 pixels at most, a
		// panorama 160; a cut swings it hundreds, up or down too): the sky as the game has it, as the
		// arcade shows it. The picture before the cut used to stay instead, 1-9 refreshes at a replay's
		// cuts (up to 100 ms with the hold before the cut), mostly for views looking down at the floor
		// with no sky in them.
		m_sky_settling_now = false;
		if (!m_replay_older && m_cut_frame >= 0 && int(m_frame) - 1 <= m_cut_frame + 4) {
			const sky_sample &cur = m_sky_hist[(m_frame - 1) % SKY_HIST];
			m_sky_settling_now = cur.frame == int(m_frame) - 1 && (cur.lands_h != cur.h || cur.lands_v != cur.v) && sky_reaches(cur);
		}
		if (!m_redrawing) {
			// testing: where the 3D is in this picture, on average (the trace's check that each picture
			// lies between its neighbours: a picture blended from the wrong pair stands out)
			const logic_frame &l = m_replay_older ? a : b;
			const bool own = !m_replay_older || m_replay_extra;
			double sum[3] = { 0, 0, 0 };
			int n = 0;
			for (size_t i = 0; i < l.slots.size(); i++) {
				if (l.slots[i].polys.empty()) continue;
				const float *mt = own && i < m_blend_ok.size() && m_blend_ok[i] ? m_blend[i].data() : l.slots[i].mat;
				for (int k = 0; k < 3; k++) sum[k] += mt[9 + k];
				n++;
			}
			for (int k = 0; k < 3; k++) m_pic_place[k] = n ? float(sum[k] / n) : 0;
		}
	}
	const auto render_done = m_render_done;     // emulation state: drawing again mustn't touch it
	const int quads = m_last_quads;
	screen_update(m_bitmap, m_bitmap.cliprect());
	m_render_done = render_done;
	m_last_quads = quads;
	m_replaying = false;
	m_replay_older = false;
	m_replay_extra = false;
	drawn();
	static const bool log = getenv("VF_LOG_SMOOTH") != nullptr;
	if (log && alpha < 1)
		fprintf(stderr, "R %.2f a=%.2f unmatched %d jumped %d blended %d\n", now, alpha, m_smooth_stats[0], m_smooth_stats[1], m_smooth_stats[2]);
	m_alpha = 1;
	return true;
}

// As a frame starts while the picture on screen is the current one: what it was drawn from, in
// case frames run on without a new picture shown (a scene change held, the hold after a cut) and
// it has to be drawn again (redraw, at new settings while paused). Every frame in play: about
// 300 KB copied (the tile graphics only when they changed).
void model1::keep_still()
{
	still_state &s = m_still;
	s.frame = m_frame;
	s.logic[0] = m_logic[0];
	s.logic[1] = m_logic[1];
	s.logic_prev = m_logic_prev;
	s.cut_frame = m_cut_frame;
	std::copy(std::begin(m_sky_hist), std::end(m_sky_hist), s.sky_hist);
	s.tile_ram = m_tiles.tile_ram;
	if (s.char_gen != m_tiles.char_gen || s.char_ram.size() != m_tiles.char_ram.size()) {
		s.char_ram = m_tiles.char_ram;
		s.char_gen = m_tiles.char_gen;
	}
	std::copy(std::begin(m_pens), std::end(m_pens), s.pens.begin());
	s.palette = m_paletteram16_ram;
	s.color_xlat = m_color_xlat_ram;
	std::copy(std::begin(m_view->lightparams), std::end(m_view->lightparams), s.lightparams.begin());
	s.light = m_view->light;
	s.spec_enable = m_view->spec_enable;
	memcpy(s.camera, &m_ram_b[0x17f0], 4);
	s.fight_cam = m_fight_cam;
	s.sky_now = m_sky_now; s.sky_last = m_sky_last;
	s.sky_base = m_sky_base; s.sky_last_tx0 = m_sky_last_tx0;
}

// Exchanges the kept state with the live one (twice: in to draw the kept moment, back out).
void model1::swap_still()
{
	still_state &s = m_still;
	std::swap(m_frame, s.frame);
	std::swap(m_logic[0], s.logic[0]);
	std::swap(m_logic[1], s.logic[1]);
	std::swap(m_logic_prev, s.logic_prev);
	std::swap(m_cut_frame, s.cut_frame);
	std::swap_ranges(std::begin(m_sky_hist), std::end(m_sky_hist), s.sky_hist);
	m_tiles.swap_rams(s.tile_ram, s.char_ram);
	m_tiles_updated = ~0ull;                         // decoded again from the RAMs now in place
	std::swap_ranges(std::begin(m_pens), std::end(m_pens), s.pens.begin());
	m_paletteram16_ram.swap(s.palette);
	m_color_xlat_ram.swap(s.color_xlat);
	m_paletteram16 = m_paletteram16_ram.data();
	m_color_xlat = m_color_xlat_ram.data();
	std::swap_ranges(std::begin(m_view->lightparams), std::end(m_view->lightparams), s.lightparams.begin());
	std::swap(m_view->light, s.light);
	std::swap(m_view->spec_enable, s.spec_enable);
	std::swap_ranges(&m_ram_b[0x17f0], &m_ram_b[0x17f4], s.camera);   // (the camera pointer: work RAM, put back after)
	std::swap(m_fight_cam, s.fight_cam);
	std::swap(m_sky_now, s.sky_now); std::swap(m_sky_last, s.sky_last);
	std::swap(m_sky_base, s.sky_base); std::swap(m_sky_last_tx0, s.sky_last_tx0);
}

// The blended matrix of every slot in the newer list. Slots pair by position when the two lists
// have the same shape (most of a fight), else by the first object drawn in them. A picture where
// many slots jump is a camera cut: nothing is blended, so parts never come apart.
// The camera's move between two frames, fitted to the paired slots' places (rigid: a turn and a
// shift, least squares, the slots that don't follow it dropped and fitted again). A paired slot far
// off it (over 2% of its depth: paired with another copy of its model) is paired instead with the
// copy that follows the move, when one does; one that none follows, of a model with three or more
// copies in the frame (scenery, not a fighter's part, which moves by itself), is marked -2: drawn
// where the newer frame has it. VF2's (model2_smooth.cpp), here only for runs of copies that
// changed length. The number paired again.
int model1::repair_by_camera(const logic_frame &a, const logic_frame &b, std::vector<int> &pair, const std::vector<char> &changed_run)
{
	struct pt { double x, y, z; };
	auto first = [](const slot &sl) { return sl.polys.empty() ? 0u : sl.polys[0]; };
	std::vector<pt> P, Q;
	std::vector<int> K;
	for (size_t k = 0; k < pair.size(); k++) {
		const int q = pair[k];
		if (q < 0 || b.slots[k].polys.empty()) continue;
		const float *pa = a.slots[q].mat, *pb = b.slots[k].mat;
		P.push_back({ pa[9], pa[10], pa[11] });
		Q.push_back({ pb[9], pb[10], pb[11] });
		K.push_back(int(k));
	}
	if (P.size() < 8) return 0;
	double R[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, T[3] = {};
	std::vector<char> use(P.size(), 1);
	auto residual = [&](const pt &p, const pt &q) {
		double o[3];
		for (int r = 0; r < 3; r++) o[r] = R[r][0] * p.x + R[r][1] * p.y + R[r][2] * p.z + T[r];
		const double dx = o[0] - q.x, dy = o[1] - q.y, dz = o[2] - q.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz) / std::max({ std::fabs(p.z), std::fabs(q.z), 1.0 });
	};
	std::vector<double> res(P.size());
	for (int pass = 0; pass < 3; pass++) {
		// the best turn for the slots in use: the orthonormal part of their covariance (Newton's
		// iteration for the polar decomposition), then the shift between the centres
		double cp[3] = {}, cq[3] = {}, H[3][3] = {};
		int n = 0;
		for (size_t i = 0; i < P.size(); i++)
			if (use[i]) { cp[0] += P[i].x; cp[1] += P[i].y; cp[2] += P[i].z; cq[0] += Q[i].x; cq[1] += Q[i].y; cq[2] += Q[i].z; n++; }
		if (n < 4) return 0;
		for (int r = 0; r < 3; r++) cp[r] /= n, cq[r] /= n;
		for (size_t i = 0; i < P.size(); i++)
			if (use[i]) {
				const double p[3] = { P[i].x - cp[0], P[i].y - cp[1], P[i].z - cp[2] }, q[3] = { Q[i].x - cq[0], Q[i].y - cq[1], Q[i].z - cq[2] };
				for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) H[r][c] += q[r] * p[c];
			}
		double M[3][3];
		memcpy(M, H, sizeof M);
		for (int it = 0; it < 30; it++) {
			const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
			if (std::fabs(det) < 1e-30) return 0;
			double C[3][3];   // the inverse's transpose: cofactors / det
			C[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) / det; C[0][1] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) / det; C[0][2] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) / det;
			C[1][0] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) / det; C[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) / det; C[1][2] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) / det;
			C[2][0] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) / det; C[2][1] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) / det; C[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) / det;
			for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) M[r][c] = 0.5 * (M[r][c] + C[r][c]);
		}
		memcpy(R, M, sizeof R);
		for (int r = 0; r < 3; r++) T[r] = cq[r] - (R[r][0] * cp[0] + R[r][1] * cp[1] + R[r][2] * cp[2]);
		std::vector<double> sorted;
		for (size_t i = 0; i < P.size(); i++) sorted.push_back(res[i] = residual(P[i], Q[i]));
		std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
		const double med = sorted[sorted.size() / 2];
		for (size_t i = 0; i < P.size(); i++) use[i] = res[i] <= std::max(3 * med, 0.002);
	}
	// A cut the parts' moves miss: the camera's own move in a frame, a turn of over 15 degrees or a
	// shift of over 8.5% of the scenery's depth (a game's fastest swing, a knockdown's, turns it 11
	// degrees and shifts it 7.7%). The parts mostly stay close (a cut to the same fighters from
	// another side: TIME OVER's far view to the one from above), and blending them drew views
	// between the two that the game never showed. Close in (the attract's last move, a replay's
	// camera swung in to the fighters) the scenery's depth is small: an ordinary swing passed 8.5%
	// pair after pair, and the close-up went unblended, stepping at the game's 30 a second while
	// the counter said 120. So a shift of 8.5-25% is a cut only out of a still camera (the last
	// pair's under 3%: a fight's camera speeding up from 4-6% to 10% was taken for one) and unlike
	// the last pair's (about as far, the same way: a swing carrying on); over 25%, or a turn over
	// 15 degrees, always. (Attract, 6000 frames: every cut was over 25% or 15 degrees, every swing
	// taken for one under 17%.)
	{
		const double tr = std::clamp((R[0][0] + R[1][1] + R[2][2] - 1) / 2, -1.0, 1.0);
		double mv = 0, dp = 0, d[3] = {};
		int n = 0;
		for (size_t i = 0; i < P.size(); i++)
			if (use[i]) {
				mv += std::sqrt((Q[i].x - P[i].x) * (Q[i].x - P[i].x) + (Q[i].y - P[i].y) * (Q[i].y - P[i].y) + (Q[i].z - P[i].z) * (Q[i].z - P[i].z));
				dp += std::max(std::fabs(P[i].z), 1.0);
				d[0] += Q[i].x - P[i].x; d[1] += Q[i].y - P[i].y; d[2] += Q[i].z - P[i].z; n++;
			}
		const double turn = std::acos(tr) * 180 / M_PI;
		if (n) for (double &v : d) v /= n;
		// (pairs can come out of order, the next frame's run ahead while the screen waits: the last
		// pair's move is the one that ended at this pair's older list)
		cam_move *slot = nullptr;
		for (cam_move &c : m_cam_move) if (c.to == b.frame) slot = &c;
		if (!slot) { slot = &m_cam_move[m_cam_move_next]; m_cam_move_next = (m_cam_move_next + 1) % int(std::size(m_cam_move)); }
		const double ratio = dp > 0 ? mv / dp : 0;
		*slot = { a.frame, b.frame, { d[0], d[1], d[2] }, turn, ratio };
		static const cam_move none;
		const cam_move *found = &none;
		for (const cam_move &c : m_cam_move)   // (that one, else the latest a few frames before it)
			if (c.to <= a.frame && c.to >= a.frame - 4 && c.to < b.frame && (found == &none || c.to > found->to)) found = &c;
		const cam_move &pv = *found;
		const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), plen = std::sqrt(pv.d[0] * pv.d[0] + pv.d[1] * pv.d[1] + pv.d[2] * pv.d[2]);
		const double diff = std::sqrt((d[0] - pv.d[0]) * (d[0] - pv.d[0]) + (d[1] - pv.d[1]) * (d[1] - pv.d[1]) + (d[2] - pv.d[2]) * (d[2] - pv.d[2]));
		const bool known = found != &none;
		const bool swing = known && n && diff < 0.5 * std::max(len, plen) && turn < 2 * pv.turn + 3;
		const bool moving = known && pv.ratio > 0.03;
		// (a turn the parts' places fit that their own axes didn't make: close in on a throw the
		// fighters' parts are most of the list, and the thrown one's swing over fit as a turn of
		// the camera's, 21 degrees, while the stage and the thrower kept theirs. The camera's turn
		// turns every part's axes with it, the fighters' too in a frame or two. A fit turned over
		// 5 degrees that the axes didn't follow isn't the camera's: nor is its move.)
		bool turned_all = true;
		if (turn > 5) {
			int with[2] = { 0, 0 }, n_axes = 0;
			for (size_t i = 0; i < P.size(); i++) {
				const float *ma = a.slots[pair[K[i]]].mat, *mb = b.slots[K[i]].mat;
				n_axes++;
				for (int conv = 0; conv < 2; conv++) {
					bool ok = true;
					for (int c = 0; c < 3 && ok; c++) {
						// (the axes as the triples or as the columns of the matrix: whichever the camera's turn turns)
						double u[3], v[3], w[3] = {};
						for (int r = 0; r < 3; r++) { u[r] = conv ? ma[3 * r + c] : ma[3 * c + r]; v[r] = conv ? mb[3 * r + c] : mb[3 * c + r]; }
						for (int r = 0; r < 3; r++) w[r] = R[r][0] * u[0] + R[r][1] * u[1] + R[r][2] * u[2];
						const double lw = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), lv = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
						ok = lw > 0 && lv > 0 && (w[0] * v[0] + w[1] * v[1] + w[2] * v[2]) > 0.985 * lw * lv;   // (within 10 degrees)
					}
					with[conv] += ok;
				}
			}
			turned_all = std::max(with[0], with[1]) * 2 > n_axes;
		}
		m_camera_cut = (turn > 15 && turned_all) || ratio > 0.25 || (ratio > 0.085 && !swing && !moving && turned_all);
		m_camera_sure = known && !m_camera_cut;
		for (size_t i = 0; i < P.size(); i++) m_cam_res[K[i]] = float(res[i]);
	}
	int repaired = 0;
	for (size_t i = 0; i < P.size(); i++) {
		const int k = K[i];
		// (only in a run of copies that changed length: VF1's fighters have parts in threes and
		// sixes, a hand's fingers, which move by themselves and pair right in order)
		if (res[i] <= 0.02 || !changed_run[k]) continue;
		const uint32_t f = first(b.slots[k]);
		int best = -1;
		double bres = std::min(res[i] * 0.25, 0.01);
		for (size_t q = 0; q < a.slots.size(); q++)
			if (first(a.slots[q]) == f && int(q) != pair[k]) {
				const float *pa = a.slots[q].mat;
				const double r = residual({ pa[9], pa[10], pa[11] }, Q[i]);
				if (r < bres) bres = r, best = int(q);
			}
		if (best >= 0) pair[k] = best, repaired++;
		else if (res[i] > 0.06) {
			int same = 0;
			for (const auto &o : b.slots) same += first(o) == f;
			if (same >= 3) pair[k] = -2, repaired++;
		}
	}
	return repaired;
}

// A part that turned far between two lists (a strand swinging over, a fast limb): its axes blended
// straight from one to the other shrink towards nothing (half way between two turned 90 degrees
// apart, 0.71 of their length; 180 apart, none), the strand drawn a pixel wide. Turned past 25
// degrees on any axis, its turn is blended as a turn and its axes' lengths as lengths.
static bool swung(const float *pa, const float *pb)
{
	for (int r = 0; r < 9; r += 3) {
		const float la = pa[r] * pa[r] + pa[r + 1] * pa[r + 1] + pa[r + 2] * pa[r + 2];
		const float lb = pb[r] * pb[r] + pb[r + 1] * pb[r + 1] + pb[r + 2] * pb[r + 2];
		const float dot = pa[r] * pb[r] + pa[r + 1] * pb[r + 1] + pa[r + 2] * pb[r + 2];
		if (la > 0 && lb > 0 && dot < 0.906f * std::sqrt(la * lb)) return true;
	}
	return false;
}
// (a matrix's turn and what's left, M = R P: R the nearest turn (its polar part, by averaging it
// with its inverse's transpose), P = R^T M its stretch, however the model's parts are scaled; the
// turn blended as a turn, the stretch straight)
static glm::mat3 polar_turn(glm::mat3 m)
{
	for (int it = 0; it < 20; it++) {
		const float d = glm::determinant(m);
		if (std::fabs(d) < 1e-12f) break;
		m = 0.5f * (m + glm::transpose(glm::inverse(m)));
	}
	return m;
}
static void turn_blend(const float *pa, const float *pb, double alpha, float *out)
{
	const glm::mat3 ma(pa[0], pa[1], pa[2], pa[3], pa[4], pa[5], pa[6], pa[7], pa[8]);
	const glm::mat3 mb(pb[0], pb[1], pb[2], pb[3], pb[4], pb[5], pb[6], pb[7], pb[8]);
	// (only a solid part: a shadow is its fighter pressed flat onto the floor, its axes far from a
	// turn's; taken for one it turned up off the floor)
	auto solid = [](const glm::mat3 &m) {
		const float v = glm::length(m[0]) * glm::length(m[1]) * glm::length(m[2]);
		return v > 0 && std::fabs(glm::determinant(m)) > 0.5f * v;
	};
	if (!solid(ma) || !solid(mb)) return;
	glm::mat3 ra = polar_turn(ma), rb = polar_turn(mb);
	const float da = glm::determinant(ra), db = glm::determinant(rb);
	// (one mirrored and the other not: no turn goes between them, every blend passes through flat;
	// the nearer one's axes as they are. Both mirrored: their turns with an axis flipped)
	if (da * db <= 0) {
		const float *near = alpha < 0.5 ? pa : pb;
		for (int k = 0; k < 9; k++) out[k] = near[k];
		return;
	}
	if (da < 0) { ra[2] = -ra[2]; rb[2] = -rb[2]; }
	const glm::mat3 sa = glm::transpose(ra) * ma, sb = glm::transpose(rb) * mb;
	const glm::quat qa = glm::normalize(glm::quat_cast(ra)), qb = glm::normalize(glm::quat_cast(rb));
	const glm::mat3 m = glm::mat3_cast(glm::slerp(qa, qb, float(alpha))) * (sa + (sb - sa) * float(alpha));
	for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) out[3 * c + r] = m[c][r];
}

void model1::blend_slots(const logic_frame &a, const logic_frame &b, double alpha)
{
	m_blend.assign(b.slots.size(), {});
	m_blend_ok.assign(b.slots.size(), 0);
	m_blend_from.assign(b.slots.size(), {});
	m_blend_to.assign(b.slots.size(), {});
	m_blend_light.assign(b.slots.size(), {});
	m_blend_cut = false;
	if (alpha >= 1 && !m_replay_older) return;
	if (a.slots.empty()) return;
	// pair the slots in order, like a diff: the same first model pairs; a different model in the
	// same place (a hand, a face swapped) pairs too; slots that appeared or went are skipped
	std::vector<int> pair(b.slots.size(), -1);
	auto first = [](const slot &sl) { return sl.polys.empty() ? 0u : sl.polys[0]; };
	size_t i = 0, j = 0;
	const size_t na = a.slots.size(), nb = b.slots.size();
	auto dist = [&](size_t ia, size_t jb) {
		const float *pa = a.slots[ia].mat, *pb = b.slots[jb].mat;
		return double(pb[9] - pa[9]) * (pb[9] - pa[9]) + double(pb[10] - pa[10]) * (pb[10] - pa[10]) + double(pb[11] - pa[11]) * (pb[11] - pa[11]);
	};
	int repaired = 0;
	std::vector<char> changed_run(nb, 0);       // slots of a run that changed length
	while (i < na && j < nb) {
		if (first(a.slots[i]) == first(b.slots[j])) {
			// A run of slots of one model (copies of a piece of scenery): when one of them came or
			// went, pairing in order pairs a copy with the next one along, and the pictures between
			// the frames draw it halfway to that one's place. The shorter run is paired where it
			// lies nearest in the longer one; runs of the same length pair in order. (VF2's fix
			// for its stages' bushes; as there, the rest pair by the diff.)
			const uint32_t f = first(a.slots[i]);
			size_t ra = 1, rb = 1;
			if (f) {
				while (i + ra < na && first(a.slots[i + ra]) == f) ra++;
				while (j + rb < nb && first(b.slots[j + rb]) == f) rb++;
			}
			if (ra == rb) { for (size_t k = 0; k < ra; k++) pair[j + k] = int(i + k); i += ra; j += rb; continue; }
			for (size_t k = 0; k < rb; k++) changed_run[j + k] = 1;
			const size_t n = std::min(ra, rb), slack = std::max(ra, rb) - n;
			size_t best = 0;
			double best_d = -1;
			for (size_t o = 0; o <= slack && o <= 64; o++) {
				double d = 0;
				for (size_t k = 0; k < n; k++) d += ra > rb ? dist(i + o + k, j + k) : dist(i + k, j + o + k);
				if (best_d < 0 || d < best_d) best_d = d, best = o;
			}
			for (size_t k = 0; k < n; k++) {
				if (ra > rb) pair[j + k] = int(i + best + k);
				else pair[j + best + k] = int(i + k);
			}
			if (best) repaired += int(n);
			i += ra; j += rb;
			continue;
		}
		size_t ka = 1, kb = 1;
		while (i + ka < na && ka <= 8 && first(a.slots[i + ka]) != first(b.slots[j])) ka++;
		while (j + kb < nb && kb <= 8 && first(b.slots[j + kb]) != first(a.slots[i])) kb++;
		const bool a_skip = i + ka < na && ka <= 8, b_skip = j + kb < nb && kb <= 8;
		if (a_skip && (!b_skip || ka <= kb)) i += ka;       // slots in the older list that went
		else if (b_skip) j += kb;                            // slots that are new
		else pair[j++] = int(i++);                           // the same place, a different model
	}
	m_camera_cut = false;
	m_camera_sure = false;
	m_cam_res.assign(nb, -1);
	repaired += repair_by_camera(a, b, pair, changed_run);
	static const bool logp = getenv("VF_LOG_SMOOTH") != nullptr;
	if (logp && repaired && !m_redrawing) fprintf(stderr, "P frame %d repaired %d\n", b.frame, repaired);
	int jumps = 0, paired = 0, missing = 0;
	std::vector<char> jumped(b.slots.size(), 0);
	for (size_t i = 0; i < b.slots.size(); i++) {
		if (pair[i] == -2) continue;                       // (scenery that came into the list: at the newer place)
		if (pair[i] < 0 && !b.slots[i].polys.empty()) missing++;
		if (pair[i] < 0 || b.slots[i].polys.empty()) continue;
		const float *pa = a.slots[pair[i]].mat, *pb = b.slots[i].mat;
		const float dx = pb[9] - pa[9], dy = pb[10] - pa[10], dz = pb[11] - pa[11];
		const float depth = std::max({ std::fabs(pa[11]), std::fabs(pb[11]), 1.0f });
		paired++;
		// a part that moved far, or turned sharply (a cut can keep the parts' places and swing the
		// camera round them: blending two far-apart turns squashes and stretches the shapes)
		bool turned = false;
		for (int r = 0; r < 9 && !turned; r += 3) {
			const float la = pa[r] * pa[r] + pa[r + 1] * pa[r + 1] + pa[r + 2] * pa[r + 2];
			const float lb = pb[r] * pb[r] + pb[r + 1] * pb[r + 1] + pb[r + 2] * pb[r + 2];
			const float dot = pa[r] * pb[r] + pa[r + 1] * pb[r + 1] + pa[r + 2] * pb[r + 2];
			if (la > 0 && lb > 0 && dot < 0.77f * std::sqrt(la * lb)) turned = true;     // over ~40 degrees
		}
		if (dx * dx + dy * dy + dz * dz > 0.09f * depth * depth || turned) jumps++, jumped[i] = 1;    // fast, or a cut
		for (int k = 0; k < 12; k++) m_blend[i][k] = float(pa[k] + (pb[k] - pa[k]) * alpha);
		static const bool no_turn = getenv("VF_NO_TURN_BLEND") != nullptr;   // TESTONLY
		if (!no_turn && swung(pa, pb)) turn_blend(pa, pb, alpha, m_blend[i].data());
		memcpy(m_blend_from[i].data(), pa, 12 * sizeof(float));
		memcpy(m_blend_to[i].data(), pb, 12 * sizeof(float));
		const slot &sa = a.slots[pair[i]];
		m_blend_light[i] = { sa.light[0], sa.light[1], sa.light[2], sa.has_light ? 1.0f : 0.0f };
		m_blend_ok[i] = 1;
	}
	// A camera cut moves nearly every part at once (fast moves only a few): then, or when many
	// parts can't be paired (another scene), the newer frame is shown whole. A few new parts (a
	// hat knocked off, Jeff's cap and shades) are drawn where the newer frame has them, the rest
	// blended: as a cut, the picture carried on past the newer list, and the fighters' fast-turning
	// limbs came out stretched (a kick that knocked Pai's hat off), or, made rigid, apart.
	// (When the camera's move is known and is no cut, a part jumped only if it moved far off the
	// camera's move: close in (depth under 1, the camera a fighter's width away) its swing past
	// moved most parts over 30%, and fighters spinning in a throw turned most of theirs over 40
	// degrees, each taken for a cut: the picture stood 2 game frames and lurched.)
	int cut_jumps = jumps;
	if (m_camera_sure) {
		cut_jumps = 0;
		for (size_t i = 0; i < b.slots.size(); i++) cut_jumps += m_blend_ok[i] && m_cam_res[i] > 0.3f;
	}
	m_blend_cut = cut_jumps * 2 > paired || missing * 8 > paired || m_camera_cut;
	// Carried on past the newer list (blend_slots(older, newer, 1 to 1.5) at a cut, half a game frame
	// on at most): each part's own blend carried on grew the fast-turning ones (a kicking leg up to
	// twice its length) and moved each by itself, apart from the next (made rigid, a hand came off
	// its wrist). Every part is drawn as the newer list has it, all moved on together by the parts'
	// average move between the two lists: the whole picture carries on, nothing comes apart. The
	// parts that moved fast (a camera swinging into a cut, TIME OVER's; a body mid-throw, a kicking
	// leg) are left out of the average, carried with the rest: their own move carried on went into
	// views and poses the game never showed. (The newer list was held as it was until the cut when
	// any part moved fast: one kicking leg, and the picture stood a list before most cuts.)
	if (alpha > 1 && !m_blend_cut) {
		double mv[3] = {};
		int n = 0;
		for (size_t i = 0; i < b.slots.size(); i++)
			if (m_blend_ok[i] && !jumped[i]) { for (int k = 0; k < 3; k++) mv[k] += m_blend_to[i][9 + k] - m_blend_from[i][9 + k]; n++; }
		for (size_t i = 0; i < b.slots.size(); i++)
			if (m_blend_ok[i]) {
				memcpy(m_blend[i].data(), m_blend_to[i].data(), 12 * sizeof(float));
				for (int k = 0; k < 3; k++) m_blend[i][9 + k] += float((alpha - 1) * mv[k] / std::max(n, 1));
			}
	}
	if (m_blend_cut) std::fill(m_blend_ok.begin(), m_blend_ok.end(), 0);
	m_smooth_stats[1] = jumps;
	static const bool logm = getenv("VF_LOG_SMOOTH") != nullptr;
	if (logm && (jumps || missing)) fprintf(stderr, "M jumps %d missing %d paired %d slots %zu/%zu\n", jumps, missing, paired, na, nb);
	for (size_t i = 0; i < b.slots.size(); i++)
		if (!b.slots[i].polys.empty()) m_smooth_stats[m_blend_ok[i] ? 2 : 0]++;
}

// While replaying: the matrix for the object being drawn (the current slot's, blended).
bool model1::smooth_matrix(uint32_t, float *out)
{
	if ((m_replay_older && !m_replay_extra) || m_replay_slot < 0 || m_replay_slot >= int(m_blend_ok.size()) || !m_blend_ok[m_replay_slot]) return false;
	memcpy(out, m_blend[m_replay_slot].data(), 12 * sizeof(float));
	if (m_replay_extra) { m_shade_from = m_shade_to = m_shade_light = nullptr; return true; }   // (its own shading)
	m_shade_from = m_blend_from[m_replay_slot].data();
	m_shade_to = m_blend_to[m_replay_slot].data();
	m_shade_light = m_blend_light[m_replay_slot].data();
	return true;
}

// The game's sky scroll (the routine at ff49a1, at the start of vblank) comes from the camera:
// across, its heading (508004: 1/32 pixel, 2048 pixels for a turn); up and down, 508006 (whole
// pixels). Each is mixed with the last game frame's (508008, 50800a): halfway across on the frames
// between the game's 30 a second, and a quarter or three quarters up and down; then written to
// tile map 2's scroll in whole pixels (h: the heading >> 5; v: a sum >> 1). When a write is that
// sum, the fraction it dropped is kept for smooth motion's sky (worked out again from the RAM it
// read: nothing writes those in between).
model1::sky_scroll model1::sky_scroll_from_ram(bool between) const
{
	auto rd = [&](uint32_t x) { return uint16_t(m_ram_b[x - 0x500000] | m_ram_b[x - 0x500000 + 1] << 8); };
	uint16_t r1 = rd(0x508004);
	if (between) r1 = uint16_t(r1 - (int16_t(uint16_t(r1 - rd(0x508008))) >> 1));
	uint16_t r3 = rd(0x508006), r4 = rd(0x50800a);
	const uint16_t d = uint16_t(r3 - r4);
	if (between) r3 = uint16_t(r3 - (int16_t(d) >> 1));
	else r4 = uint16_t(r4 + (int16_t(d) >> 1) + (d & 1));   // (rounded: the bit shifted out added back)
	const uint16_t sum = uint16_t(r3 + r4);
	sky_scroll s;
	s.h = (r1 >> 5) & 0x3ff;
	s.v = uint16_t((((sum >> 1) + 0x21) & 0x3ff) | (r3 & 0xe000));
	s.hx = float(s.h & 0x1ff) + (r1 & 31) / 32.0f;
	s.vy = float(s.v & 0x3ff) + (sum & 1) * 0.5f;
	return s;
}

// Where the sky lands for the camera in RAM now: its own scroll, unmixed (h the heading >> 5, v as
// the routine's sum of two equal values)
void model1::sky_settled(int &h, int &v) const
{
	auto rd = [&](uint32_t x) { return uint16_t(m_ram_b[x - 0x500000] | m_ram_b[x - 0x500000 + 1] << 8); };
	h = (rd(0x508004) >> 5) & 0x1ff;
	v = (rd(0x508006) + 0x21) & 0x3ff;
}

// Whether the sky can be drawn where it lands from where the game has it in a frame (the tile
// layer moves 24 pixels at most; a hi-res sky further)
bool model1::sky_reaches(const sky_sample &s) const
{
	const int dh = ((s.lands_h - s.h + 256) & 511) - 256, dv = ((s.lands_v - s.v + 512) & 1023) - 512;
	const double most = m_sky_now ? MAX_SKY_STEP : 24;
	return std::abs(dh) <= most && std::abs(dv) <= most;
}

void model1::sky_scroll_written(uint32_t a)
{
	m_sky_between = m_ram_b[0x501] & 1;
	const sky_scroll s = sky_scroll_from_ram(m_sky_between);
	if (a == 0x70a004) {
		m_sky_read_h = s.h == m_tiles.tile_r(0x5002);         // (anything else written: no fraction)
		m_sky_frac_h = m_sky_read_h ? s.hx - float(s.h & 0x1ff) : 0;
	} else {
		m_sky_read_v = s.v == m_tiles.tile_r(0x5006);
		m_sky_frac_v = m_sky_read_v ? s.vy - float(s.v & 0x3ff) : 0;
	}
	m_sky_written = m_frame;
}

// While replaying: how far to move the sky layer (arcade pixels) from where the tile chip has it
// now to where it was at the moment drawn: the game's own scroll with its fractions, each frame's
// (straight between them, as the 3D's blend; past the newest frame, toward the next one's, worked
// out as the newest ended). At a frame's own moment it is that frame's scroll, whose whole pixels
// are what the game drew. The sky's map x is -hscroll, its y +vscroll (1024 rows, in two maps).
void model1::smooth_sky_shift(float &dx, float &dy) const
{
	dx = dy = 0;
	if (!m_replaying) return;
	// The game writes the sky's scroll two frames after the display list it goes with (at a camera
	// cut the 3D changes at frame C and the sky at C+2): the sky drawn with the list for moment
	// m is the scroll at m+2, which smooth motion, two frames behind the game, has up to the
	// newest frame and the next.
	constexpr double SKY_LAG = 2;
	const double t = m_draw_time + SKY_LAG;
	const int newest = int(m_frame) - 1;
	if (newest < 0) return;
	const sky_sample &cur = m_sky_hist[newest % SKY_HIST];
	if (cur.frame != newest) return;
	// frame f's scroll less the newest's (unwrapped), or false when it's not kept
	auto at = [&](int f, bool horizontal, double &out) {
		if (f < 0 || f > newest || f <= newest - SKY_HIST) return false;
		const sky_sample &s = m_sky_hist[f % SKY_HIST];
		if (s.frame != f) return false;
		const double w = horizontal ? 512 : 1024, d = horizontal ? s.hx - cur.hx : s.vy - cur.vy;
		out = d - w * std::round(d / w);
		return true;
	};
	// A step from one frame's scroll to the next is a move, not a jump (a cut), when it's small, or
	// when it carries on at the speed of the step before it: a stage intro's fly-around turns the
	// camera 25-60 pixels a frame, which a fixed 24 took for cuts (the sky then stepped a frame at
	// a time while the 3D glided).
	auto steady = [&](double step, bool have_before, double before) {
		return std::fabs(step) <= 24 || (have_before && std::fabs(step) <= MAX_SKY_STEP && std::fabs(step - before) <= 24);
	};
	auto moment = [&](bool horizontal) {
		const int k = std::min(int(std::floor(t)), newest);
		double fr = t - k;
		double a, b, p;
		if (!at(k, horizontal, a)) return 0.0;
		if (fr <= 0) return a;
		const bool have_p = at(k - 1, horizontal, p);
		// a jump (a cut) isn't crossed: the scroll changes at the frame, as the game's
		if (k < newest) return at(k + 1, horizontal, b) && steady(b - a, have_p, a - p) ? a + (b - a) * fr : a;
		fr = std::min(fr, 1.0);
		if (cur.next) {                                   // a == 0: the newest frame's
			const double w = horizontal ? 512 : 1024, d = horizontal ? cur.nx - cur.hx : cur.ny - cur.vy;
			b = d - w * std::round(d / w);
			return steady(b, have_p, a - p) ? b * fr : 0.0;
		}
		double pp;
		const bool have_pp = at(k - 2, horizontal, pp);
		return have_p && steady(a - p, have_pp, p - pp) ? a + (a - p) * fr : a;   // else at its speed
	};
	const int h_now = m_tiles.tile_r(0x5002) & 0x1ff, v_now = m_tiles.tile_r(0x5006) & 0x3ff;
	auto wrapped = [](double d, double w) { return d - w * std::round(d / w); };
	double h_t = cur.hx + moment(true), v_t = cur.vy + moment(false);
	if (m_sky_settling_now) {
		// just after a cut, while the game's mix with the old view's camera moves the sky into
		// place: where it lands
		h_t = cur.lands_h;
		v_t = cur.lands_v;
	}
	dx = float(wrapped(h_now - h_t, 512));               // sample at x + (h_now - h_t)
	dy = float(wrapped(v_t - v_now, 1024));              // and y + (v_t - v_now)
	if (std::fabs(dx) > MAX_SKY_STEP || std::fabs(dy) > MAX_SKY_STEP) dx = dy = 0;
}

void model1::smooth_state(float &alpha, int &older, int &newer, float *place) const
{
	alpha = m_last_alpha;
	if (place) for (int k = 0; k < 3; k++) place[k] = m_pic_place[k];
	older = m_logic[0].list.empty() ? -1 : m_logic[0].frame;
	newer = m_logic[1].list.empty() ? -1 : m_logic[1].frame;
}

// Random fighter: a random fighter's moves come from three characters, each move posed for its
// own, so going from one to the next could jump (an arm swung round in a frame). As a random
// fighter's move changes, its old pose is carried into the new one: each part's turn and place
// about the hips (the slot of its hips' model), the difference between the old pose and the new
// fading out over POSE_EASE frames as the new move plays. Only the random fighters' parts: the
// game's own fighters are drawn as they are.
namespace {

constexpr double POSE_EASE = 12;   // video frames (6 of the game's)

struct rigid { glm::mat3 r; glm::vec3 t; };
rigid rigid_of(const float *m) { return { glm::mat3(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]), glm::vec3(m[9], m[10], m[11]) }; }
void store(const rigid &x, float *m)
{
	for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) m[c * 3 + r] = x.r[c][r];
	m[9] = x.t.x; m[10] = x.t.y; m[11] = x.t.z;
}
rigid operator*(const rigid &a, const rigid &b) { return { a.r * b.r, a.r * b.t + a.t }; }
rigid inverse(const rigid &a) { const glm::mat3 ri = glm::inverse(a.r); return { ri, -(ri * a.t) }; }
// an offset (a turn and a move) as far as w of the way from none
rigid offset_at(const std::array<float, 7> &o, float w)
{
	const glm::quat q = glm::slerp(glm::quat(1, 0, 0, 0), glm::quat(o[0], o[1], o[2], o[3]), w);
	return { glm::mat3_cast(q), glm::vec3(o[4], o[5], o[6]) * w };
}
// (what's left of the old pose: it goes fastest at first, so the new move gets going at once and
// the old one's shape settles out of it; a smoothstep held the old pose still at first, then swept
// it out fast: a move started late and then sped up, Lau's dive onto a downed fighter lurching)
// A fighter's parts about its hips (rel) as eased, w of each offset left: each about the hips by
// its own (o), and a limb's part (limb: the part it hangs from, drawn before it) about that part,
// by its own (lo), hung on that part as eased: the upper arm on the torso, the forearm on it, the
// hand on that, the thigh on the hips, the shin, the foot. (Each about the hips alone, a hand came
// off its wrist while the arm eased: a different arc each, and a new hand model, with no offset,
// left where the new move has it.) g: each part's offsets, or -1.
std::vector<rigid> eased_pose(const std::vector<rigid> &rel, const std::vector<int> &limb, const std::vector<int> &g,
	const std::vector<std::array<float, 7>> &o, const std::vector<std::array<float, 7>> &lo, float w, const std::vector<uint8_t> &head)
{
	std::vector<rigid> out(rel.size());
	for (size_t k = 0; k < rel.size(); k++) {
		const int p = k < limb.size() ? limb[k] : -1, gk = g[k];
		if (p >= 0 && size_t(p) < k) {
			rigid l = inverse(rel[size_t(p)]) * rel[k];
			static const bool on_torso = getenv("VF_HEAD_ON_TORSO") != nullptr;   // testing: carried on the torso, as before
			const bool h = k < head.size() && head[k];
			if (gk >= 0 && size_t(gk) < lo.size() && (!h || on_torso)) {
				const rigid d = offset_at(lo[size_t(gk)], h ? w * std::sqrt(w) : w);
				l = { d.r * l.r, l.t + d.t };
			}
			out[k] = out[size_t(p)] * l;
			// (the head: on its neck as the torso eases, but turned as the old move's head was in the
			// room, not on the torso, going over to the new one's a little sooner than the body (w^1.5):
			// carried on the torso, the up-tilt that keeps a head on the opponent while the torso leans
			// in went on as the torso straightened, the head looking at the sky after a punch)
			if (h && gk >= 0 && !on_torso) out[k].r = (offset_at(o[size_t(gk)], w * std::sqrt(w)) * rel[k]).r;
		} else
			out[k] = gk >= 0 ? offset_at(o[size_t(gk)], w) * rel[k] : rel[k];
	}
	return out;
}
float ease(double t) { const double u = 1 - std::clamp(t / POSE_EASE, 0.0, 1.0); return float(u * u); }

} // namespace

// Each player's slots in a list, when their fighter is a random one: the run of slots drawing its
// record's models (its parts, hands and head), its hips' slot, and its move (block -38).
void model1::pose_slots(logic_frame &lf)
{
	if (!m_random_fighter || m_ram_a.size() < 0x1f00) return;
	auto &rom = m_regions.maincpu;
	auto rd = [&](uint32_t a) { return uint32_t(rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | uint32_t(rom[a + 3]) << 24); };
	auto poly = [&](uint32_t m) -> uint32_t { return (m >> 16) == 0x2d && m + 12 <= rom.size() ? rd(m + 8) & 0x7fffff : 0; };
	std::vector<uint32_t> mine[2];
	uint32_t hips[2] = { 0, 0 }; uint32_t torsos[2] = { 0, 0 };
	std::map<uint32_t, int> limbs[2];   // (a limb's models: 1 upper arm, 2 forearm, 3 hand, 4 thigh, 5 shin, 6 foot)
	for (int pl = 0; pl < 2; pl++) {
		const uint32_t o = 0x6b0 + 0x1800 * pl;
		const uint32_t rec = uint32_t(m_ram_a[o] | m_ram_a[o + 1] << 8 | m_ram_a[o + 2] << 16 | uint32_t(m_ram_a[o + 3]) << 24);
		lf.moves[pl] = uint16_t(m_ram_a[o + 0x18] | m_ram_a[o + 0x19] << 8);
		if (!m_random_names.count(rec) || rec + RECORD_SIZE > rom.size()) continue;
		for (uint32_t k = 0; k + 4 <= RECORD_SIZE; k++) if (uint32_t p = poly(rd(rec + k))) mine[pl].push_back(p);
		const uint32_t head = rd(rec + 0x72);
		if ((head >> 16) == 0xfd && head + 0x224 <= rom.size())
			for (uint32_t k = 0; k < 121; k++) { const uint32_t p = poly(rd(head + 0x40 + 4 * k)); if (!p) break; mine[pl].push_back(p); }
		hips[pl] = poly(rd(rec + 0x32));
		torsos[pl] = poly(rd(rec + 0x12));
		std::sort(mine[pl].begin(), mine[pl].end());
		auto mark = [&](uint32_t p, int kind) { if (p) limbs[pl].emplace(p, kind); };
		for (uint32_t side : { 0u, 0xcu }) {   // (the far side's parts 12 on)
			mark(poly(rd(rec + 0x1a + side)), 1); mark(poly(rd(rec + 0x1e + side)), 2);
			mark(poly(rd(rec + 0x36 + side)), 4); mark(poly(rd(rec + 0x3a + side)), 5); mark(poly(rd(rec + 0x3e + side)), 6);
		}
		for (uint32_t tb : { 0x5eu, 0x62u })
			if (const uint32_t t = rd(rec + tb); t >= rec && t + 4 * 32 <= rec + RECORD_SIZE)
				for (uint32_t k = 0; k < 32; k++) mark(poly(rd(t + 4 * k)), 3);
	}
	std::vector<uint32_t> strands[2], heads[2];   // (its strands, which hang from the head, and its head's models)
	for (int pl = 0; pl < 2; pl++) {
		if (mine[pl].empty()) continue;
		strand_polys(pl, strands[pl]);
		mine[pl].insert(mine[pl].end(), strands[pl].begin(), strands[pl].end());
		std::sort(mine[pl].begin(), mine[pl].end());
		std::sort(strands[pl].begin(), strands[pl].end());
		const uint32_t o = 0x6b0 + 0x1800 * pl;
		const uint32_t rec = uint32_t(m_ram_a[o] | m_ram_a[o + 1] << 8 | m_ram_a[o + 2] << 16 | uint32_t(m_ram_a[o + 3]) << 24);
		const uint32_t head = rd(rec + 0x72);
		if ((head >> 16) == 0xfd && head + 0x224 <= rom.size())
			for (uint32_t k = 0; k < 121; k++) { const uint32_t p = poly(rd(head + 0x40 + 4 * k)); if (!p) break; heads[pl].push_back(p); }
		else if (const uint32_t p = poly(rd(rec + 0x16))) heads[pl].push_back(p);   // (no head list: the head is +16's, Dural's)
		std::sort(heads[pl].begin(), heads[pl].end());
	}
	auto has = [&](int pl, uint32_t p) { return std::binary_search(mine[pl].begin(), mine[pl].end(), p); };
	// each player's hips (the same model for both: the first is player 1's), then each slot drawing
	// one's models; one drawing models both have, the one whose hips are nearer
	int hip_slot[2] = { -1, -1 };
	for (int i = 0; i < int(lf.slots.size()); i++)
		for (uint32_t p : lf.slots[i].polys)
			for (int pl = 0; pl < 2; pl++)
				if (hips[pl] && p == hips[pl] && hip_slot[pl] < 0 && hip_slot[pl ^ 1] != i) { hip_slot[pl] = i; break; }
	auto dist2 = [&](int i, int pl) {
		const float *a = lf.slots[i].mat, *b = lf.slots[hip_slot[pl]].mat;
		return (a[9] - b[9]) * (a[9] - b[9]) + (a[10] - b[10]) * (a[10] - b[10]) + (a[11] - b[11]) * (a[11] - b[11]);
	};
	std::vector<int> slots[2];
	std::vector<uint8_t> strand_of[2];
	int root[2] = { -1, -1 }, head_slot[2] = { -1, -1 }, torso_slot[2] = { -1, -1 };
	std::vector<uint8_t> head_of[2];
	for (int i = 0; i < int(lf.slots.size()); i++) {
		bool in[2] = { false, false };
		for (uint32_t p : lf.slots[i].polys) for (int pl = 0; pl < 2; pl++) in[pl] = in[pl] || (hip_slot[pl] >= 0 && has(pl, p));
		int who = in[0] && in[1] ? (dist2(i, 0) <= dist2(i, 1) ? 0 : 1) : in[0] ? 0 : in[1] ? 1 : -1;
		if (i == hip_slot[0]) who = 0;
		if (i == hip_slot[1]) who = 1;
		if (who < 0) continue;
		if (i == hip_slot[who]) root[who] = int(slots[who].size());
		bool is_strand = false, is_head = false;
		for (uint32_t p : lf.slots[i].polys) {
			is_strand = is_strand || std::binary_search(strands[who].begin(), strands[who].end(), p);
			is_head = is_head || std::binary_search(heads[who].begin(), heads[who].end(), p);
		}
		if (is_head && head_slot[who] < 0) head_slot[who] = int(slots[who].size());
		if (torso_slot[who] < 0 && torsos[who] && !lf.slots[i].polys.empty() && lf.slots[i].polys[0] == torsos[who]) torso_slot[who] = int(slots[who].size());
		head_of[who].push_back(is_head);
		strand_of[who].push_back(is_strand && !is_head);
		slots[who].push_back(i);
	}
	// each limb's part, by the record's (the hands by its tables, and anything just after a forearm,
	// a hand from another of its tables), and the part it hangs from: the last of that kind drawn
	// before it (the game draws down each limb)
	enum { L_NONE, L_UPPER, L_FORE, L_HAND, L_THIGH, L_SHIN, L_FOOT };
	std::vector<int> limb[2];
	for (int pl = 0; pl < 2; pl++) {
		const int n = int(slots[pl].size());
		limb[pl].assign(size_t(n), -1);
		if (root[pl] < 0 || torso_slot[pl] < 0) continue;
		std::vector<int> kind(size_t(n), L_NONE);
		for (int k = 0; k < n; k++) {
			if (k == root[pl] || k == torso_slot[pl] || head_of[pl][size_t(k)] || strand_of[pl][size_t(k)]) continue;
			for (uint32_t p : lf.slots[slots[pl][size_t(k)]].polys)
				if (auto j = limbs[pl].find(p); j != limbs[pl].end()) { kind[size_t(k)] = j->second; break; }
			if (kind[size_t(k)] == L_NONE && k > 0 && kind[size_t(k - 1)] == L_FORE) kind[size_t(k)] = L_HAND;
		}
		// (the head on the torso, its strands on it: pinned to the torso's new pose, the head took the
		// new move's turn at once while the body eased: a move another character's, it held the head
		// its own way, and the head swung to the camera and away as two characters' moves took turns)
		static const bool pinned = getenv("VF_HEAD_PINNED") != nullptr;   // testing: as it was
		if (!pinned)
			for (int k = 0; k < n; k++)
				if (head_of[pl][size_t(k)] && torso_slot[pl] < k) limb[pl][size_t(k)] = torso_slot[pl];
				else if (strand_of[pl][size_t(k)])
					for (int i = k - 1; i >= 0; i--) if (head_of[pl][size_t(i)]) { limb[pl][size_t(k)] = i; break; }
		for (int k = 0; k < n; k++) {
			const int want = kind[size_t(k)] == L_UPPER ? -2 : kind[size_t(k)] == L_THIGH ? -3 : kind[size_t(k)] == L_NONE ? 0 : kind[size_t(k)] - 1;
			if (!want) continue;
			if (want == -2) limb[pl][size_t(k)] = torso_slot[pl] < k ? torso_slot[pl] : -1;
			else if (want == -3) limb[pl][size_t(k)] = root[pl] < k ? root[pl] : -1;
			else for (int i = k - 1; i >= 0; i--) if (kind[size_t(i)] == want) { limb[pl][size_t(k)] = i; break; }
		}
	}
	for (int pl = 0; pl < 2; pl++)
		if (root[pl] >= 0) { lf.limb[pl] = std::move(limb[pl]); lf.mine[pl] = std::move(slots[pl]); lf.root[pl] = root[pl]; lf.head[pl] = head_slot[pl]; lf.strand[pl] = std::move(strand_of[pl]); lf.torso[pl] = torso_slot[pl]; lf.is_head[pl] = std::move(head_of[pl]); }
}

// Each of a player's parts in a list as a key: its first model, and how many of that model came
// before it (the same model twice, a pair of hands)
std::vector<uint64_t> model1::part_keys(const logic_frame &lf, int pl)
{
	std::vector<uint64_t> keys;
	std::map<uint32_t, uint32_t> seen;
	for (int s : lf.mine[pl]) {
		const uint32_t m = s >= 0 && s < int(lf.slots.size()) && !lf.slots[s].polys.empty() ? lf.slots[s].polys[0] : 0;
		keys.push_back(uint64_t(m) << 8 | seen[m]++);
	}
	return keys;
}

// A new list: where a random fighter's move changed between the two (and the list after, as the
// jump can come a game frame after the move does), its pose in the older as drawn then (with any
// easing still going) as an offset from the newer's: the hips' own (about themselves), and each
// part's about the hips.
void model1::pose_transition(const logic_frame &a, const logic_frame &b)
{
	// The poses are the camera's view of them: across a camera cut the old one is another view (the
	// fighter eased from the old view's size and place, tiny then growing, its parts flying apart),
	// so a cut ends any easing. A cut: most of the scenery (the slots neither fighter draws, paired
	// in order by their first model) moved or turned at once (a camera swinging round in an intro moves
	// two thirds of it).
	std::vector<uint8_t> theirs(b.slots.size(), 0), theirs_a(a.slots.size(), 0);
	for (int pl = 0; pl < 2; pl++) {
		for (int s : b.mine[pl]) if (s >= 0 && s < int(theirs.size())) theirs[size_t(s)] = 1;
		for (int s : a.mine[pl]) if (s >= 0 && s < int(theirs_a.size())) theirs_a[size_t(s)] = 1;
	}
	int scenery = 0, moved = 0;
	for (size_t i = 0, j = 0; i < a.slots.size() && j < b.slots.size(); ) {
		if (theirs_a[i] || a.slots[i].polys.empty()) { i++; continue; }
		if (theirs[j] || b.slots[j].polys.empty()) { j++; continue; }
		const float *pa = a.slots[i].mat, *pb = b.slots[j].mat;
		if (a.slots[i].polys[0] == b.slots[j].polys[0]) {
			const float dx = pb[9] - pa[9], dy = pb[10] - pa[10], dz = pb[11] - pa[11];
			const float depth = std::max({ std::fabs(pa[11]), std::fabs(pb[11]), 1.0f });
			const float la = pa[0] * pa[0] + pa[1] * pa[1] + pa[2] * pa[2], lb = pb[0] * pb[0] + pb[1] * pb[1] + pb[2] * pb[2];
			const float dot = pa[0] * pb[0] + pa[1] * pb[1] + pa[2] * pb[2];
			scenery++;
			moved += dx * dx + dy * dy + dz * dz > 0.0072f * depth * depth || (la > 0 && lb > 0 && dot < 0.966f * std::sqrt(la * lb));   // (8.5% of its depth, or 15 degrees)
		}
		i++, j++;
	}
	const bool cut = scenery >= 4 && moved * 4 > scenery * 3;
	for (int pl = 0; pl < 2; pl++) {
		if (cut) { m_pose_off[pl] = pose_offset{}; continue; }
		pose_offset &po = m_pose_off[pl];
		if (b.mine[pl].empty() || a.mine[pl].empty()) { po = pose_offset{}; continue; }
		const int n = int(b.mine[pl].size());
		if (a.root[pl] < 0 || b.root[pl] < 0 || a.slots[a.mine[pl][a.root[pl]]].polys.empty() || b.slots[b.mine[pl][b.root[pl]]].polys.empty()
			|| a.slots[a.mine[pl][a.root[pl]]].polys[0] != b.slots[b.mine[pl][b.root[pl]]].polys[0]) { po = pose_offset{}; continue; }
		if (a.moves[pl] != b.moves[pl]) po.armed = 2;
		if (po.armed <= 0) continue;
		po.armed--;
		// (each part by its model, not its place in the list: the game can draw a fighter's parts in
		// another order from one list to the next (a head of several models, the hands); paired by
		// place, a part took another's offset, and the head turned up 50 to 70 degrees while the pose
		// eased after a move)
		const std::vector<uint64_t> ka = part_keys(a, pl), kb = part_keys(b, pl);
		auto find = [](const std::vector<uint64_t> &keys, uint64_t k) { for (size_t i = 0; i < keys.size(); i++) if (keys[i] == k) return int(i); return -1; };
		const bool going = po.start >= 0 && po.o.size() == po.key.size();
		const float w = going ? ease(a.frame - po.start) : 0;
		rigid root_a = rigid_of(a.slots[a.mine[pl][a.root[pl]]].mat);
		const rigid ra = inverse(root_a), root_b = rigid_of(b.slots[b.mine[pl][b.root[pl]]].mat), rb = inverse(root_b);
		if (w > 0) root_a = root_a * offset_at(po.root, w);
		std::vector<std::array<float, 7>> o(static_cast<size_t>(n), std::array<float, 7>{ 1, 0, 0, 0, 0, 0, 0 }), lo = o;
		auto as_offset = [](const rigid &d) { const glm::quat q = glm::normalize(glm::quat_cast(d.r)); return std::array<float, 7>{ q.w, q.x, q.y, q.z, d.t.x, d.t.y, d.t.z }; };
		// (the older as drawn: with any easing still on it)
		const int na = int(a.mine[pl].size());
		std::vector<rigid> rel_a(static_cast<size_t>(na)), rel_b(static_cast<size_t>(n));
		std::vector<int> ga(static_cast<size_t>(na), -1);
		for (int j = 0; j < na; j++) {
			rel_a[size_t(j)] = ra * rigid_of(a.slots[a.mine[pl][j]].mat);
			if (w > 0) ga[size_t(j)] = find(po.key, ka[size_t(j)]);
		}
		const std::vector<rigid> ela = eased_pose(rel_a, a.limb[pl], ga, po.o, po.lo, w, a.is_head[pl]);
		for (int k = 0; k < n; k++) rel_b[size_t(k)] = rb * rigid_of(b.slots[b.mine[pl][k]].mat);
		for (int k = 0; k < n; k++) {
			const int j = find(ka, kb[size_t(k)]);
			if (j < 0) continue;   // (a part the older didn't have: as it is)
			o[size_t(k)] = as_offset(ela[size_t(j)] * inverse(rel_b[size_t(k)]));
			const int pb = k < int(b.limb[pl].size()) ? b.limb[pl][size_t(k)] : -1, pa = j < int(a.limb[pl].size()) ? a.limb[pl][size_t(j)] : -1;
			const bool strand = k < int(b.strand[pl].size()) && b.strand[pl][size_t(k)];   // (the strands go as the head goes)
			if (pb >= 0 && pa >= 0 && ka[size_t(pa)] == kb[size_t(pb)] && !strand) {   // (hung from the same part in both)
				const rigid la = inverse(ela[size_t(pa)]) * ela[size_t(j)], lb = inverse(rel_b[size_t(pb)]) * rel_b[size_t(k)];
				lo[size_t(k)] = as_offset(rigid{ la.r * glm::inverse(lb.r), la.t - lb.t });
			}
		}
		// (the head and what hangs from it go with the torso: eased by its own pose, the head kept the
		// last move's tilt and slowly turned back, looking up 30 to 45 degrees after a move; the
		// strands, each piece eased alone, came apart)
		static const bool pinned = getenv("VF_HEAD_PINNED") != nullptr;   // testing: as it was
		if (pinned && b.torso[pl] >= 0 && b.torso[pl] < n)
			for (int k = 0; k < n; k++)
				if ((k < int(b.is_head[pl].size()) && b.is_head[pl][size_t(k)]) || (k < int(b.strand[pl].size()) && b.strand[pl][size_t(k)]))
					o[size_t(k)] = o[size_t(b.torso[pl])];
		po.key = kb;
		po.root = as_offset(rb * root_a);
		static const bool log = getenv("VF_POSELOG") != nullptr;   // testing
		if (log) { float mx = 0; for (auto &x : o) mx = std::max(mx, 2 * std::acos(std::min(1.0f, std::fabs(x[0]))) * 57.3f); { int nh = 0; for (auto h : b.is_head[pl]) nh += h; fprintf(stderr, "POSE %d pl %d move %x->%x n %d torso %d heads %d max %.0f deg, hips %.0f deg\n", b.frame, pl, a.moves[pl], b.moves[pl], n, b.torso[pl], nh, mx, 2 * std::acos(std::min(1.0f, std::fabs(po.root[0]))) * 57.3f); } }
		po.o = std::move(o);
		po.lo = std::move(lo);
		po.start = a.frame;
	}
}

// While a random fighter's pose eases: its slots' matrices for the moment drawn (m_blend), the
// hips and each part about them as blended, offset by what's left of the old pose. In the two
// lists the pose changed between, the pose is the newer's (blending into it would be the jump).
void model1::pose_apply(const logic_frame &b)
{
	static const bool off = getenv("VF_NOPOSE") != nullptr;   // testing: as without
	if (off) return;
	for (int pl = 0; pl < 2; pl++) {
		const pose_offset &po = m_pose_off[pl];
		if (po.start < 0 || b.mine[pl].empty() || po.o.size() != po.key.size()) continue;
		const std::vector<uint64_t> kb = part_keys(b, pl);
		const float w = ease(m_draw_time - po.start);
		const bool changing = m_logic[0].frame == int(po.start);
		if (w <= 0 && !changing) continue;
		const int rs = b.mine[pl][b.root[pl]];
		auto drawn = [&](int s) { return rigid_of(!changing && s < int(m_blend_ok.size()) && m_blend_ok[s] ? m_blend[s].data() : b.slots[s].mat); };
		const rigid root = drawn(rs), from = inverse(root), to = root * offset_at(po.root, w);
		const int n = int(b.mine[pl].size());
		std::vector<rigid> rel(static_cast<size_t>(n));
		std::vector<int> g(static_cast<size_t>(n), -1);
		for (int k = 0; k < n; k++) {
			rel[size_t(k)] = from * drawn(b.mine[pl][k]);
			for (size_t i = 0; i < po.key.size() && g[size_t(k)] < 0; i++) if (po.key[i] == kb[size_t(k)]) g[size_t(k)] = int(i);
		}
		const std::vector<rigid> eased = eased_pose(rel, b.limb[pl], g, po.o, po.lo, w, b.is_head[pl]);
		for (int k = 0; k < n; k++) {
			const int s = b.mine[pl][k];
			if (s >= int(m_blend.size()) || b.slots[s].polys.empty()) continue;
			// (a part with no offset as it is, but a limb's part on its eased part)
			if (g[size_t(k)] < 0 && (k >= int(b.limb[pl].size()) || b.limb[pl][size_t(k)] < 0)) continue;
			store(to * eased[size_t(k)], m_blend[s].data());
			m_blend_from[s] = m_blend_to[s] = m_blend[s];
			if (!m_blend_ok[s]) m_blend_light[s] = { b.slots[s].light[0], b.slots[s].light[1], b.slots[s].light[2], b.slots[s].has_light ? 1.0f : 0.0f };
			m_blend_ok[s] = 1;
		}
	}
}

// TESTONLY (VF_JOINTLOG): each fighter's limb joints as the list has them: the angle between where
// a part's model ends (its far points) and where its next part's origin is drawn, in the part's
// frame, and that origin's distance over the model's reach. Any fighter, random or not.
void model1::joint_log(const logic_frame &lf)
{
	if (m_ram_a.size() < 0x1f00) return;
	auto &rom = m_regions.maincpu;
	auto rd = [&](uint32_t a) { return uint32_t(rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | uint32_t(rom[a + 3]) << 24); };
	auto poly = [&](uint32_t m) -> uint32_t { return (m >> 16) == 0x2d && m + 12 <= rom.size() ? rd(m + 8) & 0x7fffff : 0; };
	const float *pd = reinterpret_cast<const float *>(m_poly_rom);
	static const uint32_t JOINTS[][2] = { { 0x1a, 0x1e }, { 0x26, 0x2a }, { 0x1e, 0 }, { 0x2a, 0 }, { 0x36, 0x3a }, { 0x42, 0x46 }, { 0x3a, 0x3e }, { 0x46, 0x4a } };
	std::vector<uint8_t> used(lf.slots.size(), 0);
	for (int pl = 0; pl < 2; pl++) {
		const uint32_t o = 0x6b0 + 0x1800 * pl;
		const uint32_t rec = uint32_t(m_ram_a[o] | m_ram_a[o + 1] << 8 | m_ram_a[o + 2] << 16 | uint32_t(m_ram_a[o + 3]) << 24);
		if (rec + RECORD_SIZE > rom.size() || (rec >> 16) != 0xfd) continue;
		const uint16_t move = uint16_t(m_ram_a[o + 0x18] | m_ram_a[o + 0x19] << 8);
		for (const auto &j : JOINTS) {
			const uint32_t pp = poly(rd(rec + j[0])), cp = j[1] ? poly(rd(rec + j[1])) : 0;
			if (!pp || pp >= 0x400000) continue;
			int a = -1, b = -1;
			for (int i = 0; i < int(lf.slots.size()) && a < 0; i++) if (!used[size_t(i)] && !lf.slots[size_t(i)].polys.empty() && lf.slots[size_t(i)].polys[0] == pp) a = i;
			if (a < 0) continue;
			if (!j[1]) b = a + 1 < int(lf.slots.size()) ? a + 1 : -1;   // (the hand: drawn next)
			else for (int i = a + 1; i < int(lf.slots.size()) && b < 0; i++) if (!lf.slots[size_t(i)].polys.empty() && lf.slots[size_t(i)].polys[0] == cp) b = i;
			if (b < 0) continue;
			used[size_t(a)] = 1;
			const rigid P = rigid_of(lf.slots[size_t(a)].mat), C = rigid_of(lf.slots[size_t(b)].mat);
			glm::mat3 r = P.r;
			float sc = glm::length(r[0]);
			const glm::vec3 d = glm::inverse(r) * (C.t - P.t);   // (in the model's own units)
			const auto &pts = model_points(pd, pp);
			float reach = 0;
			for (size_t k = 2; k < pts.size(); k++) reach = std::max(reach, glm::length(glm::vec3(pts[k][0], pts[k][1], pts[k][2])));   // (not the first two: the model's own pair)
			glm::vec3 e(0);
			int n = 0;
			for (size_t k = 2; k < pts.size(); k++) { const glm::vec3 v(pts[k][0], pts[k][1], pts[k][2]); if (glm::length(v) > 0.75f * reach) { e += v; n++; } }
			if (!n || glm::length(d) < 1e-6f) continue;
			e /= float(n);
			const float ang = std::acos(std::clamp(glm::dot(glm::normalize(e), glm::normalize(d)), -1.0f, 1.0f)) * 57.2958f;
			fprintf(stderr, "JOINT %d pl %d rec %06x rnd %d move %x j %02x ang %.1f len %.3f\n", lf.frame, pl, rec, int(m_random_names.count(rec)), move, j[0], ang, glm::length(d) / glm::length(e));
			(void)sc;
		}
		// each part's turn about the hips (the first slot drawing each record part)
		{
			const uint32_t hip = poly(rd(rec + 0x32));
			int hsl = -1;
			for (int i = 0; i < int(lf.slots.size()) && hsl < 0; i++) if (!lf.slots[size_t(i)].polys.empty() && lf.slots[size_t(i)].polys[0] == hip && used[size_t(i)] != 3) hsl = i;
			if (hsl >= 0) {
				used[size_t(hsl)] = 3;
				const glm::mat3 hi = glm::inverse(rigid_of(lf.slots[size_t(hsl)].mat).r);
				fprintf(stderr, "PARTS %d pl %d move %x", lf.frame, pl, move);
				for (uint32_t off : { 0x12u, 0x1au, 0x1eu, 0x26u, 0x2au, 0x36u, 0x3au, 0x42u, 0x46u }) {
					const uint32_t pp = poly(rd(rec + off));
					int sl = -1;
					for (int i = hsl; i < int(lf.slots.size()) && sl < 0; i++) if (!lf.slots[size_t(i)].polys.empty() && lf.slots[size_t(i)].polys[0] == pp) sl = i;
					if (sl < 0) for (int i = 0; i < int(lf.slots.size()) && sl < 0; i++) if (!lf.slots[size_t(i)].polys.empty() && lf.slots[size_t(i)].polys[0] == pp) sl = i;
					glm::mat3 r = sl >= 0 ? hi * rigid_of(lf.slots[size_t(sl)].mat).r : glm::mat3(1);
					for (int c = 0; c < 3; c++) r[c] = glm::normalize(r[c]);
					const glm::quat q = glm::normalize(glm::quat_cast(r));
					fprintf(stderr, " %02x:%.3f,%.3f,%.3f,%.3f", off, q.w, q.x, q.y, q.z);
				}
				fprintf(stderr, "\n");
			}
		}
		// the head against the torso: its turn in the torso's frame (the first slot drawing the head)
		const uint32_t tp = poly(rd(rec + 0x12)), hl = rd(rec + 0x72);
		std::vector<uint32_t> hps;
		if ((hl >> 16) == 0xfd && hl + 0x224 <= rom.size()) for (uint32_t k = 0; k < 121; k++) { const uint32_t q = poly(rd(hl + 0x40 + 4 * k)); if (!q) break; hps.push_back(q); }
		else if (const uint32_t q = poly(rd(rec + 0x16))) hps.push_back(q);
		int ts = -1, hs = -1;
		for (int i = 0; i < int(lf.slots.size()); i++) {
			if (lf.slots[size_t(i)].polys.empty() || used[size_t(i)] == 2) continue;
			if (ts < 0 && lf.slots[size_t(i)].polys[0] == tp) ts = i;
			else if (ts >= 0 && hs < 0) for (uint32_t q : lf.slots[size_t(i)].polys) if (std::find(hps.begin(), hps.end(), q) != hps.end()) { hs = i; break; }
		}
		if (ts >= 0 && hs >= 0) {
			used[size_t(ts)] = used[size_t(hs)] = 2;
			glm::mat3 r = glm::inverse(rigid_of(lf.slots[size_t(ts)].mat).r) * rigid_of(lf.slots[size_t(hs)].mat).r;
			for (int c = 0; c < 3; c++) r[c] = glm::normalize(r[c]);
			fprintf(stderr, "NECK %d pl %d rec %06x rnd %d move %x hs %d r", lf.frame, pl, rec, int(m_random_names.count(rec)), move, hs - ts);
			for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) fprintf(stderr, " %.4f", r[c][k]);
			// the head as drawn (the camera's frame), its place and the hips', and its lean (random_fighter_heads)
			glm::mat3 hv = rigid_of(lf.slots[size_t(hs)].mat).r;
			for (int c = 0; c < 3; c++) hv[c] = glm::normalize(hv[c]);
			fprintf(stderr, " v");
			for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) fprintf(stderr, " %.4f", hv[c][k]);
			const uint32_t hip = poly(rd(rec + 0x32));
			glm::vec3 ht = rigid_of(lf.slots[size_t(hs)].mat).t, pt(0);
			for (const auto &sl : lf.slots) if (!sl.polys.empty() && sl.polys[0] == hip) { pt = rigid_of(sl.mat).t; break; }
			// (and the other's hips: which way this one faces)
			const uint32_t o2 = 0x6b0 + 0x1800 * (pl ^ 1);
			const uint32_t rec2 = uint32_t(m_ram_a[o2] | m_ram_a[o2 + 1] << 8 | m_ram_a[o2 + 2] << 16 | uint32_t(m_ram_a[o2 + 3]) << 24);
			glm::vec3 ot = pt;
			if (rec2 + RECORD_SIZE <= rom.size()) {
				const uint32_t hip2 = poly(rd(rec2 + 0x32));
				int seen = 0;
				for (const auto &sl : lf.slots)
					if (!sl.polys.empty() && sl.polys[0] == hip2) { const glm::vec3 t = rigid_of(sl.mat).t; if (glm::length(t - pt) > 1e-3f) { ot = t; break; } if (++seen > 1) break; }
			}
			const uint32_t bk = 0x400700 + 0x1800 * pl - 0x400000;   // (its block: the blend's mode and index, ff6390)
			fprintf(stderr, " mode %d idx %d", bk + 0x1735 < m_ram_a.size() ? m_ram_a[bk + 0x1733] : 0, bk + 0x1735 < m_ram_a.size() ? m_ram_a[bk + 0x1735] : 0);
			fprintf(stderr, " up %.3f %.3f %.3f opp %.3f %.3f %.3f lean %.2f head %06x\n", ht.x - pt.x, ht.y - pt.y, ht.z - pt.z, ot.x - pt.x, ot.y - pt.y, ot.z - pt.z, m_random_names.count(rec) ? m_lean_turn[pl] : 0.0f, rd(rec + 0x72));
		}
	}
}
