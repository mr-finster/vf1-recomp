// Puts a picture's parts together on the GPU (model1::render_parts) with SDL's renderer, onto a
// target texture the size of the picture, which is then shown as the picture drawn whole was.
// Each part is uploaded only when it changed: the 2D once a game frame at most (at the arcade's
// resolution), the 3D's changed rows for each picture. Nearest texels and a blend that keeps a
// source pixel whole or leaves it out (KEY's top byte: 0xff) make the target the CPU's picture.
#pragma once
#include "../machine/model1.h"
#include "../gpu/gpu3d_shaders.h"
#include <SDL3/SDL.h>
#include <cstdio>
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
		if (const char *t = getenv("VF_GPU_MAX")) m_max = atoi(t);   // testing: a smaller GPU's limit
		m_keyed = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
											 SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
		return true;
	}
	// Low antialiasing's edge pass on the GPU (src/gpu/edge_aa.hlsl): SDL's gpu renderer only, with
	// a custom render state (a fragment shader, the 3D's own picture as its second texture, by
	// which it tells the 3D's pixels). False: not here (the CPU smooths, model1::edge_smooth).
	bool init_edge()
	{
		const char *name = SDL_GetRendererName(m_ren);
		if (getenv("VF_CPU_EDGE") || !name || strcmp(name, "gpu")) return false;
		m_dev = static_cast<SDL_GPUDevice *>(SDL_GetPointerProperty(SDL_GetRendererProperties(m_ren), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
		if (!m_dev) return false;
		SDL_GPUShaderCreateInfo si{};
		const SDL_GPUShaderFormat f = SDL_GetGPUShaderFormats(m_dev);
		if (f & SDL_GPU_SHADERFORMAT_DXIL) { si.format = SDL_GPU_SHADERFORMAT_DXIL; si.code = gpu3d_edge_aa_dxil; si.code_size = sizeof gpu3d_edge_aa_dxil; }
		else if (f & SDL_GPU_SHADERFORMAT_SPIRV) { si.format = SDL_GPU_SHADERFORMAT_SPIRV; si.code = gpu3d_edge_aa_spv; si.code_size = sizeof gpu3d_edge_aa_spv; }
		else return false;
		si.entrypoint = "main";
		si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		si.num_samplers = 2;
		m_edge_shader = SDL_CreateGPUShader(m_dev, &si);
		if (!m_edge_shader) return false;
		SDL_GPUSamplerCreateInfo ci{};
		ci.min_filter = ci.mag_filter = SDL_GPU_FILTER_NEAREST;
		ci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		ci.address_mode_u = ci.address_mode_v = ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		m_edge_sampler = SDL_CreateGPUSampler(m_dev, &ci);
		if (!m_edge_sampler) { SDL_ReleaseGPUShader(m_dev, m_edge_shader); m_edge_shader = nullptr; return false; }
		return true;
	}
	bool edge_ready() const { return m_edge_shader != nullptr; }
	// a new picture size or renderer state: everything made again
	void reset()
	{
		if (m_target) SDL_DestroyTexture(m_target);
		m_target = nullptr;
		if (m_below) SDL_DestroyTexture(m_below);
		m_below = nullptr;
		if (m_small) SDL_DestroyTexture(m_small);
		if (m_mid) SDL_DestroyTexture(m_mid);
		m_small = m_mid = nullptr;
		m_sw = m_sh = m_mw = m_mh = 0;
		drop_edge_states();
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
	// everything on the renderer's device let go, before the renderer is (after it, SDL's
	// release locks a mutex of the destroyed device: the game hung on quitting)
	void release()
	{
		reset();
		if (m_edge_sampler) SDL_ReleaseGPUSampler(m_dev, m_edge_sampler);
		if (m_edge_shader) SDL_ReleaseGPUShader(m_dev, m_edge_shader);
		m_edge_sampler = nullptr;
		m_edge_shader = nullptr;
	}
	~gpu_compose() { release(); }
	SDL_Texture *target() const { return m_target; }
	// Antialiasing on the GPU: the target, drawn k times the shown size each way, averaged down to
	// it (w x h): 2x a linear draw at half the size (each pixel the middle of a 2 x 2 block: their
	// average); 3x through twice the shown size first (3 to 2, linear: neighbours blended) then
	// halved the same way. The picture then never comes back to the CPU (it was put together and
	// averaged there with antialiasing on: a 4x picture's 30 million pixels a frame on one core)
	bool resolve(int k, int w, int h)
	{
		if (!m_target || k < 2) return false;
		auto fit = [&](SDL_Texture *&t, int &tw, int &th, int nw, int nh) {
			if (t && tw == nw && th == nh) return true;
			if (t) SDL_DestroyTexture(t);
			t = SDL_CreateTexture(m_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, nw, nh);
			if (!t) return false;
			made++;
			SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
			SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
			tw = nw; th = nh;
			return true;
		};
		if (!fit(m_small, m_sw, m_sh, w, h)) return false;
		if (k == 3 && !fit(m_mid, m_mw, m_mh, 2 * w, 2 * h)) return false;
		SDL_Texture *before = SDL_GetRenderTarget(m_ren);
		SDL_ScaleMode was = SDL_SCALEMODE_NEAREST;
		SDL_GetTextureScaleMode(m_target, &was);
		SDL_SetTextureScaleMode(m_target, SDL_SCALEMODE_LINEAR);
		bool ok = true;
		SDL_Texture *from = m_target;
		if (k == 3) {
			ok = SDL_SetRenderTarget(m_ren, m_mid) && SDL_RenderTexture(m_ren, m_target, nullptr, nullptr);
			from = m_mid;
		}
		ok = ok && SDL_SetRenderTarget(m_ren, m_small) && SDL_RenderTexture(m_ren, from, nullptr, nullptr);
		SDL_SetTextureScaleMode(m_target, was);
		SDL_SetRenderTarget(m_ren, before);
		return ok;
	}
	SDL_Texture *resolved() const { return m_small; }
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
	int made = 0;              // testing (VF_TRACE): textures made, and pixels uploaded
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
		// Low's edges smoothed here: the parts up to the 3D put together on a texture of their own,
		// drawn onto the target through the edge pass, the rest (the sides, the 2D above) over that
		const bool edge = p.d3_edge && p.d3_target >= 0 && p.d3_target < model1::G3_TARGETS && m_edge_shader;
		SDL_GPURenderState *edge_state = nullptr;
		if (edge) {
			if (!m_below || m_bw != w || m_bh != h) {
				if (m_below) SDL_DestroyTexture(m_below);
				m_below = SDL_CreateTexture(m_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
				made++;
				if (!m_below) return false;
				SDL_SetTextureBlendMode(m_below, SDL_BLENDMODE_NONE);
				SDL_SetTextureScaleMode(m_below, SDL_SCALEMODE_NEAREST);
				m_bw = w; m_bh = h;
			}
			edge_state = edge_state_for(p.d3_target);
			if (!edge_state) return SDL_SetError("the edge pass's render state: %s", SDL_GetError());
		}
		if (!SDL_SetRenderTarget(m_ren, edge ? m_below : m_target)) return false;
		bool clipped = false, below = edge;
		for (const model1::gpu_op &o : p.ops) {
			if (below && o.tex != model1::TEX_3D && m_after_3d) {   // (the 3D's on: its edges, then the rest)
				if (clipped) { SDL_SetRenderClipRect(m_ren, nullptr); clipped = false; }
				if (!edge_pass(edge_state)) { SDL_SetRenderTarget(m_ren, before); return false; }
				below = false;
			}
			m_after_3d = false;
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
				m_after_3d = true;
			} else if (m_tex[o.tex].tex) {
				const SDL_FRect s{ o.sx, o.sy, o.sw, o.sh };
				SDL_RenderTexture(m_ren, m_tex[o.tex].tex, &s, &d);
			}
		}
		if (clipped) SDL_SetRenderClipRect(m_ren, nullptr);
		if (below && !edge_pass(edge_state)) { SDL_SetRenderTarget(m_ren, before); return false; }   // (the 3D last)
		m_after_3d = false;
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
			// (judged by how wrong, as the 3D's check: colours off by a little anywhere, a driver's rounding (NVIDIA's
			// on Direct3D 12 puts pixels a hair off), aren't a broken GPU, and one such pixel put every picture on the
			// CPU, put together there at full size each time: slow above 1x. Past GPU_ROUNDING_COLOUR only a few pixels
			// are let by: each feature of the test picture covers a small part of it, and any of them wrong is a fault)
			size_t n = 0, big = 0;
			for (size_t i = 0; i < want.size(); i++) {
				if (want[i] == got[i]) continue;
				n++;
				int d = 0;
				for (int sh = 0; sh < 24; sh += 8) d = std::max(d, std::abs(int((want[i] >> sh) & 255) - int((got[i] >> sh) & 255)));
				big += d > model1::GPU_ROUNDING_COLOUR;
			}
			const size_t allowed = size_t(model1::GPU_ROUNDING_PX);
			if (big > allowed) { why = std::to_string(big) + " of " + std::to_string(want.size()) + " pixels differ from the CPU's by more than rounding"; ok = false; }
			else if (n) fprintf(stderr, "pictures put together on the GPU: %zu of %zu pixels differ from the CPU's by rounding (%zu by more than %d, up to %zu kept): kept\n",
			                    n, want.size(), big, model1::GPU_ROUNDING_COLOUR, allowed);
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
	SDL_Texture *m_small = nullptr, *m_mid = nullptr;   // (resolve's: the shown size, and twice it for 3x)
	int m_sw = 0, m_sh = 0, m_mw = 0, m_mh = 0;
	SDL_Texture *m_ext3d[model1::G3_TARGETS] = {};
	int m_tw = 0, m_th = 0, m_max = 0;
	part m_tex[model1::TEX_COUNT];
	SDL_BlendMode m_keyed = SDL_BLENDMODE_NONE;
	// Low's edge pass (init_edge): its shader and sampler, the picture beneath and with the 3D
	// (m_below), and a render state for each of the 3D's textures (made as they're first used)
	SDL_GPUDevice *m_dev = nullptr;
	SDL_GPUShader *m_edge_shader = nullptr;
	SDL_GPUSampler *m_edge_sampler = nullptr;
	SDL_Texture *m_below = nullptr;
	int m_bw = 0, m_bh = 0;
	bool m_after_3d = false;   // (the last part drawn was the 3D)
	SDL_GPURenderState *m_edge_state[model1::G3_TARGETS] = {};
	SDL_Texture *m_edge_for[model1::G3_TARGETS] = {};
	SDL_GPURenderState *edge_state_for(int i)
	{
		if (m_edge_state[i] && m_edge_for[i] == m_ext3d[i]) return m_edge_state[i];
		if (m_edge_state[i]) { SDL_DestroyGPURenderState(m_edge_state[i]); m_edge_state[i] = nullptr; }
		if (!m_ext3d[i]) return nullptr;
		SDL_GPUTexture *t = static_cast<SDL_GPUTexture *>(SDL_GetPointerProperty(SDL_GetTextureProperties(m_ext3d[i]), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
		if (!t) return nullptr;
		SDL_GPUTextureSamplerBinding b{ t, m_edge_sampler };
		SDL_GPURenderStateCreateInfo ci{};
		ci.fragment_shader = m_edge_shader;
		ci.num_sampler_bindings = 1;
		ci.sampler_bindings = &b;
		m_edge_state[i] = SDL_CreateGPURenderState(m_ren, &ci);
		m_edge_for[i] = m_ext3d[i];
		return m_edge_state[i];
	}
	void drop_edge_states()
	{
		for (int i = 0; i < model1::G3_TARGETS; i++) {
			if (m_edge_state[i]) SDL_DestroyGPURenderState(m_edge_state[i]);
			m_edge_state[i] = nullptr; m_edge_for[i] = nullptr;
		}
	}
	// the picture so far (m_below) onto the target through the edge pass
	bool edge_pass(SDL_GPURenderState *st)
	{
		if (!SDL_SetRenderTarget(m_ren, m_target) || !SDL_SetGPURenderState(m_ren, st)) return false;
		const bool ok = SDL_RenderTexture(m_ren, m_below, nullptr, nullptr);
		SDL_SetGPURenderState(m_ren, nullptr);
		return ok;
	}
};
