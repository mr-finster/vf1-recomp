// The 3D on the GPU (Display > 3D renderer). Model 1's polygons are flat: each fills its pixels
// with one colour, in the order the rasterizer draws them (model1::draw_quads: each viewport's
// list sorted far to near, the lists in the order the display list gives them), the last to fill
// a pixel keeping it. The shadows (MOIRE) are the one blend: above 1x a shadow halves the pixel
// beneath with its colour once (the first shadow over it after the last solid polygon), and a
// moving model's shadow (a VF fighter's) leaves the models alone (the solid polygon under it from a
// model that isn't scenery, in the same list); at 1x a shadow is the arcade's checkerboard, every other pixel solid.
//
// The board records the picture's polygons as it would draw them (model1::draw_quads, in GPU
// mode) and hands them over here; the backend draws them into the CPU's own picture (the bitmap's
// memory, page-aligned: a Metal buffer on the same pages, nothing copied), exactly as the CPU's
// rasterizer would have left it, and everything after (the shadows over the 2D, Low's smoothing,
// antialiasing's averaging, the 2D's composition, smooth motion) goes on as before.
//  - above 1x, the rows' spans are worked out on the GPU, a thread a polygon's row, as
//    model1::fill_quad_hd does (pixel centres, the four edges' crossings);
//  - at 1x the spans are MAME's fixed-point ones, worked out on the CPU by the rasterizer's own
//    code (fill_quad with a span sink) and handed over;
//  then a pass a span keeps each pixel's last solid polygon (an atomic maximum), a pass a shadow's
//  span its first shadow after that (an atomic minimum), and a pass a pixel writes the result.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

struct gpu3d_poly
{
	float x[4] = {}, y[4] = {};   // above 1x: its vertices (render pixels, unrounded: fill_quad_hd's)
	int32_t cx0 = 0, cx1 = 0, cy0 = 0, cy1 = 0;   // its viewport's clip (inclusive)
	int32_t y0 = 0, rows = 0;     // above 1x: its first row and how many (fill_quad_hd's y0..y1)
	uint32_t span0 = 0;           // above 1x: its rows' spans' first place after the CPU's (two a row)
	uint32_t colour = 0;          // what a solid pixel becomes; a shadow's colour
	uint32_t flags = 0;           // gpu3d_flag
	uint32_t batch = 0;           // its viewport's list (a model's shadow looks under it in its own)
};
enum gpu3d_flag : uint32_t
{
	G3_SHADOW = 1,       // blended (above 1x): halves the pixel beneath once
	G3_CHECKER = 2,      // solid on the pixels with (x ^ y) even (1x's shadows)
	G3_MODEL_SHADOW = 4, // a moving model's shadow: not over a model's polygon in its list
	G3_SCENERY = 8,      // from the scenery (a model's shadow goes over it)
	G3_ROWS = 16,        // its spans worked out on the GPU (above 1x)
	G3_LINE = 32,        // a wireframe line's pixels (draw_wireframe_line's, worked out on the CPU): solid,
	                     // but not a polygon to a model's shadow (the CPU's map of them leaves it out)
};
struct gpu3d_span { uint32_t poly; int32_t y, xa, xb; };   // pixels xa..xb of row y (none: xa > xb)

class gpu3d
{
public:
	virtual ~gpu3d() = default;
	virtual const char *name() const = 0;
	// The polygons drawn into `px` (w x h, rows w apart, page-aligned, `bytes` long), rows y0..y1
	// the only ones they touch. spans: the spans worked out on the CPU (1x's, the lines' pixels),
	// `nspans` of them; `rowspans` more after them for the rows the GPU works out. False: not drawn,
	// the picture as it was (the caller draws it).
	virtual bool draw(const gpu3d_poly *polys, size_t count, const gpu3d_span *spans, size_t nspans, size_t rowspans,
		uint32_t *px, size_t bytes, int w, int h, int y0, int y1, double *gpu_ms = nullptr) = 0;
	int test_repeat = 1;   // tests: each picture's passes run n times (a slow GPU)

	// Into a texture of the host's renderer instead (SDL_GPU on the renderer's own device): nothing
	// comes back to the CPU, and nothing waits for the GPU (the renderer's drawing, on the same
	// device after it, takes it in order). `count` targets the picture's size (RGBA8: the host
	// wraps each, target_handle, as an SDL_Texture); false: not here.
	virtual bool make_targets(int w, int h, int count) { (void)w; (void)h; (void)count; return false; }
	virtual void *target_handle(int i) const { (void)i; return nullptr; }
	virtual uint64_t targets_made() const { return 0; }   // (a new number when made again)
	// The polygons into target `t`, all of it: keyed for gpu_compose's blend (alpha 0: a colour,
	// 1: nothing; a shadow over the 2D beneath half-transparent). False: not drawn (the caller draws
	// the picture the usual way).
	virtual bool draw_target(const gpu3d_poly *polys, size_t count, const gpu3d_span *spans, size_t nspans, size_t rowspans,
		int w, int h, int y0, int y1, int t)
	{ (void)polys; (void)count; (void)spans; (void)nspans; (void)rowspans; (void)w; (void)h; (void)y0; (void)y1; (void)t; return false; }
};

// Metal (gpu3d_metal.mm, Apple only): on `mtl_device` (an id<MTLDevice>), the system's default when
// null. Null if none.
std::unique_ptr<gpu3d> gpu3d_create_metal(void *mtl_device = nullptr);
// The same on a CAMetalLayer's device (the host's SDL renderer's)
std::unique_ptr<gpu3d> gpu3d_create_metal_layer(void *ca_metal_layer);
// Its shaders compiled on another thread from now (the start's head start, while the game boots)
void gpu3d_metal_prewarm();
// SDL_GPU (gpu3d_sdlgpu.cpp: Direct3D 12 or Vulkan, Windows and Linux), on a device of its own:
// the polygons go up and each pixel's result comes back into the picture (no shared memory). The
// x86-64 CPU rasterizer's pixels. Null if it can't.
std::unique_ptr<gpu3d> gpu3d_create_sdlgpu(const char *driver = nullptr);   // (driver: SDL_GPU's, "vulkan"; none: SDL's choice)
// The same on the device of SDL's "gpu" renderer (an SDL_GPUDevice *, the host's), which can draw
// straight into the renderer's textures (draw_target). The device stays the renderer's.
std::unique_ptr<gpu3d> gpu3d_create_sdlgpu_on(void *sdl_gpu_device);
