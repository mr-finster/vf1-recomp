// Hi-res stage skies: <dir>/stage_<name>.png or .jpg (the names of the background export), loaded
// at their full size and handed to the machine. A game's other pictures use load_picture too
// (Virtua Fighter's: src/games/vf/art.h).
#pragma once
#include "../machine/model1.h"
#include "../../third_party/stb/stb_image.h"
#include <cstdio>
#include <string>
#include <vector>

// <path> as 0RGB pixels; false if it isn't there or isn't a picture
inline bool load_picture(const std::string &path, std::vector<uint32_t> &xrgb, int &w, int &h)
{
	int comp = 0;
	unsigned char *px = stbi_load(path.c_str(), &w, &h, &comp, 4);
	if (!px) return false;
	xrgb.assign(size_t(w) * h, 0);
	for (size_t i = 0; i < xrgb.size(); i++)
		xrgb[i] = uint32_t(px[4 * i]) << 16 | uint32_t(px[4 * i + 1]) << 8 | px[4 * i + 2];
	stbi_image_free(px);
	return true;
}

// The first of <dir>/<name>.png, .jpg, .jpeg there is, handed to `use`; how many it took (0 or 1)
template <class F>
int load_background(const std::string &dir, const std::string &name, F use)
{
	for (const char *ext : { ".png", ".jpg", ".jpeg" }) {
		const std::string path = dir + "/" + name + ext;
		std::vector<uint32_t> xrgb;
		int w = 0, h = 0;
		if (!load_picture(path, xrgb, w, h)) continue;
		if (!use(std::move(xrgb), w, h)) return 0;
		fprintf(stderr, "background: %s (%dx%d)\n", path.c_str(), w, h);
		return 1;
	}
	return 0;
}

// Returns how many were found.
inline int load_backgrounds(model1 &m, const std::string &dir)
{
	int n = 0;
	for (const std::string &name : m.sky_names())
		n += load_background(dir, "stage_" + name, [&](std::vector<uint32_t> xrgb, int w, int h) {
			return m.load_sky(name, std::move(xrgb), w, h);
		});
	return n;
}
