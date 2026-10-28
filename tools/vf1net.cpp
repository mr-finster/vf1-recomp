// vf1net: VF1 netplay, two boards in one process: a host and a guest, each with its own netplay session (src/net),
// talking through a simulated network (latency, jitter, loss), each playing its own side of the inputs. The host's
// board runs the native code made at start-up, its coroutine's stack in its save states; the guest's the
// interpreter (the recompiled code is one board's to a process): so it's also a check that the two agree. The
// sessions compare their states every 30 frames as netplay does (a desync ends the run); at the end a third board
// (the interpreter) runs the same inputs straight through, and both players' boards must have been in its state at
// every checkpoint. Reports the rollbacks and what a frame cost each side. VF3's vf3net, for VF1.
//   vf1net [regions dir] [--random SEED] [--frames N] [--latency MS] [--jitter MS] [--loss PERCENT] [--delay D]
//          [--interp] [--soft] [--map FILE] [--p1 F] [--p2 F] [--hashes FILE]
// --hashes FILE: the straight board's state hash every 30 frames ("frame hash", hex): a release's file against the
// last online release's says whether the two play each other (docs: the online version); the same every run
#include "../src/host/regions.h"
#include "../src/host/vf1_netplay.h"
#include "../src/rt/native.h"
#include "../src/net/session.h"
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

struct Ports { uint8_t in0 = 0, in1 = 0, in2 = 0; };   // (active high)

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
				static const uint8_t STICK[9] = { 0, 0x10, 0x20, 0x40, 0x80, 0x50, 0x90, 0x60, 0xa0 };
				held[k] = uint8_t(STICK[r % 9] | ((r >> 4) & 7));
				left[k] = 2 + int((r >> 8) % 18);
			}
			const bool start = (f + uint64_t(k) * 90) % 400 < 6;
			if (k == 0) { p.in1 = held[0]; if (start) p.in0 |= 0x10; }
			else { p.in2 = held[1]; if (start) p.in0 |= 0x20; }
		}
		return p;
	}
};

struct Board
{
	std::unique_ptr<model1> m;
	std::unique_ptr<vf1net::Adapter> adapter;
	// native: the native code (made once, by the first board that asks)
	bool open(const std::string &dir, bool native, const std::string &map_path, std::string &error)
	{
		m = std::make_unique<model1>(load_regions(dir));
		m->set_drawing(false);
		if (native) {
			m->hide_free_play_text();   // (before the code is made from the ROM)
			rec::attach(m->cpu());
			rec::hints map;
			if (!map.read(map_path)) { error = "no address map " + map_path; return false; }
			std::vector<uint8_t> mem = m->maincpu_rom();
			mem.resize(0x2000000);
			if (!rec::build_native(mem, map)) { error = "the native recompile failed"; return false; }
			m->run_cpu = [](int n) { rec::run(n); };
		}
		adapter = std::make_unique<vf1net::Adapter>(*m, native);
		if (native)   // (the match's patches: the native code made again, as the game does)
			adapter->program_changed = [this, map_path] {
				rec::hints map;
				std::vector<uint8_t> mem = m->maincpu_rom();
				mem.resize(0x2000000);
				if (!map.read(map_path) || !rec::build_native(mem, map)) fprintf(stderr, "vf1net: the native recompile failed\n");
			};
		return true;
	}
};

struct Timing
{
	double total = 0, worst = 0;
	uint64_t n = 0;
	void add(double ms) { total += ms; worst = std::max(worst, ms); n++; }
};

// A player's input word from a frame's ports (active high)
uint16_t word(const Ports &p, int player) { return vf1net::pack(uint8_t(~p.in0), uint8_t(~(player ? p.in2 : p.in1)), player); }

} // namespace

int main(int argc, char **argv)
{
	std::string dir = "build/vf", map_path = "assets/vf_map.txt", p1, p2;
	long frames = 3000;
	uint32_t seed = 1;
	vfnet::SimWire::Params p{ 40, 8, 0 };
	int delay = 2;
	bool interp = false, soft = false, names_host = false;
	const char *hashes_path = nullptr;   // soft: each side's clock a little slower while ahead, as the game's
	const char *usage = "usage: vf1net [regions dir] [--random SEED] [--frames N] [--latency MS] [--jitter MS] [--loss PERCENT] [--delay D] [--interp] [--soft] [--map FILE]\n";
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc) frames = strtol(argv[++i], nullptr, 10);
		else if (a == "--map" && i + 1 < argc) map_path = argv[++i];
		else if (a == "--p1" && i + 1 < argc) p1 = argv[++i];   // (each player's fighter, as the handshake has it: R, S<seed>, X<k>)
		else if (a == "--p2" && i + 1 < argc) p2 = argv[++i];
		else if (a == "--hashes" && i + 1 < argc) hashes_path = argv[++i];
		else if (a == "--names-host") names_host = true;   // (the host has looked up the seeds' names before: Random fighter's history on one side only)
		else if (a == "--random" && i + 1 < argc) seed = uint32_t(strtoul(argv[++i], nullptr, 10));
		else if (a == "--latency" && i + 1 < argc) p.latency_ms = atof(argv[++i]);
		else if (a == "--jitter" && i + 1 < argc) p.jitter_ms = atof(argv[++i]);
		else if (a == "--loss" && i + 1 < argc) p.loss = atof(argv[++i]) / 100;
		else if (a == "--delay" && i + 1 < argc) delay = atoi(argv[++i]);
		else if (a == "--interp") interp = true;
		else if (a == "--soft") soft = true;
		else if (a[0] != '-') dir = a;
		else { fprintf(stderr, "vf1net: unknown argument %s\n%s", a.c_str(), usage); return 2; }
	}

	auto pa = std::make_unique<Board>(), pb = std::make_unique<Board>();
	Board &A = *pa, &B = *pb;
	std::string error;
	if (!A.open(dir, !interp, map_path, error) || !B.open(dir, false, map_path, error)) { fprintf(stderr, "vf1net: %s\n", error.c_str()); return 1; }

	// the inputs for every frame (each side plays its own player's)
	std::vector<Ports> real;
	Random rnd(seed);
	for (long f = 0; f < frames + 200; f++) real.push_back(rnd.next(uint64_t(f)));

	// (the host's NVRAM: a fresh board's, as on a first run)
	if (names_host) A.m->random_fighter_names();
	A.adapter->hosting = true;
	A.adapter->own_extra = p1;
	B.adapter->own_extra = p2;
	const std::vector<uint8_t> blob = vf1net::start_blob(*A.m, p1, seed * 2654435761u + 1);
	const uint64_t content = vf1net::content_hash(*A.m);
	if (content != vf1net::content_hash(*B.m)) { fprintf(stderr, "vf1net: the two boards' ROM hashes differ\n"); return 1; }

	vfnet::SimWire wire(p, p, 7);
	vfnet::Config cfg;
	cfg.delay = delay;
	cfg.soft_sync = soft;
	cfg.handshake_ms = 300000;   // (both boots run here one after the other, on the one clock)
	const vfnet::Identity ia{ "vf1", "test", content, "HOST", p1 }, ib{ "vf1", "test", content, "GUEST", p2 };
	A.adapter->checkpoint_every = B.adapter->checkpoint_every = 300;
	A.adapter->keep_states = B.adapter->keep_states = true;
	std::vector<uint8_t> sound[2];   // (the bytes each side sent its sound board)
	A.adapter->sound_out = [&](const std::vector<uint8_t> &b) { sound[0].insert(sound[0].end(), b.begin(), b.end()); };
	B.adapter->sound_out = [&](const std::vector<uint8_t> &b) { sound[1].insert(sound[1].end(), b.begin(), b.end()); };
	vfnet::Session sa(wire.a(), *A.adapter, ia, cfg, true, blob);
	vfnet::Session sb(wire.b(), *B.adapter, ib, cfg, false);

	Timing ta, tb;
	size_t na = 0, nb = 0;   // each side's frames run (its next frame's inputs)
	double t = 1000, next_a = 1000, next_b = 1000 + 23;
	const double fa = cfg.frame_ms, fb = cfg.frame_ms * 1.002;
	const auto t0 = Clock::now();
	bool connected = false;
	vf1net::Score score_a, score_b, score_c;   // (the detector on each side, as the game runs it: after each step, guesses and all)
	while (sa.state() != vfnet::Session::State::ended && sb.state() != vfnet::Session::State::ended &&
	       (long(sa.frame()) < frames || long(sb.frame()) < frames)) {
		wire.now = t;
		sa.poll(t);
		sb.poll(t);
		if (!connected && sa.state() == vfnet::Session::State::running && sb.state() == vfnet::Session::State::running) {
			connected = true;
			printf("connected (both boots run) after %.1f s\n", std::chrono::duration<double>(Clock::now() - t0).count());
		}
		if (t >= next_a) {
			const auto s0 = Clock::now();
			if (sa.step(word(real[std::min(na, real.size() - 1)], 0))) { na++; score_a.frame(*A.m); }
			if (connected) ta.add(std::chrono::duration<double, std::milli>(Clock::now() - s0).count());
			next_a += fa / (1 - (soft ? std::clamp(sa.stats().lead * 0.02, 0.0, 0.04) : 0.0));
		}
		if (t >= next_b) {
			const auto s0 = Clock::now();
			if (sb.step(word(real[std::min(nb, real.size() - 1)], 1))) { nb++; score_b.frame(*B.m); }
			if (connected) tb.add(std::chrono::duration<double, std::milli>(Clock::now() - s0).count());
			next_b += fb / (1 - (soft ? std::clamp(sb.stats().lead * 0.02, 0.0, 0.04) : 0.0));
		}
		t += 1;
	}
	vfnet::Session::Event e;
	bool failed = false;
	while (sa.next_event(e)) if (e.kind == e.ended) { printf("host: %s\n", e.text.c_str()); failed = true; }
	while (sb.next_event(e)) if (e.kind == e.ended) { printf("guest: %s\n", e.text.c_str()); failed = true; }

	// the same inputs straight through, on a third board, against both players' checkpoints
	const uint32_t n = std::min(sa.confirmed(), sb.confirmed());
	uint32_t checked = 0;
	std::vector<uint8_t> straight_sound;
	if (!failed) {
		auto pc = std::make_unique<Board>();
		Board &C = *pc;
		if (!C.open(dir, false, map_path, error) || (C.adapter->own_extra = p2, !C.adapter->begin(blob))) { fprintf(stderr, "vf1net: the straight board didn't start\n"); return 1; }
		C.adapter->checkpoint_every = hashes_path ? 30 : 300;
		C.adapter->keep_states = true;
		C.adapter->sound_out = [&](const std::vector<uint8_t> &b) { straight_sound.insert(straight_sound.end(), b.begin(), b.end()); };
		for (uint32_t f = 0; f < n; f++) {
			const uint16_t in[2] = { sa.inputs(0)[f], sa.inputs(1)[f] };
			if (in[0] != sb.inputs(0)[f] || in[1] != sb.inputs(1)[f]) { printf("the two sides' inputs differ at frame %u\n", f); failed = true; break; }
			C.adapter->advance(in, false);
			score_c.frame(*C.m);
		}
		printf("the score: host %d-%d, guest %d-%d, straight %d-%d (rounds %d-%d)\n", score_a.matches[0], score_a.matches[1], score_b.matches[0],
		       score_b.matches[1], score_c.matches[0], score_c.matches[1], score_c.rounds[0], score_c.rounds[1]);
		for (int q = 0; q < 2; q++)
			if (score_a.matches[q] != score_c.matches[q] || score_b.matches[q] != score_c.matches[q] || score_a.rounds[q] != score_c.rounds[q] || score_b.rounds[q] != score_c.rounds[q]) {
				printf("the score differs from the straight run's\n");
				failed = true;
				break;
			}
		if (hashes_path) {
			FILE *hf = fopen(hashes_path, "w");
			if (!hf) { fprintf(stderr, "vf1net: can't write %s\n", hashes_path); return 1; }
			for (auto &[f, h] : C.adapter->checkpoints) if (f <= n) fprintf(hf, "%u %016llx\n", f, (unsigned long long)h);
			fclose(hf);
		}
		for (auto &[f, h] : C.adapter->checkpoints) {
			if (f > n || f % 300) continue;
			const bool ha = A.adapter->checkpoints[f] == h, hb = B.adapter->checkpoints[f] == h;
			if (!ha || !hb) {
				printf("after frame %u: the host's board %s a straight run, the guest's %s; host and guest %s\n", f, ha ? "is" : "differs from", hb ? "is" : "differs from",
				       A.adapter->checkpoints[f] == B.adapter->checkpoints[f] ? "the same" : "differ");
				// where, by the board's sections (past the stack's room)
				std::vector<uint8_t> st;
				std::vector<m1::StateSection> sec;
				C.m->save_state(st, &sec);
				const std::vector<uint8_t> &c = C.adapter->checkpoint_states[f];
				for (int w = 0; w < 2; w++) {
					const std::vector<uint8_t> &x = (w ? B : A).adapter->checkpoint_states[f];
					if (x.size() != c.size()) { printf("  %s: sizes %zu, %zu\n", w ? "guest" : "host", x.size(), c.size()); continue; }
					for (size_t i = 0; i < sec.size(); i++) {
						const size_t from = vf1net::STACK_ROOM + sec[i].offset, to = i + 1 < sec.size() ? vf1net::STACK_ROOM + sec[i + 1].offset : c.size() - 4;
						size_t cnt = 0, first = 0;
						for (size_t k = from; k < to; k++) if (x[k] != c[k]) { if (!cnt) first = k - from; cnt++; }
						if (cnt && i) printf("  %s: %s, %zu bytes, the first %zu in\n", w ? "guest" : "host", sec[i].name, cnt, first);
					}
				}
				FILE *fo;
				if ((fo = fopen("net_host.bin", "wb"))) { fwrite(A.adapter->checkpoint_states[f].data(), 1, c.size(), fo); fclose(fo); }
				if ((fo = fopen("net_straight.bin", "wb"))) { fwrite(c.data(), 1, c.size(), fo); fclose(fo); }
				failed = true;
				break;
			}
			checked++;
		}
		// the sound board's bytes: each side sent each frame's once, guessed or not; a wrong guess's can differ
		printf("bytes to the sound board: host %zu, guest %zu, straight %zu (to frame %u; a wrong guess's stay)\n", sound[0].size(), sound[1].size(), straight_sound.size(), n);
	}
	const auto &xa = sa.stats(), &xb = sb.stats();
	printf("%s: %u frames confirmed, %u checkpoints against a straight run; %.0f ms each way, jitter %.0f, %.0f%% lost; delay %d\n",
	       failed ? "FAILED" : "in step", n, checked, p.latency_ms, p.jitter_ms, p.loss * 100, delay);
	for (int s = 0; s < 2; s++) {
		const auto &x = s ? xb : xa;
		const Timing &tm = s ? tb : ta;
		printf("  %s: ping %.0f ms, %llu rollbacks (%.1f frames on average), %llu waits for the other side, %llu to keep in time; a step %.2f ms on average, %.2f the worst (a frame has %.1f)\n",
		       s ? "guest" : "host ", x.rtt_ms, (unsigned long long)x.rollbacks, x.rollbacks ? double(x.resim_frames) / double(x.rollbacks) : 0.0,
		       (unsigned long long)x.stalls, (unsigned long long)x.waits, tm.n ? tm.total / double(tm.n) : 0.0, tm.worst, cfg.frame_ms);
	}
	return failed ? 1 : 0;
}
