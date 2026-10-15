// The 3D on the GPU through SDL_GPU (src/gpu/gpu3d.h): Direct3D 12 on Windows, Vulkan on Linux and
// the Steam Deck (and Windows without Direct3D 12), on a device of its own (the host's renderer
// stays SDL's usual one). gpu3d_metal.mm's passes, its shaders src/gpu/gpu3d.hlsl (precompiled into
// gpu3d_shaders.h by tools/gpu/shaders.sh) with an x86-64 build's sums. The picture isn't the GPU's
// memory here: the polygons and spans go up, each pixel's result comes back (res, and a byte a
// pixel saying what it is) and is put into the picture on every core, the same pixels as Metal's.
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
		for (SDL_GPUComputePipeline *p : { m_clear, m_rows, m_solid, m_shadow, m_pixels }) if (p) SDL_ReleaseGPUComputePipeline(m_dev, p);
		for (SDL_GPUBuffer *b : { m_pbuf, m_sbuf, m_jp, m_jb, m_owner, m_depth, m_shade, m_res, m_kind }) if (b) SDL_ReleaseGPUBuffer(m_dev, b);
		if (m_up) SDL_ReleaseGPUTransferBuffer(m_dev, m_up);
		if (m_down) SDL_ReleaseGPUTransferBuffer(m_dev, m_down);
		if (m_own) SDL_DestroyGPUDevice(m_dev);
	}
	bool init()
	{
		const auto t0 = clk::now();
		const SDL_GPUShaderFormat fm = SDL_GetGPUShaderFormats(m_dev);
		const bool spirv = (fm & SDL_GPU_SHADERFORMAT_SPIRV) != 0, dxil = (fm & SDL_GPU_SHADERFORMAT_DXIL) != 0;
		if (!spirv && !dxil) { fprintf(stderr, "gpu3d sdl_gpu: %s takes neither SPIR-V nor DXIL\n", SDL_GetGPUDeviceDriver(m_dev)); return false; }
		auto make = [&](const uint8_t *spv, size_t spv_n, const uint8_t *dx, size_t dx_n, const char *name,
			uint32_t ro, uint32_t rwb, uint32_t tx, uint32_t ty) -> SDL_GPUComputePipeline * {
			SDL_GPUComputePipelineCreateInfo ci = {};
			ci.code = spirv ? spv : dx;
			ci.code_size = spirv ? spv_n : dx_n;
			ci.format = spirv ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_DXIL;
			ci.entrypoint = "main";
			ci.num_readonly_storage_buffers = ro;
			ci.num_readwrite_storage_buffers = rwb;
			ci.num_uniform_buffers = 1;
			ci.threadcount_x = tx; ci.threadcount_y = ty; ci.threadcount_z = 1;
			SDL_GPUComputePipeline *p = SDL_CreateGPUComputePipeline(m_dev, &ci);
			if (!p) fprintf(stderr, "gpu3d sdl_gpu: %s: %s\n", name, SDL_GetError());
			return p;
		};
#define K(n) gpu3d_##n##_spv, sizeof gpu3d_##n##_spv, gpu3d_##n##_dxil, sizeof gpu3d_##n##_dxil, #n
		m_clear = make(K(clear), 0, 3, 64, 1);
		m_rows = make(K(rows), 3, 1, 64, 1);
		m_solid = make(K(solid), 2, 2, 64, 1);
		m_shadow = make(K(shadow), 4, 1, 64, 1);
		m_pixels = make(K(pixels), 3, 2, 8, 8);
#undef K
		if (!m_clear || !m_rows || !m_solid || !m_shadow || !m_pixels) return false;
		const char *dn = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(m_dev), SDL_PROP_GPU_DEVICE_NAME_STRING, "?");
		snprintf(m_name, sizeof m_name, "%s (%s)", SDL_GetGPUDeviceDriver(m_dev), spirv ? "SPIR-V" : "DXIL");
		fprintf(stderr, "gpu3d sdl_gpu: %s on %s, its pipelines ready in %.0f ms\n", m_name, dn,
				std::chrono::duration<double, std::milli>(clk::now() - t0).count());
		return true;
	}
	const char *name() const override { return m_name; }

	bool draw(const gpu3d_poly *polys, size_t count, const gpu3d_span *spans, size_t nspans, size_t rowspans,
		uint32_t *px, size_t bytes, int w, int h, int y0, int y1, double *gpu_ms) override
	{
		if (gpu_ms) *gpu_ms = 0;
		y0 = std::max(y0, 0); y1 = std::min(y1, h - 1);
		if (!count || y1 < y0) return true;
		if (w <= 0 || h <= 0 || bytes < size_t(w) * h * 4) return false;
		const uint32_t npx = uint32_t(w) * h, rows_n = uint32_t(y1 - y0 + 1), words = (uint32_t(w) + 3) / 4;
		const uint32_t off = uint32_t(y0) * w, len = rows_n * uint32_t(w);
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
		const uint32_t pb = uint32_t(count * sizeof(gpu3d_poly)), sb = uint32_t(std::max<size_t>(ns, 1) * sizeof(gpu3d_span));
		const uint32_t cs = uint32_t(nspans * sizeof(gpu3d_span)), jb = uint32_t(std::max<size_t>(m_jobpoly.size(), 1) * 4);
		const uint32_t resb = len * 4, kindb = rows_n * words * 4;
		if (!grow(m_pbuf, m_pbuf_n, pb, false) || !grow(m_sbuf, m_sbuf_n, sb, true) || !grow(m_jp, m_jp_n, jb, false) || !grow(m_jb, m_jb_n, jb, false)
			|| !grow(m_owner, m_owner_n, npx * 4, true) || !grow(m_depth, m_depth_n, npx * 4, true) || !grow(m_shade, m_shade_n, npx * 4, true)
			|| !grow(m_res, m_res_n, resb, true) || !grow(m_kind, m_kind_n, kindb, true)
			|| !transfer(m_up, m_up_n, pb + cs + jb * 2, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD)
			|| !transfer(m_down, m_down_n, resb + kindb, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD))
			return false;
		uint8_t *map = (uint8_t *)SDL_MapGPUTransferBuffer(m_dev, m_up, true);
		if (!map) return false;
		memcpy(map, polys, pb);
		if (cs) memcpy(map + pb, spans, cs);
		if (!m_jobpoly.empty()) { memcpy(map + pb + cs, m_jobpoly.data(), m_jobpoly.size() * 4); memcpy(map + pb + cs + jb, m_jobbase.data(), m_jobbase.size() * 4); }
		SDL_UnmapGPUTransferBuffer(m_dev, m_up);
		const auto t0 = clk::now();
		SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(m_dev);
		if (!cb) return false;
		{
			SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
			auto up = [&](SDL_GPUBuffer *b, uint32_t at, uint32_t n) {
				if (!n) return;
				SDL_GPUTransferBufferLocation l = { m_up, at };
				SDL_GPUBufferRegion r = { b, 0, n };
				SDL_UploadToGPUBuffer(cp, &l, &r, false);
			};
			up(m_pbuf, 0, pb); up(m_sbuf, pb, cs);
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
				dispatch1(c, ns);
				SDL_EndGPUComputePass(c);
			}
			{
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

private:
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
	static void dispatch1(SDL_GPUComputePass *c, uint32_t n)
	{
		if (!n) return;
		const uint32_t groups = (n + 63) / 64;
		SDL_DispatchGPUCompute(c, std::min<uint32_t>(groups, 65535), (groups + 65534) / 65535, 1);
	}

	SDL_GPUDevice *m_dev;
	bool m_own;
	char m_name[64] = "sdl_gpu";
	SDL_GPUComputePipeline *m_clear = nullptr, *m_rows = nullptr, *m_solid = nullptr, *m_shadow = nullptr, *m_pixels = nullptr;
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
	SDL_GPUDevice *dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, getenv("GPU3D_VALIDATE") != nullptr, driver);
	if (!dev) { fprintf(stderr, "gpu3d sdl_gpu: no device: %s\n", SDL_GetError()); return nullptr; }
	auto g = std::make_unique<gpu3d_sdlgpu>(dev, true);
	if (!g->init()) return nullptr;
	return g;
}
