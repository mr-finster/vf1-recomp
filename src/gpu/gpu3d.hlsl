// The 3D on the GPU, SDL_GPU's compute (src/gpu/gpu3d_sdlgpu.cpp): Vulkan (SPIR-V) and Direct3D 12
// (DXIL), gpu3d_metal.mm's kernels in HLSL. The rows' spans above 1x are model1::fill_quad_hd's
// (ours); the pixels' rule is the rasterizer's (model1_video.cpp: draw_hline, draw_hline_moired and
// fill_quad_hd's model shadow).
//
// The one float sum (an edge's crossing, ax + (yc - ay) * dx / dy) as an x86-64 build has it: each
// step rounded on its own (`precise`), the division correctly rounded (fdiv: a GPU's divide is
// within 2.5 ulp), a float to an int as SSE's cvttss2si.
//
// The picture isn't on the GPU (a discrete card's memory isn't the CPU's): the pixels pass writes
// each pixel's result (res) and what it is (kind, a byte a pixel: 0 untouched, 1 a colour, 2 a
// shadow over the picture beneath, res its half colour), and the CPU puts them into the picture.
// Or, on the host's renderer's own device, the target pass writes the 3D straight into one of its
// textures (gpu3d::draw_target), keyed for the GPU's putting the picture together.
//
// Built once per kernel (-D K_CLEAR, K_ROWS, K_SOLID, K_SHADOW, K_PIXELS, K_TARGET) by tools/gpu/shaders.sh
// into src/gpu/gpu3d_shaders.h; resources in SDL_GPU's layout for compute (read-only storage
// buffers t0.. space 0, read-write u0.. space 1, uniforms b0 space 2).
//
// Every buffer is a byte-address buffer (ByteAddressBuffer, loads and stores by byte offset), not a
// structured buffer: SDL_GPU's Direct3D 12 backend binds storage buffers as raw views (R32_TYPELESS,
// D3D12_BUFFER_*_FLAG_RAW), and a raw view in a structured buffer's slot is undefined there. NVIDIA's
// recent drivers read it anyway; AMD's (a Radeon 780M, vf1-recomp #10) drew nothing. Vulkan has no
// such distinction (SPIR-V's storage buffers are the same either way).
#define LD(b, i) (b).Load((i) * 4u)
#define ST(b, i, v) (b).Store((i) * 4u, (v))

float fdiv(float a, float b)
{
	const uint ua = asuint(a), ub = asuint(b);
	const uint sign = (ua ^ ub) & 0x80000000u;
	int ea = int((ua >> 23) & 0xffu), eb = int((ub >> 23) & 0xffu);
	uint ma = ua & 0x7fffffu, mb = ub & 0x7fffffu;
	if (ea == 255 || eb == 255) {
		if (ea == 255 && ma != 0u) return asfloat(ua | 0x00400000u);
		if (eb == 255 && mb != 0u) return asfloat(ub | 0x00400000u);
		if (ea == 255 && eb == 255) return asfloat(0xffc00000u);
		if (ea == 255) return asfloat(sign | 0x7f800000u);
		return asfloat(sign);
	}
	if (eb == 0 && mb == 0u) {
		if (ea == 0 && ma == 0u) return asfloat(0xffc00000u);
		return asfloat(sign | 0x7f800000u);
	}
	if (ea == 0 && ma == 0u) return asfloat(sign);
	if (ea == 0) { const int sh = 23 - int(firstbithigh(ma)); ma <<= sh; ea = 1 - sh; } else ma |= 0x800000u;
	if (eb == 0) { const int sh = 23 - int(firstbithigh(mb)); mb <<= sh; eb = 1 - sh; } else mb |= 0x800000u;
	int e = ea - eb + 127;
	if (ma < mb) { ma <<= 1; e--; }
	uint q = 0u, r = ma;
	[unroll] for (int i = 0; i < 26; i++) {
		q <<= 1;
		if (r >= mb) { r -= mb; q |= 1u; }
		r <<= 1;
	}
	bool sticky = r != 0u;
	if (e <= 0) {
		const int sh = 1 - e;
		if (sh >= 26) { sticky = sticky || q != 0u; q = 0u; }
		else { sticky = sticky || (q & ((1u << sh) - 1u)) != 0u; q >>= sh; }
		e = 0;
	}
	uint m = q >> 2;
	const bool g = (q & 2u) != 0u, rest = (q & 1u) != 0u || sticky;
	if (g && (rest || (m & 1u) != 0u)) m++;
	if (e == 0) return asfloat(sign | m);
	if (m == 0x1000000u) { m >>= 1; e++; }
	if (e >= 255) return asfloat(sign | 0x7f800000u);
	return asfloat(sign | (uint(e) << 23) | (m & 0x7fffffu));
}
int ftoi(float x) { return abs(x) < 2147483648.0f ? int(x) : int(0x80000000u); }

struct poly {
	float x[4]; float y[4];
	int cx0; int cx1; int cy0; int cy1;
	int y0; int rows;
	uint span0; uint colour; uint flags; uint batch;
};
struct span { uint poly; int y; int xa; int xb; };
#define SHADOW 1u
#define CHECKER 2u
#define MODEL_SHADOW 4u
#define SCENERY 8u
#define LINE 32u
#define L 8u   // the threads a span (each every L-th pixel of it: neighbours write neighbours)
#define KEY 0xff000000u
#define SHADOW_KEY 0xfd000000u
#define MOIRE_MARK 0x01000000u

// ---- the region's owners, depths and shadows cleared ----
#ifdef K_CLEAR
RWByteAddressBuffer owner : register(u0, space1);
RWByteAddressBuffer depth : register(u1, space1);
RWByteAddressBuffer shade : register(u2, space1);
cbuffer k : register(b0, space2) { uint k_off; uint k_n; uint k_pad0; uint k_pad1; };
[numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID)
{
	const uint i = t.x + t.y * 65535u * 64u;
	if (i >= k_n) return;
	ST(owner, k_off + i, 0u); ST(depth, k_off + i, 0u); ST(shade, k_off + i, 0xffffffffu);
}
#endif

// ---- above 1x: a polygon's row, as fill_quad_hd works it out (two spans at most) ----
#ifdef K_ROWS
ByteAddressBuffer polys : register(t0, space0);
ByteAddressBuffer jobpoly : register(t1, space0);
ByteAddressBuffer jobbase : register(t2, space0);
RWByteAddressBuffer spans : register(u0, space1);
cbuffer k : register(b0, space2) { uint k_total; uint k_jobs; uint k_first; uint k_pad0; };
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	const uint t = tid.x + tid.y * 65535u * 64u;
	if (t >= k_total) return;
	uint lo = 0, hi = k_jobs - 1;
	while (lo < hi) { const uint mid = (lo + hi + 1) >> 1; if (LD(jobbase, mid) <= t) lo = mid; else hi = mid - 1; }
	const uint idx = LD(jobpoly, lo);
	const poly q = polys.Load<poly>((idx) * sizeof(poly));
	const uint r = t - LD(jobbase, lo);
	const int y = q.y0 + int(r);
	const uint at = k_first + q.span0 + 2u * r;
	span none; none.poly = idx; none.y = y; none.xa = 1; none.xb = 0;
	spans.Store<span>((at) * sizeof(span), none);
	spans.Store<span>((at + 1) * sizeof(span), none);
	precise const float yc = float(y) + 0.5f;
	float xs[4];
	int n = 0;
	[unroll] for (int i = 0; i < 4; i++) {
		const int j = (i + 1) & 3;
		float ax = q.x[i], ay = q.y[i], bx = q.x[j], by = q.y[j];
		if (ay > by || (ay == by && ax > bx)) { float tt = ax; ax = bx; bx = tt; tt = ay; ay = by; by = tt; }
		if (ay == by) continue;
		precise const float dx = bx - ax, dy = by - ay;
		if (yc < ay || yc >= by) continue;
		precise const float num = (yc - ay) * dx;
		precise const float xc = ax + fdiv(num, dy);
		xs[n++] = xc;
	}
	if (n < 2) return;
	for (int a = 1; a < n; a++)
		for (int b = a; b > 0 && xs[b] < xs[b - 1]; b--) { const float tt = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = tt; }
	for (int k = 0; k + 1 < n; k += 2) {
		precise const float sa = xs[k] - 0.5f, sb = xs[k + 1] - 0.5f;
		span s;
		s.poly = idx; s.y = y;
		s.xa = max(q.cx0, ftoi(ceil(sa)));
		s.xb = min(q.cx1, ftoi(ceil(sb)) - 1);
		spans.Store<span>((at + uint(k >> 1)) * sizeof(span), s);
	}
}
#endif

// ---- each pixel's last solid polygon (its place + 1; 0 none) ----
#ifdef K_SOLID
ByteAddressBuffer polys : register(t0, space0);
ByteAddressBuffer spans : register(t1, space0);
RWByteAddressBuffer owner : register(u0, space1);
RWByteAddressBuffer depth : register(u1, space1);
cbuffer k : register(b0, space2) { uint k_total; uint k_W; uint k_pad0; uint k_pad1; };
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	const uint t = tid.x + tid.y * 65535u * 64u;
	if (t / L >= k_total) return;
	const int lane = int(t % L);
	const span s = spans.Load<span>((t / L) * sizeof(span));
	if (s.xa > s.xb) return;
	const uint f = polys.Load<poly>((s.poly) * sizeof(poly)).flags;
	if (f & SHADOW) return;
	const uint row = uint(s.y) * k_W;
	const uint v = s.poly + 1u;
	uint was;
	if (!(f & (CHECKER | LINE))) {   // (the polygons a model's shadow looks under: model1::s_owner's)
		for (int x = s.xa + lane; x <= s.xb; x += int(L)) { owner.InterlockedMax((row + uint(x)) * 4u, v, was); depth.InterlockedMax((row + uint(x)) * 4u, v, was); }
		return;
	}
	if (f & CHECKER) {
		for (int x2 = s.xa + ((s.xa ^ s.y) & 1) + 2 * lane; x2 <= s.xb; x2 += 2 * int(L)) owner.InterlockedMax((row + uint(x2)) * 4u, v, was);
	} else
		for (int x3 = s.xa + lane; x3 <= s.xb; x3 += int(L)) owner.InterlockedMax((row + uint(x3)) * 4u, v, was);
}
#endif

// ---- each pixel's first shadow after that (its place; ~0 none) ----
#ifdef K_SHADOW
ByteAddressBuffer polys : register(t0, space0);
ByteAddressBuffer spans : register(t1, space0);
ByteAddressBuffer owner : register(t2, space0);
ByteAddressBuffer depth : register(t3, space0);
RWByteAddressBuffer shade : register(u0, space1);
cbuffer k : register(b0, space2) { uint k_total; uint k_W; uint k_pad0; uint k_pad1; };
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	const uint t = tid.x + tid.y * 65535u * 64u;
	if (t >= k_total) return;
	const span s = spans.Load<span>((t) * sizeof(span));
	if (s.xa > s.xb) return;
	const poly q = polys.Load<poly>((s.poly) * sizeof(poly));
	if (!(q.flags & SHADOW)) return;
	const bool mshadow = (q.flags & MODEL_SHADOW) != 0u;
	const uint at = uint(s.y) * k_W;
	uint was;
	for (int x = s.xa; x <= s.xb; x++) {
		const uint o = LD(owner, at + uint(x));
		if (o > s.poly) continue;                 // a solid polygon (or line) drawn after it
		const uint d = mshadow ? LD(depth, at + uint(x)) : 0u;
		if (d) {                                  // a model's shadow: not over a model's polygon in its own list
			const poly u = polys.Load<poly>((d - 1u) * sizeof(poly));
			if (u.batch == q.batch && !(u.flags & SCENERY)) continue;
		}
		shade.InterlockedMin((at + uint(x)) * 4u, s.poly, was);
	}
}
#endif

// ---- the pixels: the solid colour, or a shadow's half over the picture beneath (for the CPU) ----
#ifdef K_PIXELS
ByteAddressBuffer polys : register(t0, space0);
ByteAddressBuffer owner : register(t1, space0);
ByteAddressBuffer shade : register(t2, space0);
RWByteAddressBuffer res : register(u0, space1);    // the region's rows (from y0), W apart
RWByteAddressBuffer kind : register(u1, space1);   // a byte a pixel, rows (W + 3) / 4 words apart
cbuffer k : register(b0, space2) { uint k_W; uint k_y0; uint k_rows; uint k_pad0; };
// four pixels a thread (a word of kinds)
[numthreads(8, 8, 1)]
void main(uint3 g : SV_DispatchThreadID)
{
	const uint words = (k_W + 3u) / 4u;
	if (g.x >= words || g.y >= k_rows) return;
	uint kw = 0u;
	[unroll] for (uint i = 0; i < 4u; i++) {
		const uint x = g.x * 4u + i;
		if (x >= k_W) break;
		const uint at = (g.y + k_y0) * k_W + x;
		const uint o = LD(owner, at), s = LD(shade, at);
		if (!o && s == 0xffffffffu) continue;
		uint c, kd;
		if (s == 0xffffffffu) { c = polys.Load<poly>((o - 1u) * sizeof(poly)).colour; kd = 1u; }
		else {
			const uint h = (polys.Load<poly>((s) * sizeof(poly)).colour >> 1) & 0x7f7f7fu;
			if (o) { const uint b = polys.Load<poly>((o - 1u) * sizeof(poly)).colour; c = MOIRE_MARK | (((b >> 1) & 0x7f7f7fu) + h); kd = 1u; }
			else { c = h; kd = 2u; }   // (over the picture beneath: the CPU's)
		}
		ST(res, g.y * k_W + x, c);
		kw |= kd << (8u * i);
	}
	ST(kind, g.y * words + g.x, kw);
}
#endif

// ---- the pixels straight into a texture of the host's renderer, on its own device ----
// (gpu3d::draw_target: nothing comes back to the CPU.) The whole picture, keyed for gpu_compose's
// blend (out = src * (1 - a) + beneath * a): a colour opaque (alpha 0), nothing clear (alpha 1). A
// shadow over the 2D beneath is half of it plus the shadow's half colour, the CPU's
// ((beneath >> 1) & 0x7f7f7f) + h: alpha 128/255 and the colour h * 255 / 127, within a level of
// it in the blend's rounding.
#ifdef K_TARGET
ByteAddressBuffer polys : register(t0, space0);
ByteAddressBuffer owner : register(t1, space0);
ByteAddressBuffer shade : register(t2, space0);
[[vk::image_format("rgba8")]] RWTexture2D<unorm float4> target : register(u0, space1);
cbuffer k : register(b0, space2) { uint k_W; uint k_H; uint k_y0; uint k_y1; };
float3 rgb(uint c) { return float3((c >> 16) & 255u, (c >> 8) & 255u, c & 255u) / 255.0f; }
[numthreads(8, 8, 1)]
void main(uint3 g : SV_DispatchThreadID)
{
	if (g.x >= k_W || g.y >= k_H) return;
	float4 o = float4(0.0f, 0.0f, 0.0f, 1.0f);
	if (g.y >= k_y0 && g.y <= k_y1) {   // (only the region's owners and shadows are this picture's)
		const uint at = g.y * k_W + g.x;
		const uint ow = LD(owner, at), s = LD(shade, at);
		if (s == 0xffffffffu) {
			if (ow) o = float4(rgb(polys.Load<poly>((ow - 1u) * sizeof(poly)).colour), 0.0f);
		} else {
			const uint h = (polys.Load<poly>(s * sizeof(poly)).colour >> 1) & 0x7f7f7fu;
			if (ow) {
				const uint b = polys.Load<poly>((ow - 1u) * sizeof(poly)).colour;
				o = float4(rgb(((b >> 1) & 0x7f7f7fu) + h), 0.0f);
			} else
				o = float4(min(rgb(h) * (255.0f / 127.0f), 1.0f), 128.0f / 255.0f);
		}
	}
	target[g.xy] = o;
}
#endif
