// Loads the regions tools/romset.py builds from the user's own dump (development builds; the hosts
// otherwise read the MAME set itself: src/machine/model1_romset.h).
#pragma once
#include "../machine/model1.h"
#include <string>

inline std::vector<uint8_t> load_file(const std::string &path, game::id title)
{
	std::vector<uint8_t> d;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) {
		fprintf(stderr, "missing %s (%s)\n", path.c_str(), game::get(title).make_regions.data());
		exit(1);
	}
	fseek(f, 0, SEEK_END); d.resize(ftell(f)); fseek(f, 0, SEEK_SET);
	fread(d.data(), 1, d.size(), f);
	fclose(f);
	return d;
}

// Builds the machine, with the TGP's own program when the set has it.
inline model1::regions load_regions(const std::string &dir, game::id title)
{
	model1::regions r;
	r.title = title;
	r.maincpu = load_file(dir + "/maincpu.bin", title);
	r.copro_tables = load_file(dir + "/copro_tables.bin", title);
	r.polygons = load_file(dir + "/polygons.bin", title);
	r.sndcpu = load_file(dir + "/sndcpu.bin", title);
	r.pcm1 = load_file(dir + "/pcm1.bin", title);
	r.pcm2 = load_file(dir + "/pcm2.bin", title);
	if (FILE *f = fopen((dir + "/copro_data.bin").c_str(), "rb")) { fclose(f); r.copro_data = load_file(dir + "/copro_data.bin", title); }
	if (FILE *f = fopen((dir + "/tgp_program.bin").c_str(), "rb")) { fclose(f); r.tgp_program = load_file(dir + "/tgp_program.bin", title); }
	if (FILE *f = fopen((dir + "/ioboard.bin").c_str(), "rb")) { fclose(f); r.ioboard = load_file(dir + "/ioboard.bin", title); }
	return r;
}
