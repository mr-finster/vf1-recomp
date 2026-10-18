// The 3D on the GPU, Metal (src/gpu/gpu3d.h). The rows' spans above 1x are model1::fill_quad_hd's
// (ours), worked out a thread a row; the pixels' rule is the rasterizer's (model1_video.cpp:
// draw_hline, draw_hline_moired and fill_quad_hd's model shadow).
#include "gpu3d.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

const char *const kShaders = R"(
#include <metal_stdlib>
using namespace metal;
struct poly {
	float x[4], y[4];
	int cx0, cx1, cy0, cy1;
	int y0, rows;
	uint span0, colour, flags, batch;
};
struct span { uint poly; int y, xa, xb; };
constant uint SHADOW = 1, CHECKER = 2, MODEL_SHADOW = 4, SCENERY = 8, LINE = 32;
constant uint L = 8;   // the threads a span (each every L-th pixel of it: neighbours write neighbours)
constant uint KEY = 0xff000000u, SHADOW_KEY = 0xfd000000u, MOIRE_MARK = 0x01000000u;

// ---- above 1x: a polygon's row, as fill_quad_hd works it out (two spans at most) ----
kernel void k_rows(uint t [[thread_position_in_grid]], constant uint &total [[buffer(0)]],
	const device poly *polys [[buffer(1)]], const device uint *jobpoly [[buffer(2)]], const device uint *jobbase [[buffer(3)]],
	constant uint2 &jb [[buffer(4)]], device span *spans [[buffer(5)]])
{
	if (t >= total) return;
	uint lo = 0, hi = jb.x - 1;   // jb: the jobs, the rows' spans' first place
	while (lo < hi) { const uint mid = (lo + hi + 1) >> 1; if (jobbase[mid] <= t) lo = mid; else hi = mid - 1; }
	const uint idx = jobpoly[lo];
	const device poly &q = polys[idx];
	const uint r = t - jobbase[lo];
	const int y = q.y0 + int(r);
	device span *out = spans + jb.y + q.span0 + 2 * r;
	out[0] = span{ idx, y, 1, 0 };
	out[1] = span{ idx, y, 1, 0 };
	const float yc = float(y) + 0.5f;
	float xs[4];
	int n = 0;
	for (int i = 0; i < 4; i++) {
		const int j = (i + 1) & 3;
		float ax = q.x[i], ay = q.y[i], bx = q.x[j], by = q.y[j];
		if (ay > by || (ay == by && ax > bx)) { float tt = ax; ax = bx; bx = tt; tt = ay; ay = by; by = tt; }
		if (ay == by) continue;
		const float dx = bx - ax, dy = by - ay;
		if (yc < ay || yc >= by) continue;
		xs[n++] = ax + precise::divide((yc - ay) * dx, dy);
	}
	if (n < 2) return;
	for (int a = 1; a < n; a++)
		for (int b = a; b > 0 && xs[b] < xs[b - 1]; b--) { const float tt = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = tt; }
	for (int k = 0; k + 1 < n; k += 2) {
		const int xa = max(q.cx0, int(ceil(xs[k] - 0.5f)));
		const int xb = min(q.cx1, int(ceil(xs[k + 1] - 0.5f)) - 1);
		out[k >> 1] = span{ idx, y, xa, xb };
	}
}

// ---- each pixel's last solid polygon (its place + 1; 0 none) ----
kernel void k_solid(uint t [[thread_position_in_grid]], constant uint &total [[buffer(0)]],
	const device poly *polys [[buffer(1)]], const device span *spans [[buffer(5)]],
	device atomic_uint *owner [[buffer(6)]], constant uint &W [[buffer(7)]], device atomic_uint *depth [[buffer(10)]])
{
	if (t / L >= total) return;
	const int lane = int(t % L);
	const span s = spans[t / L];
	if (s.xa > s.xb) return;
	const uint f = polys[s.poly].flags;
	if (f & SHADOW) return;
	device atomic_uint *row = owner + uint(s.y) * W;
	const uint v = s.poly + 1;
	if (!(f & (CHECKER | LINE))) {   // (the polygons a model's shadow looks under: model1::s_owner's)
		device atomic_uint *drow = depth + uint(s.y) * W;
		for (int x = s.xa + lane; x <= s.xb; x += int(L)) { atomic_fetch_max_explicit(&row[x], v, memory_order_relaxed); atomic_fetch_max_explicit(&drow[x], v, memory_order_relaxed); }
		return;
	}
	if (f & CHECKER) {
		for (int x = s.xa + ((s.xa ^ s.y) & 1) + 2 * lane; x <= s.xb; x += 2 * int(L)) atomic_fetch_max_explicit(&row[x], v, memory_order_relaxed);
	} else
		for (int x = s.xa + lane; x <= s.xb; x += int(L)) atomic_fetch_max_explicit(&row[x], v, memory_order_relaxed);
}

// ---- each pixel's first shadow after that (its place; ~0 none) ----
kernel void k_shadow(uint t [[thread_position_in_grid]], constant uint &total [[buffer(0)]],
	const device poly *polys [[buffer(1)]], const device span *spans [[buffer(5)]],
	const device uint *owner [[buffer(6)]], constant uint &W [[buffer(7)]], device atomic_uint *shade [[buffer(8)]],
	const device uint *depth [[buffer(10)]])
{
	if (t >= total) return;
	const span s = spans[t];
	if (s.xa > s.xb) return;
	const device poly &q = polys[s.poly];
	if (!(q.flags & SHADOW)) return;
	const bool mshadow = (q.flags & MODEL_SHADOW) != 0;
	const uint at = uint(s.y) * W;
	for (int x = s.xa; x <= s.xb; x++) {
		const uint o = owner[at + uint(x)];
		if (o > s.poly) continue;                 // a solid polygon (or line) drawn after it
		const uint d = mshadow ? depth[at + uint(x)] : 0u;
		if (d) {                                  // a model's shadow: not over a model's polygon in its own list
			const device poly &u = polys[d - 1];
			if (u.batch == q.batch && !(u.flags & SCENERY)) continue;
		}
		atomic_fetch_min_explicit(&shade[at + uint(x)], s.poly, memory_order_relaxed);
	}
}

// ---- the pixels: the solid colour, or the picture beneath; a shadow's half over it ----
kernel void k_pixels(uint2 g [[thread_position_in_grid]], constant uint4 &k [[buffer(0)]],
	const device poly *polys [[buffer(1)]], const device uint *owner [[buffer(6)]], const device uint *shade [[buffer(8)]],
	device uint *px [[buffer(9)]])
{
	// k: W, the first row, rows
	if (g.x >= k.x || g.y >= k.z) return;
	const uint at = (g.y + k.y) * k.x + g.x;
	const uint o = owner[at], s = shade[at];
	if (!o && s == 0xffffffffu) return;
	uint c = o ? polys[o - 1].colour : px[at];
	if (s != 0xffffffffu) {
		const uint h = (polys[s].colour >> 1) & 0x7f7f7fu;
		c = (!o && c == KEY) ? (SHADOW_KEY | h) : (MOIRE_MARK | (((c >> 1) & 0x7f7f7fu) + h));
	}
	px[at] = c;
}
)";

struct mpoly { float x[4], y[4]; int32_t cx0, cx1, cy0, cy1, y0, rows; uint32_t span0, colour, flags, batch; };
static_assert(sizeof(mpoly) == sizeof(gpu3d_poly), "gpu3d_poly is the shader's poly");

class gpu3d_metal final : public gpu3d
{
public:
	explicit gpu3d_metal(id<MTLDevice> dev) : m_dev(dev) {}
	bool init()
	{
		const auto t0 = std::chrono::steady_clock::now();
		NSError *err = nil;
		MTLCompileOptions *co = [MTLCompileOptions new];
		// no fast math: the CPU's float arithmetic as it is
		if (@available(macOS 15.0, *)) co.mathMode = MTLMathModeSafe;
		else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			co.fastMathEnabled = NO;
#pragma clang diagnostic pop
		}
		std::string src = kShaders;
		if (getenv("GPU3D_COLD")) src += "// " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "\n";   // (tests: not from Metal's cache)
		id<MTLLibrary> lib = [m_dev newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:co error:&err];
		if (!lib) { fprintf(stderr, "gpu3d metal: shaders: %s\n", err ? err.localizedDescription.UTF8String : "?"); return false; }
		auto kernel = [&](NSString *name) -> id<MTLComputePipelineState> {
			id<MTLFunction> f = [lib newFunctionWithName:name];
			id<MTLComputePipelineState> p = f ? [m_dev newComputePipelineStateWithFunction:f error:&err] : nil;
			if (!p) fprintf(stderr, "gpu3d metal: %s: %s\n", name.UTF8String, err ? err.localizedDescription.UTF8String : "?");
			return p;
		};
		m_k_rows = kernel(@"k_rows"); m_k_solid = kernel(@"k_solid"); m_k_shadow = kernel(@"k_shadow"); m_k_pixels = kernel(@"k_pixels");
		if (!m_k_rows || !m_k_solid || !m_k_shadow || !m_k_pixels) return false;
		m_queue = [m_dev newCommandQueue];
		fprintf(stderr, "gpu3d metal: %s, its shaders ready in %.0f ms\n", m_dev.name.UTF8String,
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
		return m_queue != nil;
	}
	const char *name() const override { return "metal"; }

	bool draw(const gpu3d_poly *polys, size_t count, const gpu3d_span *spans, size_t nspans, size_t rowspans,
		uint32_t *px, size_t bytes, int w, int h, int y0, int y1, double *gpu_ms) override
	{
		@autoreleasepool {
		if (gpu_ms) *gpu_ms = 0;
		y0 = std::max(y0, 0); y1 = std::min(y1, h - 1);
		if (!count || y1 < y0) return true;
		if (w <= 0 || h <= 0 || (uintptr_t(px) & 16383) || (bytes & 16383) || bytes < size_t(w) * h * 4) return false;
		// the picture's own pages, as a buffer (made again when they move)
		if (px != m_px || bytes != m_px_bytes) {
			m_pxbuf = [m_dev newBufferWithBytesNoCopy:px length:bytes options:MTLResourceStorageModeShared deallocator:nil];
			if (!m_pxbuf) return false;
			m_px = px; m_px_bytes = bytes;
		}
		const size_t npx = size_t(w) * h;
		if (!m_owner || m_owner.length < npx * 4) {
			m_owner = [m_dev newBufferWithLength:npx * 4 options:MTLResourceStorageModePrivate];
			m_shade = [m_dev newBufferWithLength:npx * 4 options:MTLResourceStorageModePrivate];
			m_depth = [m_dev newBufferWithLength:npx * 4 options:MTLResourceStorageModePrivate];
			if (!m_owner || !m_shade || !m_depth) { m_owner = m_shade = m_depth = nil; return false; }
		}
		// the rows worked out here: a job a polygon, its rows' first place among them all
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
		auto put = [&](id<MTLBuffer> __strong &b, const void *src, size_t n) {
			n = std::max<size_t>(n, 16);
			if (!b || b.length < n) b = [m_dev newBufferWithLength:n * 3 / 2 options:MTLResourceStorageModeShared];
			if (src) memcpy(b.contents, src, n);
			return b != nil;
		};
		if (!put(m_pbuf, polys, count * sizeof(gpu3d_poly)) || !put(m_sbuf, nullptr, (nspans + rowspans) * sizeof(gpu3d_span))
			|| !put(m_jp, m_jobpoly.data(), m_jobpoly.size() * 4) || !put(m_jb, m_jobbase.data(), m_jobbase.size() * 4)) return false;
		if (spans) memcpy(m_sbuf.contents, spans, nspans * sizeof(gpu3d_span));
		const auto t0 = std::chrono::steady_clock::now();
		id<MTLCommandBuffer> cb = [m_queue commandBuffer];
		const uint32_t W = uint32_t(w), ns = uint32_t(nspans + rowspans), jb[2] = { uint32_t(m_jobpoly.size()), uint32_t(nspans) };
		const size_t off = size_t(y0) * w * 4, len = size_t(y1 - y0 + 1) * w * 4;
		for (int rep = 0; rep < std::max(1, test_repeat); rep++) {
			id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
			[bl fillBuffer:m_owner range:NSMakeRange(off, len) value:0];
			[bl fillBuffer:m_depth range:NSMakeRange(off, len) value:0];
			if (any_shadow) [bl fillBuffer:m_shade range:NSMakeRange(off, len) value:0xff];
			[bl endEncoding];
			id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
			[ce setBuffer:m_pbuf offset:0 atIndex:1];
			[ce setBuffer:m_sbuf offset:0 atIndex:5];
			[ce setBuffer:m_owner offset:0 atIndex:6];
			[ce setBytes:&W length:4 atIndex:7];
			[ce setBuffer:m_shade offset:0 atIndex:8];
			[ce setBuffer:m_pxbuf offset:0 atIndex:9];
			[ce setBuffer:m_depth offset:0 atIndex:10];
			if (rows) {
				[ce setComputePipelineState:m_k_rows];
				[ce setBytes:&rows length:4 atIndex:0];
				[ce setBuffer:m_jp offset:0 atIndex:2];
				[ce setBuffer:m_jb offset:0 atIndex:3];
				[ce setBytes:jb length:8 atIndex:4];
				dispatch1(ce, m_k_rows, rows);
				[ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
			}
			[ce setComputePipelineState:m_k_solid];
			[ce setBytes:&ns length:4 atIndex:0];
			dispatch1(ce, m_k_solid, ns * 8);
			[ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
			if (any_shadow) {
				[ce setComputePipelineState:m_k_shadow];
				dispatch1(ce, m_k_shadow, ns);
				[ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
			}
			if (rep + 1 == std::max(1, test_repeat)) {   // (a slow GPU's repeats leave the picture alone)
				if (!any_shadow) {
					[ce endEncoding];
					bl = [cb blitCommandEncoder];
					[bl fillBuffer:m_shade range:NSMakeRange(off, len) value:0xff];
					[bl endEncoding];
					ce = [cb computeCommandEncoder];
					[ce setBuffer:m_pbuf offset:0 atIndex:1];
					[ce setBuffer:m_owner offset:0 atIndex:6];
					[ce setBuffer:m_shade offset:0 atIndex:8];
					[ce setBuffer:m_pxbuf offset:0 atIndex:9];
				}
				const uint32_t k[4] = { W, uint32_t(y0), uint32_t(y1 - y0 + 1), 0 };
				[ce setComputePipelineState:m_k_pixels];
				[ce setBytes:k length:sizeof k atIndex:0];
				const NSUInteger tw = m_k_pixels.threadExecutionWidth, th = std::max<NSUInteger>(1, m_k_pixels.maxTotalThreadsPerThreadgroup / tw);
				[ce dispatchThreadgroups:MTLSizeMake((W + tw - 1) / tw, (k[2] + th - 1) / th, 1) threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
			}
			[ce endEncoding];
		}
		[cb commit];
		[cb waitUntilCompleted];
		if (cb.status != MTLCommandBufferStatusCompleted) {
			fprintf(stderr, "gpu3d metal: a picture failed (%s)\n", cb.error ? cb.error.localizedDescription.UTF8String : "?");
			return false;
		}
		(void)t0;
		if (gpu_ms) *gpu_ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
		return true;
		}
	}

private:
	static void dispatch1(id<MTLComputeCommandEncoder> ce, id<MTLComputePipelineState> p, uint32_t n)
	{
		if (!n) return;
		const NSUInteger tg = std::min<NSUInteger>(64, p.maxTotalThreadsPerThreadgroup);
		[ce dispatchThreadgroups:MTLSizeMake((n + tg - 1) / tg, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
	}

	id<MTLDevice> m_dev;
	id<MTLCommandQueue> m_queue = nil;
	id<MTLComputePipelineState> m_k_rows = nil, m_k_solid = nil, m_k_shadow = nil, m_k_pixels = nil;
	id<MTLBuffer> m_pxbuf = nil, m_owner = nil, m_shade = nil, m_depth = nil, m_pbuf = nil, m_sbuf = nil, m_jp = nil, m_jb = nil;
	void *m_px = nullptr;
	size_t m_px_bytes = 0;
	std::vector<uint32_t> m_jobpoly, m_jobbase;
};

}

std::unique_ptr<gpu3d> gpu3d_create_metal(void *mtl_device)
{
	@autoreleasepool {
	id<MTLDevice> dev = mtl_device ? (__bridge id<MTLDevice>)mtl_device : MTLCreateSystemDefaultDevice();
	if (!dev) return nullptr;
	auto g = std::make_unique<gpu3d_metal>(dev);
	if (!g->init()) return nullptr;
	return g;
	}
}

// The start's head start: the system's device's backend made (its shaders compiled from source the
// first time a build runs, cached by Metal after) on another thread while the game boots, taken by
// gpu3d_create_metal_layer if the host's renderer draws on that device.
namespace {
std::mutex g_pre_mutex;
std::condition_variable g_pre_cv;
bool g_pre_started = false, g_pre_done = false;
std::unique_ptr<gpu3d> g_pre;
id<MTLDevice> g_pre_dev = nil;
}

void gpu3d_metal_prewarm()
{
	{
		std::lock_guard<std::mutex> l(g_pre_mutex);
		if (g_pre_started) return;
		g_pre_started = true;
	}
	std::thread([] {
		@autoreleasepool {
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		std::unique_ptr<gpu3d> g = dev ? gpu3d_create_metal((__bridge void *)dev) : nullptr;
		std::lock_guard<std::mutex> l(g_pre_mutex);
		g_pre = std::move(g);
		g_pre_dev = dev;
		g_pre_done = true;
		g_pre_cv.notify_all();
		}
	}).detach();
}

std::unique_ptr<gpu3d> gpu3d_create_metal_layer(void *ca_metal_layer)
{
	@autoreleasepool {
	CAMetalLayer *layer = (__bridge CAMetalLayer *)ca_metal_layer;
	id<MTLDevice> dev = layer ? layer.device : nil;
	if (!dev) return nullptr;
	{
		std::unique_lock<std::mutex> l(g_pre_mutex);
		if (g_pre_started) {
			g_pre_cv.wait(l, [] { return g_pre_done; });
			if (g_pre && g_pre_dev == dev) return std::move(g_pre);
			g_pre.reset();
		}
	}
	return gpu3d_create_metal((__bridge void *)dev);
	}
}
