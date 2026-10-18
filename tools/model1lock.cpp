// Lockstep check: runs the interpreter and the recompiled code side by side and compares the
// CPU registers after every scanline and the RAM after every frame, playing the game's scripted run
// (src/games/script.h). model1lock has no recompiled C++ in it (for --native, any game's); each
// game's <game>lock-static links its generated C++ (build/gen/<game>_rec.cpp: make <game>-lockstep).
//   model1lock [--game vf|vr] [--scenario NAME] <regions dir> <frames> [--native [map.txt]] [--strict] [--scenarios]
// --native checks the native code made at start-up (src/rt/native.cpp) instead of the C++ build;
// --scenarios also checks where the run gets to (VR: the course selection, the race, the results).
#include "../src/host/regions.h"
#include "../src/games/script.h"
#include "../src/rt/native.h"
#include "tool_args.h"
#include <cstring>
#include <cstdlib>

int main(int argc, char **argv)
{
	tool_args opt;
	if (!opt.take(argc, argv)) return 2;
	const std::string dir = argc > 1 ? argv[1] : opt.default_dir();
	const game::id title = opt.game_for(dir);
	int frames = argc > 2 ? atoi(argv[2]) : 600;
	bool strict = false, checks = false;
	for (int i = 3; i < argc; i++)
		if (!strcmp(argv[i], "--scenarios")) checks = true;
	auto play = game::make_auto_play(title, opt.scenario, 12345, checks);
	if (!play) { fprintf(stderr, "%s has no scenario \"%s\"\n", game::get(title).display_name.data(), opt.scenario.c_str()); return 2; }
	if (frames < play->min_frames()) {
		fprintf(stderr, "%s scenarios require at least %d frames\n", game::get(title).display_name.data(), play->min_frames());
		return 2;
	}
	model1 a(load_regions(dir, title)), b(load_regions(dir, title));
	a.reset();
	b.reset();
	rec::attach(b.cpu(), title.name);
	b.run_cpu = [](int n) { rec::run(n); };
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--strict")) {
			strict = true;
		} else if (!strcmp(argv[i], "--native")) {
			rec::hints map;
			const std::string path = i + 1 < argc && argv[i + 1][0] != '-' ? argv[i + 1] : "assets/" + std::string(game::get(title).recompiler_map);
			if (!map.read(path)) { printf("no address map %s\n", path.c_str()); return 1; }
			std::vector<uint8_t> mem = load_regions(dir, title).maincpu;
			mem.resize(0x2000000);
			rec::native_stats st;
			if (!rec::build_native(mem, map, &st)) { printf("native recompile failed\n"); return 1; }
			printf("native: %zu functions, %zu instructions (%zu as native code), %zu KB of code in %.0f ms\n",
				   st.functions, st.instructions, st.native, st.code_bytes / 1024, st.ms);
		}
	}
	for (int f = 1; f <= frames; f++) {
		// scripted inputs, identical for both
		const game::script_inputs in = play->frame(f);
		for (int c = 0; c < 3; c++)
			if (in.analog[c] >= 0 && (!a.set_analog_input(c, uint8_t(in.analog[c])) || !b.set_analog_input(c, uint8_t(in.analog[c])))) {
				fprintf(stderr, "%s lockstep requires the Model 1 I/O board ROM (model1io.zip)\n", game::get(title).display_name.data());
				return 1;
			}
		for (int p = 0; p < 3; p++) a.m_inputs[p] = b.m_inputs[p] = uint8_t(~in.pressed[p]);
		std::vector<std::array<uint32_t, 34>> la, lb;
		int fallback_line = -1;
		a.on_line = [&](int) { std::array<uint32_t, 34> r; for (int i = 0; i < 34; i++) r[i] = i == 33 ? a.cpu().psw() : a.cpu().reg(i); la.push_back(r); };
		b.on_line = [&](int line) {
			if (strict && fallback_line < 0 && rec::fallback_count()) fallback_line = line;
			std::array<uint32_t, 34> r;
			for (int i = 0; i < 34; i++) r[i] = i == 33 ? b.cpu().psw() : b.cpu().reg(i);
			lb.push_back(r);
		};
		a.run_frame();
		b.run_frame();
		if (const std::string bad = play->check(f, a); !bad.empty()) {
			fprintf(stderr, "%s\n", bad.c_str());
			return 1;
		}
		if (strict && rec::fallback_count()) {
			printf("frame %d line %d: interpreter fallback at %06x (%zu fallback calls total)\n",
				   f, fallback_line, rec::first_fallback_address(), rec::fallback_count());
			return 1;
		}
		for (size_t l = 0; l < la.size(); l++)
			if (la[l] != lb[l]) {
				printf("frame %d line %zu: registers differ\n", f, l);
				for (int i = 0; i < 34; i++)
					if (la[l][i] != lb[l][i]) printf("  r%d interp=%08x rec=%08x\n", i, la[l][i], lb[l][i]);
				return 1;
			}
		if (a.nvram() != b.nvram()) { printf("frame %d: battery RAM differs\n", f); return 1; }
		if (a.work_ram() != b.work_ram()) { printf("frame %d: work RAM differs\n", f); return 1; }
		if (memcmp(a.frame().data(), b.frame().data(), 496 * 384 * 4)) { printf("frame %d: picture differs\n", f); return 1; }
		if (f % 300 == 0) printf("frame %d ok\n", f);
	}
	if (const std::string bad = play->finish(); !bad.empty()) {
		fprintf(stderr, "%s\n", bad.c_str());
		return 1;
	}
	printf("%d frames identical\n", frames);
}
