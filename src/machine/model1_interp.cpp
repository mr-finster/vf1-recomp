// Smooth motion above 1x. VF's game logic runs at 30 Hz: it builds a new display list every
// second frame, so the picture moves 30 times a second whatever the screen can show. In smooth
// mode each new list is kept (with the sky's scroll), and the picture is drawn for any moment
// between the last two: every object's matrix (its place, turn and the camera's) is blended
// with the same object's in the list before, and the sky's scroll likewise. The game itself runs
// untouched; the picture is one game frame (two video frames) behind it.
#include "model1.h"
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/matrix.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace {

// An object in a list: its polygons' address, and which time that address comes in the list.
uint64_t object_key(uint32_t poly_adr, uint32_t nth) { return uint64_t(poly_adr) << 20 | nth; }

// (records) A camera as the game keeps it: 12 floats, the rotation's columns, then the move.
glm::mat4 camera_matrix(const float *m)
{
	glm::mat4 r(1);
	for (int c = 0; c < 3; c++) r[c] = glm::vec4(m[c * 3], m[c * 3 + 1], m[c * 3 + 2], 0);
	r[3] = glm::vec4(m[9], m[10], m[11], 1);
	return r;
}

// A camera that turns over 25 degrees or jumps over 8 units in a game frame (a car's move is about 3)
bool camera_cut(const glm::mat4 &from, const glm::mat4 &to)
{
	const glm::mat3 turn = glm::mat3(to) * glm::transpose(glm::mat3(from));
	const float angle = std::acos(std::clamp((turn[0][0] + turn[1][1] + turn[2][2] - 1) / 2, -1.0f, 1.0f));
	const float jump = glm::length(glm::vec3(glm::affineInverse(to)[3]) - glm::vec3(glm::affineInverse(from)[3]));
	return angle > glm::radians(25.0f) || jump > 8;
}

// a turn as a rotation vector (the shorter way round) and back
glm::vec3 turn_vector(glm::quat q)
{
	if (q.w < 0) q = -q;
	const glm::vec3 v(q.x, q.y, q.z);
	const float s = glm::length(v);
	return s < 1e-7f ? 2.0f * v : v * (2 * std::atan2(s, q.w) / s);
}

glm::quat turn_quat(const glm::vec3 &v)
{
	const float a = glm::length(v);
	return a < 1e-7f ? glm::quat(1, 0.5f * v.x, 0.5f * v.y, 0.5f * v.z) : glm::angleAxis(a, v / a);
}

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
// (video_traits::records) A word of a display list written: what the game draws with it, when
// it is a matrix command's (the record in the register, else the still world), and the camera.
void model1::list_written(int buffer, uint32_t word)
{
	if (!m_smooth || !m_video->records) return;
	const game::draw_records &r = *m_video->records;
	std::vector<uint32_t> &owners = m_list_owner[buffer];
	if (owners.empty()) owners.assign(0x8000, 0);
	uint32_t owner = 0;
	for (const game::matrix_writer &w : r.writers)
		if (m_maincpu.pc() == w.pc) {
			const uint32_t rec = m_maincpu.reg(w.record_register);
			owner = rec >= r.records_from && rec < r.records_to ? rec : 1;
			memcpy(m_list_camera[buffer].data(), &m_ram_b[r.camera - 0x500000], sizeof m_list_camera[buffer]);
			m_list_has_camera[buffer] = true;
		}
	owners[word & 0x7fff] = owner;
}

void model1::index_objects(logic_frame &lf)
{
	const std::vector<uint32_t> &owners = m_list_owner[m_picture_buffer];
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
			if (!owners.empty()) sl.owner = owners[off & 0x7fff];
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
		lf.camera = m_list_camera[m_picture_buffer];
		// (a list with nothing of a record or the world in it keeps an older list's camera)
		lf.has_camera = m_list_has_camera[m_picture_buffer] && m_video->records
			&& std::any_of(lf.slots.begin(), lf.slots.end(), [](const slot &sl) { return sl.owner != 0; });
		objects = lf.objects;
		static const bool log = getenv("M1_LOG_SMOOTH") != nullptr;
		if (lf.hash != m_logic[1].hash) {
			smooth_turn(lf, m_logic[1]);
			m_logic_prev = std::move(m_logic[0]);
			m_logic[0] = std::move(m_logic[1]);
			m_logic[1] = std::move(lf);
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
	if (!m_video->smooth_frames) {
		m_replaying = false;
		m_replay_older = false;
		m_replay_extra = false;
		screen_update(m_bitmap, m_bitmap.cliprect());
		drawn();
		return true;
	}
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
	blend_slots(a, b, alpha);
	// A cut can't be blended: the older view stays until the newer one's moment (as the blend
	// would), so the cut lands when the game's sky, written two frames later, changes too. It
	// stood still meanwhile, a game frame (4 refreshes at 120 Hz) at every camera cut; now it
	// carries on from the list before it as it was moving and eases to a stop at the cut, half
	// a game frame's move on at most (straight on, a fast camera swung into views the game never
	// showed).
	const bool cut = m_blend_cut;
	m_replay_older = cut && alpha < 1;
	m_replay_extra = false;
	if (m_replay_older && !m_logic_prev.list.empty() && b.frame - a.frame == a.frame - m_logic_prev.frame) {
		blend_slots(m_logic_prev, a, 1 + 0.5 * (1 - (1 - alpha) * (1 - alpha)));
		m_replay_extra = !m_blend_cut;
	}
	m_blend_cut = cut;
	if (cut && b.frame != m_cut_frame) m_cut_frame = b.frame;
	// After a cut the game moves the sky into place over a few frames (at most 4): the picture
	// before the cut used to stay until it stopped jumping, a picture held for 2-7 refreshes at
	// every replay's camera cut; now the new view shows at once, its sky where it settles
	// (smooth_sky_shift).
	// (the camera's RAM has the new view from the cut's frame on; the scroll the game writes
	// follows it a frame later, mixed with the old view's, and lands one or two frames after that)
	m_sky_settling_now = false;
	if (!m_replay_older && m_cut_frame >= 0 && int(m_frame) - 1 <= m_cut_frame + 4) {
		const sky_sample &cur = m_sky_hist[(m_frame - 1) % SKY_HIST];
		m_sky_settling_now = cur.frame == int(m_frame) - 1 && (cur.lands_h != cur.h || cur.lands_v != cur.v);
		// (the tile layer moves 24 pixels at most: further, the picture before the cut stays a frame)
		if (m_sky_settling_now && !m_redrawing && !sky_reaches(cur)) { m_replaying = false; m_alpha = 1; return false; }
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
	const auto render_done = m_render_done;     // emulation state: drawing again mustn't touch it
	const int quads = m_last_quads;
	screen_update(m_bitmap, m_bitmap.cliprect());
	m_render_done = render_done;
	m_last_quads = quads;
	m_replaying = false;
	m_replay_older = false;
	m_replay_extra = false;
	drawn();
	static const bool log = getenv("M1_LOG_SMOOTH") != nullptr;
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
	memcpy(s.camera, &m_ram_b[m_profile->camera_ram], 4);
	s.camera_wide = m_camera_wide;
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
	std::swap_ranges(&m_ram_b[m_profile->camera_ram], &m_ram_b[m_profile->camera_ram + 4], s.camera);   // (the camera pointer: work RAM, put back after)
	std::swap(m_camera_wide, s.camera_wide);
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
	// between the two that the game never showed.
	{
		const double tr = std::clamp((R[0][0] + R[1][1] + R[2][2] - 1) / 2, -1.0, 1.0);
		double mv = 0, dp = 0;
		for (size_t i = 0; i < P.size(); i++)
			if (use[i]) {
				mv += std::sqrt((Q[i].x - P[i].x) * (Q[i].x - P[i].x) + (Q[i].y - P[i].y) * (Q[i].y - P[i].y) + (Q[i].z - P[i].z) * (Q[i].z - P[i].z));
				dp += std::max(std::fabs(P[i].z), 1.0);
			}
		m_camera_cut = std::acos(tr) > 15 * M_PI / 180 || (dp > 0 && mv > 0.085 * dp);
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
	if (m_video->records && a.has_camera && b.has_camera) { blend_by_records(a, b, alpha); return; }
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
	repaired += repair_by_camera(a, b, pair, changed_run);
	static const bool logp = getenv("M1_LOG_SMOOTH") != nullptr;
	if (logp && repaired && !m_redrawing) fprintf(stderr, "P frame %d repaired %d\n", b.frame, repaired);
	int jumps = 0, paired = 0, missing = 0;
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
		if (dx * dx + dy * dy + dz * dz > 0.09f * depth * depth || turned) jumps++;    // fast, or a cut
		for (int k = 0; k < 12; k++) m_blend[i][k] = float(pa[k] + (pb[k] - pa[k]) * alpha);
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
	m_blend_cut = jumps * 2 > paired || missing * 8 > paired || m_camera_cut;
	// Carried on past the newer list (blend_slots(older, newer, 1 to 1.5) at a cut, half a game frame
	// on at most): each part's own blend carried on grew the fast-turning ones (a kicking leg up to
	// twice its length) and moved each by itself, apart from the next (made rigid, a hand came off
	// its wrist). Every part is drawn as the newer list has it, all moved on together by the parts'
	// average move between the two lists: the whole picture carries on, nothing comes apart. Only
	// when nothing moved fast: a camera swinging into a cut (TIME OVER's), a body mid-throw or a
	// kicking leg carried on into views and poses the game never showed, so then the newer list
	// is held as it is until the cut.
	if (alpha > 1 && !m_blend_cut) {
		double mv[3] = {};
		int n = 0;
		for (size_t i = 0; i < b.slots.size() && !jumps; i++)
			if (m_blend_ok[i]) { for (int k = 0; k < 3; k++) mv[k] += m_blend_to[i][9 + k] - m_blend_from[i][9 + k]; n++; }
		for (size_t i = 0; i < b.slots.size(); i++)
			if (m_blend_ok[i]) {
				memcpy(m_blend[i].data(), m_blend_to[i].data(), 12 * sizeof(float));
				for (int k = 0; k < 3; k++) m_blend[i][9 + k] += float((alpha - 1) * mv[k] / std::max(n, 1));
			}
	}
	if (m_blend_cut) std::fill(m_blend_ok.begin(), m_blend_ok.end(), 0);
	m_smooth_stats[1] = jumps;
	static const bool logm = getenv("M1_LOG_SMOOTH") != nullptr;
	if (logm && (jumps || missing)) fprintf(stderr, "M jumps %d missing %d paired %d slots %zu/%zu\n", jumps, missing, paired, na, nb);
	for (size_t i = 0; i < b.slots.size(); i++)
		if (!b.slots[i].polys.empty()) m_smooth_stats[m_blend_ok[i] ? 2 : 0]++;
}

// (records) The camera's turn smoothed, as each list comes. The game turns its camera unevenly
// (in a bend, 0.5 degrees a game frame and then 0.06, never made up), so the road's lines jerked
// sideways. An alpha-beta filter: the turn expected at the last list's speed, a part of the miss
// taken in (0.4) and a part into the speed (0.1); a steady turn comes through as it is. Never
// over a degree from the camera's own (a car the camera follows stays near its place; the excess
// half into the speed): what is left is evened out, the camera's sweeps are not. Afresh at a cut.
void model1::smooth_turn(logic_frame &lf, const logic_frame &before)
{
	lf.turn_age = 0;
	if (!lf.has_camera) return;
	const glm::mat4 cam = camera_matrix(lf.camera.data());
	const glm::quat q = glm::quat_cast(glm::mat3(cam));
	glm::quat est = q;
	glm::vec3 rate(0);
	lf.turn_age = 1;
	if (before.has_camera && before.turn_age > 0 && !camera_cut(camera_matrix(before.camera.data()), cam)) {
		lf.turn_age = 2;
		if (before.turn_age == 1)                    // the first move after a cut: its speed as it is
			rate = turn_vector(q * glm::inverse(glm::quat_cast(glm::mat3(camera_matrix(before.camera.data())))));
		else {
			constexpr float ALPHA = 0.4f, BETA = 0.1f, LIMIT = glm::radians(1.0f);
			const glm::vec3 last_rate(before.turn_rate[0], before.turn_rate[1], before.turn_rate[2]);
			const glm::quat expected = turn_quat(last_rate) * glm::quat(before.turn[0], before.turn[1], before.turn[2], before.turn[3]);
			const glm::vec3 miss = turn_vector(q * glm::inverse(expected));
			est = turn_quat(miss * ALPHA) * expected;
			rate = last_rate + miss * BETA;
			const glm::vec3 off = turn_vector(q * glm::inverse(est));
			const float apart = glm::length(off);
			if (apart > LIMIT) {
				const glm::vec3 excess = off * (1 - LIMIT / apart);
				est = turn_quat(excess) * est;
				rate += excess * 0.5f;
			}
		}
	}
	est = glm::normalize(est);
	lf.turn = { est.w, est.x, est.y, est.z };
	lf.turn_rate = { rate.x, rate.y, rate.z };
}

// A game whose draw records are known (video_traits::records: Virtua Racing). Each matrix in its
// lists came from a record (a car: its body, livery and wheels), from the still world, or from
// neither (the logo), and the camera it was drawn with is kept. So nothing is guessed: each
// record's parts are paired in the order the record drew them (each model's in order when it drew
// a different number); the still world is where the older camera saw it; a part of a record not
// paired (a wheel that changed model) moves with its record's first paired part; parts of no
// record are paired by model, and so is a still-world part that sat in its place on screen while
// the camera moved (the SEGA logo, drawn without the camera). Each is blended on screen, from the
// older matrix to the newer (a car the camera follows stays put; blending the camera and the car
// apart swung it about), a part along a curve that leaves the older list at the last move's speed
// (the game's steps are uneven). Then all of it is turned about the eye to the camera's smoothed
// turn (smooth_turn). A camera_cut isn't blended.
void model1::blend_by_records(const logic_frame &a, const logic_frame &b, double alpha)
{
	auto to_mat = camera_matrix;
	auto from_mat = [](const glm::mat4 &m, float *out) {
		for (int c = 0; c < 3; c++) for (int e = 0; e < 3; e++) out[c * 3 + e] = m[c][e];
		for (int e = 0; e < 3; e++) out[9 + e] = m[3][e];
	};
	auto camera = [&](const logic_frame &l) { return to_mat(l.camera.data()); };
	const glm::mat4 ca = camera(a), cb = camera(b), ib = glm::affineInverse(cb);
	const glm::mat3 turn = glm::mat3(cb) * glm::transpose(glm::mat3(ca));
	const float angle = std::acos(std::clamp((turn[0][0] + turn[1][1] + turn[2][2] - 1) / 2, -1.0f, 1.0f));
	const float jump = glm::length(glm::vec3(ib[3]) - glm::vec3(glm::affineInverse(ca)[3]));
	m_blend_cut = camera_cut(ca, cb);
	auto first = [](const slot &sl) { return sl.polys.empty() ? 0u : sl.polys[0]; };
	using owners_t = std::unordered_map<uint32_t, std::vector<size_t>>;
	auto by_owner = [](const logic_frame &l) {
		owners_t o;
		for (size_t i = 0; i < l.slots.size(); i++) if (!l.slots[i].polys.empty()) o[l.slots[i].owner].push_back(i);
		return o;
	};
	// each part of y: x's part it is (-1: none), and where x had it on screen
	auto older_views = [&](const logic_frame &x, const logic_frame &y, std::vector<int> &match, std::vector<glm::mat4> &view, std::vector<char> &known) {
		const size_t ny = y.slots.size();
		match.assign(ny, -1);
		view.assign(ny, glm::mat4(1));
		known.assign(ny, 0);
		const owners_t ox = by_owner(x), oy = by_owner(y);
		for (const auto &[owner, ys] : oy) {
			auto it = ox.find(owner);
			if (it == ox.end()) continue;
			const std::vector<size_t> &xs = it->second;
			if (owner >= 2 && xs.size() == ys.size()) {
				for (size_t k = 0; k < ys.size(); k++) match[ys[k]] = int(xs[k]);
				continue;
			}
			std::unordered_map<uint32_t, std::vector<size_t>> models;
			for (size_t q : xs) models[first(x.slots[q])].push_back(q);
			std::unordered_map<uint32_t, size_t> nth;
			for (size_t i : ys) {
				const uint32_t f = first(y.slots[i]);
				auto m = models.find(f);
				const size_t k = nth[f]++;
				if (m != models.end() && k < m->second.size()) match[i] = int(m->second[k]);
			}
		}
		const glm::mat4 cx = camera(x), iy = glm::affineInverse(camera(y)), ix = glm::affineInverse(cx);
		for (size_t i = 0; i < ny; i++) {
			const slot &sy = y.slots[i];
			if (sy.polys.empty()) continue;
			if (sy.owner == 1) {
				// the still world: where the older camera saw it. Unless its model's copy in the older
				// list sat in its place on screen and the camera's move doesn't put it there: a part
				// drawn without the camera (the SEGA logo, which the game draws as it draws the track)
				view[i] = cx * iy * to_mat(sy.mat);
				known[i] = 1;
				if (match[i] >= 0) {
					const glm::vec3 at(sy.mat[9], sy.mat[10], sy.mat[11]);
					const float close = 0.01f * std::max(std::fabs(at.z), 0.1f);
					const glm::mat4 own = to_mat(x.slots[match[i]].mat);
					if (glm::length(glm::vec3(own[3]) - at) <= close && glm::length(glm::vec3(view[i][3]) - at) > close) view[i] = own;
					else match[i] = -1;
				}
				continue;
			}
			if (match[i] < 0) continue;
			view[i] = to_mat(x.slots[match[i]].mat);
			known[i] = 1;
			// a record's part that went far in the world: the record used for another thing
			if (sy.owner >= 2 && glm::length(glm::vec3((ix * view[i])[3]) - glm::vec3((iy * to_mat(sy.mat))[3])) > 20) match[i] = -1, known[i] = 0;
		}
		for (const auto &[owner, ys] : oy) {
			if (owner < 2) continue;
			int lead = -1;
			for (size_t i : ys) if (match[i] >= 0) { lead = int(i); break; }
			if (lead < 0) continue;
			// the record's move in the world, taken back from the newer part's place
			const glm::mat4 back = ix * view[lead] * glm::affineInverse(iy * to_mat(y.slots[lead].mat));
			for (size_t i : ys) if (match[i] < 0) { view[i] = cx * back * iy * to_mat(y.slots[i].mat); known[i] = 1; }
		}
	};
	std::vector<int> match, match_prev;
	std::vector<glm::mat4> from, prev;
	std::vector<char> known, known_prev;
	older_views(a, b, match, from, known);
	// the list before the older one, a game frame before it: the moves into the older list
	const logic_frame &p = m_logic_prev;
	const float u = float(alpha);
	const bool has_prev = u <= 1 && &a == &m_logic[0] && p.has_camera && !p.list.empty()
		&& p.frame < a.frame && a.frame - p.frame <= 2 * (b.frame - a.frame);
	if (has_prev) older_views(p, a, match_prev, prev, known_prev);
	const float gap = float(b.frame - a.frame) / float(std::max(a.frame - p.frame, 1));
	const size_t nb = b.slots.size();
	for (size_t i = 0; i < nb; i++) {
		const slot &sb = b.slots[i];
		if (sb.polys.empty() || !known[i]) continue;
		float older_m[12];
		from_mat(from[i], older_m);
		for (int k = 0; k < 12; k++) m_blend[i][k] = older_m[k] + (sb.mat[k] - older_m[k]) * u;
		// leaving the older list at its last move's speed, when the two moves are near alike
		bool have = false;
		glm::vec3 was(0);
		if (has_prev && match[i] >= 0 && known_prev[match[i]]) {
			was = glm::vec3(from[i][3]) - glm::vec3(prev[match[i]][3]);
			have = true;
		}
		if (have) {
			const glm::vec3 now(sb.mat[9] - older_m[9], sb.mat[10] - older_m[10], sb.mat[11] - older_m[11]);
			was *= gap;
			if (glm::length(was - now) <= 0.5f * std::max(glm::length(was), glm::length(now)) + 0.02f)
				for (int k = 0; k < 3; k++) m_blend[i][9 + k] += u * (1 - u) * (1 - u) * (was[k] - now[k]);
		}
		const slot &light_from = match[i] >= 0 ? a.slots[match[i]] : sb;
		memcpy(m_blend_from[i].data(), older_m, sizeof older_m);
		memcpy(m_blend_to[i].data(), sb.mat, sizeof older_m);
		m_blend_light[i] = { light_from.light[0], light_from.light[1], light_from.light[2], light_from.has_light ? 1.0f : 0.0f };
		m_blend_ok[i] = 1;
	}
	// The camera's turn smoothed (smooth_turn): everything drawn with the camera turned about the
	// eye from the blend's turn to the smoothed one (the cars and the world alike, so nothing slides
	// on the track); a curve through the smoothed turns at each list's turn speed. Past the newer
	// list (before a cut), as at it.
	if (!m_blend_cut && a.turn_age > 0 && b.turn_age == 2) {
		const float t = std::min(u, 1.0f);
		const glm::quat qa(a.turn[0], a.turn[1], a.turn[2], a.turn[3]), qb(b.turn[0], b.turn[1], b.turn[2], b.turn[3]);
		const glm::vec3 wa(a.turn_rate[0], a.turn_rate[1], a.turn_rate[2]), wb(b.turn_rate[0], b.turn_rate[1], b.turn_rate[2]);
		const float h10 = t * t * t - 2 * t * t + t, h01 = 3 * t * t - 2 * t * t * t, h11 = t * t * t - t * t;
		const glm::mat3 smooth = glm::mat3_cast(turn_quat(wa * h10 + turn_vector(qb * glm::inverse(qa)) * h01 + wb * h11) * qa);
		const glm::mat3 ra(ca), rb(cb);
		const glm::mat4 steer(smooth * glm::inverse(ra + (rb - ra) * t));
		for (size_t i = 0; i < nb; i++) {
			const slot &sb = b.slots[i];
			if (sb.polys.empty() || (sb.owner == 1 && match[i] >= 0)) continue;     // (drawn without the camera)
			if (!m_blend_ok[i]) {                    // not blended: as it is, but turned too
				memcpy(m_blend[i].data(), sb.mat, sizeof sb.mat);
				memcpy(m_blend_from[i].data(), sb.mat, sizeof sb.mat);
				memcpy(m_blend_to[i].data(), sb.mat, sizeof sb.mat);
				m_blend_light[i] = { sb.light[0], sb.light[1], sb.light[2], sb.has_light ? 1.0f : 0.0f };
				m_blend_ok[i] = 1;
			}
			from_mat(steer * to_mat(m_blend[i].data()), m_blend[i].data());
		}
	}
	static const bool logm = getenv("M1_LOG_SMOOTH") != nullptr;
	if (logm && m_blend_cut && !m_redrawing) fprintf(stderr, "M cut frame %d turn %.1f move %.2f\n", b.frame, glm::degrees(angle), jump);
	if (m_blend_cut) std::fill(m_blend_ok.begin(), m_blend_ok.end(), 0);
	for (size_t i = 0; i < nb; i++)
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

// The game's sky scroll (its routine at the start of vblank) comes from the camera (the RAM in
// game::video_traits::sky_scroll): across, its heading (1/32 pixel, 2048 pixels for a turn); up
// and down, its height (whole pixels). Each is mixed with the last game frame's: halfway across on
// the frames between the game's 30 a second, and a quarter or three quarters up and down; then
// written to the sky map's scroll in whole pixels (h: the heading >> 5; v: a sum >> 1). When a
// write is that sum, the fraction it dropped is kept for smooth motion's sky (worked out again
// from the RAM it read: nothing writes those in between).
model1::sky_scroll model1::sky_scroll_from_ram(bool between) const
{
	const game::sky_camera &cam = *m_video->sky_scroll;
	auto rd = [&](uint32_t x) { return uint16_t(m_ram_b[x - 0x500000] | m_ram_b[x - 0x500000 + 1] << 8); };
	uint16_t r1 = rd(cam.heading);
	if (between) r1 = uint16_t(r1 - (int16_t(uint16_t(r1 - rd(cam.last_heading))) >> 1));
	uint16_t r3 = rd(cam.height), r4 = rd(cam.last_height);
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
	if (!m_video->sky_scroll) {         // (no camera known: where the tile chip has it)
		h = m_tiles.tile_r(0x5002) & 0x1ff;
		v = m_tiles.tile_r(0x5006) & 0x3ff;
		return;
	}
	const game::sky_camera &cam = *m_video->sky_scroll;
	auto rd = [&](uint32_t x) { return uint16_t(m_ram_b[x - 0x500000] | m_ram_b[x - 0x500000 + 1] << 8); };
	h = (rd(cam.heading) >> 5) & 0x1ff;
	v = (rd(cam.height) + 0x21) & 0x3ff;
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
	if (!m_video->sky_scroll) return;   // (no camera known: no fractions, the scroll as written)
	m_sky_between = m_ram_b[m_video->sky_scroll->between - 0x500000] & 1;
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
