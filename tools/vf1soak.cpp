// vf1soak: the rollback soak test, netplay's first check (no network). Two boards play the same inputs: one
// straight through on the interpreter, and one as rollback netplay would run it, with player 2's inputs arriving
// --delay frames late. The second board guesses each of player 2's frames (the last input it had, as GGPO does),
// and as each real input arrives it goes back to that frame's save state and runs the frames since again (every
// frame by default, even when its guess was right; --only-misses: only when it was wrong). Each time a frame
// becomes certain its state must be the straight board's, byte for byte; at the first that isn't, the two states
// are written out and the parts that differ named.
//   vf1soak [regions dir] [--random SEED] [--frames N] [--delay D] [--only-misses] [--interp] [--map FILE]
//           [--hashes FILE] [--quiet] [--draw] [--random-fighters]
// The rolled-back board runs the native code made at start-up (as the game does; assets/vf_map.txt), its
// coroutine's stack in its save states (rec::save_stack); --interp: the interpreter on both. Both boards start as a
// match will: free play, the FREE PLAY notices hidden, the boot run unseen, no sound board, the I/O stand-in. Two
// random players press Start now and then (a versus: one joins, the loser comes back in). --hashes FILE: the
// straight board's state hash for each frame, to compare runs on other machines (x86-64 and ARM64 must match).
// Also reports what a frame costs (save, restore, run), against the 17.4 ms a frame has at 57.5 Hz.
#include "../src/host/regions.h"
#include "../src/rt/native.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// A frame's inputs, active high (a set bit: held): IN0's coins and starts, each player's
struct Ports { uint8_t in0 = 0, in1 = 0, in2 = 0; };
constexpr uint8_t P2_IN0 = 0x02 | 0x20;   // coin 2, start 2

// Two random players: the stick and buttons held for a few frames at a time, Start pressed now and then
struct Random
{
	std::mt19937 rng;
	uint8_t held[2] = {};
	int left[2] = {};
	explicit Random(uint32_t seed) : rng(seed) {}
	Ports next(uint64_t f)
	{
		Ports p;
		for (int k = 0; k < 2; k++) {
			if (left[k]-- <= 0) {
				const uint32_t r = rng();
				static const uint8_t STICK[9] = { 0, 0x10, 0x20, 0x40, 0x80, 0x50, 0x90, 0x60, 0xa0 };   // (none, the four ways, the diagonals)
				held[k] = uint8_t(STICK[r % 9] | ((r >> 4) & 7));   // P K G in bits 0-2
				left[k] = 2 + int((r >> 8) % 18);
			}
			// Start for 6 frames every few seconds, the players a second and a half apart
			const bool start = (f + uint64_t(k) * 90) % 400 < 6;
			if (k == 0) { p.in1 = held[0]; if (start) p.in0 |= 0x10; }
			else { p.in2 = held[1]; if (start) p.in0 |= 0x20; }
		}
		return p;
	}
};

Ports mix(const Ports &a, const Ports &b) { return { uint8_t((a.in0 & ~P2_IN0) | (b.in0 & P2_IN0)), a.in1, b.in2 }; }
bool same_p2(const Ports &a, const Ports &b) { return (a.in0 & P2_IN0) == (b.in0 & P2_IN0) && a.in2 == b.in2; }

void apply(model1 &m, const Ports &p)
{
	m.m_inputs[0] = uint8_t(~p.in0);
	m.m_inputs[1] = uint8_t(~p.in1);
	m.m_inputs[2] = uint8_t(~p.in2);
}

struct Stat
{
	double total = 0, worst = 0;
	uint64_t n = 0;
	void add(double ms) { total += ms; worst = std::max(worst, ms); n++; }
	double mean() const { return n ? total / double(n) : 0; }
};

void report(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, size_t n_a, size_t n_b, const std::vector<m1::StateSection> &sec)
{
	if (n_a != n_b) printf("  the states' sizes differ: %zu, %zu\n", n_a, n_b);
	const size_t n = std::min(n_a, n_b);
	for (size_t i = 0; i < sec.size(); i++) {
		const size_t from = sec[i].offset, to = i + 1 < sec.size() ? sec[i + 1].offset : n;
		if (from >= n) break;
		size_t count = 0, first = 0;
		for (size_t k = from; k < std::min(to, n); k++)
			if (a[k] != b[k]) { if (!count) first = k - from; count++; }
		if (count)
			printf("  %s: %zu bytes differ, the first %zu bytes in (offset %zu in the state)\n", sec[i].name, count, first, from + first);
	}
}

bool write_file(const char *path, const uint8_t *p, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(p, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

// A board as a match starts it: the boot unseen, then free play (the game's first boot writes its settings: set
// to free play, booted again)
void boot(model1 &m, bool rec_on)
{
	for (int pass = 0; pass < 2; pass++) {
		m.reset();
		if (rec_on) rec::attach(m.cpu());   // (a fresh game stack)
		std::fill_n(&m.work_ram()[0x17f0], 4, 0);
		m.set_drawing(false);
		for (int i = 0; i < 900 && !m.past_boot(); i++) m.run_frame();
		if (!m.force_free_play()) break;
	}
}

} // namespace

int main(int argc, char **argv)
{
	std::string dir = "build/vf", map_path = "assets/vf_map.txt", hashes_path;
	long frames = 20000, delay = 7;
	uint32_t seed = 1;
	bool quiet = false, interp = false, only_misses = false, draw = false, fighters = false;   // draw: the straight board draws every frame, the rolled-back one its first runs (as the game)
	const char *usage = "usage: vf1soak [regions dir] [--random SEED] [--frames N] [--delay D] [--only-misses] [--interp] [--map FILE] [--hashes FILE] [--quiet]\n";
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc) frames = strtol(argv[++i], nullptr, 10);
		else if (a == "--random" && i + 1 < argc) seed = uint32_t(strtoul(argv[++i], nullptr, 10));
		else if (a == "--delay" && i + 1 < argc) delay = strtol(argv[++i], nullptr, 10);
		else if (a == "--hashes" && i + 1 < argc) hashes_path = argv[++i];
		else if (a == "--map" && i + 1 < argc) map_path = argv[++i];
		else if (a == "--only-misses") only_misses = true;
		else if (a == "--interp") interp = true;
		else if (a == "--draw") draw = true;
		else if (a == "--random-fighters") fighters = true;
		else if (a == "--quiet") quiet = true;
		else if (a[0] != '-') dir = a;
		else { fprintf(stderr, "vf1soak: unknown argument %s\n%s", a.c_str(), usage); return 2; }
	}
	if (delay < 1 || delay > 60) { fprintf(stderr, "%s", usage); return 2; }
	auto pa = std::make_unique<model1>(load_regions(dir)), pb = std::make_unique<model1>(load_regions(dir));
	model1 &A = *pa, &B = *pb;   // A straight through (the interpreter), B rolled back
	const bool rec_on = !interp;
	A.hide_free_play_text();   // (the ROM's notices: before the code is made from it)
	B.hide_free_play_text();
	if (fighters)   // (Random fighter for both players, with its moves: its code patches before the code is made, as the game's)
		for (model1 *m : { &A, &B }) {
			m->set_random_player(0, "*");
			m->set_random_player(1, "*");
			m->set_strands_moved(true);
		}
	if (rec_on) {
		rec::attach(B.cpu());
		rec::hints map;
		if (!map.read(map_path)) { fprintf(stderr, "vf1soak: no address map %s\n", map_path.c_str()); return 1; }
		std::vector<uint8_t> mem = B.maincpu_rom();
		mem.resize(0x2000000);
		rec::native_stats st;
		if (!rec::build_native(mem, map, &st)) { fprintf(stderr, "vf1soak: the native recompile failed\n"); return 1; }
		B.run_cpu = [](int n) { rec::run(n); };
	}
	if (fighters)
		for (model1 *m : { &A, &B }) {
			m->set_random_seed(20261025);
			m->set_random_fighter(true, true, false);
		}
	boot(A, false);
	boot(B, rec_on);
	if (draw) { A.set_drawing(true); A.set_smooth(true); B.set_smooth(true); }
	printf("both booted at frame %llu / %llu, coin setting %04x\n", (unsigned long long)A.frame_number(), (unsigned long long)B.frame_number(), unsigned(A.coin_setting()));

	Random rnd(seed);
	FILE *hashes = nullptr;
	if (!hashes_path.empty() && !(hashes = fopen(hashes_path.c_str(), "w"))) { fprintf(stderr, "vf1soak: can't write %s\n", hashes_path.c_str()); return 1; }

	// B's save states, the one before each of the last delay + 1 frames (ring[f % size]: the state before frame f,
	// the board's then the stack's, at ring_board[f % size]). A runs delay frames behind B: it runs frame k once k is
	// certain, after checking B's state before k is its own.
	const size_t ring_n = size_t(delay) + 1;
	std::vector<std::vector<uint8_t>> ring(ring_n);
	std::vector<size_t> ring_board(ring_n);
	auto save_b = [&](size_t i) {
		B.save_state(ring[i]);
		ring_board[i] = ring[i].size();
		if (rec_on) rec::save_stack(ring[i]);
	};
	std::vector<m1::StateSection> sections;
	std::vector<uint8_t> straight;
	std::vector<Ports> real, used;
	Stat t_save, t_load, t_run, t_tick;
	uint64_t rollbacks = 0, misses = 0, checked = 0;
	size_t stack_most = 0;
	const size_t H = model1::STATE_HEADER;

	auto t0 = Clock::now();
	bool failed = false;
	for (uint64_t f = 0; f < uint64_t(frames); f++) {
		const Ports c = rnd.next(f);
		real.push_back(c);
		auto tick = Clock::now();
		if (f >= uint64_t(delay)) {
			const uint64_t k = f - uint64_t(delay);
			std::vector<uint8_t> &mine = ring[k % ring_n];
			const size_t mine_n = ring_board[k % ring_n];
			A.save_state(straight, &sections);
			if (hashes) fprintf(hashes, "%llu %016llx\n", (unsigned long long)k, (unsigned long long)m1::state_hash(straight.data() + H, straight.size() - H));
			if (mine_n != straight.size() || memcmp(mine.data() + H, straight.data() + H, mine_n - H) != 0) {
				printf("frame %llu: the rolled-back board's state differs from the straight one's (before the frame)\n", (unsigned long long)k);
				report(straight, mine, straight.size(), mine_n, sections);
				if (write_file("soak_straight.bin", straight.data(), straight.size()) && write_file("soak_rolled.bin", mine.data(), mine_n))
					printf("  both written: soak_straight.bin, soak_rolled.bin\n");
				failed = true;
				break;
			}
			checked++;
			apply(A, real[k]);
			A.run_frame();

			const bool miss = !same_p2(used[k], real[k]);
			misses += miss;
			tick = Clock::now();
			if (miss || !only_misses) {
				rollbacks++;
				auto t = Clock::now();
				if (!B.load_state(mine.data(), mine_n) || (rec_on && !rec::load_stack(mine.data() + mine_n, mine.size() - mine_n))) {
					printf("frame %llu: the save state didn't load\n", (unsigned long long)k);
					failed = true;
					break;
				}
				t_load.add(ms_since(t));
				for (uint64_t g = k; g < f; g++) {
					if (g != k) {
						t = Clock::now();
						save_b(g % ring_n);
						t_save.add(ms_since(t));
					}
					used[g] = mix(real[g], real[k]);
					t = Clock::now();
					apply(B, used[g]);
					B.set_drawing(false);
					B.run_frame();
					t_run.add(ms_since(t));
				}
			}
		}
		auto t = Clock::now();
		save_b(f % ring_n);
		t_save.add(ms_since(t));
		stack_most = std::max(stack_most, rec::stack_used());
		used.push_back(mix(real[f], f >= uint64_t(delay) ? real[f - uint64_t(delay)] : Ports{}));
		t = Clock::now();
		apply(B, used[f]);
		B.set_drawing(draw);
		B.run_frame();
		if (draw && f % 3 == 0) B.render_smooth(double(B.frame_number()) - 0.5);   // (a picture between frames, as the screen asks)
		t_run.add(ms_since(t));
		t_tick.add(ms_since(tick));

		if (!quiet && f % 1000 == 999)
			printf("frame %llu: %llu checked, %llu rollbacks, %llu guesses wrong (game mode %u, the stack %zu bytes)\n", (unsigned long long)f + 1,
			       (unsigned long long)checked, (unsigned long long)rollbacks, (unsigned long long)misses, unsigned(A.game_mode()), rec::stack_used());
	}
	if (hashes) fclose(hashes);
	const double budget = 1000.0 / 57.524160;
	printf("%s: %llu frames checked, %llu rollbacks of %ld frames (%llu guesses wrong), %.1f s\n", failed ? "FAILED" : "identical",
	       (unsigned long long)checked, (unsigned long long)rollbacks, delay, (unsigned long long)misses, ms_since(t0) / 1000);
	printf("state %.2f MB (the stack at most %zu bytes); ms mean / worst: save %.2f / %.2f, restore %.2f / %.2f, a frame's run %.2f / %.2f\n",
	       double(ring_board[0]) / 1048576, stack_most, t_save.mean(), t_save.worst, t_load.mean(), t_load.worst, t_run.mean(), t_run.worst);
	printf("a frame with its rollback: %.2f ms mean, %.2f worst, of the %.1f ms a frame has (the board only: no picture, no sound; the straight board's interpreter counted in)\n",
	       t_tick.mean(), t_tick.worst, budget);
	return failed ? 1 : 0;
}
