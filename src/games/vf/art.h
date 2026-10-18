// Virtua Fighter's own pictures: the title and player select's widescreen sides (<dir>/title.png,
// select.png: 16:9 canvases), and the Cheats' secret character's select icon.
#pragma once
#include "../../host/backgrounds.h"
#include "gameplay.h"
#include <iterator>

namespace game::vf {

// Returns how many were found.
inline int load_screens(gameplay &vf, const std::string &dir)
{
	int n = 0;
	for (const std::string &name : vf.screen_names())
		n += load_background(dir, name, [&](std::vector<uint32_t> xrgb, int w, int h) {
			return vf.load_screen(name, std::move(xrgb), w, h);
		});
	return n;
}

// The select's icon for the character in Akira's place (<dir>/NAME.png, 56 x 64, as
// SECRET_ICON_NAMES has them), from the first of `dirs` that has it; none: Akira's own.
inline bool load_secret_icon(gameplay &vf, int slot, const std::vector<std::string> &dirs)
{
	const char *name = slot > 0 && slot < int(std::size(UNUSED_CHARACTERS)) ? SECRET_ICON_NAMES[slot] : nullptr;
	if (name)
		for (const std::string &dir : dirs) {
			const std::string path = dir + "/" + name + ".png";
			std::vector<uint32_t> xrgb;
			int w = 0, h = 0;
			if (!load_picture(path, xrgb, w, h)) continue;
			if (w == 56 && h == 64 && vf.set_akira_icon(xrgb.data())) {
				fprintf(stderr, "icon: %s\n", path.c_str());
				return true;
			}
			fprintf(stderr, "icon: %s isn't one (56 x 64, 15 colours at most)\n", path.c_str());
		}
	vf.set_akira_icon(nullptr);
	return false;
}

}
