// V60 -> C++ static recompiler for Model 1, the analysis half (src/rt/analyse.cpp):
//
//   v60recomp <maincpu.bin> <hints.txt[,hints2.txt...]> <listing.txt> [map.txt]
//
// Writes the listing tools/recomp/emit.py turns into C++. The hints are coverage runs (build/cov)
// or the shipped address map. The output is derived from the ROM: it goes to build/, never git.
#include "../../src/rt/analyse.h"
#include <cstdio>

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: v60recomp maincpu.bin hints.txt[,more.txt] listing.txt\n");
		return 1;
	}
	std::vector<uint8_t> mem(0x2000000);
	{
		FILE *f = fopen(argv[1], "rb");
		if (!f) { fprintf(stderr, "no %s\n", argv[1]); return 1; }
		fread(mem.data(), 1, mem.size(), f);
		fclose(f);
	}
	rec::hints h;
	std::string list = argv[2];
	for (size_t pos = 0; pos <= list.size();) {
		size_t c = list.find(',', pos);
		std::string path = list.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
		pos = c == std::string::npos ? list.size() + 1 : c + 1;
		if (!h.read(path)) { fprintf(stderr, "no hints file %s\n", path.c_str()); return 1; }
	}
	rec::program p = rec::analyse(mem, h, true);
	rec::write_listing(p, argv[3]);
	if (argc > 4) rec::write_map(p, argv[4]);      // optional: the address map that ships
	size_t bad = 0;
	for (auto &[a, in] : p.insns) if (in.kind == rec::insn_kind::bad) bad++;
	printf("%zu instructions (%zu covered by the runs), %zu functions, %zu undecodable stops\n",
		   p.insns.size(), h.covered.size(), p.functions.size(), bad);
}
