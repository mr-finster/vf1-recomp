// Hi-res stage skies: <dir>/stage_<name>.png or .jpg (the names of the background export), and
// the title and player select's widescreen sides (<dir>/title.png, select.png), loaded at their
// full size and handed to the machine. Returns how many were found.
#pragma once
#include "../machine/model1.h"
#include "../../third_party/stb/stb_image.h"
#include <cstdio>
#include <string>
#include <vector>

inline int load_backgrounds(model1 &m, const std::string &dir)
{
	int n = 0;
	for (const std::string &name : model1::sky_names())
		for (const char *ext : { ".png", ".jpg", ".jpeg" }) {
			const std::string path = dir + "/stage_" + name + ext;
			int w = 0, h = 0, comp = 0;
			unsigned char *px = stbi_load(path.c_str(), &w, &h, &comp, 4);
			if (!px) continue;
			std::vector<uint32_t> xrgb(size_t(w) * h);
			for (size_t i = 0; i < xrgb.size(); i++)
				xrgb[i] = uint32_t(px[4 * i]) << 16 | uint32_t(px[4 * i + 1]) << 8 | px[4 * i + 2];
			stbi_image_free(px);
			if (m.load_sky(name, std::move(xrgb), w, h)) {
				fprintf(stderr, "background: %s (%dx%d)\n", path.c_str(), w, h);
				n++;
			}
			break;
		}
	for (const std::string &name : model1::screen_names())     // title.png, select.png: 16:9 canvases
		for (const char *ext : { ".png", ".jpg", ".jpeg" }) {
			const std::string path = dir + "/" + name + ext;
			int w = 0, h = 0, comp = 0;
			unsigned char *px = stbi_load(path.c_str(), &w, &h, &comp, 4);
			if (!px) continue;
			std::vector<uint32_t> xrgb(size_t(w) * h);
			for (size_t i = 0; i < xrgb.size(); i++)
				xrgb[i] = uint32_t(px[4 * i]) << 16 | uint32_t(px[4 * i + 1]) << 8 | px[4 * i + 2];
			stbi_image_free(px);
			if (m.load_screen(name, std::move(xrgb), w, h)) {
				fprintf(stderr, "background: %s (%dx%d)\n", path.c_str(), w, h);
				n++;
			}
			break;
		}
	return n;
}

// Cheats: the select's icon for the character in Akira's place (<dir>/NAME.png, 56 x 64, as
// model1::SECRET_ICON_NAMES has them), from the first of `dirs` that has it; none: Akira's own.
inline bool load_secret_icon(model1 &m, int slot, const std::vector<std::string> &dirs)
{
	const char *name = slot > 0 && slot < int(std::size(model1::UNUSED_CHARACTERS)) ? model1::SECRET_ICON_NAMES[slot] : nullptr;
	if (name)
		for (const std::string &dir : dirs) {
			const std::string path = dir + "/" + name + ".png";
			int w = 0, h = 0, comp = 0;
			unsigned char *px = stbi_load(path.c_str(), &w, &h, &comp, 4);
			if (!px) continue;
			std::vector<uint32_t> xrgb(size_t(w) * h);
			for (size_t i = 0; i < xrgb.size(); i++)
				xrgb[i] = uint32_t(px[4 * i]) << 16 | uint32_t(px[4 * i + 1]) << 8 | px[4 * i + 2];
			stbi_image_free(px);
			if (w == 56 && h == 64 && m.set_akira_icon(xrgb.data())) {
				fprintf(stderr, "icon: %s\n", path.c_str());
				return true;
			}
			fprintf(stderr, "icon: %s isn't one (56 x 64, 15 colours at most)\n", path.c_str());
		}
	m.set_akira_icon(nullptr);
	return false;
}
