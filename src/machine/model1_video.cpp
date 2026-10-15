#include "model1.h"
#include <set>
#include <mutex>
#include "parallel.h"
#include <array>
// license:BSD-3-Clause
// copyright-holders:Olivier Galibert




#include <glm/geometric.hpp>

#define LOG_TGP (1U << 1)

#define VERBOSE (0)


// Model 1 geometrizer TGP and rasterizer simulation
enum { FRAC_SHIFT = 16 };
enum { MOIRE = 0x01000000 };
constexpr uint32_t MOIRE_MARK = 0x01000000;   // in a drawn pixel's unused top byte: already shaded
bool model1::s_moire_blend = false;
int model1::s_mark_y1 = 1 << 30, model1::s_mark_y2 = -1;
bool model1::s_hd_raster = false;
std::vector<int32_t> model1::s_owner;
int32_t model1::s_owner_base = 0;
static int s_last_count = 0;           // the polygons in the last pass that noted owners
int model1::s_depth_w = 0;
int model1::s_depth_pass = 0;
thread_local int model1::s_cur_quad = 0;
thread_local bool model1::s_cur_fshadow = false;
// The polygons are drawn in row bands, several at once (draw_quads): each band notes its own
// shaded rows (merged after) and only plots its own rows of a wireframe line, which is clipped
// against the whole view as before so its pixels don't change.
static thread_local int t_mark_y1 = 1 << 30, t_mark_y2 = -1;
static thread_local int t_band_y1 = -(1 << 30), t_band_y2 = 1 << 30;
static thread_local rectangle t_full_clip;
static thread_local bool t_banded = false;
std::vector<uint8_t> model1::s_qstage;
// Recording for the GPU at 1x: the rasterizer's own spans, kept instead of drawn
static thread_local std::vector<gpu3d_span> *t_sink = nullptr;
static thread_local uint32_t t_sink_poly = 0;

// Crack-free filling for scales above 1: the edges are evaluated at each pixel row's centre from
// the vertices' unrounded positions, and a pixel is filled when its centre lies inside (left
// edge inclusive, right edge exclusive; rows half-open the same way). Two polygons that share an
// edge compute the same crossing on every row, so the pixels along it go to exactly one of them:
// no backdrop showing through between neighbours, nothing drawn twice.
void model1::fill_quad_hd(bitmap_rgb32 &bitmap, view_t *view, const quad_t &q, int color)
{
	float vx[4], vy[4];
	for (int i = 0; i < 4; i++) { vx[i] = q.p[i]->fx; vy[i] = q.p[i]->fy; }
	float ymin = std::min({vy[0], vy[1], vy[2], vy[3]}), ymax = std::max({vy[0], vy[1], vy[2], vy[3]});
	int y0 = std::max(view->y1, int(std::ceil(ymin - 0.5f)));
	int y1 = std::min(view->y2, int(std::ceil(ymax - 0.5f)) - 1);
	// The edges, set up once: each oriented top to bottom so both polygons sharing it compute it the
	// same way, horizontal ones dropped. The crossing below is the same expression as when this was
	// worked out on every row, so every pixel is the same.
	struct edge { float ax, ay, by, dx, dy; } e[4];
	int ne = 0;
	for (int i = 0; i < 4; i++) {
		int j = (i + 1) & 3;
		float ax = vx[i], ay = vy[i], bx = vx[j], by = vy[j];
		if (ay > by || (ay == by && ax > bx)) { std::swap(ax, bx); std::swap(ay, by); }
		if (ay == by) continue;
		e[ne++] = { ax, ay, by, bx - ax, by - ay };
	}
	for (int y = y0; y <= y1; y++) {
		float yc = y + 0.5f;
		float xs[4];
		int n = 0;
		for (int k = 0; k < ne; k++) {
			const edge &g = e[k];
			if (yc < g.ay || yc >= g.by) continue;
			xs[n++] = g.ax + (yc - g.ay) * g.dx / g.dy;
		}
		if (n < 2) continue;
		for (int a = 1; a < n; a++)        // (insertion sort: at most four)
			for (int b = a; b > 0 && xs[b] < xs[b - 1]; b--) std::swap(xs[b], xs[b - 1]);
		for (int k = 0; k + 1 < n; k += 2) {
			int xa = std::max(view->x1, int(std::ceil(xs[k] - 0.5f)));
			int xb = std::min(view->x2, int(std::ceil(xs[k + 1] - 0.5f)) - 1);
			if (xa > xb) continue;
			if (s_depth_pass && s_cur_fshadow) {   // a fighter's shadow
				uint32_t *pix = &bitmap.pix(y);
				const int32_t *own = &s_owner[size_t(y) * s_depth_w];
				for (int x = xa; x <= xb; x++) {
					if (pix[x] == KEY) {                   // over the 2D (in the GPU's parts): its half, for later
						t_mark_y1 = std::min(t_mark_y1, y);
						t_mark_y2 = std::max(t_mark_y2, y);
						pix[x] = SHADOW_KEY | ((uint32_t(color) >> 1) & 0x7f7f7f);
						continue;
					}
					const int o = own[x] - s_owner_base - 1;     // the polygon that painted it (< 0: none)
					if ((pix[x] & MOIRE_MARK) || (o >= 0 && !s_qstage[o])) continue;   // not on a fighter
					t_mark_y1 = std::min(t_mark_y1, y);
					t_mark_y2 = std::max(t_mark_y2, y);
					pix[x] = MOIRE_MARK | (((pix[x] >> 1) & 0x7f7f7f) + ((uint32_t(color) >> 1) & 0x7f7f7f));
				}
				continue;
			}
			if (s_depth_pass == 1 && !(color & MOIRE)) {   // who painted each pixel
				int32_t *own = &s_owner[size_t(y) * s_depth_w];
				std::fill(own + xa, own + xb + 1, s_owner_base + s_cur_quad + 1);
			}
			if (color & MOIRE) draw_hline_moired(bitmap, xa, xb, y, color);
			else draw_hline(bitmap, xa, xb, y, color);
		}
	}
}

uint32_t model1::readi(int adr) const
{
	return m_display_list_current[(adr + 0)&0x7fff] | (m_display_list_current[(adr + 1)&0x7fff] << 16);
}

int16_t model1::readi16(int adr) const
{
	return m_display_list_current[(adr + 0)&0x7fff];
}

float model1::readf(int adr) const
{
	return u2f(readi(adr));
}

void model1::view_t::transform_point(point_t *p) const
{
	point_t q = *p;
	float xx = translation[0] * q.x + translation[3] * q.y + translation[6] * q.z + translation[ 9] + vxx;
		p->y = translation[1] * q.x + translation[4] * q.y + translation[7] * q.z + translation[10] + vyy;
	float zz = translation[2] * q.x + translation[5] * q.y + translation[8] * q.z + translation[11] + vzz;
	p->x = ayyc * xx - ayys * zz;
	p->z = ayys * xx + ayyc * zz;
}

void model1::view_t::transform_vector(glm::vec3& p) const
{
	glm::vec3 q(p);
	glm::vec3 row1(translation[0], translation[3], translation[6]);
	glm::vec3 row2(translation[1], translation[4], translation[7]);
	glm::vec3 row3(translation[2], translation[5], translation[8]);
	p = glm::vec3(glm::dot(q, row1), glm::dot(q, row2), glm::dot(q, row3));
	//p->set_x(translation[0] * q.x() + translation[3] * q.y() + translation[6] * q.z());
	//p->set_y(translation[1] * q.x() + translation[4] * q.y() + translation[7] * q.z());
	//p->set_z(translation[2] * q.x() + translation[5] * q.y() + translation[8] * q.z());
}

void model1::cross_product(point_t* o, const point_t* p, const point_t* q) const
{
	o->x = (p->y * q->z) - (q->y * p->z);
	o->y = (p->z * q->x) - (q->z * p->x);
	o->z = (p->x * q->y) - (q->x * p->y);
}

float model1::view_determinant(const point_t *p1, const point_t *p2, const point_t *p3) const
{
	float x1 = p2->x - p1->x;
	float y1 = p2->y - p1->y;
	float z1 = p2->z - p1->z;
	float x2 = p3->x - p1->x;
	float y2 = p3->y - p1->y;
	float z2 = p3->z - p1->z;

	return p1->x * (y1 * z2 - y2 * z1) + p1->y * (z1 * x2 - z2 * x1) + p1->z * (x1 * y2 - x2 * y1);
}

void model1::view_t::project_point(point_t *p) const
{
	p->xx = p->x / p->z;
	p->yy = p->y / p->z;
	p->fx = (xc + (p->xx * zoomx + viewx) + ext) * scale;
	p->fy = (yc - (p->yy * zoomy + viewy)) * scale;
	p->s.x = int32_t(p->fx);
	p->s.y = int32_t(p->fy);
}

void model1::view_t::project_point_direct(point_t *p) const
{
	p->xx = p->x /*/ p->z*/;
	p->yy = p->y /*/ p->z*/;
	p->fx = (xc + p->xx + ext) * scale;
	p->fy = (yc - p->yy) * scale;
	p->s.x = int32_t(p->fx);
	p->s.y = int32_t(p->fy);
}

void model1::draw_hline(bitmap_rgb32 &bitmap, int x1, int x2, int y, int color)
{
	if (t_sink) { t_sink->push_back({ t_sink_poly, y, x1, x2 }); return; }
	uint32_t *const base = &bitmap.pix(y);
	while(x1 <= x2)
	{
		base[x1] = color;
		x1++;
	}
}

void model1::draw_hline_moired(bitmap_rgb32 &bitmap, int x1, int x2, int y, int color)
{
	if (t_sink) { t_sink->push_back({ t_sink_poly, y, x1, x2 }); return; }
	uint32_t *const base = &bitmap.pix(y);
	if (s_moire_blend) {
		// The arcade's checkerboard shades a pixel once however many shadows overlap (they
		// all hit the same half of the pixels), so blend once: a marked pixel is already
		// shaded. Opaque polygons drawn later overwrite the mark; screen_update clears the rest.
		t_mark_y1 = std::min(t_mark_y1, y);
		t_mark_y2 = std::max(t_mark_y2, y);
		for (; x1 <= x2; x1++)
			if (base[x1] == KEY) base[x1] = SHADOW_KEY | ((uint32_t(color) >> 1) & 0x7f7f7f);   // over the 2D (GPU's parts): its half, for later
			else if (!(base[x1] & MOIRE_MARK))
				base[x1] = MOIRE_MARK | (((base[x1] >> 1) & 0x7f7f7f) + ((uint32_t(color) >> 1) & 0x7f7f7f));
		return;
	}
	const uint32_t c = uint32_t(color) & 0xffffff;   // (not the MOIRE flag: the top byte says "drawn" to the GPU)
	while(x1 <= x2)
	{
		if(!((x1^y) & 1))
		{
			base[x1] = c;
		}
		x1++;
	}
}

void model1::fill_slope(bitmap_rgb32 &bitmap, view_t *view, int color, int32_t x1, int32_t x2, int32_t sl1, int32_t sl2, int32_t y1, int32_t y2, int32_t *nx1, int32_t *nx2)
{
	if(y1 > view->y2)
	{
		return;
	}

	if (y2 <= view->y1)
	{
		int delta = y2 - y1;
		*nx1 = x1 + delta * sl1;
		*nx2 = x2 + delta * sl2;
		return;
	}

	if (y2 > view->y2)
	{
		y2 = view->y2 + 1;
	}

	if (y1 < view->y1)
	{
		int delta = view->y1 - y1;
		x1 += delta * sl1;
		x2 += delta * sl2;
		y1 = view->y1;
	}

	if (x1 > x2 || (x1 == x2 && sl1 > sl2))
	{
		int32_t t = x1;
		x1 = x2;
		x2 = t;

		t = sl1;
		sl1 = sl2;
		sl2 = t;

		int32_t *tp = nx1;
		nx1 = nx2;
		nx2 = tp;
	}

	while(y1 < y2)
	{
		if(y1 >= view->y1)
		{
			int xx1 = x1>>FRAC_SHIFT;
			int xx2 = x2>>FRAC_SHIFT;
			if(xx1 <= view->x2 || xx2 >= view->x1)
			{
				if(xx1 < view->x1)
				{
					xx1 = view->x1;
				}
				if(xx2 > view->x2)
				{
					xx2 = view->x2;
				}

				if(color & MOIRE)
				{
					draw_hline_moired(bitmap, xx1, xx2, y1, color);
				}
				else
				{
					draw_hline(bitmap, xx1, xx2, y1, color);
				}
			}
		}

		x1 += sl1;
		x2 += sl2;
		y1++;
	}
	*nx1 = x1;
	*nx2 = x2;
}

void model1::fill_line(bitmap_rgb32 &bitmap, view_t *view, int color, int32_t y, int32_t x1, int32_t x2)
{
	int xx1 = x1>>FRAC_SHIFT;
	int xx2 = x2>>FRAC_SHIFT;

	if(y > view->y2 || y < view->y1)
		return;

	if(xx1 <= view->x2 || xx2 >= view->x1) {
		if(xx1 < view->x1)
			xx1 = view->x1;
		if(xx2 > view->x2)
			xx2 = view->x2;

		if(color & MOIRE)
			draw_hline_moired(bitmap, xx1, xx2, y, color);
		else
			draw_hline(bitmap, xx1, xx2, y, color);
	}
}

// Draw a solid line between two screen points (integer Bresenham).  Wireframe
// primitives reach the rasterizer as a degenerate quad with two coincident
// vertex pairs (A,A,B,B); the scanline filler only touches one pixel per row,
// collapsing near-horizontal wires to dots (e.g. the Star Wars Arcade target
// box).  The endpoints come from the unclipped projection and can be garbage
// (a degenerate projection yields inf/NaN, which float->int conversion turns
// into INT32_MIN on x86 hosts), so clip the segment to the viewport first,
// like fill_slope() does for the filler - unclipped, wingwar hung during boot
// walking a ~2^31-pixel line.
// A wireframe line's pixels (clipped, then Bresenham), each handed to plot(x, y): the CPU draws
// them, the GPU's recording keeps them
template <class F>
static void line_pixels(const rectangle &clip, int bw, int bh, int x1, int y1, int x2, int y2, bool moire, F plot)
{
	// Liang-Barsky clip against the viewport rectangle (doubles hold every
	// int32 exactly, so in-range endpoints pass through unchanged)
	const double lx1 = x1, ly1 = y1;
	const double cdx = double(x2) - lx1, cdy = double(y2) - ly1;
	const double p[4] = { -cdx, cdx, -cdy, cdy };
	const double q[4] = { lx1 - clip.min_x, clip.max_x - lx1, ly1 - clip.min_y, clip.max_y - ly1 };
	double t0 = 0.0, t1 = 1.0;
	for (int i = 0; i < 4; i++)
	{
		if (p[i] == 0.0)
		{
			if (q[i] < 0.0)
				return; // parallel to this edge and outside it
		}
		else
		{
			const double t = q[i] / p[i];
			if (p[i] < 0.0)
				t0 = std::max(t0, t);
			else
				t1 = std::min(t1, t);
		}
	}
	if (t0 > t1)
		return; // entirely outside the viewport

	if (t1 < 1.0)
	{
		x2 = int(std::lround(lx1 + t1 * cdx));
		y2 = int(std::lround(ly1 + t1 * cdy));
	}
	if (t0 > 0.0)
	{
		x1 = int(std::lround(lx1 + t0 * cdx));
		y1 = int(std::lround(ly1 + t0 * cdy));
	}

	int dx = std::abs(x2 - x1), dy = std::abs(y2 - y1);
	int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
	int err = dx - dy;
	for (;;)
	{
		if (x1 >= 0 && x1 < bw && y1 >= 0 && y1 < bh && y1 >= t_band_y1 && y1 <= t_band_y2)
			if (!moire || !((x1 ^ y1) & 1))
				plot(x1, y1);
		if (x1 == x2 && y1 == y2)
			break;
		int e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x1 += sx; }
		if (e2 < dx)  { err += dx; y1 += sy; }
	}
}


static void draw_wireframe_line(bitmap_rgb32 &bitmap, const rectangle &clip, int x1, int y1, int x2, int y2, uint32_t color, bool moire)
{
	line_pixels(clip, bitmap.width(), bitmap.height(), x1, y1, x2, y2, moire, [&](int x, int y) { bitmap.pix(y, x) = color; });
}

void model1::fill_quad(bitmap_rgb32 &bitmap, view_t *view, const quad_t& q) const
{
	spoint_t p[8];
	int color = q.col;

	if (color < 0)
	{
		color = -1 - color;
		M1_LOG(LOG_TGP, "VIDEOD: Q (%d, %d)-(%d, %d)-(%d, %d)-(%d, %d)\n",
					q.p[0]->s.x, q.p[0]->s.y,
					q.p[1]->s.x, q.p[1]->s.y,
					q.p[2]->s.x, q.p[2]->s.y,
					q.p[3]->s.x, q.p[3]->s.y);
	}

	// A wireframe primitive arrives as a degenerate quad with only two distinct
	// screen vertices (A,A,B,B); rasterize it as a line so near-horizontal wires
	// keep their full span instead of collapsing to one pixel per row.
	{
		int ax = q.p[0]->s.x, ay = q.p[0]->s.y;
		int bx = ax, by = ay, ndist = 1;
		for (int i = 1; i < 4; i++)
		{
			int vx = q.p[i]->s.x, vy = q.p[i]->s.y;
			if (vx == ax && vy == ay)
				continue;
			if (ndist == 1)
			{
				bx = vx;
				by = vy;
				ndist = 2;
			}
			else if (vx != bx || vy != by)
			{
				ndist = 3;
				break;
			}
		}
		if (ndist == 2)
		{
			// clip to the viewport, exactly like the scanline filler below does
			draw_wireframe_line(bitmap, t_banded ? t_full_clip : rectangle(view->x1, view->x2, view->y1, view->y2), ax, ay, bx, by, color & ~MOIRE, (color & MOIRE) != 0);
			return;
		}
	}

	if (s_hd_raster)
	{
		fill_quad_hd(bitmap, view, q, color);
		return;
	}

	for (int i = 0; i < 4; i++)
	{
		p[i].x = p[i+4].x = q.p[i]->s.x << FRAC_SHIFT;
		p[i].y = p[i+4].y = q.p[i]->s.y;
	}

	int pmin = 0;
	int pmax = 0;
	for (int i = 1; i < 4; i++)
	{
		if(p[i].y < p[pmin].y)
		{
			pmin = i;
		}
		if(p[i].y > p[pmax].y)
		{
			pmax = i;
		}
	}

	int32_t cury = p[pmin].y;
	int32_t limy = p[pmax].y;

	int32_t x1, x2;
	if (cury == limy)
	{
		x1 = p[0].x;
		x2 = p[0].x;
		for (int i = 1; i < 4; i++)
		{
			if (p[i].x < x1)
			{
				x1 = p[i].x;
			}
			if (p[i].x > x2)
			{
				x2 = p[i].x;
			}
		}
		fill_line(bitmap, view, color, cury, x1, x2);
		return;
	}

	if(cury > view->y2)
	{
		return;
	}
	if(limy <= view->y1)
	{
		return;
	}

	if(limy > view->y2)
	{
		limy = view->y2;
	}

	int ps1 = pmin+4;
	int ps2 = pmin;

	goto startup;

	int32_t sl1, sl2;
	for(;;)
	{
		if (p[ps1 - 1].y == p[ps2 + 1].y)
		{
			fill_slope(bitmap, view, color, x1, x2, sl1, sl2, cury, p[ps1 - 1].y, &x1, &x2);
			cury = p[ps1 - 1].y;
			if(cury >= limy)
			{
				break;
			}
			ps1--;
			ps2++;

		startup:
			while(p[ps1-1].y == cury)
			{
				ps1--;
			}
			while(p[ps2+1].y == cury)
			{
				ps2++;
			}
			x1 = p[ps1].x;
			x2 = p[ps2].x;
			sl1 = (x1 - p[ps1 - 1].x) / (cury - p[ps1 - 1].y);
			sl2 = (x2 - p[ps2 + 1].x) / (cury - p[ps2 + 1].y);
		}
		else if (p[ps1 - 1].y < p[ps2 + 1].y)
		{
			fill_slope(bitmap, view, color, x1, x2, sl1, sl2, cury, p[ps1 - 1].y, &x1, &x2);
			cury = p[ps1 - 1].y;
			if(cury >= limy)
			{
				break;
			}
			ps1--;
			while(p[ps1 - 1].y == cury)
			{
				ps1--;
			}
			x1 = p[ps1].x;
			sl1 = (x1 - p[ps1 - 1].x) / (cury - p[ps1 - 1].y);
		}
		else
		{
			fill_slope(bitmap, view, color, x1, x2, sl1, sl2, cury, p[ps2 + 1].y, &x1, &x2);
			cury = p[ps2 + 1].y;
			if(cury >= limy)
			{
				break;
			}
			ps2++;
			while(p[ps2 + 1].y == cury)
			{
				ps2++;
			}
			x2 = p[ps2].x;
			sl2 = (x2 - p[ps2 + 1].x) / (cury - p[ps2 + 1].y);
		}
	}
	if(cury == limy)
	{
		fill_line(bitmap, view, color, cury, x1, x2);
	}
}

#if 0
void model1::draw_line(bitmap_rgb32 &bitmap, model1::view_t *view, int color, int x1, int y1, int x2, int y2) const
{
	if ((x1 < view->x1 && x2 < view->x1) ||
		(x1 > view->x2 && x2 > view->x2) ||
		(y1 < view->y1 && y2 < view->y1) ||
		(y1 > view->y2 && y2 > view->y2))
		return;

	int x = x1;
	int y = y1;
	int s1x = 1;
	int s1y = 0;
	int s2x = 0;
	int s2y = 1;

	int d1 = x2-x1;
	int d2 = y2-y1;
	if (d1 < 0)
	{
		s1x = -s1x;
		d1 = -d1;
	}
	if (d2 < 0)
	{
		s2y = -s2y;
		d2 = -d2;
	}
	if (d1 < d2)
	{
		int t = s1x;
		s1x = s2x;
		s2x = t;
		t = s1y;
		s1y = s2y;
		s2y = t;
		t = d1;
		d1 = d2;
		d2 = t;
	}

	if(d1 > 600)
	{
		return;
	}

	int cur = 0;
	while (x != x2 || y != y2)
	{
		if (x >= view->x1 && x <= view->x2 && y >= view->y1 && y <= view->y2)
		{
			bitmap.pix(y, x) = color;
		}
		x += s1x;
		y += s1y;
		cur += d2;
		if (cur >= d1)
		{
			cur -= d1;
			x += s2x;
			y += s2y;
		}
	}
	if (x >= view->x1 && x <= view->x2 && y >= view->y1 && y <= view->y2)
	{
		bitmap.pix(y, x) = color;
	}
}
#endif

static int comp_quads(const void *q1, const void *q2)
{
	model1::quad_t* const* c1 = static_cast<model1::quad_t* const*>(q1);
	model1::quad_t* const* c2 = static_cast<model1::quad_t* const*>(q2);

	return (*c1)->compare(*c2);
}

int model1::quad_t::compare(const model1::quad_t* other) const
{
	float z2 = other->z;

	if (z < z2)
		return +1;
	if (z > z2)
		return -1;

	if (this - other < 0)
		return -1;

	return +1;
}

void model1::sort_quads() const
{
	const int count = m_quadpt - &m_quaddb[0];
	for (int i = 0; i < count; i++)
	{
		m_quadind[i] = &m_quaddb[i];
	}
	qsort(&m_quadind[0], count, sizeof(model1::quad_t*), comp_quads);
}

void model1::unsort_quads() const
{
	const int count = m_quadpt - &m_quaddb[0];
	for (int i = 0; i < count; i++)
	{
		m_quadind[i] = &m_quaddb[i];
	}
}


void model1::draw_quads(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	view_t *view = m_view.get();
	int count = m_quadpt - &m_quaddb[0];
	m_frame_quads += count;
	if (count > 0) {                                 // the rows the 3D may draw in (the GPU's parts)
		m_d3_y0 = std::min(m_d3_y0, std::max(view->y1, cliprect.min_y));
		m_d3_y1 = std::max(m_d3_y1, std::min(view->y2, cliprect.max_y));
	}

	/* clip to the cliprect */
	int save_x1 = view->x1;
	int save_x2 = view->x2;
	int save_y1 = view->y1;
	int save_y2 = view->y2;
	view->x1 = std::max(view->x1, cliprect.min_x);
	view->x2 = std::min(view->x2, cliprect.max_x);
	view->y1 = std::max(view->y1, cliprect.min_y);
	view->y2 = std::min(view->y2, cliprect.max_y);

	if (m_rec) { if (count > 0) record_quads(view, count); }
	else {
		const uint64_t t0 = trace_ns();
		raster_quads(bitmap, view, count);
		m_raster_ms += (trace_ns() - t0) / 1e6;
	}

	view->x1 = save_x1;
	view->x2 = save_x2;
	view->y1 = save_y1;
	view->y2 = save_y2;
}

// The CPU's drawing of the list (m_quadind's first `count`) within the view's clip
void model1::raster_quads(bitmap_rgb32 &bitmap, view_t *view, int count)
{
	// debug: VF_PICK=x,y,frame prints every polygon that writes that pixel on that frame
	static int pick_x = -1, pick_y = -1, pick_f = -1;
	static const char *pick_env = getenv("VF_PICK");
	if (pick_x < 0 && pick_env) sscanf(pick_env, "%d,%d,%d", &pick_x, &pick_y, &pick_f);
	const bool picking = pick_x >= 0 && int(m_frame) == pick_f && pick_x < bitmap.width() && pick_y < bitmap.height();
	// Above 1x (with the blended shadows): the polygons paint in the arcade's order, noting which
	// painted each pixel, and a fighter's shadow doesn't shade a fighter (the arcade's
	// one-depth-per-polygon sort let it paint over one lying on the floor). VF's shadows sit off
	// the floor, so depth can't tell floor from fighter; the model can: the stages' come first in
	// the polygon data, the fighters' from 80000 on.
	static const bool no_depth = getenv("VF_NO_SHADOW_DEPTH") != nullptr;   // testing: the arcade's order
	const bool depth = s_hd_raster && s_moire_blend && !no_depth;
	if (depth) {
		// the owners: numbered above every earlier pass's (and the buffer cleared when they run out),
		// instead of clearing the picture's worth each time
		const size_t n = size_t(bitmap.width()) * bitmap.height();
		if (bitmap.width() != s_depth_w || s_owner.size() < n || int64_t(s_owner_base) + s_last_count + count + 2 >= INT32_MAX) {
			s_depth_w = bitmap.width();
			s_owner.assign(std::max(n, s_owner.size()), 0);
			s_owner_base = 0;
		} else
			s_owner_base += s_last_count + 1;
		s_last_count = count;
		s_qstage.resize(count);
		for (int i = 0; i < count; i++) s_qstage[i] = stage_model(m_quadind[i]->obj);
		s_depth_pass = 1;
	}
	struct pass_end { ~pass_end() { s_depth_pass = 0; s_cur_fshadow = false; } } pass_end_;
	// Above 1x: the rows in bands, a thread each, every band drawing all the polygons in order
	// within its rows: the same picture, several times faster. (Picking prints, so stays serial.)
	static const bool serial = getenv("VF_SERIAL_POLYS") != nullptr;   // testing: the one-thread path
	if (s_hd_raster && !picking && !serial && count > 0 && view->y2 >= view->y1) {
		const rectangle full(view->x1, view->x2, view->y1, view->y2);
		const view_t vbase = *view;
		std::mutex marks;
		parallel_rows(view->y2 - view->y1 + 1, [&](int r0, int r1) {
			view_t v = vbase;
			v.y1 = vbase.y1 + r0;
			v.y2 = vbase.y1 + r1 - 1;
			t_banded = true; t_full_clip = full; t_band_y1 = v.y1; t_band_y2 = v.y2;
			t_mark_y1 = 1 << 30; t_mark_y2 = -1;
			s_cur_fshadow = false;
			for (int i = 0; i < count; i++) {
				s_cur_quad = i;
				s_cur_fshadow = depth && fighter_shadow(*m_quadind[i]);
				fill_quad(bitmap, &v, *m_quadind[i]);
			}
			s_cur_fshadow = false;
			t_banded = false; t_band_y1 = -(1 << 30); t_band_y2 = 1 << 30;
			std::lock_guard<std::mutex> lk(marks);
			s_mark_y1 = std::min(s_mark_y1, t_mark_y1);
			s_mark_y2 = std::max(s_mark_y2, t_mark_y2);
		}, 4);                        // 4 bands a thread, taken as threads free up: the floor rows are heavy
		count = 0;                    // drawn: skip the serial loop below
	}
	t_mark_y1 = 1 << 30; t_mark_y2 = -1;
	struct merge_marks { ~merge_marks() { s_mark_y1 = std::min(s_mark_y1, t_mark_y1); s_mark_y2 = std::max(s_mark_y2, t_mark_y2); } } merge_marks_;
	for (int i = 0; i < count; i++)
	{
		s_cur_quad = i;
		s_cur_fshadow = depth && fighter_shadow(*m_quadind[i]);
		uint32_t before = picking ? bitmap.pix(pick_y, pick_x) : 0;
		fill_quad(bitmap, view, *m_quadind[i]);
		if (picking && bitmap.pix(pick_y, pick_x) != before) {
			const quad_t &q = *m_quadind[i];
			fprintf(stderr, "pick: quad %d/%d obj %06x col %06x z %.2f:", i, count, q.obj, q.col & 0xffffff, q.z);
			for (int k = 0; k < 4; k++)
				fprintf(stderr, " (%d,%d z%.2f)", q.p[k]->s.x, q.p[k]->s.y, q.p[k]->z);
			fprintf(stderr, "\n");
		}
#if 0
		quad_t *q = m_quadind[i];
		draw_line(bitmap, m_palette->black_pen(), q.p[0]->s.x, q.p[0]->s.y, q.p[1]->s.x, q.p[1]->s.y);
		draw_line(bitmap, m_palette->black_pen(), q.p[1]->s.x, q.p[1]->s.y, q.p[2]->s.x, q.p[2]->s.y);
		draw_line(bitmap, m_palette->black_pen(), q.p[2]->s.x, q.p[2]->s.y, q.p[3]->s.x, q.p[3]->s.y);
		draw_line(bitmap, m_palette->black_pen(), q.p[3]->s.x, q.p[3]->s.y, q.p[0]->s.x, q.p[0]->s.y);
#endif
	}

}
#if 0
uint16_t model1::scale_color(uint16_t color, float level) const
{
	int r = ((color >> 10) & 31) * level;
	int g = ((color >>  5) & 31) * level;
	int b =         (color & 31) * level;
	return (r << 10) | (g << 5) | b;
}
#endif
// xe = xc + (x/z * zm + tx)
// xe < x1 => xc + (x/z * zm + tx) < x1
//         => x/z < (x1-xc-tx)/zm
//         => x < z*(x1-xc-tx)/zm

// ye = yc - (y/z * zm + ty)
// ye < y1 => yc - (y/z * zm + ty) < y1
//         => -y/z < (y1-yc+ty)/zm
//         => y > -z*(y1-yc+ty)/zm

// xf = zf*a
// xx = x1*t+x2(1-t); zz = z1*t+z2(1-t)
// => x1*t+x2(1-t) = z1*t*a+z2*(1-t)*a
// => t*(x1-x2+a*(z2-z1) = -x2+a*z2
// => t = (z2*a-x2)/((z2-z1)*a-(x2-x1))

void model1::view_t::recompute_frustum()
{
	// MAME's frustum runs through the last pixel's corner; the crack-free filler samples pixel
	// centres, so above 1x it reaches the far edge of the last row and column
	const float edge = s_hd_raster ? 1.0f : 0.0f;
	a_left   = ( lx1 - xc - viewx) / zoomx;
	a_right  = ( lx2 + edge - xc - viewx) / zoomx;
	a_bottom = (-ly1 + yc - viewy) / zoomy;
	a_top    = (-(ly2 + edge) + yc - viewy) / zoomy;
}

bool model1::fclip_isc_bottom(view_t *view, point_t *p)
{
	return p->y > (p->z * view->a_bottom);
}

void model1::fclip_clip_bottom(view_t *view, point_t *pt, point_t *p1, point_t *p2)
{
	float t = (p2->z * view->a_bottom-p2->y) / ((p2->z - p1->z) * view->a_bottom - (p2->y - p1->y));
	pt->x = p1->x * t + p2->x * (1 - t);
	pt->y = p1->y * t + p2->y * (1 - t);
	pt->z = p1->z * t + p2->z * (1 - t);
	view->project_point(pt);
}

bool model1::fclip_isc_top(view_t *view, point_t *p)
{
	return p->y < (p->z * view->a_top);
}

void model1::fclip_clip_top(view_t *view, point_t *pt, point_t *p1, point_t *p2)
{
	float t = (p2->z * view->a_top - p2->y) / ((p2->z - p1->z) * view->a_top - (p2->y - p1->y));
	pt->x = p1->x * t + p2->x * (1 - t);
	pt->y = p1->y * t + p2->y * (1 - t);
	pt->z = p1->z * t + p2->z * (1 - t);
	view->project_point(pt);
}

bool model1::fclip_isc_left(view_t *view, point_t *p)
{
	return p->x < (p->z * view->a_left);
}

void model1::fclip_clip_left(view_t *view, point_t *pt, point_t *p1, point_t *p2)
{
	float t = (p2->z * view->a_left - p2->x) / ((p2->z - p1->z) * view->a_left - (p2->x - p1->x));
	pt->x = p1->x * t + p2->x * (1 - t);
	pt->y = p1->y * t + p2->y * (1 - t);
	pt->z = p1->z * t + p2->z * (1 - t);
	view->project_point(pt);
}

bool model1::fclip_isc_right(view_t *view, point_t *p)
{
	return p->x > (p->z * view->a_right);
}

void model1::fclip_clip_right(view_t *view, point_t *pt, point_t *p1, point_t *p2)
{
	float t = (p2->z * view->a_right - p2->x) / ((p2->z - p1->z) * view->a_right - (p2->x - p1->x));
	pt->x = p1->x * t + p2->x * (1 - t);
	pt->y = p1->y * t + p2->y * (1 - t);
	pt->z = p1->z * t + p2->z * (1 - t);
	view->project_point(pt);
}

void model1::fclip_push_quad_next(int level, quad_t& q, point_t *p1, point_t *p2, point_t *p3, point_t *p4)
{
	quad_t cquad(q.col, q.z, p1, p2, p3, p4);
	fclip_push_quad(level+1, cquad);
}

void model1::fclip_push_quad(int level, quad_t& q)
{
	view_t *view = m_view.get();

	if (level == 4)
	{
		M1_LOG(LOG_TGP, "VIDEOCQ %d", level);
		for (int i = 0; i < 4; i++)
			M1_LOG(LOG_TGP, " (%f, %f, %f)", q.p[i]->x, q.p[i]->y, q.p[i]->z);
		M1_LOG(LOG_TGP, "\n");
		*m_quadpt = q;
		m_quadpt++;
		return;
	}

	bool is_out[4];
	for (int i = 0; i < 4; i++)
	{
		is_out[i] = m_clipfn[level].m_isclipped(view, q.p[i]);
	}

	M1_LOG(LOG_TGP, "VIDEOCQ %d", level);
	for (int i = 0; i < 4; i++)
		M1_LOG(LOG_TGP, " (%f, %f, %f, %d)", q.p[i]->x, q.p[i]->y, q.p[i]->z, is_out[i]);
	M1_LOG(LOG_TGP, "\n");

	// No clipping
	if(!is_out[0] && !is_out[1] && !is_out[2] && !is_out[3])
	{
		fclip_push_quad(level+1, q);
		return;
	}

	// Full clipping
	if(is_out[0] && is_out[1] && is_out[2] && is_out[3])
		return;

	// Find n so that point n is clipped and n-1 isn't
	int i;
	for (i = 0; i < 4; i++)
		if(is_out[i] && !is_out[(i-1)&3])
			break;

	point_t* pt[4];
	bool is_out2[4];
	for (int j = 0; j < 4; j++)
	{
		pt[j] = q.p[(i + j) & 3];
		is_out2[j] = is_out[(i + j) & 3];
	}

	// At this point, pt[0] is clipped out and pt[3] isn't.  Test for the 4 possible cases
	if (is_out2[1])
	{
		if (is_out2[2])
		{
			// pt 0,1,2 clipped out, one triangle left
			m_clipfn[level].m_clip(view, m_pointpt, pt[2], pt[3]);
			point_t* pi1 = m_pointpt++;
			m_clipfn[level].m_clip(view, m_pointpt, pt[3], pt[0]);
			point_t* pi2 = m_pointpt++;
			fclip_push_quad_next(level, q, pi1, pt[3], pi2, pi2);
		}
		else
		{
			// pt 0,1 clipped out, one quad left
			m_clipfn[level].m_clip(view, m_pointpt, pt[1], pt[2]);
			point_t* pi1 = m_pointpt++;
			m_clipfn[level].m_clip(view, m_pointpt, pt[3], pt[0]);
			point_t* pi2 = m_pointpt++;
			fclip_push_quad_next(level, q, pi1, pt[2], pt[3], pi2);
		}
	}
	else
	{
		if (is_out2[2])
		{
			// pt 0,2 clipped out, shouldn't happen, two triangles
			m_clipfn[level].m_clip(view, m_pointpt, pt[0], pt[1]);
			point_t* pi1 = m_pointpt++;
			m_clipfn[level].m_clip(view, m_pointpt, pt[1], pt[2]);
			point_t* pi2 = m_pointpt++;
			fclip_push_quad_next(level, q, pi1, pt[1], pi2, pi2);
			m_clipfn[level].m_clip(view, m_pointpt, pt[2], pt[3]);
			pi1 = m_pointpt++;
			m_clipfn[level].m_clip(view, m_pointpt, pt[3], pt[0]);
			pi2 = m_pointpt++;
			fclip_push_quad_next(level, q, pi1, pt[3], pi2, pi2);
		}
		else
		{
			// pt 0 clipped out, one decagon left, split in quad+tri
			m_clipfn[level].m_clip(view, m_pointpt, pt[0], pt[1]);
			point_t* pi1 = m_pointpt++;
			m_clipfn[level].m_clip(view, m_pointpt, pt[3], pt[0]);
			point_t* pi2 = m_pointpt++;
			fclip_push_quad_next(level, q, pi1, pt[1], pt[2], pt[3]);
			fclip_push_quad_next(level, q, pt[3], pi2, pi1, pi1);
		}
	}
}

float model1::min4f(float a, float b, float c, float d)
{
	float m = a;
	if(b<m)
		m = b;
	if(c<m)
		m = c;
	if(d<m)
		m = d;
	return m;
}

float model1::max4f(float a, float b, float c, float d)
{
	float m = a;
	if (b > m)
		m = b;
	if (c > m)
		m = c;
	if (d > m)
		m = d;
	return m;
}

#ifdef UNUSED_DEFINITION
static const uint8_t num_of_times[]={1,1,1,1,2,2,2,3};
#endif
float model1::compute_specular(glm::vec3& normal, glm::vec3& light, float diffuse, int lmode)
{
	if (!m_view->spec_enable)
		return 0.0f;

	const lightparam_t &lp = m_view->lightparams[lmode];
	if (lp.p == 0 || lp.s <= 0.0f)
		return 0.0f;

	// z component of the reflected light vector, as computed by the Model 2
	// GEO program: R.z = 2*(N.L)*N.z - L.z, raised to a power of two selected
	// by the power field and scaled by the specular scale.
	float s = (2.0f * diffuse * normal.z) - light.z;
	if (s <= 0.0f)
		return 0.0f;
	if (lp.p >= 2)
		s *= s;
	if (lp.p >= 4)
		s *= s;
	if (lp.p >= 7)
		s *= s;
	return std::min(s * lp.s, 1.0f);
}

// old_z is the flat-z "reuse" register (poly zmode 0 = keep previous poly's
// sort z). It persists ACROSS objects within a display-list walk: NetMerc's
// garage door wings (all-zmode-0 dynamic objects) must inherit the ~325k z of
// the preceding object so the z~40k wall occludes them (real-hardware
// capture); resetting per object made them sort at z=0 (nearest) and paint
// through the wall.
void model1::push_object(uint32_t tex_adr, uint32_t poly_adr, uint32_t size, float &old_z)
{
	// Protect against bad data when attacking a super destroyer
	if(tex_adr == 0xffffffff || size >= 0x1000000)
		return;
	// VF's name entry: the wheel's letters N-Z, the symbols and BACK take their colours from the
	// block at 94000, END from 94040, which the game fills with FFFF at boot and never sets: MAME
	// (and we, before) drew them black. Cabinet captures (the user's, 2026-10-13: "Congratulations!"
	// 26 and 24) show them blue and END red, front and back alike; the palette's pens 0x372 (blue)
	// and 0x373 (red) are those colours and nothing else uses them. Only these two blocks, only
	// with FFFF in them, and only the wheel's letter models (380a36-381c2d), which no other scene
	// draws.
	const int ne_pen = (poly_adr >= 0x380a36 && poly_adr <= 0x381c2d) ? tex_adr == 0x94000 ? 0x372 : tex_adr == 0x94040 ? 0x373 : 0 : 0;

#if 0
	int dump;
#endif

	float *poly_data;
	if (poly_adr & 0x800000)
		poly_data = (float *)m_poly_ram.get();
	else
		poly_data = (float *)m_poly_rom;

	poly_adr &= 0x7fffff;
#if 0
	dump = poly_adr == 0x944ea;
	dump = 0;
#endif

#if 0
	if (poly_adr < 0x10000 || poly_adr >= 0x80000)
		return;

	if (poly_adr < 0x40000 || poly_adr >= 0x50000)
		return;

	if (poly_adr == 0x4c7db || poly_adr == 0x4cdaa || poly_adr == 0x4d3e7)
		return;

	if (poly_adr != 0x483e5)
		return;
#endif

	if (true) {
		M1_LOG(LOG_TGP, "XVIDEO:   draw object (%x, %x, %x)\n", tex_adr, poly_adr, size);
	}

	if (!size)
		size = 0xffffffff;

	point_t *old_p0 = m_pointpt++;
	point_t *old_p1 = m_pointpt++;

	old_p0->x = poly_data[poly_adr + 0];
	old_p0->y = poly_data[poly_adr + 1];
	old_p0->z = poly_data[poly_adr + 2];
	old_p1->x = poly_data[poly_adr + 3];
	old_p1->y = poly_data[poly_adr + 4];
	old_p1->z = poly_data[poly_adr + 5];
	m_view->transform_point(old_p0);
	m_view->transform_point(old_p1);

	if (old_p0->z > 0)
	{
		m_view->project_point(old_p0);
	}
	else
	{
		old_p0->s.x = old_p0->s.y = 0; old_p0->fx = old_p0->fy = 0;
	}

	if (old_p1->z > 0)
	{
		m_view->project_point(old_p1);
	}
	else
	{
		old_p1->s.x = old_p1->s.y = 0; old_p1->fx = old_p1->fy = 0;
	}

	poly_adr += 6;

	for (int i = 0; i < size; i++)
	{
#if 0
		M1_LOG(LOG_TGP, "VIDEO:     %08x (%f, %f, %f) (%f, %f, %f) (%f, %f, %f)\n",
			*(uint32_t *)(poly_data + poly_adr) & ~(0x01800303),
			poly_data[poly_adr + 1], poly_data[poly_adr + 2], poly_data[poly_adr + 3],
			poly_data[poly_adr + 4], poly_data[poly_adr + 5], poly_data[poly_adr + 6],
			poly_data[poly_adr + 7], poly_data[poly_adr + 8], poly_data[poly_adr + 9]);
#endif
		uint32_t flags = *reinterpret_cast<uint32_t*>(poly_data + poly_adr);

		int type = flags & 3;
		if (!type)
			break;

		if (flags & 0x00001000)
			tex_adr++;
		// Flags bit 22 selects the second light parameter bank. netmerc uploads
		// two banks (0x00-0x0c organic/terrain, 0x80-0x9e metal) and mixes
		// polygons from both within a single display list; the other games
		// never set the bit. In the Model 2 GEO attribute word this same bit
		// is the top bit of the texture parameter index.
		int lightmode = ((flags >> 17) & 15) | ((flags & 0x00400000) ? 0x80 : 0);

		point_t *p0 = m_pointpt++;
		point_t *p1 = m_pointpt++;

		glm::vec3 vn(poly_data[poly_adr + 1], poly_data[poly_adr + 2], poly_data[poly_adr + 3]);
		const glm::vec3 vn_model = vn;
		p0->x = poly_data[poly_adr + 4];
		p0->y = poly_data[poly_adr + 5];
		p0->z = poly_data[poly_adr + 6];
		p1->x = poly_data[poly_adr + 7];
		p1->y = poly_data[poly_adr + 8];
		p1->z = poly_data[poly_adr + 9];

		if (type == 2)
		{
			p1->x = p0->x;
			p1->y = p0->y;
			p1->z = p0->z;
		}

		int link = (flags >> 8) & 3;

		m_view->transform_vector(vn);

		m_view->transform_point(p0);
		m_view->transform_point(p1);
		if (p0->z > 0)
		{
			m_view->project_point(p0);
		}
		else
		{
			p0->s.x = p0->s.y = 0; p0->fx = p0->fy = 0;
		}

		if (p1->z > 0)
		{
			m_view->project_point(p1);
		}
		else
		{
			p1->s.x = p1->s.y = 0; p1->fx = p1->fy = 0;
		}

#if 0
		if (dump) {
			M1_LOG(LOG_TGP, "VIDEO:     %08x (%f, %f, %f) (%f, %f, %f)\n",
				*(uint32_t *)(poly_data + poly_adr),
				p0->x, p0->y, p0->z,
				p1->x, p1->y, p1->z);

			M1_LOG(LOG_TGP, "VIDEO:     %08x (%d, %d) (%d, %d) (%d, %d) (%d, %d)\n",
				*(uint32_t *)(poly_data + poly_adr),
				old_p0->s.x, old_p0->s.y,
				old_p1->s.x, old_p1->s.y,
				p0->s.x, p0->s.y,
				p1->s.x, p1->s.y);
		}
#endif

		quad_t cquad;

		if (!link)
			goto next;

		if (!(flags & 0x00004000) && view_determinant(old_p1, old_p0, p0) > 0)
			goto next;

		vn = glm::normalize(vn);

		cquad.p[0] = old_p1;
		cquad.p[1] = old_p0;
		cquad.p[2] = p0;
		cquad.p[3] = p1;

		switch ((flags >> 10) & 3)
		{
			case 0:
				cquad.z = old_z;
				break;
			case 1:
				cquad.z = old_z = min4f(old_p1->z, old_p0->z, p0->z, p1->z);
				break;
			case 2:
				cquad.z = old_z = max4f(old_p1->z, old_p0->z, p0->z, p1->z);
				break;
			case 3:
			default:
				cquad.z = 0.0;
				break;
		}

		{
#if 0
			float dif = mult_vector(&vn, &view->light);
			float ln = view->lightparams[lightmode].a + view->lightparams[lightmode].d*MAX(0.0, dif);
			cquad.col = scale_color(m_pens[0x1000 | (m_tgp_ram[tex_adr - 0x40000] & 0x3ff)], MIN(1.0, ln));
			cquad.col = scale_color(m_pens[0x1000 | (m_tgp_ram[tex_adr - 0x40000] & 0x3ff)], MIN(1.0, ln));
#endif

			// the shade: from the normal turned by the object's matrix and the light
			auto lum_of = [&](glm::vec3 n, glm::vec3 light) {
				float dif = glm::dot(n, light);
				float spec = compute_specular(n, light, dif, lightmode);
				float ln = m_view->lightparams[lightmode].a + m_view->lightparams[lightmode].d * std::max(0.0f, dif) + spec;
				return int(255.0f * std::min(1.0f, ln));
			};
			int lumval = lum_of(vn, m_view->light);
			// Smooth motion, an object blended between two frames: its shade too. Lit as at each frame
			// (that frame's matrix and light) and the two colours mixed, so a turning model's shades
			// glide instead of stepping from one frame's to the next's partway, exact at each frame.
			int lum_from = -1, lum_to = -1;
			if (m_shade_from && m_alpha < 1) {
				auto turned = [&](const float *t) {
					const glm::vec3 r1(t[0], t[3], t[6]), r2(t[1], t[4], t[7]), r3(t[2], t[5], t[8]);
					return glm::normalize(glm::vec3(glm::dot(vn_model, r1), glm::dot(vn_model, r2), glm::dot(vn_model, r3)));
				};
				const glm::vec3 light_from = m_shade_light[3] > 0 ? glm::vec3(m_shade_light[0], m_shade_light[1], m_shade_light[2]) : m_view->light;
				lum_from = lum_of(turned(m_shade_from), light_from);
				lum_to = lum_of(turned(m_shade_to), m_view->light);
			}
			// bits [11:10] of the tile word select a colour mode: bit 10 marks a
			// flat UI element (radar blips, HUD text) drawn unlit, and mode 01
			// additionally blinks by rotating the BGR channels on alternate frames.
			uint16_t tex_data = m_tgp_ram[tex_adr - 0x40000];
			if (ne_pen && tex_data == 0xffff) tex_data = uint16_t(0xfc00 | ne_pen);   // (see above; still unlit)
			int color = m_paletteram16[0x1000 | (tex_data & 0x3ff)];
			int r = (color >> 0x0) & 0x1f;
			int g = (color >> 0x5) & 0x1f;
			int b = (color >> 0xA) & 0x1f;
			if (((tex_data >> 10) & 3) == 1 && (m_screen->frame_number() & 1))
			{
				// rotate b -> g -> r -> b
				int tmp = b;
				b = r;
				r = g;
				g = tmp;
			}

			auto colour_of = [&](int lum) {
				lum >>= 2; //there must be a luma translation table somewhere
				if (lum > 0x3f)
					lum = 0x3f;
				else if (lum < 0)
					lum = 0;

				// flat UI element: draw unlit at full intensity instead of shading it
				if (tex_data & 0x400)
					lum = 0x3f;

				const int rr = (m_color_xlat[(r << 8) | lum | 0x0] >> 3) & 0x1f;
				const int gg = (m_color_xlat[(g << 8) | lum | 0x2000] >> 3) & 0x1f;
				const int bb = (m_color_xlat[(b << 8) | lum | 0x4000] >> 3) & 0x1f;
				return uint32_t(pal5bit(rr) << 16) | (pal5bit(gg) << 8) | (pal5bit(bb) << 0);
			};
			if (lum_from >= 0) {
				const uint32_t c0 = colour_of(lum_from), c1 = colour_of(lum_to);
				uint32_t c = 0;
				for (int sh = 0; sh <= 16; sh += 8) {
					const float v0 = float((c0 >> sh) & 0xff), v1 = float((c1 >> sh) & 0xff);
					c |= uint32_t(std::lround(v0 + (v1 - v0) * m_alpha)) << sh;
				}
				cquad.col = c;
			} else
				cquad.col = colour_of(lumval);
		}

		if (flags & 0x00002000)
			cquad.col |= MOIRE;

		fclip_push_quad(0, cquad);

	next:
		poly_adr += 10;
		switch (link) {
			case 0:
			case 2:
				old_p0 = p0;
				old_p1 = p1;
				break;
			case 1:
				old_p1 = p0;
				break;
			case 3:
				old_p0 = p1;
				break;
		}
	}
}


int model1::push_direct(int list_offset) {
	uint32_t tex_adr = readi(list_offset + 2);
	//  v1      = readi(list_offset+2+2);
	//  v2      = readi(list_offset+2+10);

	point_t *old_p0 = m_pointpt++;
	point_t *old_p1 = m_pointpt++;

	old_p0->x = readf(list_offset + 2 + 4);
	old_p0->y = readf(list_offset + 2 + 6);
	old_p0->z = readf(list_offset + 2 + 8);
	old_p1->x = readf(list_offset + 2 + 12);
	old_p1->y = readf(list_offset + 2 + 14);
	old_p1->z = readf(list_offset + 2 + 16);

	M1_LOG(LOG_TGP, "VIDEOD start direct\n");
	M1_LOG(LOG_TGP, "VIDEOD (%f, %f, %f) (%f, %f, %f)\n",
		old_p0->x, old_p0->y, old_p0->z,
		old_p1->x, old_p1->y, old_p1->z);

	// Direct (2D-projected) polys are screen-space: project_point_direct only
	// offsets x/y by the viewport centre and ignores z, so project every vertex
	// unconditionally.  Guarding on z > 0 and snapping behind-"camera" vertices
	// to (0,0) spawns huge spurious triangles radiating from the screen origin
	// (the bogus red cross over the SWA cockpit once direct polys draw on top).
	m_view->project_point_direct(old_p0);
	m_view->project_point_direct(old_p1);

	list_offset += 18;

	for (;;) {
		uint32_t flags = readi(list_offset + 2);

		int type = flags & 3;
		if (!type)
			break;

		if (flags & 0x00001000)
			tex_adr++;

		// list+2+2 is luminosity
		// list+2+4 is 0?
		// list+2+12 is z?

		point_t *p0 = m_pointpt++;
		point_t *p1 = m_pointpt++;

		uint32_t lum = readi(list_offset + 2 + 2);
		//      v1    = readi(list_offset+2+4);

		float z = 0;
		if (type == 2)
		{
			p0->x = readf(list_offset + 2 + 6);
			p0->y = readf(list_offset + 2 + 8);
			p0->z = readf(list_offset + 2 + 10);
			z = p0->z;
			M1_LOG(LOG_TGP, "VIDEOD %08x %08x (%f, %f, %f)\n",
				flags, lum,
				p0->x, p0->y, p0->z);
			*p1 = *p0;
			list_offset += 12;
		}
		else
		{
			p0->x = readf(list_offset + 2 + 6);
			p0->y = readf(list_offset + 2 + 8);
			p0->z = readf(list_offset + 2 + 10);
			p1->x = readf(list_offset + 2 + 14);
			p1->y = readf(list_offset + 2 + 16);
			p1->z = readf(list_offset + 2 + 18);
			z = readf(list_offset + 2 + 12);
			M1_LOG(LOG_TGP, "VIDEOD %08x %08x (%f, %f, %f) (%f, %f, %f)\n",
				flags, lum,
				p0->x, p0->y, p0->z,
				p1->x, p1->y, p1->z);
			list_offset += 20;
		}

		int link = (flags >> 8) & 3;

		//m_view->transform_point(p0);
		//m_view->transform_point(p1);
		m_view->project_point_direct(p0);
		m_view->project_point_direct(p1);

		if (old_p0 && old_p1)
			M1_LOG(LOG_TGP, "VIDEOD:     %08x (%d, %d) (%d, %d) (%d, %d) (%d, %d)\n",
				flags,
				old_p0->s.x, old_p0->s.y,
				old_p1->s.x, old_p1->s.y,
				p0->s.x, p0->s.y,
				p1->s.x, p1->s.y);
		else
			M1_LOG(LOG_TGP, "VIDEOD:     %08x (%d, %d) (%d, %d)\n",
				flags,
				p0->s.x, p0->s.y,
				p1->s.x, p1->s.y);

		quad_t cquad;
		if (!link)
			goto next;

		cquad.p[0] = old_p1;
		cquad.p[1] = old_p0;
		cquad.p[2] = p0;
		cquad.p[3] = p1;
		cquad.z = z;
		{
			int lumval = ((float)(lum >> 24)) * 2.0f;
			int color = m_paletteram16[0x1000 | (m_tgp_ram[tex_adr - 0x40000] & 0x3ff)];
			int r = (color >> 0x0) & 0x1f;
			int g = (color >> 0x5) & 0x1f;
			int b = (color >> 0xA) & 0x1f;
			lumval >>= 2; //there must be a luma translation table somewhere
			if (lumval > 0x3f) lumval = 0x3f;
			else if (lumval < 0) lumval = 0;
			r = (m_color_xlat[(r << 8) | lumval | 0x0] >> 3) & 0x1f;
			g = (m_color_xlat[(g << 8) | lumval | 0x2000] >> 3) & 0x1f;
			b = (m_color_xlat[(b << 8) | lumval | 0x4000] >> 3) & 0x1f;
			cquad.col = (pal5bit(r) << 16) | (pal5bit(g) << 8) | (pal5bit(b) << 0);
		}
		//cquad.col  = scale_color(m_pens[0x1000|(m_tgp_ram[tex_adr-0x40000] & 0x3ff)],((float) (lum>>24)) / 128.0);
		if (flags & 0x00002000)
		{
			cquad.col |= MOIRE;
			// A moiré (stipple-translucent) direct poly whose flat colour resolves
			// to the transparent texture pen (index 0x1000 -> black) is a "fade
			// toward backdrop" overlay, not a black fill.  Painting the pen
			// literally gives a black/scene checkerboard (the Star Wars Arcade
			// gunner 2nd-viewport overlay); the arcade alternates backdrop/scene.
			// Paint the backdrop pen (palette[0]) so the stipple reads
			// backdrop/scene.
			if ((m_tgp_ram[tex_adr - 0x40000] & 0x3ff) == 0)
				cquad.col = MOIRE | (m_palette->pen(0) & 0xffffff);

			// wingwar's display list carries a constant degenerate moiré record
			// (all four vertices on one point) that the flat-quad filler would
			// paint as a stray dot at the viewport centre; not seen on hardware.
			if (cquad.p[0]->s.x == cquad.p[1]->s.x && cquad.p[1]->s.x == cquad.p[2]->s.x && cquad.p[2]->s.x == cquad.p[3]->s.x &&
				cquad.p[0]->s.y == cquad.p[1]->s.y && cquad.p[1]->s.y == cquad.p[2]->s.y && cquad.p[2]->s.y == cquad.p[3]->s.y)
				goto next;
		}

		// Direct polys are screen-space - project_point_direct only offsets x/y by
		// the viewport centre and ignores z.  fclip_push_quad's frustum clip tests
		// vertices against z-scaled planes (y > z*a_bottom, ...), which for a direct
		// poly with small/zero z collapses every plane onto the origin and culls the
		// whole poly (e.g. the SWA FIGHTER CONTROL console moiré rect, all z = 0).
		// Skip the frustum clip and queue the quad directly; fill_quad still scissors
		// it to the viewport rectangle.
		*m_quadpt++ = cquad;

	next:
		switch (link) {
		case 0:
		case 2:
			old_p0 = p0;
			old_p1 = p1;
			break;
		case 1:
			old_p1 = p0;
			break;
		case 3:
			old_p0 = p1;
			break;
		}
	}
	list_offset += 4;
	return list_offset;
}

int model1::skip_direct(int list_offset) const
{
	list_offset += 18;

	while (true) {
		uint32_t flags = readi(list_offset + 2);

		int type = flags & 3;
		if (!type)
			break;

		if (type == 2)
			list_offset += 12;
		else
			list_offset += 20;
	}
	list_offset += 4;
	return list_offset;
}

void model1::draw_objects(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	if (m_quadpt != &m_quaddb[0])
	{
		M1_LOG(LOG_TGP, "VIDEO: sort&draw\n");
		sort_quads();
		draw_quads(bitmap, cliprect);
	}

	m_quadpt = &m_quaddb[0];
	m_pointpt = &m_pointdb[0];
}


int model1::draw_direct(bitmap_rgb32 &bitmap, const rectangle &cliprect, int list_offset)
{
	M1_LOG(LOG_TGP, "VIDEO:   draw direct %x\n", readi(list_offset + 2));

	draw_objects(bitmap, cliprect);

	list_offset = push_direct(list_offset);

	unsort_quads();
	draw_quads(bitmap, cliprect);

	m_quadpt = &m_quaddb[0];
	m_pointpt = &m_pointdb[0];

	return list_offset;
}


void model1::set_current_render_list()
{
	if(!(m_listctl[0] & 4))
		m_listctl[0] = (m_listctl[0] & ~0x40) | (m_listctl[0] & 8 ? 0x40 : 0);
	m_display_list_current = m_listctl[0] & 0x40 ? m_display_list1 : m_display_list0;
}

// The list to show (smooth motion keeps a copy of it to draw from). The name entry flips the
// selection every frame but only ever writes list 1, so every other frame showed list 0's
// long-gone picture: the ring and the fighter flashing. A selected list the game hasn't touched in
// a while, when the other was just written: the other is the picture. (In play the two are written
// in turn and neither is ever more than a few frames old.) Only the picture: the lists' uploads,
// which the game's own maths goes on, are still made from the selected one (tgp_scan).
const uint16_t *model1::picture_list()
{
	set_current_render_list();
	int cur = m_display_list_current == m_display_list1 ? 1 : 0;
	auto age = [&](int b) { return m_list_written[b] == ~0ull ? ~0ull : m_frame - m_list_written[b]; };
	static const bool as_selected = getenv("VF_LIST_AS_SELECTED") != nullptr;   // testing: the old way
	if (!as_selected && age(cur) > 8 && age(cur ^ 1) <= 2) cur ^= 1;
	return cur ? m_display_list1 : m_display_list0;
}

int model1::get_list_number()
{
	if(!(m_listctl[0] & 4))
		m_listctl[0] = (m_listctl[0] & ~0x40) | (m_listctl[0] & 8 ? 0x40 : 0);
	return m_listctl[0] & 0x40 ? 0 : 1;
}

void model1::end_frame()
{
	if((m_listctl[0] & 4) && (m_screen->frame_number() & 1))
		m_listctl[0] ^= 0x40;
}

u16 model1::model1_listctl_r(offs_t offset)
{
	if(!offset)
		return m_listctl[0] | 0x30;
	else
		return m_listctl[1];
}

void model1::model1_listctl_w(offs_t offset, u16 data, u16 mem_mask)
{
	COMBINE_DATA(m_listctl + offset);
	M1_LOG(LOG_TGP, "VIDEO: control=%08x\n", (m_listctl[1] << 16) | m_listctl[0]);
}

void model1::model1_paletteram_w(offs_t offset, u16 data, u16 mem_mask)
{
	COMBINE_DATA(&m_paletteram16[offset]);
	u16 const v = m_paletteram16[offset];
	int r = pal5bit((v >> 0) & 0x1f);
	int g = pal5bit((v >> 5) & 0x1f);
	int b = pal5bit((v >> 10) & 0x1f);
	// Bit 15 is an intensity/shade bit, not unused as the plain xBGR_555 decode
	// assumes.  Almost every entry has it set (full brightness); a handful clear
	// it to render dimmed (e.g. Star Wars Arcade's grey menu backdrop pen 0x7777).
	// Halve the channels when it is clear so those pens stop reading too bright.
	if (!BIT(v, 15))
	{
		r >>= 1;
		g >>= 1;
		b >>= 1;
	}
	m_palette->set_pen_color(offset, r, g, b);
}

void model1::view_t::init_translation_matrix()
{
	memset(translation, 0, sizeof(translation));
	translation[0] = 1.0;
	translation[4] = 1.0;
	translation[8] = 1.0;
}

void model1::view_t::set_viewport(float xcenter, float ycenter, float xl, float xr, float yb, float yt)
{
	static const bool log_viewport = getenv("VF_LOG_VIEWPORT") != nullptr;
	if (log_viewport) {
		static std::set<std::array<int, 6>> seen;
		std::array<int, 6> k{int(xcenter), int(ycenter), int(xl), int(xr), int(yb), int(yt)};
		if (seen.insert(k).second)
			fprintf(stderr, "viewport c=(%g,%g) x=%g..%g y=%g..%g\n", xcenter, ycenter, xl, xr, yb, yt);
	}
	xc = xcenter;
	yc = ycenter;
	lx1 = xl;
	lx2 = xr;
	ly1 = yb;
	ly2 = yt;
	// widescreen: a full-width view (the fight) opens out to the sides; smaller windows stay put
	if (ext > 0 && xl <= 1 && xr >= SCREEN_W - 2) {
		lx1 -= ext;
		lx2 += ext;
	}
	// the clip rectangle in render pixels: logical pixel n covers n*scale .. (n+1)*scale-1
	x1 = int((lx1 + ext) * scale);
	x2 = int((lx2 + ext + 1) * scale) - 1;
	y1 = int(ly1 * scale);
	y2 = int((ly2 + 1) * scale) - 1;

	recompute_frustum();
}

void model1::view_t::set_lightparam(int index, float diffuse, float ambient, float specular, int power)
{
	lightparams[index].d = diffuse;
	lightparams[index].a = ambient;
	lightparams[index].s = specular;
	lightparams[index].p = power;
}

void model1::view_t::set_zoom(float x, float y)
{
	zoomx = x;
	zoomy = y;

	recompute_frustum();
}

// MAME normalizes the light vector. Sarah's stage lights the fighters from below (y > 0, the only
// light from below in the game) and pulses it by its length, 0.5 to 1.7 and back every 256 frames,
// with the floor; normalized, that pulse is lost and the undersides stay at one shade. The cabinet
// shows them brighter: kept at its length there (as the Model 2 GEO uses its light vector).
glm::vec3 model1::view_t::light_vector(float x, float y, float z)
{
	const glm::vec3 v(x, y, z);
	return y > 0 ? v : glm::normalize(v);
}

void model1::view_t::set_light_direction(float x, float y, float z)
{
	light = light_vector(x, y, z);
}

void model1::view_t::set_translation_matrix(float* mat)
{
	for (int i = 0; i < 12; i++)
	{
		translation[i] = mat[i];
	}
}

void model1::view_t::set_view_translation(float x, float y)
{
	viewx = x;
	viewy = y;

	recompute_frustum();
}



void model1::tgp_render(bitmap_rgb32 &bitmap, const rectangle &cliprect, render_pass pass)
{
	m_render_done = 1;
	if (m_replaying || (m_listctl[1] & 0x1f) == 0x1f)
	{
		// smooth motion draws the kept list (model1_interp.cpp)
		if (m_replaying) { m_display_list_current = m_logic[m_replay_older ? 0 : 1].list.data(); m_replay_slot = 0; }
		else set_current_render_list();
		M1_LOG(LOG_TGP, "VIDEO: render list %d\n", get_list_number());

		m_view->init_translation_matrix();
		float old_z = 0;

		int list_offset = 0;
		for (;;) {
			int type = readi(list_offset + 0);

			switch (type)
			{
			case 0:
				list_offset += 2;
				break;
			case 1:
				// command 0x01 = object drawn below the cat1 HUD tilemaps
				if (pass == RENDER_BELOW_HUD) {
					quad_t *const first_quad = m_quadpt;
					struct stamp_t { model1 *m; quad_t *from; uint32_t adr; ~stamp_t() { for (quad_t *q = from; q < m->m_quadpt; q++) q->obj = adr; } }
						stamp{ this, first_quad, readi(list_offset + 4) };
					float blended[12], current[12];
					if (m_replaying && smooth_matrix(readi(list_offset + 4), blended)) {
						memcpy(current, m_view->translation, sizeof current);
						m_view->set_translation_matrix(blended);
						push_object(readi(list_offset + 2), readi(list_offset + 4), readi(list_offset + 6), old_z);
						m_view->set_translation_matrix(current);
						m_shade_from = m_shade_to = m_shade_light = nullptr;
					} else
						push_object(readi(list_offset + 2), readi(list_offset + 4), readi(list_offset + 6), old_z);
				}
				list_offset += 8;
				break;
			case 0x41:
				// command 0x41 = object drawn above the HUD (e.g. SWA radar blips)
				m_above_hud_seen = true;
				if (pass == RENDER_ABOVE_HUD)
					push_object(readi(list_offset + 2), readi(list_offset + 4), readi(list_offset + 6), old_z);
				list_offset += 8;
				break;
			case 2:
			{
				// direct (2D-projected) polys; push them in the below-HUD pass so
				// they share the per-viewport z-sort with the 0x01 objects
				if (pass == RENDER_BELOW_HUD)
					list_offset = push_direct(list_offset);
				else
					list_offset = skip_direct(list_offset);
				break;
			}
			case 3:
			{
				M1_LOG(LOG_TGP, "VIDEO:   viewport (%d, %d, %d, %d, %d, %d, %d)\n",
					readi(list_offset + 2),
					readi16(list_offset + 4), readi16(list_offset + 6), readi16(list_offset + 8),
					readi16(list_offset + 10), readi16(list_offset + 12), readi16(list_offset + 14));

				draw_objects(bitmap, cliprect);

				float xc = readi16(list_offset + 4);
				float yc = 383 - (readi16(list_offset + 6) - 39);
				float x1 = readi16(list_offset + 8);
				float y2 = 383 - (readi16(list_offset + 10) - 39);
				float x2 = readi16(list_offset + 12);
				float y1 = 383 - (readi16(list_offset + 14) - 39);

				m_view->set_viewport(xc, yc, x1, x2, y1, y2);

				list_offset += 16;
				break;
			}
			case 4:
			{
				int adr = readi(list_offset + 2);
				int len = readi(list_offset + 4) + 1;
				M1_LOG(LOG_TGP, "ZVIDEO:   color write, adr=%x, len=%x\n", adr, len);
				for (int i = 0; i < len && !m_replaying; i++)   // (a kept copy's: made already, at its vblank)
				{
					m_tgp_ram[adr - 0x40000 + i] = readi16(list_offset + 6 + 2 * i);
				}
				list_offset += 6 + len * 2;
				break;
			}
			case 5:
			{
				int adr = readi(list_offset + 2);
				int len = readi(list_offset + 4);
				for (int i = 0; i < len && !m_replaying; i++)   // (a kept copy's: made already, at its vblank)
				{
					m_poly_ram[adr - 0x800000 + i] = readi(list_offset + 2 * i + 6);
				}
				list_offset += 6 + len * 2;
				break;
			}
			case 6:
			{
				int adr = readi(list_offset + 2);
				int len = readi(list_offset + 4);
				M1_LOG(LOG_TGP, "VIDEO:   upload data, adr=%x, len=%x\n", adr, len);
				for (int i = 0; i < len; i++)
				{
					int v = readi(list_offset + 6 + i * 2);
					float diffuse = (float(v & 0xff)) / 255.0f;
					float ambient = (float((v >> 8) & 0xff)) / 255.0f;
					float specular = (float((v >> 16) & 0xff)) / 255.0f;
					int power = (v >> 24) & 0xff;
					m_view->set_lightparam(i + adr, diffuse, ambient, specular, power);
				}
				list_offset += 6 + len * 2;
				break;
			}
			case 7:
				// Mode word, as in the Model 2 GEO command 7: bit 0 enables the
				// specular term (netmerc toggles it mid-list, on for the enemy
				// mechs and off for terrain/cockpit); bit 1 tracks the display
				// list double buffer parity.
				M1_LOG(LOG_TGP, "VIDEO:   mode word (%d)\n", readi(list_offset + 2));
				m_view->spec_enable = BIT(readi(list_offset + 2), 0);
				list_offset += 4;
				break;
			case 8:
				M1_LOG(LOG_TGP, "VIDEO:   select mode %08x\n", readi(list_offset + 2));
				list_offset += 4;
				break;
			case 9:
				M1_LOG(LOG_TGP, "VIDEO:   zoom (%f, %f)\n", readf(list_offset + 2), readf(list_offset + 4));
				m_view->set_zoom(readf(list_offset + 2) * 4, readf(list_offset + 4) * 4);
				list_offset += 6;
				break;
			case 0xa:
				M1_LOG(LOG_TGP, "VIDEO:   light vector (%f, %f, %f)\n", readf(list_offset + 2), readf(list_offset + 4), readf(list_offset + 6));
				m_view->set_light_direction(readf(list_offset + 2), readf(list_offset + 4), readf(list_offset + 6));
				list_offset += 8;
				break;
			case 0xb:
			{
				M1_LOG(LOG_TGP, "VIDEO:   matrix (%f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f)\n",
					readf(list_offset + 2), readf(list_offset + 4), readf(list_offset + 6),
					readf(list_offset + 8), readf(list_offset + 10), readf(list_offset + 12),
					readf(list_offset + 14), readf(list_offset + 16), readf(list_offset + 18),
					readf(list_offset + 20), readf(list_offset + 22), readf(list_offset + 24));
				float mat[12];
				for (int i = 0; i < 12; i++)
				{
					mat[i] = readf(list_offset + 2 + 2 * i);
				}
				m_view->set_translation_matrix(mat);
				if (m_replaying) m_replay_slot++;
				list_offset += 26;
				break;
			}
			case 0xc:
				M1_LOG(LOG_TGP, "VIDEO:   trans (%f, %f)\n", readf(list_offset + 2), readf(list_offset + 4));
				m_view->set_view_translation(readf(list_offset + 2), readf(list_offset + 4));
				list_offset += 6;
				break;
			case 0xf:
				//case -1:
				goto end;
			default:
				M1_LOG(LOG_TGP, "VIDEO:   unknown type %d\n", type);
				goto end;
			}
		}
	end:
		draw_objects(bitmap, cliprect);
	}
}


void model1::tgp_scan()
{
#if 0
	if (machine().input().code_pressed_once(KEYCODE_F)) {
		FILE *fp;
		fp = fopen("tgp-ram.bin", "w+b");
		if (fp) {
			fwrite(m_tgp_ram, (0x100000 - 0x40000) * 2, 1, fp);
			fclose(fp);
		}
		exit(0);
	}
#endif
	if (!m_render_done && (m_listctl[1] & 0x1f) == 0x1f)
	{
		set_current_render_list();
		// Skip everything but the data uploads
		M1_LOG(LOG_TGP, "VIDEO: scan list %d\n", get_list_number());

		int list_offset = 0;
		for (;;)
		{
			int type = readi(list_offset + 0);
			switch (type) {
				case 0:
					list_offset += 2;
					break;
				case 1:
				case 0x41:
					list_offset += 8;
					break;
				case 2:
					list_offset = skip_direct(list_offset);

					break;
				case 3:
					list_offset += 16;
					break;
				case 4:
				{
					int adr = readi(list_offset + 2);
					int len = readi(list_offset + 4) + 1;
					M1_LOG(LOG_TGP, "ZVIDEO:   scan color write, adr=%x, len=%x\n", adr, len);
					for (int i = 0; i<len; i++)
					{
						m_tgp_ram[adr - 0x40000 + i] = readi16(list_offset + 6 + 2 * i);
					}
					list_offset += 6 + len * 2;
					break;
				}
				case 5:
				{
					int adr = readi(list_offset + 2);
					int len = readi(list_offset + 4);
					for (int i = 0; i < len; i++)
					{
						m_poly_ram[adr - 0x800000 + i] = readi(list_offset + 2 * i + 6);
					}
					list_offset += 6 + len * 2;
					break;
				}
				case 6:
				{
					int adr = readi(list_offset + 2);
					int len = readi(list_offset + 4);
					//M1_LOG(LOG_TGP, "VIDEO:   upload data, adr=%x, len=%x\n", adr, len);
					for (int i = 0; i<len; i++)
					{
						int v = readi(list_offset + 6 + i * 2);
						m_view->lightparams[i + adr].d = (float(v & 0xff)) / 255.0f;
						m_view->lightparams[i + adr].a = (float((v >> 8) & 0xff)) / 255.0f;
						m_view->lightparams[i + adr].s = (float((v >> 16) & 0xff)) / 255.0f;
						m_view->lightparams[i + adr].p = (v >> 24) & 0xff;
						//M1_LOG(LOG_TGP, "         %02X\n",v);
					}
					list_offset += 6 + len * 2;
					break;
				}
				case 7:
					list_offset += 4;
					break;
				case 8:
					list_offset += 4;
					break;
				case 9:
					list_offset += 6;
					break;
				case 0xa:
					list_offset += 8;
					break;
				case 0xb:
					list_offset += 26;
					break;
				case 0xc:
					list_offset += 6;
					break;
				case 0xf:
				case -1:
					goto end;
				default:
					M1_LOG(LOG_TGP, "VIDEO:   unknown type %d\n", type);
					goto end;
			}
		}
	end:
		;
	}
	m_render_done = 0;
}

void model1::video_start()
{
	m_view = std::make_unique<view_t>();

	m_poly_ram = make_unique_clear<uint32_t[]>(0x400000);
	m_tgp_ram = make_unique_clear<uint16_t[]>(0x100000-0x40000);
	m_pointdb = std::make_unique<point_t[]>(1000000*2);
	m_quaddb  = std::make_unique<quad_t[]>(1000000);
	m_quadind = make_unique_clear<quad_t *[]>(1000000);
	m_overlay_stride = m_bitmap.width();   // the overlay buffers: made when first needed

	m_pointpt = &m_pointdb[0];
	m_quadpt = &m_quaddb[0];
	m_listctl[0] = m_listctl[1] = 0;

	m_clipfn[0].m_isclipped = &model1::fclip_isc_bottom;
	m_clipfn[0].m_clip = &model1::fclip_clip_bottom;
	m_clipfn[1].m_isclipped = &model1::fclip_isc_top;
	m_clipfn[1].m_clip = &model1::fclip_clip_top;
	m_clipfn[2].m_isclipped = &model1::fclip_isc_left;
	m_clipfn[2].m_clip = &model1::fclip_clip_left;
	m_clipfn[3].m_isclipped = &model1::fclip_isc_right;
	m_clipfn[3].m_clip = &model1::fclip_clip_right;

}

// Snapshot the HUD after the cat1 tilemaps are composited: a pixel is a bright
// "feature" (grid lines, struts) when any channel rises above near-black,
// otherwise it is transparent/screen background where the above-HUD blips show.
void model1::build_overlay_mask(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	constexpr int feature_level = 8; // anything darker counts as HUD background
	if (!m_overlay_block) {
		m_overlay_block = std::make_unique<uint8_t[]>(size_t(bitmap.width()) * bitmap.height());
		m_overlay_hud_snapshot = std::make_unique<uint32_t[]>(size_t(bitmap.width()) * bitmap.height());
	}
	for (int y = cliprect.min_y; y <= cliprect.max_y; y++)
	{
		uint32_t const *const src = &bitmap.pix(y);
		for (int x = cliprect.min_x; x <= cliprect.max_x; x++)
		{
			uint32_t argb = src[x];
			int r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
			int i = y * m_overlay_stride + x;
			m_overlay_block[i] = (r > feature_level || g > feature_level || b > feature_level);
			m_overlay_hud_snapshot[i] = argb;
		}
	}
}

// Restore the bright HUD features over the above-HUD blip pass, so blips only
// remain where the HUD pixel was transparent / near-black.
void model1::apply_overlay_stencil(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	for (int y = cliprect.min_y; y <= cliprect.max_y; y++)
	{
		uint32_t *const dst = &bitmap.pix(y);
		for (int x = cliprect.min_x; x <= cliprect.max_x; x++)
		{
			int i = y * m_overlay_stride + x;
			if (m_overlay_block[i])
				dst[x] = m_overlay_hud_snapshot[i];
		}
	}
}

void model1::screen_vblank_model1(int state)
{
	// on rising edge
	if (state)
	{
		tgp_scan();
		end_frame();
		M1_LOG(LOG_TGP, "TGP: vsync\n");
	}
}

// ---- the 3D on the GPU (src/gpu/gpu3d.h) ----

// draw_quads in GPU mode: the list as it would be drawn, recorded (and kept, to be drawn by the CPU
// if the GPU can't)
void model1::record_quads(view_t *view, int count)
{
	const uint64_t t0 = trace_ns();
	static const bool no_depth = getenv("VF_NO_SHADOW_DEPTH") != nullptr;
	const bool depth = s_hd_raster && s_moire_blend && !no_depth;
	const uint32_t batch = uint32_t(m_rec_batches.size());
	m_rec_batches.push_back({ view->x1, view->x2, view->y1, view->y2, m_rec_quads.size(), size_t(count) });
	for (int i = 0; i < count; i++) {
		const quad_t &q = *m_quadind[i];
		m_rec_quads.push_back(q);
		for (int k = 0; k < 4; k++) m_rec_points.push_back(*q.p[k]);
		if (m_rec_bad || view->y2 < view->y1) continue;
		int color = q.col;
		if (color < 0) color = -1 - color;
		// a wireframe line (two distinct vertices, as fill_quad tells): its pixels worked out here, as
		// draw_wireframe_line plots them, each a span
		{
			const int ax = q.p[0]->s.x, ay = q.p[0]->s.y;
			int bx = ax, by = ay, ndist = 1;
			for (int k = 1; k < 4 && ndist < 3; k++) {
				const int vx = q.p[k]->s.x, vy = q.p[k]->s.y;
				if (vx == ax && vy == ay) continue;
				if (ndist == 1) { bx = vx; by = vy; ndist = 2; }
				else if (vx != bx || vy != by) ndist = 3;
			}
			if (ndist == 2) {
				gpu3d_poly g;
				g.flags = G3_LINE | ((color & MOIRE) ? G3_CHECKER : 0);
				g.colour = uint32_t(color & ~MOIRE);
				g.batch = batch;
				const uint32_t idx = uint32_t(m_rec_polys.size());
				const size_t from = m_rec_spans.size();
				line_pixels(rectangle(view->x1, view->x2, view->y1, view->y2), m_bitmap_rec_w, m_bitmap_rec_h, ax, ay, bx, by, (color & MOIRE) != 0,
							[&](int x, int y) { m_rec_spans.push_back({ idx, y, x, x }); });
				if (m_rec_spans.size() == from) continue;
				for (size_t k = from; k < m_rec_spans.size(); k++) { m_rec_y0 = std::min(m_rec_y0, m_rec_spans[k].y); m_rec_y1 = std::max(m_rec_y1, m_rec_spans[k].y); }
				m_rec_polys.push_back(g);
				continue;
			}
		}
		gpu3d_poly g;
		g.cx0 = view->x1; g.cx1 = view->x2; g.cy0 = view->y1; g.cy1 = view->y2;
		g.batch = batch;
		const bool fighter = depth && fighter_shadow(q);
		if (fighter) g.flags |= G3_FIGHTER | G3_SHADOW;
		else if (color & MOIRE) g.flags |= s_moire_blend ? G3_SHADOW : G3_CHECKER;
		if (stage_model(q.obj)) g.flags |= G3_STAGE;
		g.colour = (g.flags & G3_CHECKER) ? uint32_t(color) & 0xffffff : uint32_t(color);
		const uint32_t idx = uint32_t(m_rec_polys.size());
		int ya, yb;
		if (s_hd_raster) {
			bool ok = true;
			for (int k = 0; k < 4; k++) {
				g.x[k] = q.p[k]->fx; g.y[k] = q.p[k]->fy;
				ok = ok && std::fabs(g.x[k]) < 1e7f && std::fabs(g.y[k]) < 1e7f;   // (NaN fails too)
			}
			if (!ok) { if (!m_rec_bad && getenv("VF_LOG_GPU3D")) fprintf(stderr, "gpu3d: a vertex out of range (frame %llu, obj %06x: %g,%g %g,%g %g,%g %g,%g)\n", (unsigned long long)m_frame, q.obj, g.x[0], g.y[0], g.x[1], g.y[1], g.x[2], g.y[2], g.x[3], g.y[3]); m_rec_bad = true; continue; }
			const float ymin = std::min({ g.y[0], g.y[1], g.y[2], g.y[3] }), ymax = std::max({ g.y[0], g.y[1], g.y[2], g.y[3] });
			ya = std::max(view->y1, int(std::ceil(ymin - 0.5f)));
			yb = std::min(view->y2, int(std::ceil(ymax - 0.5f)) - 1);
			if (yb < ya) continue;
			g.flags |= G3_ROWS;
			g.y0 = ya; g.rows = yb - ya + 1;
			g.span0 = 2 * m_rec_rows;
			m_rec_rows += uint32_t(g.rows);
		} else {
			// 1x: the rasterizer's spans (fill_quad), kept
			const size_t from = m_rec_spans.size();
			static bitmap_rgb32 none;
			t_sink = &m_rec_spans; t_sink_poly = idx;
			fill_quad(none, view, q);
			t_sink = nullptr;
			if (m_rec_spans.size() == from) continue;
			ya = 1 << 30; yb = -1;
			for (size_t k = from; k < m_rec_spans.size(); k++) { ya = std::min(ya, m_rec_spans[k].y); yb = std::max(yb, m_rec_spans[k].y); }
		}
		m_rec_y0 = std::min(m_rec_y0, ya); m_rec_y1 = std::max(m_rec_y1, yb);
		if (g.flags & G3_SHADOW) { m_rec_m0 = std::min(m_rec_m0, ya); m_rec_m1 = std::max(m_rec_m1, yb); }
		m_rec_polys.push_back(g);
	}
	m_rec_ms += (trace_ns() - t0) / 1e6;
}

void model1::gpu3d_begin(const bitmap_rgb32 &bitmap)
{
	m_bitmap_rec_w = bitmap.width(); m_bitmap_rec_h = bitmap.height();
	m_rec = true;
	m_rec_bad = false;
	m_rec_batches.clear(); m_rec_quads.clear(); m_rec_points.clear(); m_rec_polys.clear(); m_rec_spans.clear();
	m_rec_rows = 0;
	m_rec_y0 = m_rec_m0 = 1 << 30;
	m_rec_y1 = m_rec_m1 = -1;
	m_rec_ms = 0;
}

// The recording drawn by the CPU, as draw_quads would have
void model1::rec_replay(bitmap_rgb32 &bitmap)
{
	view_t *view = m_view.get();
	const int sx1 = view->x1, sx2 = view->x2, sy1 = view->y1, sy2 = view->y2;
	for (const rec_batch &b : m_rec_batches) {
		view->x1 = b.x1; view->x2 = b.x2; view->y1 = b.y1; view->y2 = b.y2;
		for (size_t i = 0; i < b.count; i++) {
			quad_t &q = m_rec_quads[b.first + i];
			for (int k = 0; k < 4; k++) q.p[k] = &m_rec_points[(b.first + i) * 4 + k];
			m_quadind[i] = &q;
		}
		raster_quads(bitmap, view, int(b.count));
	}
	view->x1 = sx1; view->x2 = sx2; view->y1 = sy1; view->y2 = sy2;
}

void model1::gpu3d_end(bitmap_rgb32 &bitmap)
{
	m_rec = false;
	const uint64_t t0 = trace_ns();
	bool ok = true;
	double gms = 0;
	const bool verify = m_gpu3d_verify > 0 && !m_rec_bad && m_rec_polys.size() >= 200 && (m_g3.pictures % 60) == 0;
	const bool check = verify || (m_gpu3d_check > 0 && !m_rec_bad && (m_g3.pictures % uint64_t(m_gpu3d_check)) == 0);
	if (check) {   // (tests: the picture before the 3D, for the CPU's)
		if (m_check_cpu.width() != bitmap.width() || m_check_cpu.height() != bitmap.height()) m_check_cpu.allocate(bitmap.width(), bitmap.height());
		memcpy(m_check_cpu.data(), bitmap.data(), size_t(bitmap.width()) * bitmap.height() * 4);
	}
	if (m_rec_bad) ok = false;
	else if (!m_rec_polys.empty()) {
		ok = m_gpu3d->draw(m_rec_polys.data(), m_rec_polys.size(), m_rec_spans.data(), m_rec_spans.size(), size_t(m_rec_rows) * 2,
						   bitmap.data(), bitmap.bytes(),
						   bitmap.width(), bitmap.height(), m_rec_y0, m_rec_y1, &gms);
		if (!ok) { m_g3.failed++; fprintf(stderr, "the 3D on the GPU: a picture failed: drawn by the CPU\n"); }
	}
	double ms = m_rec_ms + (trace_ns() - t0) / 1e6;
	if (!ok) {
		m_g3.cpu_fallback++;
		rec_replay(bitmap);
		ms = m_rec_ms + (trace_ns() - t0) / 1e6;
	} else {
		m_g3.pictures++;
		// the rows a shadow may have marked (more than the CPU's own is no matter: elsewhere the
		// marks' clearing changes nothing)
		if (s_moire_blend && m_rec_m0 <= m_rec_m1) { s_mark_y1 = std::min(s_mark_y1, m_rec_m0); s_mark_y2 = std::max(s_mark_y2, m_rec_m1); }
		if (check) {
			rec_replay(m_check_cpu);
			const size_t n = size_t(bitmap.width()) * bitmap.height();
			const uint32_t *a = m_check_cpu.data(), *g = bitmap.data();
			gpu_diff dd = gpu_compare(a, g, n);
			if (verify) { const long long k = gpu_test_diff(dd.all, (long long)n) - dd.all; dd.all += k; dd.cover += k; }   // (tests: as drawn on one only)
			size_t diff = size_t(dd.all), first = dd.first;
			m_g3.checked++;
			if (diff) {
				m_g3.differ++;
				fprintf(stderr, "gpu3d check: frame %llu %dx%d: %zu pixels differ (first at %zu,%zu: cpu %08x gpu %08x)\n",
						(unsigned long long)m_frame, bitmap.width(), bitmap.height(), diff, first % bitmap.width(), first / bitmap.width(), a[first], g[first]);
			}
			if (verify) {
				if (diff && gpu_diff_ok(dd, (long long)n)) {   // (rounding: the GPU's kept)
					fprintf(stderr, "the 3D on the GPU: %zu of %d x %d pixels differ by rounding (%lld drawn on one only, %lld by more than %d, colours by %d at most): keeping the GPU\n",
							diff, bitmap.width(), bitmap.height(), dd.cover, dd.big, GPU_ROUNDING_COLOUR, dd.most);
					if (--m_gpu3d_verify == 0) fprintf(stderr, "the 3D on the GPU: the game's pictures checked against the CPU's\n");
				} else if (diff) {   // (not the CPU's: the CPU's picture shown, and the CPU's from now on)
					memcpy(bitmap.data(), m_check_cpu.data(), n * 4);
					m_g3.failed++;
					m_gpu3d_verify = 0;
					fprintf(stderr, "the 3D on the GPU: its picture isn't the CPU's (%zu pixels differ, %lld drawn on one only, %lld by more than %d): the CPU's 3D from now on\n",
							diff, dd.cover, dd.big, GPU_ROUNDING_COLOUR);
				} else if (--m_gpu3d_verify == 0)
					fprintf(stderr, "the 3D on the GPU: the game's pictures checked against the CPU's, pixel for pixel\n");
			}
		}
	}
	m_g3.gpu = ok;
	m_g3.gpu_ms = gms;
	m_g3.rec_ms = m_rec_ms;
	m_g3.polys = int(m_rec_polys.size());
	m_g3.ms = ms;
	m_rec_quads.clear();   // (the points' copies stay allocated)
}

// Auto's measure (the host's Display > 3D renderer: Auto): a made-up picture the size of a fight's
// (a floor of large polygons, the backdrop's, two fighters' worth of small ones, their shadows)
// drawn unseen at the render scale by the CPU and by the GPU, `reps` times each; the median times
// (ms, the GPU's wall time: recorded, drawn and back). False: the GPU couldn't.
bool model1::bench3d(int reps, double &cpu_ms, double &gpu_ms, long long *differ)
{
	if (!m_gpu3d) return false;
	const int W = m_bitmap.width(), H = m_bitmap.height();
	static bitmap_rgb32 scratch;
	if (scratch.width() != W || scratch.height() != H) scratch.allocate(W, H);
	std::vector<point_t> pts;
	std::vector<quad_t> qs;
	uint32_t seed = 0x1234567;
	auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1 << 24); };
	auto quad = [&](float x, float y, float w, float h, float skew, uint32_t col, uint32_t obj) {
		const float xs[4] = { x, x + w, x + w + skew, x + skew }, ys[4] = { y, y, y + h, y + h };
		for (int k = 0; k < 4; k++) {
			point_t p;
			p.fx = xs[k]; p.fy = ys[k];
			p.s.x = int(xs[k]); p.s.y = int(ys[k]);
			pts.push_back(p);
		}
		qs.push_back(quad_t(int(col), 0, nullptr, nullptr, nullptr, nullptr));
		qs.back().obj = obj;
	};
	// the backdrop's and the floor's, far to near (a stage's model)
	for (int i = 0; i < 60; i++) quad(rnd() * W, H * 0.2f + rnd() * H * 0.2f, W * (0.05f + 0.1f * rnd()), H * 0.1f * rnd() + 4, 0, (seed & 0xffffff) & 0x7f7f7f, 0x10000);
	for (int r = 0; r < 8; r++)
		for (int c = 0; c < 8; c++) quad(-W * 0.2f + c * W * 0.18f, H * 0.4f + r * H * 0.075f, W * 0.18f, H * 0.075f, W * 0.02f * (c - 4), 0x405060 + r * 0x010101 + c, 0x10000);
	// the fighters' shadows, then the fighters (small polygons, about a thousand)
	for (int i = 0; i < 16; i++) quad(W * 0.3f + rnd() * W * 0.4f, H * 0.78f + rnd() * H * 0.05f, W * 0.03f, H * 0.02f, 0, 0x01000000 | 0x202020, 0x90000);
	for (int i = 0; i < 1000; i++) {
		const float cx = (i & 1) ? W * 0.4f : W * 0.6f;
		quad(cx + (rnd() - 0.5f) * W * 0.12f, H * 0.25f + rnd() * H * 0.55f, W * (0.004f + 0.012f * rnd()), H * (0.006f + 0.02f * rnd()), (rnd() - 0.5f) * 6, seed & 0x7f7f7f, 0x90000);
	}
	for (size_t i = 0; i < qs.size(); i++)
		for (int k = 0; k < 4; k++) qs[i].p[k] = &pts[i * 4 + k];
	view_t *view = m_view.get();
	const int sx1 = view->x1, sx2 = view->x2, sy1 = view->y1, sy2 = view->y2;
	const int mk1 = s_mark_y1, mk2 = s_mark_y2;
	view->x1 = 0; view->x2 = W - 1; view->y1 = 0; view->y2 = H - 1;
	auto setind = [&] { for (size_t i = 0; i < qs.size(); i++) m_quadind[i] = &qs[i]; };
	auto run_cpu = [&] {
		scratch.fill(KEY);
		setind();
		const uint64_t t0 = trace_ns();
		raster_quads(scratch, view, int(qs.size()));
		return (trace_ns() - t0) / 1e6;
	};
	auto run_gpu = [&]() -> double {
		scratch.fill(KEY);
		setind();
		const bool on = m_gpu3d_on;
		const uint64_t t0 = trace_ns();
		gpu3d_begin(scratch);
		record_quads(view, int(qs.size()));
		const uint64_t was = m_g3.failed;
		const int chk = m_gpu3d_check, ver = m_gpu3d_verify;
		m_gpu3d_check = m_gpu3d_verify = 0;
		gpu3d_end(scratch);
		m_gpu3d_check = chk; m_gpu3d_verify = ver;
		m_gpu3d_on = on;
		if (m_g3.gpu) m_g3.pictures--;
		return m_g3.failed != was || !m_g3.gpu ? -1.0 : (trace_ns() - t0) / 1e6;
	};
	std::vector<double> c, g;
	bool ok = run_gpu() >= 0;   // (its warm-up)
	for (int i = 0; i < reps && ok; i++) {
		c.push_back(run_cpu());
		const double t = run_gpu();
		if (t < 0) ok = false;
		g.push_back(t);
	}
	if (differ && ok) {   // (the picture each way, compared)
		run_cpu();
		std::vector<uint32_t> cpu(scratch.data(), scratch.data() + size_t(W) * H);
		ok = run_gpu() >= 0;
		m_bench_diff = gpu_compare(cpu.data(), scratch.data(), cpu.size());
		const long long k = gpu_test_diff(m_bench_diff.all, (long long)cpu.size()) - m_bench_diff.all;   // (tests: as drawn on one only)
		m_bench_diff.all += k; m_bench_diff.cover += k;
		*differ = m_bench_diff.all;
	}
	view->x1 = sx1; view->x2 = sx2; view->y1 = sy1; view->y2 = sy2;
	s_mark_y1 = mk1; s_mark_y2 = mk2;
	m_g3 = gpu3d_stats{ 0, 0, 0, false, 0, m_g3.failed, m_g3.cpu_fallback, m_g3.checked, m_g3.differ, m_g3.pictures };
	if (!ok) return false;
	std::sort(c.begin(), c.end()); std::sort(g.begin(), g.end());
	cpu_ms = c[c.size() / 2]; gpu_ms = g[g.size() / 2];
	return true;
}
