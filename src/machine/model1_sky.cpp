// Hi-res replacements for the stages' sky panoramas.
//
// Each stage's sky is a record in the program's data: 8 blocks of 32 x rows tile map entries,
// 2048 pixels around for the full turn of the camera. As the camera turns, the game streams
// columns of it into tile map 2 (rows 6 on), a 64-column ring, and scrolls the map. To draw a
// replacement picture in its place we need, every frame, which panorama column is where on the
// screen: the map columns the game streamed are matched against the record's own columns (the
// ones that occur once in the panorama), which gives the panorama position of the screen's left
// edge; the rows come from where the tile chip drew each line from. The replacement is then
// drawn across the whole width, so widescreen shows the real panorama to the sides.
#include "model1.h"
#include <future>
#include <memory>
#include <thread>

namespace {

// The records' places in the program ROM (all in the bank at 1100000), by the export's names.
// `top`: a record of one row the game streams over the panorama's top row (0: none). Wolf's stage
// has one (in ROMX at 2e7aa4): the dithered blend from the sky's colour into the clouds. Drawn from
// the panorama's own top row instead, the clouds started at a hard line above 1x.
struct sky_record { const char *name; uint32_t rom; uint32_t top = 0; };
const sky_record SKY_RECORDS[] = {
	{ "beach_clouds", 0x110002c }, { "canyon_mesas", 0x11117a8 }, { "night_city", 0x1134370 },
	{ "lake_mountain", 0x114c878 }, { "desert_dunes", 0x11655b4 }, { "karst_mountains", 0x116af60 },
	{ "dusk_clouds", 0x118a0e8 },
	{ "sunset_clouds", 0x11190b8, 0x2e7aa4 },   // Wolf's stage
};
constexpr uint32_t SKY_BANK = 0x1100000;
constexpr int SKY_MAP = 2, SKY_ROW0 = 6;    // streamed into tile map 2 from row 6 (704300)

// A streamed column's identity: its tiles from the second row down. (Wolf's stage writes over the
// top row of every column it streams, so whole columns never matched his sky's record.)
uint64_t column_hash(const uint16_t *e, int stride, int rows)
{
	e += stride; rows -= 1;
	uint64_t h = 1469598103934665603ull;
	for (int r = 0; r < rows; r++) h = (h ^ e[size_t(r) * stride]) * 1099511628211ull;
	return h;
}

} // namespace

std::vector<std::string> model1::sky_names()
{
	std::vector<std::string> n;
	for (const auto &r : SKY_RECORDS) n.push_back(r.name);
	return n;
}

// Every stage's sky record, read once: the stages' own skies are drawn from these (built with the
// game's palette, see draw_sky) unless a replacement picture is loaded for one.
void model1::init_skies()
{
	if (!m_skies.empty()) return;
	m_skies.reserve(std::size(SKY_RECORDS));      // stable: the current sky is kept by pointer
	const auto &rom = m_regions.maincpu;
	auto rd16 = [&](uint32_t o) { return o + 1 < rom.size() ? uint16_t(rom[o] | rom[o + 1] << 8) : uint16_t(0); };
	auto rd32 = [&](uint32_t o) { return uint32_t(rd16(o)) | uint32_t(rd16(o + 2)) << 16; };
	for (const auto &rec : SKY_RECORDS) {
		sky_art a;
		a.name = rec.name;
		for (int b = 0; b < 8; b++) {
			uint32_t blk = (rec.rom & ~0xfffffu) + (rd32(rec.rom + 8 + 4 * b) & 0xfffff);
			a.rows = std::max(a.rows, std::min(int(rd32(blk + 4)), 48));
		}
		if (a.rows <= 0) continue;
		a.entries.assign(size_t(256) * a.rows, 0);
		for (int b = 0; b < 8; b++) {
			uint32_t blk = (rec.rom & ~0xfffffu) + (rd32(rec.rom + 8 + 4 * b) & 0xfffff);
			int rows = std::min(int(rd32(blk + 4)), 48), stride = int(rd32(blk + 8));
			for (int y = 0; y < rows; y++)
				for (int x = 0; x < 32; x++)
					a.entries[size_t(y) * 256 + b * 32 + x] = rd16(blk + 12 + 2 * (y * stride + x));
		}
		if (rec.top)                              // the row the game writes over the top one
			for (int b = 0; b < 8; b++) {
				uint32_t blk = (rec.top & ~0xfffffu) + (rd32(rec.top + 8 + 4 * b) & 0xfffff);
				if (rd32(blk + 4) < 1) continue;
				for (int x = 0; x < 32; x++) a.entries[size_t(b) * 32 + x] = rd16(blk + 12 + 2 * x);
			}
		// a panorama may repeat (the desert's two halves are the same): placed within one repeat
		a.period = 256;
		while (a.period > 8) {
			const int half = a.period / 2;
			bool same = true;
			for (int y = 0; y < a.rows && same; y++)
				for (int x = 0; x < 256 && same; x++)
					same = a.entries[size_t(y) * 256 + x] == a.entries[size_t(y) * 256 + (x + half) % 256];
			if (!same) break;
			a.period = half;
		}
		// the columns that occur once in a repeat: those place the panorama unambiguously
		std::unordered_map<uint64_t, int> count;
		for (int p = 0; p < a.period; p++) count[column_hash(&a.entries[p], 256, a.rows)]++;
		for (int p = 0; p < a.period; p++) {
			uint64_t hsh = column_hash(&a.entries[p], 256, a.rows);
			if (count[hsh] == 1) a.unique[hsh] = p;
		}
		// the palette lines and tiles it uses, to notice when its colours or pictures change
		for (uint16_t e : a.entries) {
			const int line = (e >> 7) & 0xff;
			if (std::find(a.lines.begin(), a.lines.end(), line) == a.lines.end()) a.lines.push_back(line);
			const uint32_t code = m_tiles.tile_code(e);
			if (std::find(a.codes.begin(), a.codes.end(), code) == a.codes.end()) a.codes.push_back(code);
		}
		m_skies.push_back(std::move(a));
	}
}

bool model1::load_sky(const std::string &name, std::vector<uint32_t> px, int w, int h)
{
	init_skies();
	if (w <= 0 || h <= 0 || px.size() < size_t(w) * h) return false;
	for (sky_art &a : m_skies)
		if (a.name == name) {
			a.px = std::move(px);
			a.w = w;
			a.h = h;
			a.original = false;
			return true;
		}
	return false;
}

// A stage's own sky is drawn from its record with the game's palette and tiles as they are now.
// While either is changing (a fade, a stage loading its tiles) the tile layer is drawn as it is;
// once they have held still for a few frames the panorama is built again.
bool model1::prepare_sky(sky_art &a)
{
	if (!a.original) return true;
	uint64_t sig = 1469598103934665603ull;
	for (int line : a.lines)
		for (int i = 0; i < 16; i++) sig = (sig ^ m_pens[(line * 16 + i) & 0x1fff]) * 1099511628211ull;
	for (uint32_t code : a.codes)                    // the tiles' pictures (a stage loads them over frames)
		for (int i = 0; i < 16; i++) sig = (sig ^ m_tiles.char_r(code * 16 + i)) * 1099511628211ull;
	if (sig == a.pen_sig && !a.px.empty()) return true;
	if (m_redrawing) return false;                   // drawing a shown picture again: as it was, settling or not
	if (sig != a.pending_sig) { a.pending_sig = sig; a.pending_count = 0; }
	if (++a.pending_count < 6) return false;
	const uint64_t t0 = trace_ns();
	a.w = 2048;
	a.h = a.rows * 8;
	a.px.assign(size_t(a.w) * a.h, 0);
	m_tiles.render_entries(a.entries.data(), 256, a.rows, a.px.data());
	a.pen_sig = sig;
	if (m_sky_scaled_for == &a) m_sky_scaled_for = nullptr;
	m_work.sky_build++;
	m_work.sky_build_ns += trace_ns() - t0;
	return true;
}

void model1::set_sky_filter(bool smooth)
{
	m_sky_smooth = smooth;
	m_sky_scaled_for = nullptr;                    // scaled again for the next frame
}

// After the sky layer is drawn into m_tile_tmp with m_tile_coord: which sky, and the panorama
// x of the 4:3 picture's left edge.
void model1::find_sky()
{
	m_sky_now = nullptr;
	const int tw = m_tile_tmp.width();
	const uint32_t c0 = m_tile_coord[size_t(SCREEN_H / 2) * tw + m_ext];
	if (c0 == ~0u || (c0 >> 18) != SKY_MAP) return;
	const int tx0 = int(c0 & 511);                   // map x at the picture's left edge
	int best_votes = 0, best_base = 0;
	sky_art *best = nullptr;
	for (sky_art &a : m_skies) {
		std::unordered_map<int, int> votes;
		for (int t = 0; t < 64; t++) {
			const int xs = (t * 8 - tx0) & 511;           // where map column t is on the screen
			if (xs > SCREEN_W - 8) continue;              // off screen: may be stale
			uint16_t col[48];
			for (int r = 0; r < a.rows; r++) col[r] = m_tiles.tile_r((SKY_MAP << 12) | ((SKY_ROW0 + r) * 64 + t));
			auto it = a.unique.find(column_hash(col, 1, a.rows));
			if (it != a.unique.end()) votes[(it->second * 8 - xs) & (a.period * 8 - 1)]++;
		}
		for (auto &[base, v] : votes)
			if (v > best_votes) { best_votes = v; best = &a; best_base = base; }
	}
	if (best_votes >= 2) {
		m_sky_now = m_sky_last = best;
		m_sky_base = best_base;
		if (!prepare_sky(*m_sky_now) || !sky_ready(*m_sky_now)) m_sky_now = nullptr;
		m_sky_last_tx0 = tx0;
		return;
	}
	// nothing distinctive in view (open sky): carry on from the last placement by the scroll, as
	// long as the columns on screen are that sky's (not a new stage's, or a screen between)
	if (m_sky_last) {
		const int d = ((tx0 - m_sky_last_tx0 + 256) & 511) - 256;
		const int base = (m_sky_base + d) & 2047;
		if (!sky_agrees(*m_sky_last, tx0, base)) { m_sky_last = nullptr; return; }
		m_sky_now = prepare_sky(*m_sky_last) && sky_ready(*m_sky_last) ? m_sky_last : nullptr;
		m_sky_base = base;
		m_sky_last_tx0 = tx0;
	}
}

// Which stage's sky the map holds now (an index into the records, or -1): from all 64 streamed
// columns, so it needs no drawing and is ready each game frame. For telling a cut between stages
// whose 3D doesn't change at the same moment (the ending's replays) from a camera move.
int model1::sky_identity()
{
	if (m_skies.empty()) init_skies();
	int best = -1, best_n = 0;
	for (size_t k = 0; k < m_skies.size(); k++) {
		const sky_art &a = m_skies[k];
		int n = 0;
		for (int t = 0; t < 64; t++) {
			uint16_t col[48];
			for (int r = 0; r < a.rows; r++) col[r] = m_tiles.tile_r((SKY_MAP << 12) | ((SKY_ROW0 + r) * 64 + t));
			n += a.unique.count(column_hash(col, 1, a.rows)) != 0;
		}
		if (n > best_n) { best_n = n; best = int(k); }
	}
	return best_n >= 4 ? best : -1;
}

// Whether the map's columns on screen are the record's at panorama x `base` (the screen's left).
bool model1::sky_agrees(const sky_art &a, int tx0, int base) const
{
	int checked = 0, same = 0;
	for (int t = 0; t < 64; t++) {
		const int xs = (t * 8 - tx0) & 511;
		if (xs > SCREEN_W - 8 || ((base + xs) & 7)) continue;
		uint16_t col[48];
		for (int r = 0; r < a.rows; r++) col[r] = m_tiles.tile_r((SKY_MAP << 12) | ((SKY_ROW0 + r) * 64 + t));
		const int p = ((base + xs) & 2047) / 8;
		checked++;
		same += column_hash(col, 1, a.rows) == column_hash(&a.entries[p], 256, a.rows);
	}
	return checked >= 4 && same * 4 >= checked * 3;
}

// One axis of a resample: each output sample's source pixels and weights. Shrinking averages
// every source pixel the output pixel covers (no shimmer in fine detail); enlarging blends the two
// nearest. `wrap`: the axis is a circle (the panorama's x).
namespace {
struct taps { std::vector<int> first, count; std::vector<float> w; };
enum { TAPS_AREA, TAPS_NEAREST, TAPS_CUBIC };
taps make_taps(int src, int dst, bool wrap, int mode = TAPS_AREA)
{
	taps t;
	t.first.resize(dst); t.count.resize(dst);
	const double k = double(src) / dst;
	auto at = [&](int i) { return float(wrap ? (i % src + src) % src : std::clamp(i, 0, src - 1)); };
	for (int o = 0; o < dst; o++) {
		t.first[o] = int(t.w.size());
		if (mode == TAPS_NEAREST) {                      // the original's pixels, just bigger
			t.w.push_back(1); t.w.push_back(at(int(std::floor((o + 0.5) * k))));
		} else if (mode == TAPS_CUBIC && k <= 1) {       // Catmull-Rom: smooth, edges kept
			const double x = (o + 0.5) * k - 0.5;
			const int i0 = int(std::floor(x));
			const double f = x - i0;
			const double w[4] = {
				((-0.5 * f + 1.0) * f - 0.5) * f, (1.5 * f - 2.5) * f * f + 1.0,
				((-1.5 * f + 2.0) * f + 0.5) * f, (0.5 * f - 0.5) * f * f };
			for (int j = 0; j < 4; j++) { t.w.push_back(float(w[j])); t.w.push_back(at(i0 - 1 + j)); }
		} else if (k > 1) {                              // area: the span [o*k, (o+1)*k)
			const double a = o * k, b = (o + 1) * k;
			for (int i = int(a); i < int(std::ceil(b)); i++) {
				const double cover = std::min(b, i + 1.0) - std::max(a, double(i));
				if (cover <= 0) continue;
				t.w.push_back(float(cover / k));
				t.w.push_back(float(wrap ? (i % src + src) % src : std::min(i, src - 1)));
			}
		} else {                                         // bilinear
			double x = (o + 0.5) * k - 0.5;
			if (!wrap) x = std::clamp(x, 0.0, double(src - 1));
			const int i0 = int(std::floor(x));
			const double f = x - i0;
			const int a0 = wrap ? (i0 % src + src) % src : std::clamp(i0, 0, src - 1);
			const int a1 = wrap ? ((i0 + 1) % src + src) % src : std::clamp(i0 + 1, 0, src - 1);
			t.w.push_back(float(1 - f)); t.w.push_back(float(a0));
			t.w.push_back(float(f)); t.w.push_back(float(a1));
		}
		t.count[o] = (int(t.w.size()) - t.first[o]) / 2;
	}
	return t;
}
}

// A light Gaussian blur (sigma 0.6 pixels; across wraps round, down stops at the edges).
std::vector<uint32_t> model1::soften(const std::vector<uint32_t> &px, int w, int h, const rows_fn &rows)
{
	const float sigma = 0.6f;
	float k[5], sum = 0;
	for (int i = -2; i <= 2; i++) sum += k[i + 2] = std::exp(-0.5f * i * i / (sigma * sigma));
	for (float &v : k) v /= sum;
	auto blur = [&](const std::vector<uint32_t> &in, bool across) {
		std::vector<uint32_t> out(in.size());
		rows(h, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
				for (int x = 0; x < w; x++) {
					float r = 0, g = 0, b = 0;
					for (int i = -2; i <= 2; i++) {
						const int xx = across ? ((x + i) % w + w) % w : x, yy = across ? y : std::clamp(y + i, 0, h - 1);
						const uint32_t c = in[size_t(yy) * w + xx];
						r += k[i + 2] * ((c >> 16) & 0xff); g += k[i + 2] * ((c >> 8) & 0xff); b += k[i + 2] * (c & 0xff);
					}
					out[size_t(y) * w + x] = uint32_t(r + 0.5f) << 16 | uint32_t(g + 0.5f) << 8 | uint32_t(b + 0.5f);
				}
		});
		return out;
	};
	return blur(blur(px, true), false);
}

// The replacement resampled to exactly the render scale: 2048 x s by rows x 8 x s, so drawing it
// is copying rows. Made when the stage or the scale changes: on threads of its own, while the
// game goes on (sky_ready).
void model1::scale_sky_px(const sky_art &a, int s, bool smooth, std::vector<uint32_t> &out, const rows_fn &rows)
{
	const int W = 2048 * s, H = a.rows * 8 * s;
	out.assign(size_t(W) * H, 0);
	// the stages' own skies: the pixels as they are (filter off), or softened a touch (the
	// dithering and stair steps blur away) and enlarged with Catmull-Rom; pictures: area / blend
	const int mode = !a.original ? TAPS_AREA : smooth ? TAPS_CUBIC : TAPS_NEAREST;
	const std::vector<uint32_t> *source = &a.px;
	std::vector<uint32_t> soft;
	if (a.original && smooth) {
		soft = soften(a.px, a.w, a.h, rows);
		source = &soft;
	}
	const taps tx = make_taps(a.w, W, true, mode), ty = make_taps(a.h, H, false, mode);
	// across first (to W x a.h, as floats per channel), then down
	std::vector<float> mid(size_t(W) * a.h * 3);
	rows(a.h, [&](int y0, int y1) {
		for (int y = y0; y < y1; y++) {
			const uint32_t *src = &(*source)[size_t(y) * a.w];
			float *d = &mid[size_t(y) * W * 3];
			for (int X = 0; X < W; X++) {
				float r = 0, g = 0, b = 0;
				for (int k = 0; k < tx.count[X]; k++) {
					const float w = tx.w[tx.first[X] + 2 * k];
					const uint32_t c = src[int(tx.w[tx.first[X] + 2 * k + 1])];
					r += w * ((c >> 16) & 0xff); g += w * ((c >> 8) & 0xff); b += w * (c & 0xff);
				}
				d[3 * X] = r; d[3 * X + 1] = g; d[3 * X + 2] = b;
			}
		}
	});
	rows(H, [&](int Y0, int Y1) {
		for (int Y = Y0; Y < Y1; Y++) {
			uint32_t *d = &out[size_t(Y) * W];
			for (int X = 0; X < W; X++) {
				float r = 0, g = 0, b = 0;
				for (int k = 0; k < ty.count[Y]; k++) {
					const float w = ty.w[ty.first[Y] + 2 * k];
					const float *m = &mid[(size_t(ty.w[ty.first[Y] + 2 * k + 1]) * W + X) * 3];
					r += w * m[0]; g += w * m[1]; b += w * m[2];
				}
				auto c8 = [](float v) { return uint32_t(std::clamp(int(v + 0.5f), 0, 255)); };
				d[X] = c8(r) << 16 | c8(g) << 8 | c8(b);
			}
		}
	});
}

// Here and now, on the drawing threads (a picture drawn again that can't wait).
void model1::scale_sky(const sky_art &a)
{
	m_sky_edges_art = nullptr;                      // the edge colours are worked out again
	scale_sky_px(a, m_scale, m_sky_smooth, m_sky_scaled, [](int n, const std::function<void(int, int)> &f) { parallel_rows(n, f); });
	m_sky_scaled_for = &a;
	m_sky_scaled_s = m_scale;
	m_sky_gen = ++m_sky_gen_next;
}

// Whether the sky is ready at the render scale. A new one (a stage, a scale, the filter) is scaled
// on threads of its own while the game runs on, the stage's tile layer drawn as it is meanwhile, and
// taken from SKY_LEAD game frames after it started (waited for then, if it isn't done: so which
// frames show it doesn't depend on how fast the machine is). Scaling it here and now took 25-35 ms
// at 3x-4x: a picture or two missed at every stage's start.
bool model1::sky_ready(const sky_art &a)
{
	constexpr uint64_t SKY_LEAD = 10;
	if (m_sky_scaled_for == &a && m_sky_scaled_s == m_scale) return true;
	const uint64_t version = a.original ? a.pen_sig : 0;
	const bool same_job = m_sky_job.valid() && m_sky_job_for == &a && m_sky_job_s == m_scale &&
						  m_sky_job_smooth == m_sky_smooth && m_sky_job_version == version;
	if (!same_job) {
		if (m_sky_job.valid()) m_sky_job.wait();      // (an old one's, no longer wanted)
		m_sky_job_for = &a; m_sky_job_s = m_scale; m_sky_job_smooth = m_sky_smooth; m_sky_job_version = version;
		m_sky_job_frame = m_frame;
		m_sky_job_gen = ++m_sky_gen_next;
		if (getenv("VF_DBG_SKY")) fprintf(stderr, "DBG sky job frame %llu art %p s %d smooth %d version %llx\n", (unsigned long long)m_frame, (const void *)&a, m_scale, int(m_sky_smooth), (unsigned long long)version);
		auto art = std::make_shared<sky_art>();
		art->px = a.px; art->w = a.w; art->h = a.h; art->rows = a.rows; art->original = a.original;
		const int s = m_scale;
		const bool smooth = m_sky_smooth;
		std::vector<uint32_t> *out = &m_sky_job_px;
		m_sky_job = std::async(std::launch::async, [art, s, smooth, out] {
			const uint64_t t0 = trace_ns();
			// its own threads: the drawing threads' pool takes one caller at a time
			scale_sky_px(*art, s, smooth, *out, [](int n, const std::function<void(int, int)> &f) {
				const int t = std::clamp(int(std::thread::hardware_concurrency()) / 2, 1, 4);
				std::vector<std::thread> th;
				for (int i = 1; i < t; i++) th.emplace_back([&, i] { f(n * i / t, n * (i + 1) / t); });
				f(0, n / t);
				for (auto &x : th) x.join();
			});
			return trace_ns() - t0;
		});
	}
	// the game stopped (paused or in the menu, a new scale or shape chosen): no frames run to wait
	// out, so it's taken as soon as it's done (else the sky stayed black until play went on)
	if (!m_stopped && m_frame < m_sky_job_frame + SKY_LEAD) return false;
	m_work.sky_scale_ns += m_sky_job.get();
	m_work.sky_scale++;
	m_sky_scaled.swap(m_sky_job_px);
	m_sky_job_px.clear();
	m_sky_job_px.shrink_to_fit();
	m_sky_edges_art = nullptr;
	m_sky_scaled_for = &a;
	m_sky_scaled_s = m_scale;
	m_sky_gen = m_sky_job_gen;
	return true;
}

bool model1::sky_pending(gpu_image &im) const
{
	if (!m_sky_job.valid() || m_sky_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
	const int W = 2048 * m_sky_job_s, H = m_sky_job_px.empty() ? 0 : int(m_sky_job_px.size() / size_t(W));
	if (H == 0) return false;
	im = { m_sky_job_px.data(), W, H, size_t(W), m_sky_job_gen };
	return true;
}

// Over the sky layer's rows, the replacement, across the full width.
void model1::draw_sky(bitmap_rgb32 &bitmap, float sdx, float sdy)
{
	const sky_art &a = *m_sky_now;
	const int s = m_scale, out_w = bitmap.width(), tw = m_tile_tmp.width();
	if (m_sky_scaled_for != &a || m_sky_scaled_s != s) {   // (not ready: sky_ready said so first)
		const uint64_t t0 = trace_ns();
		scale_sky(a);
		m_work.sky_scale_sync++;
		m_work.sky_scale_ns += trace_ns() - t0;
	}
	const int W = 2048 * s, H = a.rows * 8 * s;
	// the rows: the panorama's row for one line drawn from it, and the rest by the vertical scroll,
	// so the picture's top and bottom carry on over the plain sky and ground around it
	int ref_y = -1, ref_ty = 0;
	for (int y = 0; y < SCREEN_H && ref_y < 0; y++) {
		const uint32_t c = m_tile_coord[size_t(y) * tw + m_ext + SCREEN_W / 2];
		if (c == ~0u || (c >> 18) != SKY_MAP) continue;
		const int ty = int((c & 0x3ffff) >> 9) - SKY_ROW0 * 8;
		if (ty >= 0 && ty < a.rows * 8) { ref_y = y; ref_ty = ty; }
	}
	if (ref_y < 0) return;
	// the picture column at output x 0, and the picture row at output y 0 (smooth motion's shift
	// in arcade pixels, drawn at full resolution)
	const int ox = sky_shift_x(sdx, s), oy = sky_shift_y(sdy, s);
	const int x_off = (((m_sky_base - m_ext) * s + ox) % W + W) % W;
	static const bool log = getenv("VF_LOG_SKY") != nullptr;
	if (log && !m_redrawing) {
		const sky_sample &cs = m_sky_hist[(m_frame - 1) % SKY_HIST];
		fprintf(stderr, "K %.3f base %d dx %.3f x %.3f dy %.3f h %d v %d read %d%d hx %.3f vy %.3f next %d f %llu\n", m_draw_time, m_sky_base, sdx, m_sky_base + sdx, sdy,
				m_tiles.tile_r(0x5002) & 0x1ff, m_tiles.tile_r(0x5006) & 0x3ff, int(m_sky_read_h), int(m_sky_read_v), cs.hx, cs.vy, int(cs.next), (unsigned long long)m_frame);
	}
	const int y_base = (ref_ty - ref_y) * s + oy;
	// Above and below the panorama (the camera looking up or down past it): its plain top and
	// bottom colours, the commonest along its first and last rows. Repeating those rows instead
	// drew a vertical streak wherever a cloud touched the edge.
	if (m_sky_edges_for != &m_sky_scaled || m_sky_edges_s != s || m_sky_edges_art != &a) {
		auto commonest = [&](int row) {
			std::unordered_map<uint32_t, int> n;
			const uint32_t *r = &m_sky_scaled[size_t(row) * W];
			for (int x = 0; x < W; x += 4) n[r[x]]++;
			uint32_t best = r[0]; int bn = 0;
			for (auto &[c, k] : n) if (k > bn) { bn = k; best = c; }
			return best;
		};
		m_sky_top_colour = commonest(0);
		m_sky_bottom_colour = commonest(H - 1);
		m_sky_edges_for = &m_sky_scaled; m_sky_edges_s = s; m_sky_edges_art = &a;
	}
	if (m_parts) { parts_sky(a, x_off, y_base, oy); return; }   // the GPU copies the rows
	parallel_rows(SCREEN_H * s, [&](int Y0, int Y1) {
		for (int Yo = Y0; Yo < Y1; Yo++) {
			const int ly = std::clamp(int(std::floor(double(Yo + oy) / s)), 0, SCREEN_H - 1);
			if (m_tile_coord[size_t(ly) * tw + m_ext + SCREEN_W / 2] == ~0u) continue;   // not this layer
			uint32_t *d = &bitmap.pix(Yo);
			const int Y = y_base + Yo;
			if (Y < 0 || Y >= H) {
				std::fill(d, d + out_w, Y < 0 ? m_sky_top_colour : m_sky_bottom_colour);
				continue;
			}
			const uint32_t *src = &m_sky_scaled[size_t(Y) * W];
			for (int X = 0, x = x_off; X < out_w;) {
				const int n = std::min(out_w - X, W - x);
				memcpy(d + X, src + x, size_t(n) * 4);
				X += n;
				x = 0;
			}
		}
	});
}

// ---- the full-screen pictures: title and player select ----

namespace {
// Each picture's backdrop map (layers 4 and 5), by backdrop_hash(): the same whatever the palette
// does, so the sides fade with the picture.
struct screen_record { const char *name; uint32_t hash; };
// The player select has three layouts: player 1 alone (portrait on the left), player 2 alone
// (portrait on the right: player 2 starting from the demo) and both (VS: a challenger joining).
// Cheats' Siba (Majido) in Akira's place changes the map (his own select icon): its first frame
// and player 1 alone, as a7c660d4 and fa5cc994 are Akira's.
// 46ffddc5: the select's very first frame, before its first layout (its sides came a frame late).
const screen_record SCREEN_RECORDS[] = { { "title", 0x6e31a62b }, { "select", 0xfa5cc994 },
										 { "select", 0x282467f8 }, { "select", 0xa7c660d4 }, { "select", 0x46ffddc5 },
										 { "select", 0x14d0cd96 }, { "select", 0x0bc13456 } };
} // namespace

std::vector<std::string> model1::screen_names()
{
	std::vector<std::string> n;
	for (const auto &r : SCREEN_RECORDS)
		if (std::find(n.begin(), n.end(), r.name) == n.end()) n.push_back(r.name);
	return n;
}

bool model1::load_screen(const std::string &name, std::vector<uint32_t> xrgb, int w, int h)
{
	std::vector<uint32_t> hashes;                  // (the select: one per layout, the same picture)
	for (const auto &r : SCREEN_RECORDS)
		if (name == r.name) hashes.push_back(r.hash);
	if (hashes.empty() || w <= 0 || h <= 0) return false;
	std::erase_if(m_screens, [&](const screen_art &a) { return a.name == name; });
	m_screens.push_back({ name, std::move(hashes), std::move(xrgb), w, h });
	m_screen_now = nullptr;
	return true;
}

// The sides of a 4:3 screen in widescreen, from its picture (only its sides are used: the shipped
// ones have nothing in the middle). The game's layer 4 (just drawn into m_tile_tmp) sets the
// brightness: its 8 columns at each edge of the 4:3 picture against the 8 columns of the side
// beside them, so a fade in or out takes the sides with it.
void model1::draw_screen_sides(bitmap_rgb32 &bitmap)
{
	const screen_art &a = *m_screen_now;
	constexpr uint32_t KEY = 0xff000000;
	const double k = double(a.h) / SCREEN_H;                     // picture pixels per arcade pixel
	auto pic_x = [&](double ax) { return std::clamp(int(a.w / 2.0 + (ax - SCREEN_W / 2.0) * k), 0, a.w - 1); };
	auto pic_y = [&](double ay) { return std::clamp(int(ay * k), 0, a.h - 1); };
	// per side, the game's colour over the replacement's at its 8 edge columns
	float gain[2][3];
	for (int side = 0; side < 2; side++) {
		double game[3] = {}, pic[3] = {};
		for (int y = 0; y < SCREEN_H; y += 2)
			for (int i = 0; i < 8; i++) {
				const int ax = side ? SCREEN_W - 1 - i : i;
				const uint32_t g = m_tile_tmp.pix(y, m_ext + ax);
				if (g == KEY) continue;
				const double px = side ? SCREEN_W + i + 0.5 : -1 - i + 0.5;
				const uint32_t p = a.px[size_t(pic_y(y + 0.5)) * a.w + pic_x(px)];
				for (int c = 0; c < 3; c++) {
					game[c] += (g >> (16 - 8 * c)) & 0xff;
					pic[c] += (p >> (16 - 8 * c)) & 0xff;
				}
			}
		for (int c = 0; c < 3; c++) {
			float r = pic[c] > 0 ? float(game[c] / pic[c]) : 1.0f;
			if (std::fabs(r - 1.0f) < 0.08f) r = 1.0f;           // not fading: as it is
			gain[side][c] = std::clamp(r, 0.0f, 2.0f);
		}
	}
	const int s = m_scale, left = m_ext * s, right = (m_ext + SCREEN_W) * s, out_w = bitmap.width();
	if (&bitmap == &m_sides) {                     // (the GPU's copy: painted again only when it changes)
		if (m_sides_art == &a && !memcmp(m_sides_gain, gain, sizeof m_sides_gain)) return;
		m_sides_art = &a;
		memcpy(m_sides_gain, gain, sizeof m_sides_gain);
		m_sides_gen++;
	}
	parallel_rows(SCREEN_H * s, [&](int Y0, int Y1) {
		for (int Y = Y0; Y < Y1; Y++) {
			const uint32_t *row = &a.px[size_t(pic_y((Y + 0.5) / s)) * a.w];
			uint32_t *d = &bitmap.pix(Y);
			for (int X = 0; X < out_w; X++) {
				if (X == left) X = right;
				if (X >= out_w) break;
				const uint32_t p = row[pic_x((X + 0.5) / s - m_ext)];
				const float *g = gain[X >= right];
				uint32_t o = 0;
				for (int c = 0; c < 3; c++)
					o |= uint32_t(std::min(255.0f, ((p >> (16 - 8 * c)) & 0xff) * g[c] + 0.5f)) << (16 - 8 * c);
				d[X] = o;
			}
		}
	});
}
