// Sega System 24 tilemap chip (315-5242 family) as Model 1 uses it for its 2D layers: four
// 64x64 maps of 8x8 4bpp tiles, per-layer scroll, line scroll and the 128-pixel window mask.
// Ported from MAME's segaic24.cpp (BSD-3-Clause, copyright-holders: Olivier Galibert).
#pragma once
#include "mame_lite.h"

class segas24_tile
{
public:
	enum { DRAW_OPAQUE = 0x10000 };

	segas24_tile() : char_ram(0x80000 / 2), tile_ram(0x10000 / 2) {}

	uint16_t tile_r(offs_t o) const { return tile_ram[o & 0x7fff]; }
	uint16_t char_r(offs_t o) const { return char_ram[o & 0x3ffff]; }
	uint32_t tile_code(uint16_t entry) const { return entry & tile_mask; }
	// Writes note the map entries and tiles they change, so update() decodes only those again.
	void tile_w(offs_t o, uint16_t d, uint16_t m)
	{
		o &= 0x7fff;
		const uint16_t v = (tile_ram[o] & ~m) | (d & m);
		if (v == tile_ram[o]) return;
		tile_ram[o] = v;
		if (o < 0x4000) { m_entry_dirty[o] = 1; m_entries_dirty = true; }
	}
	void char_w(offs_t o, uint16_t d, uint16_t m)
	{
		o &= 0x3ffff;
		const uint16_t v = (char_ram[o] & ~m) | (d & m);
		if (v == char_ram[o]) return;
		char_ram[o] = v;
		m_char_dirty[o >> 4] = 1;
		m_chars_dirty = true;
		char_gen++;
	}
	// For drawing a picture again from an earlier frame's 2D: the RAMs swapped with a kept copy (the
	// maps are all decoded again at the next update). char_gen counts the tile graphics' changes, so
	// a copy of them is made only when they changed.
	uint64_t char_gen = 0;
	void swap_rams(std::vector<uint16_t> &tiles, std::vector<uint16_t> &chars) { tile_ram.swap(tiles); char_ram.swap(chars); m_dirty = true; }
	void clear() { std::fill(tile_ram.begin(), tile_ram.end(), 0); std::fill(char_ram.begin(), char_ram.end(), 0); m_dirty = true; char_gen++; }   // (power_on)
	// The save state (model1::state): both RAMs; after a load every map entry and tile is decoded again
	template <class IO> void state(IO &io)
	{
		io.vec(tile_ram);
		io.vec(char_ram);
		if (IO::loading) { m_dirty = true; char_gen++; }
	}

	// One map as a 512x512 picture (after update()), for exporting the artwork.
	void render_map(int l, uint32_t *out) const
	{
		for (size_t i = 0; i < size_t(512) * 512; i++) out[i] = m_pens[pix[l][i] & 0x1fff];
	}

	// Map entries (w x h, as the tile RAM holds them) as a picture, 8 pixels per tile; after update().
	void render_entries(const uint16_t *e, int w, int h, uint32_t *out) const
	{
		for (int ty = 0; ty < h; ty++)
			for (int tx = 0; tx < w; tx++) {
				uint16_t val = e[ty * w + tx];
				uint32_t code = val & tile_mask, color = (val >> 7) & 0xff;
				const uint16_t *c = &char_ram[(code * 16) & 0x3ffff];
				for (int y = 0; y < 8; y++) {
					uint32_t row = (uint32_t(c[y * 2]) << 16) | c[y * 2 + 1];
					for (int x = 0; x < 8; x++)
						out[size_t(ty * 8 + y) * (w * 8) + tx * 8 + x] = m_pens[(color * 16 + ((row >> (28 - 4 * x)) & 0xf)) & 0x1fff];
				}
			}
	}

	// Decodes the four maps into 512x512 pen and flag planes; call once per frame before draw(). The
	// planes hold pen numbers, not colours (a palette change needs nothing done), and are decoded
	// again only where map entries or the tiles they show changed (all of it the first time, and
	// after swap_rams).
	void update(const uint32_t *pens)
	{
		m_pens = pens;
		if (pix[0].empty()) {
			for (int l = 0; l < 4; l++) { pix[l].resize(512 * 512); flags[l].resize(512 * 512); }
			m_dirty = true;
		}
		if (m_dirty) {
			for (int e = 0; e < 0x4000; e++) decode_entry(e);
		} else if (m_entries_dirty || m_chars_dirty) {
			if (m_chars_dirty)
				for (int e = 0; e < 0x4000; e++)
					if (m_char_dirty[(tile_ram[e] & tile_mask) & 0x3fff]) m_entry_dirty[e] = 1;
			for (int e = 0; e < 0x4000; e++)
				if (m_entry_dirty[e]) decode_entry(e);
		}
		if (m_dirty || m_entries_dirty) std::fill(m_entry_dirty.begin(), m_entry_dirty.end(), 0);
		if (m_dirty || m_chars_dirty) std::fill(m_char_dirty.begin(), m_char_dirty.end(), 0);
		m_dirty = m_entries_dirty = m_chars_dirty = false;
	}

	void draw(bitmap_rgb32 &bitmap, const rectangle &cliprect, int layer, int lpri, int flags_in)
	{
		uint16_t hscr = tile_ram[0x5000 + (layer >> 1)];
		uint16_t vscr = tile_ram[0x5004 + (layer >> 1)];
		uint16_t ctrl = tile_ram[0x5004 + ((layer >> 1) & 2)];
		const uint16_t *mask = &tile_ram[layer & 4 ? 0x6800 : 0x6000];
		uint16_t tpri = layer & 1;
		layer >>= 1;

		if (vscr & 0x8000)
			return;

		if (ctrl & 0x6000) {
			// Window/scroll split modes, drawn through the generic scrolled path.
			if (layer & 1)
				return;
			int sy = vscr & 0x1ff;
			if (hscr & 0x8000) {
				const uint16_t *hscrtb = &tile_ram[0x4000 + 0x200 * layer];
				for (int y = cliprect.min_y; y <= cliprect.max_y; y++) {
					rectangle c = cliprect; c.min_y = c.max_y = y;
					uint16_t h = hscrtb[y] & 0x1ff;
					int l1 = layer;
					if (((ctrl & 0x6000) >> 13) == 1) {
						uint16_t v = (-vscr) & 0x1ff;
						if (!((-vscr) & 0x200)) l1 ^= 1;
						if (y >= v) l1 ^= 1;
						draw_scrolled(bitmap, c, l1, -h, sy, tpri, flags_in);
					} else {
						rectangle c1 = c, c2 = c;
						if (c1.max_x >= h) c1.max_x = h - 1;
						if (c2.min_x < h) c2.min_x = h;
						if (!(hscrtb[y] & 0x200)) l1 ^= 1;
						draw_scrolled(bitmap, c1, l1, -h, sy, tpri, flags_in);
						draw_scrolled(bitmap, c2, l1 ^ 1, -h, sy, tpri, flags_in);
					}
				}
			} else {
				int sx = -(hscr & 0x1ff);
				rectangle c1 = cliprect, c2 = cliprect;
				if (((ctrl & 0x6000) >> 13) == 1) {
					uint16_t v = (-vscr) & 0x1ff;
					if (c1.max_y >= v) c1.max_y = v - 1;
					if (c2.min_y < v) c2.min_y = v;
					if (!((-vscr) & 0x200)) layer ^= 1;
				} else {
					uint16_t h = hscr & 0x1ff;
					if (c1.max_x >= h) c1.max_x = h - 1;
					if (c2.min_x < h) c2.min_x = h;
					if (!(hscr & 0x200)) layer ^= 1;
				}
				draw_scrolled(bitmap, c1, layer, sx, sy, tpri, flags_in);
				draw_scrolled(bitmap, c2, layer ^ 1, sx, sy, tpri, flags_in);
			}
			return;
		}

		int win = layer & 1;
		if (hscr & 0x8000) {
			const uint16_t *hscrtb = &tile_ram[0x4000 + 0x200 * layer];
			vscr &= 0x1ff;
			for (int y = 0; y < 384; y++) {
				uint16_t h = (-hscrtb[y]) & 0x1ff;
				if (h + 496 <= 512)
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, h, vscr, 0, y, 496, y + 1);
				else {
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, h, vscr, 0, y, 512 - h, y + 1);
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, 0, vscr, 512 - h, y, 496, y + 1);
				}
				vscr = (vscr + 1) & 0x1ff;
			}
		} else {
			hscr = (-hscr) & 0x1ff;
			vscr = (+vscr) & 0x1ff;
			if (hscr + 496 <= 512) {
				if (vscr + 384 <= 512)
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, hscr, vscr, 0, 0, 496, 384);
				else {
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, hscr, vscr, 0, 0, 496, 512 - vscr);
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, hscr, 0, 0, 512 - vscr, 496, 384);
				}
			} else {
				if (vscr + 384 <= 512) {
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, hscr, vscr, 0, 0, 512 - hscr, 384);
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, 0, vscr, 512 - hscr, 0, 496, 384);
				} else {
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, hscr, vscr, 0, 0, 512 - hscr, 512 - vscr);
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, 0, vscr, 512 - hscr, 0, 496, 512 - vscr);
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, hscr, 0, 0, 512 - vscr, 512 - hscr, 384);
					draw_rect(layer, bitmap, mask, tpri, flags_in, win, 0, 0, 512 - hscr, 512 - vscr, 496, 384);
				}
			}
		}
	}

	std::vector<uint16_t> char_ram, tile_ram;
	int m_ox = 0;   // destination x of the arcade's column 0 (widescreen draws centre the picture)

	// Widescreen: draws a layer across [-ext, 496 + ext) by letting the 512-pixel map wrap, the
	// way it scrolls anyway. Only the plain scroll modes; the split modes fall back to draw().
	void draw_wide(bitmap_rgb32 &bitmap, int layer, int flags_in, int ext)
	{
		uint16_t hscr = tile_ram[0x5000 + (layer >> 1)];
		uint16_t vscr = tile_ram[0x5004 + (layer >> 1)];
		uint16_t ctrl = tile_ram[0x5004 + ((layer >> 1) & 2)];
		const uint16_t *mask = &tile_ram[layer & 4 ? 0x6800 : 0x6000];
		uint8_t tpri = uint8_t((layer & 1) | 0x10);
		int l = layer >> 1;
		if (vscr & 0x8000) return;
		if (ctrl & 0x6000) { draw(bitmap, bitmap.cliprect(), layer, 0, flags_in); return; }
		int win = l & 1;
		const uint16_t *hscrtb = &tile_ram[0x4000 + 0x200 * l];
		for (int y = 0; y < 384; y++) {
			int sy, h;
			if (hscr & 0x8000) { h = (-hscrtb[y]) & 0x1ff; sy = (vscr + y) & 0x1ff; }
			else { h = (-hscr) & 0x1ff; sy = (vscr + y) & 0x1ff; }
			const uint16_t *src = &pix[l][size_t(sy) * 512];
			const uint8_t *srct = &flags[l][size_t(sy) * 512];
			uint32_t *dst = &bitmap.pix(y, m_ox - ext);
			for (int x = -ext; x < 496 + ext; x++, dst++) {
				int mx = std::clamp(x, 0, 495);
				uint16_t m = mask[y * 4 + (mx >> 7)];
				if (win) m = ~m;
				if (m & (0x8000 >> ((mx & 127) >> 3))) continue;
				int i = (h + x) & 511;
				if (srct[i] == tpri || (flags_in & DRAW_OPAQUE))
					*dst = m_pens[src[i] & 0x1fff];
			}
		}
	}

private:
	static constexpr uint32_t tile_mask = 0x3fff;
	std::vector<uint16_t> pix[4];
	std::vector<uint8_t> flags[4];
	const uint32_t *m_pens = nullptr;
	bool m_dirty = true;                        // everything to decode
public:
	bool all_to_decode() const { return m_dirty || pix[0].empty(); }
private:
	std::vector<uint8_t> m_entry_dirty = std::vector<uint8_t>(0x4000), m_char_dirty = std::vector<uint8_t>(0x4000);
	bool m_entries_dirty = false, m_chars_dirty = false;

	// one map entry (map e >> 12, its 64x64 place e & 0xfff) into the planes
	void decode_entry(int e)
	{
		const int l = e >> 12, ty = (e >> 6) & 63, tx = e & 63;
		const uint16_t val = tile_ram[e];
		const uint32_t code = val & tile_mask, color = (val >> 7) & 0xff;
		const uint8_t cat = (val & 0x8000) ? 1 : 0;
		const uint16_t *c = &char_ram[(code * 16) & 0x3ffff];
		uint16_t *pm = pix[l].data();
		uint8_t *fm = flags[l].data();
		for (int y = 0; y < 8; y++) {
			const uint32_t row = (uint32_t(c[y * 2]) << 16) | c[y * 2 + 1];
			const size_t base = size_t(ty * 8 + y) * 512 + tx * 8;
			for (int x = 0; x < 8; x++) {
				const uint8_t p = (row >> (28 - 4 * x)) & 0xf;
				pm[base + x] = uint16_t(color * 16 + p);
				fm[base + x] = p ? uint8_t(0x10 | cat) : 0;
			}
		}
	}
public:
	// When set (a buffer the size of the destination), draw_rect also records where each pixel
	// it draws came from: map << 18 | y * 512 + x. For replacing the sky with a hi-res picture.
	uint32_t *m_coord = nullptr;
private:

	void draw_scrolled(bitmap_rgb32 &bm, const rectangle &c, int l, int scrollx, int scrolly, uint16_t tpri, int fl)
	{
		uint8_t want = uint8_t(tpri | 0x10);
		// c is in arcade pixels (0..495); the destination is offset by m_ox
		const int x0 = std::max({c.min_x, 0, -m_ox}), x1 = std::min({c.max_x, 495, bm.width() - 1 - m_ox});
		for (int y = std::max(c.min_y, 0); y <= std::min({c.max_y, 383, bm.height() - 1}); y++)
			for (int x = x0; x <= x1; x++) {
				size_t i = size_t((y + scrolly) & 511) * 512 + ((x + scrollx) & 511);
				if (flags[l][i] == want || (fl & DRAW_OPAQUE)) {
					bm.pix(y, x + m_ox) = m_pens[pix[l][i] & 0x1fff];
					if (m_coord) m_coord[size_t(y) * bm.width() + x + m_ox] = uint32_t(l) << 18 | uint32_t(i);
				}
			}
	}

	void draw_rect(int l, bitmap_rgb32 &dm, const uint16_t *mask, uint16_t tpri, int fl, int win,
				   int sx, int sy, int xx1, int yy1, int xx2, int yy2)
	{
		tpri |= 0x10;
		int dst_x = xx1;            // MAME takes the destination before the group shift
		mask += yy1 * 4;
		while (xx1 >= 128) { xx1 -= 128; xx2 -= 128; mask++; }
		for (int y = 0; y < yy2 - yy1; y++) {
			int dy = yy1 + y;
			if (dy >= dm.height()) break;
			const uint16_t *src = &pix[l][size_t((sy + y) & 511) * 512 + sx];
			const uint8_t *srct = &flags[l][size_t((sy + y) & 511) * 512 + sx];
			uint32_t *dst = &dm.pix(dy, dst_x + m_ox);
			uint32_t *crd = m_coord ? m_coord + size_t(dy) * dm.width() + dst_x + m_ox : nullptr;
			const uint16_t *mask1 = mask + y * 4;
			int llx = xx2, cur_x = xx1;
			while (llx > 0) {
				uint16_t m = *mask1++;
				if (win) m = ~m;
				int llx1 = llx >= 128 ? 128 : llx;
				for (int x = cur_x; x < llx1; x++) {
					if ((*srct == tpri || (fl & DRAW_OPAQUE)) && !(m & (0x8000 >> (x >> 3)))) {
						*dst = m_pens[*src & 0x1fff];
						if (crd) *crd = uint32_t(l) << 18 | uint32_t(src - pix[l].data());
					}
					src++; srct++; dst++;
					if (crd) crd++;
				}
				llx -= 128;
				cur_x = 0;
			}
		}
	}
};
