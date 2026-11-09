// The picture in parts, for the GPU to put together (the host does it with SDL's renderer): the
// 2D stays at the arcade's resolution, made once a game frame (uploaded only when it changed),
// and the GPU enlarges it by the render scale, picking the nearest texel, which at a whole-number
// scale is exactly the CPU's pixel copying. Only the 3D is drawn at the render scale, onto KEY
// (nothing there), into frame(); its other rows stay KEY from one picture to the next.
//
// The ops in order: the backdrop colour; layer 6 and layer 4 (the sky: a hi-res panorama's rows,
// or the layer itself, enlarged, moved by smooth motion or stretched across a widescreen picture);
// layers 2 and 0 put together (MID); the 3D; layers 7, 5, 3 and 1 put together (TOP); a 4:3
// screen's black sides. A shadow where the 3D drew nothing blends with the 2D beneath it: worked
// out on the CPU from the parts (parts_below), for those pixels alone. What isn't put together this
// way is drawn whole by the CPU, as before: a 4:3 screen with painted sides (the title, the player
// select).
#include "model1.h"
#include <chrono>

uint64_t model1::trace_ns()
{
	return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

int model1::render_parts(double now, gpu_parts &p, bool shown)
{
	p.ops.clear();
	p.d3_changed.clear();
	p.d3_all = false;
	p.d3_target = -1;
	p.d3_edge = false;
	for (auto &i : p.img) i.px = nullptr;
	m_last_parts = nullptr;
	if (!m_smooth || (m_scale == 1 && m_ext == 0)) return render_smooth(now, shown) ? 2 : 0;   // (1x 4:3: the arcade's own)
	m_parts = &p;
	m_parts_whole = false;
	const bool drew = render_smooth(now, shown);
	m_parts = nullptr;
	if (!drew) return 0;
	if (m_parts_whole) {
		// the same moment again, whole (as redraw() does: the stage sky's settling isn't counted twice)
		p.ops.clear();
		p.d3_changed.clear();
		p.d3_target = -1;
		p.d3_edge = false;
		m_redrawing = true;
		render_smooth(now, shown);
		m_redrawing = false;
		return 2;
	}
	m_last_parts = &p;
	// only looked at (not handed to the GPU): the next picture's 3D goes up whole, as the GPU's copy
	// hasn't had this one's changes
	if (!shown) m_bitmap_whole = true;
	return 1;
}

// Whether the picture last drawn is black (a curtain): sampled, as drawn whole or in parts.
bool model1::frame_blank() const
{
	const int w = m_bitmap.width(), h = m_bitmap.height();
	const bool parts = m_frame_parts && m_last_parts;
	const uint32_t *px = m_bitmap.data();
	for (size_t i = 0, n = size_t(w) * h; i < n; i += 97) {
		const uint32_t c = parts ? compose_pixel(*m_last_parts, int(i % w), int(i / w)) : px[i];
		if (((c >> 16) & 0xff) > 12 || ((c >> 8) & 0xff) > 12 || (c & 0xff) > 12) return false;
	}
	return true;
}

// The 3D's picture: KEY again where the last one drew (all of it after a whole picture), and the
// backdrop's colour under everything.
void model1::parts_begin(bitmap_rgb32 &bitmap)
{
	const int w = bitmap.width(), h = bitmap.height();
	if (m_bitmap_whole || int(m_span.size()) != h) {
		parallel_rows(h, [&](int a, int b) { for (int y = a; y < b; y++) std::fill_n(&bitmap.pix(y), w, KEY); });
		m_span.assign(h, { 0, int16_t(w) });           // (all of it to upload)
		m_parts->d3_all = true;
	} else {
		parallel_rows(h, [&](int a, int b) {
			for (int y = a; y < b; y++)
				if (m_span[y].first < m_span[y].second) std::fill(&bitmap.pix(y, m_span[y].first), &bitmap.pix(y, m_span[y].second), KEY);
		});
		m_parts->d3_all = false;
	}
	m_d3_y0 = 1 << 30;
	m_d3_y1 = -1;
	m_parts->img[TEX_3D] = { &bitmap.pix(0), w, h, size_t(w), 0 };
	gpu_op fill;
	fill.colour = m_pens[0];
	fill.dw = float(w);
	fill.dh = float(h);
	m_parts->ops.push_back(fill);
}

// What the 3D drew (each row's first and last pixel that isn't KEY), and what changed for the
// GPU: in bands of rows, the columns put back to KEY or drawn.
void model1::parts_end(bitmap_rgb32 &bitmap)
{
	const int w = bitmap.width(), h = bitmap.height();
	const int d0 = std::max(0, m_d3_y0), d1 = std::min(h - 1, m_d3_y1);
	constexpr int BAND = 32;
	const int bands = (h + BAND - 1) / BAND;
	std::vector<gpu_parts::rect> changed(bands, { w, 0, 0, 0 });
	parallel_rows(bands, [&](int a, int b) {
		for (int k = a; k < b; k++) {
			gpu_parts::rect &r = changed[k];
			r.y0 = k * BAND; r.y1 = std::min(h, r.y0 + BAND);
			for (int y = r.y0; y < r.y1; y++) {
				auto &sp = m_span[y];
				if (sp.first < sp.second) { r.x0 = std::min(r.x0, int(sp.first)); r.x1 = std::max(r.x1, int(sp.second)); }
				int x0 = w, x1 = 0;
				if (y >= d0 && y <= d1) {
					const uint32_t *p = &bitmap.pix(y);
					x0 = 0;                                 // (8 at a time: most of a row is all KEY or none)
					while (x0 + 8 <= w && !((p[x0] ^ KEY) | (p[x0 + 1] ^ KEY) | (p[x0 + 2] ^ KEY) | (p[x0 + 3] ^ KEY) |
											(p[x0 + 4] ^ KEY) | (p[x0 + 5] ^ KEY) | (p[x0 + 6] ^ KEY) | (p[x0 + 7] ^ KEY))) x0 += 8;
					while (x0 < w && p[x0] == KEY) x0++;
					if (x0 < w) {
						x1 = w;
						while (x1 - 8 >= x0 && !((p[x1 - 1] ^ KEY) | (p[x1 - 2] ^ KEY) | (p[x1 - 3] ^ KEY) | (p[x1 - 4] ^ KEY) |
												 (p[x1 - 5] ^ KEY) | (p[x1 - 6] ^ KEY) | (p[x1 - 7] ^ KEY) | (p[x1 - 8] ^ KEY))) x1 -= 8;
						while (x1 > x0 && p[x1 - 1] == KEY) x1--;
					}
				}
				sp = { int16_t(x0 < x1 ? x0 : 0), int16_t(x0 < x1 ? x1 : 0) };
				if (x0 < x1) { r.x0 = std::min(r.x0, x0); r.x1 = std::max(r.x1, x1); }
			}
		}
	});
	// Each upload costs about as much as 80,000 pixels (Metal through SDL: about 30 us a call and
	// 0.36 ns a pixel): neighbouring bands go up together when that's cheaper.
	if (!m_parts->d3_all) {
		auto cost = [](const gpu_parts::rect &r) { return 80000.0 + double(r.x1 - r.x0) * (r.y1 - r.y0); };
		for (const auto &r : changed) {
			if (r.x0 >= r.x1) continue;
			auto &v = m_parts->d3_changed;
			if (!v.empty()) {
				gpu_parts::rect &l = v.back();
				const gpu_parts::rect u{ std::min(l.x0, r.x0), l.y0, std::max(l.x1, r.x1), r.y1 };
				if (cost(u) <= cost(l) + cost(r)) { l = u; continue; }
			}
			v.push_back(r);
		}
	}
	m_bitmap_whole = false;
}

// Layers drawn over one another at 1x as the CPU draws them enlarged (KEY: nothing), with the
// widescreen curtains (a row a layer covers edge to edge carried out across the sides). Made when
// a layer was drawn again; its number changes only when the result does, so an unchanged HUD
// isn't uploaded again.
void model1::parts_composite(bitmap_rgb32 &out, uint64_t &key, uint64_t &gen, std::initializer_list<int> layers)
{
	const int tw = SCREEN_W + 2 * m_ext;
	uint64_t k = 1469598103934665603ull;
	auto mix = [&](uint64_t v) { k = (k ^ v) * 1099511628211ull; };
	mix(uint64_t(m_ext)); mix(m_wide_frame);
	for (int l : layers) mix(cached_layer(l, 0).gen);
	if (k == key && out.width() == tw) return;
	key = k;
	static thread_local bitmap_rgb32 made;
	if (made.width() != tw) made.allocate(tw, SCREEN_H);
	made.fill(KEY);
	for (int l : layers) {
		const layer_cache &c = m_layer_cache[l & 7];
		for (int y = 0; y < SCREEN_H; y++) {
			if (c.x0[y] >= c.x1[y]) continue;
			const uint32_t *src = &c.px.pix(y);
			uint32_t *d = &made.pix(y);
			for (int x = c.x0[y]; x < c.x1[y]; x++)
				if (src[x] != KEY) d[x] = src[x];
			const uint32_t el = src[m_ext], er = src[m_ext + SCREEN_W - 1];
			if (m_ext && m_wide_frame && el != KEY && er != KEY) {
				int covered = 0;
				for (int x = m_ext; x < m_ext + SCREEN_W; x++) covered += src[x] != KEY;
				if (covered * 10 >= SCREEN_W * 9) {
					std::fill(d, d + m_ext, el);
					std::fill(d + m_ext + SCREEN_W, d + tw, er);
				}
			}
		}
	}
	if (out.width() == tw && !memcmp(&out.pix(0), &made.pix(0), size_t(tw) * SCREEN_H * 4)) return;
	std::swap(out, made);
	gen++;
}

// A backdrop's middle 496 columns padded by PAD on each side, the edge texels repeated: moved by a
// whole number of pixels, or stretched, the GPU then picks exactly the texel the CPU's clamp did.
const bitmap_rgb32 &model1::padded_middle(const bitmap_rgb32 &src, bitmap_rgb32 &out, uint64_t &key, uint64_t &gen, uint64_t src_gen)
{
	const uint64_t k = src_gen << 16 | uint64_t(m_ext);   // (m_ext < 65536)
	if (k == key && out.width() == SCREEN_W + 2 * PAD) return out;
	key = k;
	m_work.padded++;
	out.allocate(SCREEN_W + 2 * PAD, SCREEN_H + 2 * PAD);
	for (int y = 0; y < SCREEN_H + 2 * PAD; y++) {
		const uint32_t *s = &src.pix(std::clamp(y - PAD, 0, SCREEN_H - 1), m_ext);
		uint32_t *d = &out.pix(y);
		std::fill(d, d + PAD, s[0]);
		memcpy(d + PAD, s, SCREEN_W * 4);
		std::fill(d + PAD + SCREEN_W, d + SCREEN_W + 2 * PAD, s[SCREEN_W - 1]);
	}
	gen++;
	return out;
}

// A pixel of the picture as the GPU puts the parts together: the ops in order, each texel picked as
// the GPU picks it (the nearest to the pixel's centre), KEY left out.
uint32_t model1::compose_pixel(const gpu_parts &p, int x, int y, int upto)
{
	uint32_t c = 0;
	for (const gpu_op &o : p.ops) {
		if (o.tex == upto && upto >= 0) break;
		if (x < o.clip_x0 || x >= o.clip_x1 || x < o.dx || x >= o.dx + o.dw || y < o.dy || y >= o.dy + o.dh) continue;
		if (o.tex < 0) { c = o.colour & 0xffffff; continue; }
		const gpu_image &im = p.img[o.tex];
		if (!im.px) continue;
		const int u = int(std::floor(o.sx + (x + 0.5f - o.dx) * (o.sw / o.dw)));
		const int v = int(std::floor(o.sy + (y + 0.5f - o.dy) * (o.sh / o.dh)));
		if (u < 0 || v < 0 || u >= im.w || v >= im.h) continue;
		const uint32_t t = im.px[size_t(v) * im.stride + u];
		if (t != KEY) c = t & 0xffffff;
	}
	return c;
}

void model1::compose_cpu(const gpu_parts &p, int w, int h, std::vector<uint32_t> &out)
{
	out.resize(size_t(w) * h);
	parallel_rows(h, [&](int a, int b) {
		for (int y = a; y < b; y++)
			for (int x = 0; x < w; x++) out[size_t(y) * w + x] = compose_pixel(p, x, y);
	});
}

// The 2D at a pixel of the picture, as the GPU will put it together under the 3D.
uint32_t model1::parts_below(int x, int y) const { return compose_pixel(*m_parts, x, y, TEX_3D); }

// A layer's number for the GPU: the same as long as its pixels are (it is drawn again each frame).
uint64_t model1::kept_gen(kept_image &k, const bitmap_rgb32 &b, uint64_t src_gen)
{
	if (k.src == &b && k.src_gen == src_gen) return k.gen;
	const size_t n = size_t(b.width()) * b.height();
	if (k.px.size() != n || memcmp(k.px.data(), &b.pix(0), n * 4)) { k.px.assign(&b.pix(0), &b.pix(0) + n); k.gen++; }
	k.src = &b;
	k.src_gen = src_gen;
	return k.gen;
}

// Layer 6 or 4 (opaque; KEY where it has nothing), as draw_tiles would draw it.
void model1::parts_layer(int layer, int flags, const bitmap_rgb32 &px, bool stretch, float sdx, float sdy)
{
	(void)flags;
	gpu_parts &p = *m_parts;
	const bool l4 = layer == 4;
	const int s = m_scale, tw = SCREEN_W + 2 * m_ext, out_w = tw * s;
	const int id = l4 ? TEX_L4 : TEX_L6, idm = l4 ? TEX_L4M : TEX_L6M;
	const uint64_t src_gen = l4 ? m_l4_gen : m_layer_cache[layer & 7].gen;
	gpu_op o;
	if (sdx != 0 || sdy != 0) {
		// moved by smooth motion: in the 4:3 middle (all across when stretched), from the middle's
		// texels (clamped at its edges: the padding)
		const bitmap_rgb32 &m = padded_middle(px, l4 ? m_l4m : m_l6m, l4 ? m_l4m_key : m_l6m_key, l4 ? m_l4m_gen : m_l6m_gen, src_gen);
		p.img[idm] = { &m.pix(0), m.width(), m.height(), size_t(m.width()), l4 ? m_l4m_gen : m_l6m_gen };
		const int oy = sky_shift_y(sdy, s), ox = sky_shift_x(sdx, s);
		o.tex = idm;
		o.sh = float(SCREEN_H + 2 * PAD);
		o.dy = float(-oy - PAD * s);
		o.dh = float((SCREEN_H + 2 * PAD) * s);
		if (stretch) {
			// the columns picked as draw_tiles picks them, at the render width (1:1 across)
			const bitmap_rgb32 &st = stretched(l4, m, 0, SCREEN_H + 2 * PAD, sdx, true, l4 ? m_l4m_gen : m_l6m_gen);
			p.img[idm] = { &st.pix(0), st.width(), st.height(), size_t(st.width()), m_stretched_gen[l4] };
			o.sw = float(out_w);
			o.dw = float(out_w);
		} else {
			o.sw = float(SCREEN_W + 2 * PAD);
			o.dx = float(m_ext * s - ox - PAD * s);
			o.dw = float((SCREEN_W + 2 * PAD) * s);
			o.clip_x0 = m_ext * s;
			o.clip_x1 = (m_ext + SCREEN_W) * s;
		}
	} else {
		p.img[id] = { &px.pix(0), tw, SCREEN_H, size_t(tw), kept_gen(m_kept[l4], px, src_gen) };
		o.tex = id;
		o.sh = SCREEN_H;
		o.dh = float(SCREEN_H * s);
		if (stretch) {                                // the 4:3 layer across the width, as above
			const bitmap_rgb32 &st = stretched(l4, px, 0, SCREEN_H, 0, false, p.img[id].gen);
			p.img[id] = { &st.pix(0), st.width(), st.height(), size_t(st.width()), m_stretched_gen[l4] };
			o.sw = float(out_w);
		} else o.sw = float(tw);
		o.dw = float(out_w);
	}
	p.ops.push_back(o);
}

// A backdrop's columns across the render width, picked as draw_tiles does: moved by smooth motion,
// from the padded middle (clamped to it); else from the layer's 4:3 middle.
const bitmap_rgb32 &model1::stretched(int i, const bitmap_rgb32 &src, int y0, int rows, float sdx, bool moved, uint64_t src_gen)
{
	const int out_w = (SCREEN_W + 2 * m_ext) * m_scale;
	uint32_t sbits;
	memcpy(&sbits, &sdx, 4);
	const uint64_t key = (src_gen * 1000003u + uint64_t(out_w)) * 0x9e3779b97f4a7c15ull ^ (uint64_t(sbits) << 1 | uint64_t(moved));
	bitmap_rgb32 &out = m_stretched[i];
	if (key == m_stretched_key[i] && out.width() == out_w && out.height() == rows) return out;
	m_stretched_key[i] = key;
	m_stretched_gen[i]++;
	out.allocate(out_w, rows);
	std::vector<int> col(out_w);
	for (int X = 0; X < out_w; X++)
		col[X] = moved ? PAD + std::clamp(int(std::floor((X + 0.5f) * SCREEN_W / out_w + sdx)), 0, SCREEN_W - 1)
					   : m_ext + int((X + 0.5f) * SCREEN_W / out_w);
	parallel_rows(rows, [&](int a, int b) {
		for (int y = a; y < b; y++) {
			const uint32_t *s = &src.pix(y0 + y);
			uint32_t *d = &out.pix(y);
			for (int X = 0; X < out_w; X++) d[X] = s[col[X]];
		}
	});
	return out;
}

// A hi-res sky's rows (draw_sky's, as copies of the panorama at the render scale): the rows where
// the layer is, each from the panorama's row at that height (or its plain top or bottom colour
// beyond it), x_off along, wrapping round.
void model1::parts_sky(const sky_art &a, int x_off, int y_base, int oy)
{
	gpu_parts &p = *m_parts;
	const int s = m_scale, tw = m_tile_tmp.width(), out_w = tw * s;
	const int W = 2048 * s, H = a.rows * 8 * s;
	p.img[TEX_SKY] = { m_sky_scaled.data(), W, H, size_t(W), m_sky_gen };
	auto flush = [&](int kind, int y0, int y1) {
		if (kind == 0 || y1 <= y0) return;
		if (kind != 1) {
			gpu_op o;
			o.colour = kind == 2 ? m_sky_top_colour : m_sky_bottom_colour;
			o.dy = float(y0); o.dw = float(out_w); o.dh = float(y1 - y0);
			p.ops.push_back(o);
			return;
		}
		for (int X = 0, x = x_off; X < out_w;) {
			const int n = std::min(out_w - X, W - x);
			gpu_op o;
			o.tex = TEX_SKY;
			o.sx = float(x); o.sy = float(y_base + y0); o.sw = float(n); o.sh = float(y1 - y0);
			o.dx = float(X); o.dy = float(y0); o.dw = float(n); o.dh = float(y1 - y0);
			p.ops.push_back(o);
			X += n;
			x = 0;
		}
	};
	int kind = 0, start = 0;
	for (int Yo = 0; Yo < SCREEN_H * s; Yo++) {
		const int ly = std::clamp(int(std::floor(double(Yo + oy) / s)), 0, SCREEN_H - 1);
		int k = 0;
		if (m_tile_coord[size_t(ly) * tw + m_ext + SCREEN_W / 2] != ~0u || m_fc_live) {   // (the free camera's: every row)
			const int Y = y_base + Yo;
			k = Y < 0 ? 2 : Y >= H ? 3 : 1;
		}
		if (k != kind) { flush(kind, start, Yo); kind = k; start = Yo; }
	}
	flush(kind, start, SCREEN_H * s);
}
