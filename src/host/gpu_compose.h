// Puts a picture's parts together on the GPU (model1::render_parts) with SDL's renderer, onto a
// target texture the size of the picture, which is then shown as the picture drawn whole was.
// Each part is uploaded only when it changed: the 2D once a game frame at most (at the arcade's
// resolution), the 3D's changed rows for each picture. Nearest texels and a blend that keeps a
// source pixel whole or leaves it out (KEY's top byte: 0xff) make the target the CPU's picture.
#pragma once
#include "../machine/model1.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

class gpu_compose
{
public:
	// false: this renderer can't (no render targets, or SDL's software renderer, which blends on
	// one CPU thread: slower than drawing the picture whole)
	bool init(SDL_Renderer *ren)
	{
		m_ren = ren;
		const char *name = SDL_GetRendererName(ren);
		if (!name || !strcmp(name, "software")) return false;
		m_max = int(SDL_GetNumberProperty(SDL_GetRendererProperties(ren), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0));
		if (const char *t = getenv("M1_GPU_MAX")) m_max = atoi(t);   // testing: a smaller GPU's limit
		m_keyed = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
											 SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
		return true;
	}
	// a new picture size or renderer state: everything made again
	void reset()
	{
		if (m_target) SDL_DestroyTexture(m_target);
		m_target = nullptr;
		for (auto &t : m_tex) { if (t.tex) SDL_DestroyTexture(t.tex); t = {}; }
		if (m_pre.tex) SDL_DestroyTexture(m_pre.tex);
		m_pre = {};
	}

	// A part that will be needed soon (the next stage sky, model1::sky_pending), uploaded ahead a
	// band of rows a call, into a texture of its own that draw() then takes in place of the old.
	void preload(const model1::gpu_image &im, int64_t max_px = 3 << 20)
	{
		if (m_max > 0 && (im.w > m_max || im.h > m_max)) return;
		if (m_pre.gen != im.gen || !m_pre.tex || m_pre.w != im.w || m_pre.h != im.h) {
			if (!m_pre.tex || m_pre.w != im.w || m_pre.h != im.h) {
				if (m_pre.tex) SDL_DestroyTexture(m_pre.tex);
				m_pre = {};
				m_pre.tex = SDL_CreateTexture(m_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, im.w, im.h);
				if (!m_pre.tex) return;
				made++;
				if (!SDL_SetTextureBlendMode(m_pre.tex, m_keyed) || !SDL_SetTextureScaleMode(m_pre.tex, SDL_SCALEMODE_NEAREST)) {
					SDL_DestroyTexture(m_pre.tex); m_pre = {}; return;
				}
				m_pre.w = im.w; m_pre.h = im.h;
			}
			m_pre.gen = im.gen;
			m_pre.rows = 0;
		}
		if (m_pre.rows >= im.h) return;
		const int n = int(std::clamp<int64_t>(max_px / im.w, 1, im.h - m_pre.rows));
		const SDL_Rect r{ 0, m_pre.rows, im.w, n };
		SDL_UpdateTexture(m_pre.tex, &r, im.px + size_t(m_pre.rows) * im.stride, int(im.stride * 4));
		uploaded += uint64_t(im.w) * n;
		m_pre.rows += n;
	}
	~gpu_compose() { reset(); }
	SDL_Texture *target() const { return m_target; }
	// The 3D's own textures on the renderer's device (gpu3d::draw_target's, wrapped by the host): a
	// picture with d3_target >= 0 takes its 3D from one of them, nothing uploaded. (The host's: not
	// destroyed here.)
	bool set_3d_targets(SDL_Texture *const *t, int n)
	{
		for (int i = 0; i < model1::G3_TARGETS; i++) {
			m_ext3d[i] = i < n ? t[i] : nullptr;
			if (m_ext3d[i] && (!SDL_SetTextureBlendMode(m_ext3d[i], m_keyed) || !SDL_SetTextureScaleMode(m_ext3d[i], SDL_SCALEMODE_NEAREST))) return false;
		}
		return true;
	}
	int made = 0;              // testing (M1_TRACE): textures made, and pixels uploaded
	uint64_t uploaded = 0;

	// The parts onto the target (w x h, scaled into the window with `scale_mode`); false: failed
	// (the caller draws the picture whole instead).
	bool draw(const model1::gpu_parts &p, int w, int h, SDL_ScaleMode scale_mode)
	{
		// a part larger than this GPU's textures go (the hi-res sky at 5x and 6x: 10240 or 12288
		// pixels across, where some stop at 8192) fails here, not in the driver
		for (const model1::gpu_image &im : p.img)
			if (im.px && m_max > 0 && (im.w > m_max || im.h > m_max))
				return SDL_SetError("a %dx%d part is larger than this GPU's %d", im.w, im.h, m_max);
		if (m_max > 0 && (w > m_max || h > m_max)) return SDL_SetError("a %dx%d picture is larger than this GPU's %d", w, h, m_max);
		if (!m_target || m_tw != w || m_th != h) {
			reset();
			m_target = SDL_CreateTexture(m_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
			made++;
			if (!m_target) return false;
			SDL_SetTextureBlendMode(m_target, SDL_BLENDMODE_NONE);
			m_tw = w; m_th = h;
		}
		SDL_SetTextureScaleMode(m_target, scale_mode);
		for (int i = 0; i < model1::TEX_COUNT; i++) {
			const model1::gpu_image &im = p.img[i];
			if (!im.px) continue;
			part &t = m_tex[i];
			// uploaded ahead: that texture taken, the old one kept for the next
			if (m_pre.tex && m_pre.gen == im.gen && t.gen != im.gen && m_pre.rows >= im.h && m_pre.w == im.w && m_pre.h == im.h) {
				std::swap(t.tex, m_pre.tex);
				const int ow = t.w, oh = t.h;
				t.w = im.w; t.h = im.h; t.gen = im.gen;
				m_pre.w = ow; m_pre.h = oh; m_pre.gen = ~0ull; m_pre.rows = 0;
				if (!m_pre.tex) m_pre = {};
				continue;
			}
			bool fresh = false;
			if (!t.tex || t.w != im.w || t.h != im.h) {
				if (t.tex) SDL_DestroyTexture(t.tex);
				t = {};
				t.tex = SDL_CreateTexture(m_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, im.w, im.h);
				made++;
				if (!t.tex) return false;
				if (!SDL_SetTextureBlendMode(t.tex, m_keyed) || !SDL_SetTextureScaleMode(t.tex, SDL_SCALEMODE_NEAREST)) return false;
				t.w = im.w; t.h = im.h;
				fresh = true;
			}
			if (i == model1::TEX_3D) {                // what changed (all of it the first time)
				if (fresh || p.d3_all) { SDL_UpdateTexture(t.tex, nullptr, im.px, int(im.stride * 4)); uploaded += uint64_t(im.w) * im.h; }
				else
					for (const auto &c : p.d3_changed) {
						const SDL_Rect r{ c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0 };
						uploaded += uint64_t(r.w) * r.h;
						SDL_UpdateTexture(t.tex, &r, im.px + size_t(c.y0) * im.stride + c.x0, int(im.stride * 4));
					}
			} else if (fresh || t.gen != im.gen) {
				SDL_UpdateTexture(t.tex, nullptr, im.px, int(im.stride * 4));
				uploaded += uint64_t(im.w) * im.h;
				t.gen = im.gen;
			}
		}
		SDL_Texture *before = SDL_GetRenderTarget(m_ren);
		if (!SDL_SetRenderTarget(m_ren, m_target)) return false;
		bool clipped = false;
		for (const model1::gpu_op &o : p.ops) {
			const bool clip = o.clip_x0 > 0 || o.clip_x1 < w;
			if (clip) {
				const SDL_Rect c{ o.clip_x0, 0, o.clip_x1 - o.clip_x0, h };
				SDL_SetRenderClipRect(m_ren, &c);
			} else if (clipped)
				SDL_SetRenderClipRect(m_ren, nullptr);
			clipped = clip;
			const SDL_FRect d{ o.dx, o.dy, o.dw, o.dh };
			if (o.tex < 0) {
				SDL_SetRenderDrawBlendMode(m_ren, SDL_BLENDMODE_NONE);
				SDL_SetRenderDrawColor(m_ren, Uint8(o.colour >> 16), Uint8(o.colour >> 8), Uint8(o.colour), 255);
				SDL_RenderFillRect(m_ren, &d);
			} else if (o.tex == model1::TEX_3D && p.d3_target >= 0) {
				const SDL_FRect s{ o.sx, o.sy, o.sw, o.sh };
				SDL_Texture *t = p.d3_target < model1::G3_TARGETS ? m_ext3d[p.d3_target] : nullptr;
				if (!t) { if (clipped) SDL_SetRenderClipRect(m_ren, nullptr); SDL_SetRenderTarget(m_ren, before); return SDL_SetError("no texture for the GPU's 3D"); }
				SDL_RenderTexture(m_ren, t, &s, &d);
			} else if (m_tex[o.tex].tex) {
				const SDL_FRect s{ o.sx, o.sy, o.sw, o.sh };
				SDL_RenderTexture(m_ren, m_tex[o.tex].tex, &s, &d);
			}
		}
		if (clipped) SDL_SetRenderClipRect(m_ren, nullptr);
		SDL_SetRenderTarget(m_ren, before);
		return true;
	}

	// Testing: the target's pixels (0x00rrggbb), read back from the GPU.
	bool read(std::vector<uint32_t> &out)
	{
		SDL_Texture *before = SDL_GetRenderTarget(m_ren);
		SDL_SetRenderTarget(m_ren, m_target);
		SDL_Surface *s = SDL_RenderReadPixels(m_ren, nullptr);
		SDL_SetRenderTarget(m_ren, before);
		if (!s) return false;
		SDL_Surface *x = SDL_ConvertSurface(s, SDL_PIXELFORMAT_XRGB8888);
		SDL_DestroySurface(s);
		if (!x) return false;
		out.resize(size_t(x->w) * x->h);
		for (int y = 0; y < x->h; y++)
			for (int i = 0; i < x->w; i++) out[size_t(y) * x->w + i] = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(x->pixels) + size_t(y) * x->pitch)[i] & 0xffffff;
		SDL_DestroySurface(x);
		return true;
	}

	// At startup: known pictures put together here and by the CPU (model1::compose_cpu), which must
	// be the same pixel for pixel, as they are on Metal, Direct3D, OpenGL and Vulkan when the renderer
	// samples and blends as asked. False (with what failed): the CPU draws the pictures for the session.
	bool self_check(std::string &why)
	{
		uint32_t seed = 12345;
		auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
		// images at the arcade's resolution, with KEY (nothing there) in places
		std::vector<uint32_t> px[model1::TEX_COUNT];
		model1::gpu_parts p;
		const int s = 3, e = 10, aw = 64, ah = 48, w = (aw + 2 * e) * s, h = ah * s;
		auto image = [&](int i, int iw, int ih, int keyed) {
			px[i].resize(size_t(iw) * ih);
			for (auto &c : px[i]) c = int(rnd() % 100) < keyed ? model1::KEY : rnd() & 0xffffff;
			p.img[i] = { px[i].data(), iw, ih, size_t(iw), 1 };
		};
		image(model1::TEX_L6, aw + 2 * e, ah, 0);
		image(model1::TEX_L4, aw + 2 * model1::PAD, ah + 2 * model1::PAD, 40);
		image(model1::TEX_MID, aw, ah, 50);
		image(model1::TEX_TOP, aw, ah, 80);
		image(model1::TEX_SKY, 300, 20, 0);
		image(model1::TEX_3D, w, h, 100);
		auto boxes = [&](int n) {   // the 3D: boxes of colour on KEY
			for (int k = 0; k < n; k++) {
				const int x0 = rnd() % w, y0 = rnd() % h, x1 = std::min(w, x0 + 1 + int(rnd() % 60)), y1 = std::min(h, y0 + 1 + int(rnd() % 40));
				const uint32_t c = rnd() & 0xffffff;
				for (int y = y0; y < y1; y++) std::fill(&px[model1::TEX_3D][size_t(y) * w + x0], &px[model1::TEX_3D][size_t(y) * w + x1], c);
			}
		};
		boxes(12);
		auto op = [&](int tex, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh, int c0 = 0, int c1 = 1 << 30) {
			model1::gpu_op o;
			o.tex = tex; o.sx = sx; o.sy = sy; o.sw = sw; o.sh = sh; o.dx = dx; o.dy = dy; o.dw = dw; o.dh = dh; o.clip_x0 = c0; o.clip_x1 = c1;
			p.ops.push_back(o);
		};
		// as render_parts makes them: the backdrop colour; a layer enlarged and one stretched across
		// the picture (66 texels over 252 pixels: no pixel centre falls on a texel's edge, where the
		// GPU may pick either); a moved one from its padded image, within a clip; sky rows copied; the 2D under
		// the 3D, the 3D, the 2D over it; black sides
		op(-1, 0, 0, 0, 0, 0, 0, float(w), float(h)); p.ops.back().colour = 0x123456;
		op(model1::TEX_L6, 0, 0, float(aw + 2 * e), float(ah), 0, 0, float(w), float(h));
		op(model1::TEX_L4, float(model1::PAD), float(model1::PAD), float(aw + 2), float(ah), 0, 0, float(w), float(h));
		op(model1::TEX_L4, 0, 0, float(aw + 2 * model1::PAD), float(ah + 2 * model1::PAD), float(e * s - 7 - model1::PAD * s), float(-5 - model1::PAD * s),
		   float((aw + 2 * model1::PAD) * s), float((ah + 2 * model1::PAD) * s), e * s, w - e * s);
		for (int r = 0; r < 20; r++) op(model1::TEX_SKY, float(r * 7 % 50), float(r), float(w / 2), 1, float(r * 3), float(r * s + 9), float(w / 2), 1);
		op(model1::TEX_MID, 0, 0, float(aw), float(ah), float(e * s), 0, float(aw * s), float(h));
		op(model1::TEX_3D, 0, 0, float(w), float(h), 0, 0, float(w), float(h));
		op(model1::TEX_TOP, 0, 0, float(aw), float(ah), float(e * s), 0, float(aw * s), float(h));
		op(-1, 0, 0, 0, 0, 0, 0, float(e * s), float(h));
		op(-1, 0, 0, 0, 0, float(w - e * s), 0, float(e * s), float(h));
		reset();
		std::vector<uint32_t> want, got;
		bool ok = true;
		for (int pass = 0; pass < 2 && ok; pass++) {
			if (pass == 1) {   // the next picture: the 3D's changed rows alone uploaded, a layer changed
				boxes(6);
				p.d3_changed.push_back({ 0, 0, w, h / 3 });
				p.d3_changed.push_back({ w / 4, h / 2, w / 2, h });
				for (auto &c : px[model1::TEX_MID]) if (int(rnd() % 4) == 0) c = rnd() & 0xffffff;
				p.img[model1::TEX_MID].gen++;
			}
			if (!draw(p, w, h, SDL_SCALEMODE_NEAREST)) { why = std::string("couldn't put the picture together (") + SDL_GetError() + ")"; ok = false; break; }
			if (!read(got) || got.size() != size_t(w) * h) { why = std::string("couldn't read the picture back (") + SDL_GetError() + ")"; ok = false; break; }
			if (pass == 1) {   // what the CPU sees: the rows not uploaded are the first picture's
				std::vector<uint32_t> d3 = px[model1::TEX_3D];
				px[model1::TEX_3D] = m_first_3d;
				for (const auto &c : p.d3_changed)
					for (int y = c.y0; y < c.y1; y++) std::copy(&d3[size_t(y) * w + c.x0], &d3[size_t(y) * w + c.x1], &px[model1::TEX_3D][size_t(y) * w + c.x0]);
				p.img[model1::TEX_3D].px = px[model1::TEX_3D].data();
			} else m_first_3d = px[model1::TEX_3D];
			model1::compose_cpu(p, w, h, want);
			size_t n = 0;
			for (size_t i = 0; i < want.size(); i++) n += want[i] != got[i];
			if (n) { why = std::to_string(n) + " of " + std::to_string(want.size()) + " pixels differ from the CPU's"; ok = false; }
		}
		reset();
		m_first_3d.clear();
		return ok;
	}

private:
	std::vector<uint32_t> m_first_3d;
	struct part { SDL_Texture *tex = nullptr; int w = 0, h = 0; uint64_t gen = ~0ull; };
	struct pre_part { SDL_Texture *tex = nullptr; int w = 0, h = 0, rows = 0; uint64_t gen = ~0ull; } m_pre;
	SDL_Renderer *m_ren = nullptr;
	SDL_Texture *m_target = nullptr;
	SDL_Texture *m_ext3d[model1::G3_TARGETS] = {};
	int m_tw = 0, m_th = 0, m_max = 0;
	part m_tex[model1::TEX_COUNT];
	SDL_BlendMode m_keyed = SDL_BLENDMODE_NONE;
};
