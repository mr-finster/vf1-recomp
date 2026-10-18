// The launch-time loader against tools/romset.py: every region built from the player's zip in C++
// must match the one the Python tool writes to build/<game>/ (both need the player's own ROMs).
//     romset_test <game> roms/<game>.zip [roms/model1io.zip] build/<game>
// The games are the ones linked in (src/games/*/romset.cpp), found by name.
#include "../src/machine/model1_romset.h"
#include <cstdio>
#include <string>
#include <vector>

static bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	out.resize(size_t(ftell(f)));
	fseek(f, 0, SEEK_SET);
	const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
	fclose(f);
	return ok;
}

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: %s game game.zip [model1io.zip] regions-dir\n", argv[0]); return 2; }
	const romset::definition *found = romset::find(argv[1]);
	if (!found) { fprintf(stderr, "%s: no such game in this test\n", argv[1]); return 2; }
	const romset::definition &set = *found;
	std::vector<std::string> zips(argv + 2, argv + argc - 1);
	const std::string dir = argv[argc - 1];
	const romset::result r = romset::load(set, zips);
	if (!r.ok) { fprintf(stderr, "%s: %s\n", argv[1], r.error.c_str()); return 1; }
	if (r.regions.title != set.title) { fprintf(stderr, "%s: wrong title\n", argv[1]); return 1; }
	if (!romset::is_game_set(set, zips[0])) { fprintf(stderr, "%s: %s not recognised\n", argv[1], zips[0].c_str()); return 1; }
	for (const romset::definition *other : romset::registered())
		if (other != &set && romset::is_game_set(*other, zips[0])) { fprintf(stderr, "%s: taken for %s\n", argv[1], other->zip); return 1; }
	struct { const char *file; const std::vector<uint8_t> &got; } regions[] = {
		{ "maincpu", r.regions.maincpu }, { "copro_tables", r.regions.copro_tables }, { "copro_data", r.regions.copro_data },
		{ "polygons", r.regions.polygons }, { "sndcpu", r.regions.sndcpu }, { "pcm1", r.regions.pcm1 },
		{ "pcm2", r.regions.pcm2 }, { "tgp_program", r.regions.tgp_program }, { "ioboard", r.regions.ioboard },
	};
	int bad = 0, checked = 0;
	for (const auto &reg : regions) {
		std::vector<uint8_t> want;
		if (!read_file(dir + "/" + reg.file + ".bin", want)) {
			if (!reg.got.empty()) { fprintf(stderr, "%s: %s loaded, but %s/%s.bin is missing\n", argv[1], reg.file, dir.c_str(), reg.file); bad++; }
			continue;
		}
		checked++;
		if (want != reg.got) { fprintf(stderr, "%s: %s differs (%zu bytes vs %zu)\n", argv[1], reg.file, reg.got.size(), want.size()); bad++; }
	}
	if (bad) return 1;
	printf("%s: %d regions match %s\n", argv[1], checked, dir.c_str());
	return 0;
}
