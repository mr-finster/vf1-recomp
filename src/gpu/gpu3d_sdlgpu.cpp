// The 3D on the GPU through SDL_GPU (src/gpu/gpu3d.h): Direct3D 12 on Windows, Vulkan on Linux and
// the Steam Deck (and Windows without Direct3D 12). gpu3d_metal.mm's passes, its shaders
// src/gpu/gpu3d.hlsl (precompiled into gpu3d_shaders.h by tools/gpu/shaders.sh) with an x86-64
// build's sums. Two ways:
//  - on the device of SDL's "gpu" renderer (gpu3d_create_sdlgpu_on), a picture put together in parts
//    goes straight into one of the renderer's textures (draw_target): nothing comes back, nothing
//    waits but for the picture before last in the same texture (at most two on the way);
//  - else, and for the pictures checked against the CPU's, the polygons and spans go up and each
//    pixel's result comes back (res, and a byte a pixel saying what it is) and is put into the
//    picture on every core, the same pixels as Metal's (draw: waited for).
#include "gpu3d.h"
#include "gpu3d_shaders.h"
#include "../machine/parallel.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t KEY = 0xff000000u, SHADOW_KEY = 0xfd000000u, MOIRE_MARK = 0x01000000u;
using clk = std::chrono::steady_clock;

class gpu3d_sdlgpu final : public gpu3d
{
public:
	gpu3d_sdlgpu(SDL_GPUDevice *dev, bool own) : m_dev(dev), m_own(own) {}
	~gpu3d_sdlgpu() override
	{
		wait_targets();
		for (SDL_GPUComputePipeline *p : { m_clear, m_rows, m_solid, m_shadow, m_pixels, m_target }) if (p) SDL_ReleaseGPUComputePipeline(m_dev, p);
		for (SDL_GPUTexture *t : m_tg) SDL_ReleaseGPUTexture(m_dev, t);
		for (SDL_GPUBuffer *b : { m_pbuf, m_sbuf, m_jp, m_jb, m_owner, m_depth, m_shade, m_res, m_kind }) if (b) SDL_ReleaseGPUBuffer(m_dev, b);
		if (m_up) SDL_ReleaseGPUTransferBuffer(m_dev, m_up);
		if (m_down) SDL_ReleaseGPUTransferBuffer(m_dev, m_down);
		if (m_own) SDL_DestroyGPUDevice(m_dev);
	}
	bool init()
	{
		const auto t0 = clk::now();
		const SDL_GPUShaderFormat fm = SDL_GetGPUShaderFormats(m_dev);
		const bool spirv = (fm & SDL_GPU_SHADERFORMAT_SPIRV) != 0, dxil = (fm & SDL_GPU_SHADERFORMAT_DXIL) != 0, msl = (fm & SDL_GPU_SHADERFORMAT_MSL) != 0;   // (MSL: Metal, Macs)
		if (!spirv && !dxil && !msl) { fprintf(stderr, "gpu3d sdl_gpu: %s takes none of SPIR-V, DXIL or MSL\n", SDL_GetGPUDeviceDriver(m_dev)); return false; }
		auto make = [&](const uint8_t *spv, size_t spv_n, const uint8_t *dx, size_t dx_n, const char *ms, size_t ms_n, const char *name,
			uint32_t ro, uint32_t rwb, uint32_t tx, uint32_t ty, uint32_t rwt = 0) -> SDL_GPUComputePipeline * {
			SDL_GPUComputePipelineCreateInfo ci = {};
			ci.code = spirv ? spv : dxil ? dx : reinterpret_cast<const uint8_t *>(ms);
			ci.code_size = spirv ? spv_n : dxil ? dx_n : ms_n - 1;   // (MSL: its source, less the end)
			ci.format = spirv ? SDL_GPU_SHADERFORMAT_SPIRV : dxil ? SDL_GPU_SHADERFORMAT_DXIL : SDL_GPU_SHADERFORMAT_MSL;
			ci.entrypoint = spirv || dxil ? "main" : "main0";   // (SPIRV-Cross's name for it: tools/gpu/shaders.sh)
			ci.num_readonly_storage_buffers = ro;
			ci.num_readwrite_storage_buffers = rwb;
			ci.num_readwrite_storage_textures = rwt;
			ci.num_uniform_buffers = 1;
			ci.threadcount_x = tx; ci.threadcount_y = ty; ci.threadcount_z = 1;
			SDL_GPUComputePipeline *p = SDL_CreateGPUComputePipeline(m_dev, &ci);
			if (!p) fprintf(stderr, "gpu3d sdl_gpu: %s: %s\n", name, SDL_GetError());
			return p;
		};
#define K(n) gpu3d_##n##_spv, sizeof gpu3d_##n##_spv, gpu3d_##n##_dxil, sizeof gpu3d_##n##_dxil, gpu3d_##n##_msl, sizeof gpu3d_##n##_msl, #n
		m_clear = make(K(clear), 0, 3, 64, 1);
		m_rows = make(K(rows), 3, 1, 64, 1);
		m_solid = make(K(solid), 2, 2, 64, 1);
		m_shadow = make(K(shadow), 4, 1, 64, 1);
		m_pixels = make(K(pixels), 3, 2, 8, 8);
#undef K
		if (!m_clear || !m_rows || !m_solid || !m_shadow || !m_pixels) return false;
		// (straight into the renderer's textures: only on its device, and where RGBA8 can be written)
		if (!m_own && SDL_GPUTextureSupportsFormat(m_dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D,
				SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE))
			m_target = make(gpu3d_target_spv, sizeof gpu3d_target_spv, gpu3d_target_dxil, sizeof gpu3d_target_dxil, gpu3d_target_msl, sizeof gpu3d_target_msl, "target", 3, 0, 8, 8, 1);
		const char *dn = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(m_dev), SDL_PROP_GPU_DEVICE_NAME_STRING, "?");
		snprintf(m_name, sizeof m_name, "%s (%s)", SDL_GetGPUDeviceDriver(m_dev), spirv ? "SPIR-V" : dxil ? "DXIL" : "MSL");
		m_software = strstr(dn, "llvmpipe") || strstr(dn, "lavapipe") || strstr(dn, "softpipe") || strstr(dn, "SwiftShader") || strstr(dn, "Software") || strstr(dn, "Basic Render");   // (Windows' WARP: "Microsoft Basic Render Driver")
		fprintf(stderr, "gpu3d sdl_gpu: %s on %s, its pipelines ready in %.0f ms\n", m_name, dn,
				std::chrono::duration<double, std::milli>(clk::now() - t0).count());
		return true;
	}
	const char *name() const override { return m_name; }
	bool software() const override { return m_software; }

	bool draw(const gpu3d_poly *polys, size_t count, const gpu3d_span *spans, size_t nspans, size_t rowspans,
		uint32_t *px, size_t bytes, int w, int h, int y0, int y1, double *gpu_ms) override
	{
		if (w <= 0 || h <= 0 || bytes < size_t(w) * h * 4) return false;
		return run(polys, count, spans, nspans, rowspans, px, w, h, y0, y1, gpu_ms, -1);
	}

	bool make_targets(int w, int h, int count) override
	{
		if (!m_target || w <= 0 || h <= 0 || count <= 0) return false;
		if (m_tw == w && m_th == h && int(m_tg.size()) == count) return true;
		wait_targets();
		for (SDL_GPUTexture *t : m_tg) SDL_ReleaseGPUTexture(m_dev, t);
		m_tg.clear();
		m_tw = m_th = 0;
		for (int i = 0; i < count; i++) {
			SDL_GPUTextureCreateInfo ti = {};
			ti.type = SDL_GPU_TEXTURETYPE_2D;
			ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
			ti.width = uint32_t(w); ti.height = uint32_t(h);
			ti.layer_count_or_depth = 1; ti.num_levels = 1;
			ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
			SDL_GPUTexture *t = SDL_CreateGPUTexture(m_dev, &ti);
			if (!t) { fprintf(stderr, "gpu3d sdl_gpu: a %dx%d target: %s\n", w, h, SDL_GetError()); return false; }
			m_tg.push_back(t);
		}
		m_fence.assign(m_tg.size(), nullptr);
		m_tw = w; m_th = h;
		m_made++;
		return true;
	}
	void *target_handle(int i) const override { return i >= 0 && i < int(m_tg.size()) ? m_tg[i] : nullptr; }
	uint64_t targets_made() const override { return m_made; }
	bool draw_target(const gpu3d_poly *polys, size_t count, const gpu3d_span *spans, size_t nspans, size_t rowspans,
		int w, int h, int y0, int y1, int t) override
	{
		if (t < 0 || t >= int(m_tg.size()) || w != m_tw || h != m_th) return false;
		// (the picture last drawn into this target done first: at most one a target on the way, so
		// the CPU never runs further ahead of the GPU than that; it's long done unless the GPU is
		// the slower)
		if (m_fence[t]) {
			const bool ok = SDL_WaitForGPUFences(m_dev, true, &m_fence[t], 1);
			SDL_ReleaseGPUFence(m_dev, m_fence[t]);
			m_fence[t] = nullptr;
			if (!ok) { fprintf(stderr, "gpu3d sdl_gpu: a picture failed (%s)\n", SDL_GetError()); return false; }
		}
		return run(polys, count, spans, nspans, rowspans, nullptr, w, h, y0, y1, nullptr, t);
	}

private:
	// The passes, then either each pixel's result back into px (target -1: waited for), or the
	// picture into target t (handed over, not waited for).
	bool run(const gpu3d_poly *polys, size_t count, const gpu3d_span *spans, size_t nspans, size_t rowspans,
		uint32_t *px, int w, int h, int y0, int y1, double *gpu_ms, int target)
	{
		if (gpu_ms) *gpu_ms = 0;
		y0 = std::max(y0, 0); y1 = std::min(y1, h - 1);
		const bool to_target = target >= 0;
		const bool none = !count || y1 < y0;
		if (none && !to_target) return true;
		const uint32_t npx = uint32_t(w) * h, rows_n = none ? 0 : uint32_t(y1 - y0 + 1), words = (uint32_t(w) + 3) / 4;
		const uint32_t off = none ? 0 : uint32_t(y0) * w, len = rows_n * uint32_t(w);
		m_jobpoly.clear(); m_jobbase.clear();
		uint32_t rows = 0;
		bool any_shadow = false;
		for (size_t i = 0; i < count; i++) {
			any_shadow = any_shadow || (polys[i].flags & G3_SHADOW);
			if (!(polys[i].flags & G3_ROWS) || polys[i].rows <= 0) continue;
			m_jobpoly.push_back(uint32_t(i));
			m_jobbase.push_back(rows);
			rows += uint32_t(polys[i].rows);
		}
		const uint32_t ns = uint32_t(nspans + rowspans);
		const uint32_t pb = uint32_t(std::max<size_t>(count, 1) * sizeof(gpu3d_poly)), sb = uint32_t(std::max<size_t>(ns, 1) * sizeof(gpu3d_span));
		const uint32_t cs = uint32_t(nspans * sizeof(gpu3d_span)), jb = uint32_t(std::max<size_t>(m_jobpoly.size(), 1) * 4);
		const uint32_t resb = len * 4, kindb = rows_n * words * 4;
		if (!grow(m_pbuf, m_pbuf_n, pb, false) || !grow(m_sbuf, m_sbuf_n, sb, true) || !grow(m_jp, m_jp_n, jb, false) || !grow(m_jb, m_jb_n, jb, false)
			|| !grow(m_owner, m_owner_n, npx * 4, true) || !grow(m_depth, m_depth_n, npx * 4, true) || !grow(m_shade, m_shade_n, npx * 4, true)
			|| !transfer(m_up, m_up_n, pb + cs + jb * 2, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD))
			return false;
		if (!to_target && (!grow(m_res, m_res_n, resb, true) || !grow(m_kind, m_kind_n, kindb, true)
			|| !transfer(m_down, m_down_n, resb + kindb, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD)))
			return false;
		const auto t0 = clk::now();
		SDL_GPUCommandBuffer *cb = nullptr;
		if (!none) {
			uint8_t *map = (uint8_t *)SDL_MapGPUTransferBuffer(m_dev, m_up, true);
			if (!map) return false;
			memcpy(map, polys, count * sizeof(gpu3d_poly));
			if (cs) memcpy(map + pb, spans, cs);
			if (!m_jobpoly.empty()) { memcpy(map + pb + cs, m_jobpoly.data(), m_jobpoly.size() * 4); memcpy(map + pb + cs + jb, m_jobbase.data(), m_jobbase.size() * 4); }
			SDL_UnmapGPUTransferBuffer(m_dev, m_up);
			cb = SDL_AcquireGPUCommandBuffer(m_dev);
			if (!cb) return false;
			{
				SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
				auto up = [&](SDL_GPUBuffer *b, uint32_t at, uint32_t n) {
					if (!n) return;
					SDL_GPUTransferBufferLocation l = { m_up, at };
					SDL_GPUBufferRegion r = { b, 0, n };
					SDL_UploadToGPUBuffer(cp, &l, &r, false);
				};
				up(m_pbuf, 0, uint32_t(count * sizeof(gpu3d_poly))); up(m_sbuf, pb, cs);
				if (!m_jobpoly.empty()) { up(m_jp, pb + cs, jb); up(m_jb, pb + cs + jb, jb); }
				SDL_EndGPUCopyPass(cp);
			}
			const uint32_t W = uint32_t(w);
			for (int rep = 0; rep < std::max(1, test_repeat); rep++) {
				{
					SDL_GPUStorageBufferReadWriteBinding bw[3] = {};
					bw[0].buffer = m_owner; bw[1].buffer = m_depth; bw[2].buffer = m_shade;
					SDL_GPUComputePass *c = SDL_BeginGPUComputePass(cb, nullptr, 0, bw, 3);
					SDL_BindGPUComputePipeline(c, m_clear);
					const uint32_t k[4] = { off, len, 0, 0 };
					SDL_PushGPUComputeUniformData(cb, 0, k, sizeof k);
					dispatch1(c, len);
					SDL_EndGPUComputePass(c);
				}
				if (rows) {
					SDL_GPUStorageBufferReadWriteBinding bw = {}; bw.buffer = m_sbuf;
					SDL_GPUComputePass *c = SDL_BeginGPUComputePass(cb, nullptr, 0, &bw, 1);
					SDL_BindGPUComputePipeline(c, m_rows);
					SDL_GPUBuffer *ro[3] = { m_pbuf, m_jp, m_jb };
					SDL_BindGPUComputeStorageBuffers(c, 0, ro, 3);
					const uint32_t k[4] = { rows, uint32_t(m_jobpoly.size()), uint32_t(nspans), 0 };
					SDL_PushGPUComputeUniformData(cb, 0, k, sizeof k);
					dispatch1(c, rows);
					SDL_EndGPUComputePass(c);
				}
				{
					SDL_GPUStorageBufferReadWriteBinding bw[2] = {};
					bw[0].buffer = m_owner; bw[1].buffer = m_depth;
					SDL_GPUComputePass *c = SDL_BeginGPUComputePass(cb, nullptr, 0, bw, 2);
					SDL_BindGPUComputePipeline(c, m_solid);
					SDL_GPUBuffer *ro[2] = { m_pbuf, m_sbuf };
					SDL_BindGPUComputeStorageBuffers(c, 0, ro, 2);
					const uint32_t k[4] = { ns, W, 0, 0 };
					SDL_PushGPUComputeUniformData(cb, 0, k, sizeof k);
					dispatch1(c, ns * 8);
					SDL_EndGPUComputePass(c);
				}
				if (any_shadow) {
					SDL_GPUStorageBufferReadWriteBinding bw = {}; bw.buffer = m_shade;
					SDL_GPUComputePass *c = SDL_BeginGPUComputePass(cb, nullptr, 0, &bw, 1);
					SDL_BindGPUComputePipeline(c, m_shadow);
					SDL_GPUBuffer *ro[4] = { m_pbuf, m_sbuf, m_owner, m_depth };
					SDL_BindGPUComputeStorageBuffers(c, 0, ro, 4);
					const uint32_t k[4] = { ns, W, 0, 0 };
					SDL_PushGPUComputeUniformData(cb, 0, k, sizeof k);
					dispatch1(c, ns * 8);   // (8 threads a span, gpu3d.hlsl's L)
					SDL_EndGPUComputePass(c);
				}
				if (!to_target) {
					SDL_GPUStorageBufferReadWriteBinding bw[2] = {};
					bw[0].buffer = m_res; bw[1].buffer = m_kind;
					SDL_GPUComputePass *c = SDL_BeginGPUComputePass(cb, nullptr, 0, bw, 2);
					SDL_BindGPUComputePipeline(c, m_pixels);
					SDL_GPUBuffer *ro[3] = { m_pbuf, m_owner, m_shade };
					SDL_BindGPUComputeStorageBuffers(c, 0, ro, 3);
					const uint32_t k[4] = { W, uint32_t(y0), rows_n, 0 };
					SDL_PushGPUComputeUniformData(cb, 0, k, sizeof k);
					SDL_DispatchGPUCompute(c, (words + 7) / 8, (rows_n + 7) / 8, 1);
					SDL_EndGPUComputePass(c);
				}
			}
		} else {
			cb = SDL_AcquireGPUCommandBuffer(m_dev);
			if (!cb) return false;
		}
		if (to_target) {   // (none: the target all clear)
			SDL_GPUStorageTextureReadWriteBinding tw = {};
			tw.texture = m_tg[target];
			SDL_GPUComputePass *c = SDL_BeginGPUComputePass(cb, &tw, 1, nullptr, 0);
			SDL_BindGPUComputePipeline(c, m_target);
			SDL_GPUBuffer *ro[3] = { m_pbuf, m_owner, m_shade };
			SDL_BindGPUComputeStorageBuffers(c, 0, ro, 3);
			const uint32_t k[4] = { uint32_t(w), uint32_t(h), none ? 1u : uint32_t(y0), none ? 0u : uint32_t(y1) };
			SDL_PushGPUComputeUniformData(cb, 0, k, sizeof k);
			SDL_DispatchGPUCompute(c, (uint32_t(w) + 7) / 8, (uint32_t(h) + 7) / 8, 1);
			SDL_EndGPUComputePass(c);
			m_fence[target] = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
			if (!m_fence[target]) { fprintf(stderr, "gpu3d sdl_gpu: submit: %s\n", SDL_GetError()); return false; }
			return true;
		}
		{
			SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
			SDL_GPUBufferRegion r = { m_res, 0, resb };
			SDL_GPUTransferBufferLocation l = { m_down, 0 };
			SDL_DownloadFromGPUBuffer(cp, &r, &l);
			r = { m_kind, 0, kindb }; l.offset = resb;
			SDL_DownloadFromGPUBuffer(cp, &r, &l);
			SDL_EndGPUCopyPass(cp);
		}
		SDL_GPUFence *fe = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
		if (!fe) { fprintf(stderr, "gpu3d sdl_gpu: submit: %s\n", SDL_GetError()); return false; }
		const bool ok = SDL_WaitForGPUFences(m_dev, true, &fe, 1);
		SDL_ReleaseGPUFence(m_dev, fe);
		if (!ok) { fprintf(stderr, "gpu3d sdl_gpu: a picture failed (%s)\n", SDL_GetError()); return false; }
		if (gpu_ms) *gpu_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
		// the results into the picture, on every core
		const uint8_t *d = (const uint8_t *)SDL_MapGPUTransferBuffer(m_dev, m_down, false);
		if (!d) return false;
		const uint32_t *res = (const uint32_t *)d;
		const uint32_t *kind = (const uint32_t *)(d + resb);
		parallel_rows(int(rows_n), [&](int a, int b) {
			for (int r = a; r < b; r++) {
				uint32_t *p = px + size_t(y0 + r) * w;
				const uint32_t *rr = res + size_t(r) * w, *kr = kind + size_t(r) * words;
				for (uint32_t wd = 0; wd < words; wd++) {
					uint32_t k = kr[wd];
					if (!k) continue;
					for (uint32_t i = 0; k; i++, k >>= 8) {
						const uint32_t kd = k & 0xff, x = wd * 4 + i;
						if (kd == 1) p[x] = rr[x];
						else if (kd == 2) {
							const uint32_t c = p[x], hh = rr[x];
							p[x] = c == KEY ? (SHADOW_KEY | hh) : (MOIRE_MARK | (((c >> 1) & 0x7f7f7fu) + hh));
						}
					}
				}
			}
		});
		SDL_UnmapGPUTransferBuffer(m_dev, m_down);
		return true;
	}

	bool grow(SDL_GPUBuffer *&b, uint32_t &have, uint32_t n, bool rw)
	{
		n = std::max<uint32_t>(n, 16);
		if (b && have >= n) return true;
		if (b) SDL_ReleaseGPUBuffer(m_dev, b);
		SDL_GPUBufferCreateInfo bi = {};
		bi.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | (rw ? SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE : 0);
		bi.size = have = n + n / 2;
		b = SDL_CreateGPUBuffer(m_dev, &bi);
		if (!b) fprintf(stderr, "gpu3d sdl_gpu: a buffer of %u bytes: %s\n", have, SDL_GetError());
		return b != nullptr;
	}
	bool transfer(SDL_GPUTransferBuffer *&t, uint32_t &have, uint32_t n, SDL_GPUTransferBufferUsage usage)
	{
		n = std::max<uint32_t>(n, 16);
		if (t && have >= n) return true;
		if (t) SDL_ReleaseGPUTransferBuffer(m_dev, t);
		SDL_GPUTransferBufferCreateInfo ti = {};
		ti.usage = usage;
		ti.size = have = n + n / 2;
		t = SDL_CreateGPUTransferBuffer(m_dev, &ti);
		if (!t) fprintf(stderr, "gpu3d sdl_gpu: a transfer buffer of %u bytes: %s\n", have, SDL_GetError());
		return t != nullptr;
	}
	void wait_targets()
	{
		for (SDL_GPUFence *&f : m_fence)
			if (f) { SDL_WaitForGPUFences(m_dev, true, &f, 1); SDL_ReleaseGPUFence(m_dev, f); f = nullptr; }
	}
	static void dispatch1(SDL_GPUComputePass *c, uint32_t n)
	{
		if (!n) return;
		const uint32_t groups = (n + 63) / 64;
		SDL_DispatchGPUCompute(c, std::min<uint32_t>(groups, 65535), (groups + 65534) / 65535, 1);
	}

	SDL_GPUDevice *m_dev;
	bool m_own;
	char m_name[64] = "sdl_gpu";
	bool m_software = false;
	SDL_GPUComputePipeline *m_clear = nullptr, *m_rows = nullptr, *m_solid = nullptr, *m_shadow = nullptr, *m_pixels = nullptr, *m_target = nullptr;
	std::vector<SDL_GPUTexture *> m_tg;
	std::vector<SDL_GPUFence *> m_fence;   // each target's last picture
	int m_tw = 0, m_th = 0;
	uint64_t m_made = 0;
	SDL_GPUBuffer *m_pbuf = nullptr, *m_sbuf = nullptr, *m_jp = nullptr, *m_jb = nullptr, *m_owner = nullptr, *m_depth = nullptr, *m_shade = nullptr, *m_res = nullptr, *m_kind = nullptr;
	uint32_t m_pbuf_n = 0, m_sbuf_n = 0, m_jp_n = 0, m_jb_n = 0, m_owner_n = 0, m_depth_n = 0, m_shade_n = 0, m_res_n = 0, m_kind_n = 0;
	SDL_GPUTransferBuffer *m_up = nullptr, *m_down = nullptr;
	uint32_t m_up_n = 0, m_down_n = 0;
	std::vector<uint32_t> m_jobpoly, m_jobbase;
};

}

std::unique_ptr<gpu3d> gpu3d_create_sdlgpu(const char *driver)
{
	// (GPU3D_VALIDATE=1: the driver's validation, Vulkan's layers)
	SDL_GPUDevice *dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, getenv("GPU3D_VALIDATE") != nullptr, driver);
	if (!dev) { fprintf(stderr, "gpu3d sdl_gpu: no device: %s\n", SDL_GetError()); return nullptr; }
	auto g = std::make_unique<gpu3d_sdlgpu>(dev, true);
	if (!g->init()) return nullptr;
	return g;
}

std::unique_ptr<gpu3d> gpu3d_create_sdlgpu_on(void *sdl_gpu_device)
{
	if (!sdl_gpu_device) return nullptr;
	auto g = std::make_unique<gpu3d_sdlgpu>(static_cast<SDL_GPUDevice *>(sdl_gpu_device), false);
	if (!g->init()) return nullptr;
	return g;
}

// (Windows' headers last, after everything above: their macros touch nothing here)
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#endif
bool gpu3d_prefer_vulkan()
{
	bool card = false;
#ifdef _WIN32
	// (DXGI's adapters, the software one skipped: any other, any vendor)
	using create_t = HRESULT(WINAPI *)(REFIID, void **);
	HMODULE dxgi = LoadLibraryA("dxgi.dll");
	auto create = dxgi ? reinterpret_cast<create_t>(reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
	IDXGIFactory1 *factory = nullptr;
	if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) && factory) {
		IDXGIAdapter1 *a = nullptr;
		for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
			DXGI_ADAPTER_DESC1 d;
			if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414) card = true;   // (0x1414: Microsoft's own, Basic Render)
			a->Release();
		}
		factory->Release();
	}
	if (dxgi) FreeLibrary(dxgi);
#endif
	return card;
}
