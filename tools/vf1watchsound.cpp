// vf1watchsound: a watcher's match sound after catching up, against one's who watched from the start (vf2watchsound's
// sweep, for VF1). A match (two random players, the game's board through vf1net::Adapter on the interpreter, as a
// watcher's runs it) gives each frame's bytes for the sound board; then the match's sound board (main.cpp's: the
// game's own, taken; a fresh one each run) is given them: from the start, every frame heard (main.cpp's heard way: whole messages, a byte
// every 8 of a frame's 424 lines); late, the frames before each join point through the UnheardQueue as main.cpp's
// sound thread runs it, at a pace against the catch-up (ratio: board frames a frame caught up; burst: the thread
// getting a pass's frames at once), with v1.6.1's bound (UNHEARD_TIMED: frames let go when it's behind) and with the
// hotfix's (the catch-up waits for it: none let go), the rest heard. For the window after the join point: how much of its output is
// the same sample for sample, and its loudness against the from-start board's.
//   vf1watchsound <regions dir> [--frames N] [--step F] [--window F] [--random SEED] [--catch-up N]
#include "../src/host/regions.h"
#include "../src/host/vf1_netplay.h"
#include "../src/machine/m1audio.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

struct Ports { uint8_t in0 = 0, in1 = 0, in2 = 0; };
struct Random   // (vf1net's)
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
uint16_t word(const Ports &p, int player) { return vf1net::pack(uint8_t(~p.in0), uint8_t(~(player ? p.in2 : p.in1)), player); }

// main.cpp's heard frame: the bytes up to the last whole message, a byte every 8 lines
struct Heard
{
	std::vector<uint8_t> bytes;
	uint8_t status = 0;
	int owed = 0, held = 0;
	void frame(m1audio &b, const std::vector<uint8_t> &in, std::vector<int16_t> &out)
	{
		bytes.insert(bytes.end(), in.begin(), in.end());
		constexpr int LINES = 424, BYTE_LINES = 8;
		const double line_us = 656 * 1e6 / 16e6;
		size_t whole = 0;
		{
			uint8_t st = status;
			int need = owed;
			for (size_t i = 0; i < bytes.size(); i++) {
				if (bytes[i] >= 0xf8) { if (!need) whole = i + 1; continue; }
				if (bytes[i] & 0x80) { st = bytes[i]; need = vf1net::midi_data_bytes(st); if (!need) whole = i + 1; continue; }
				if (!need) need = vf1net::midi_data_bytes(st);
				if (!need || --need == 0) whole = i + 1;
			}
		}
		held = whole == 0 && !bytes.empty() ? held + 1 : 0;
		if (held > 3 * 58) whole = bytes.size();
		size_t sent = 0;
		for (int line = 0; line < LINES; line++) {
			if (line % BYTE_LINES == 0 && sent < whole) {
				const uint8_t x = bytes[sent++];
				if (x >= 0xf8) {}
				else if (x & 0x80) status = x, owed = vf1net::midi_data_bytes(x);
				else if (owed || (owed = vf1net::midi_data_bytes(status))) owed--;
				b.rx_byte(x);
			}
			b.run(line_us, out);
			while (b.tx_pending()) b.tx_byte();
		}
		bytes.erase(bytes.begin(), bytes.begin() + ptrdiff_t(sent));
	}
};

double rms(const std::vector<int16_t> &v)
{
	double s = 0;
	for (int16_t x : v) s += double(x) * double(x);
	return v.empty() ? 0 : std::sqrt(s / double(v.size()));
}

} // namespace

int main(int argc, char **argv)
{
	std::string dir;
	long frames = 20000, step = 4000, window = 1200;
	uint32_t seed = 1;
	size_t catch_up = vf1net::UNHEARD_TIMED;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc) frames = strtol(argv[++i], nullptr, 10);
		else if (a == "--step" && i + 1 < argc) step = strtol(argv[++i], nullptr, 10);
		else if (a == "--window" && i + 1 < argc) window = strtol(argv[++i], nullptr, 10);
		else if (a == "--random" && i + 1 < argc) seed = uint32_t(strtoul(argv[++i], nullptr, 10));
		else if (a == "--catch-up" && i + 1 < argc) catch_up = size_t(strtoul(argv[++i], nullptr, 10));
		else if (a[0] != '-' && dir.empty()) dir = a;
		else { fprintf(stderr, "usage: vf1watchsound <regions dir> [--frames N] [--step F] [--window F] [--random SEED] [--catch-up N]\n"); return 2; }
	}
	model1 m(load_regions(dir));
	m.set_drawing(false);
	vf1net::Adapter ad(m, false);
	if (!ad.begin(vf1net::start_blob(m, "", 7))) { fprintf(stderr, "vf1watchsound: the match didn't start\n"); return 1; }
	std::vector<std::vector<uint8_t>> fr;
	ad.sound_out = [&](const std::vector<uint8_t> &b) { fr.back() = b; };
	Random rnd(seed);
	size_t bytes = 0;
	for (long f = 0; f < frames; f++) {
		const Ports p = rnd.next(uint64_t(f));
		const uint16_t in[2] = { word(p, 0), word(p, 1) };
		fr.emplace_back();
		ad.advance(in, false);
		bytes += fr.back().size();
	}
	printf("the match: %ld frames, %zu bytes for the sound board\n", frames, bytes);
	// (a fresh board each run, as the game's first match has: one board's reset doesn't give its first run again)
	std::unique_ptr<m1audio> board;
	auto fresh = [&]() -> m1audio & { board.reset(); m.enable_sound(); board = m.take_audio(); return *board; };
	// from the start (one board a process: each run from a reset)
	std::vector<std::vector<int16_t>> ref;   // (each frame's output)
	{
		m1audio &b = fresh();
		Heard h;
		for (long f = 0; f < frames; f++) { ref.emplace_back(); h.frame(b, fr[size_t(f)], ref.back()); }
	}
	{   // (the board's reset gives the same run again: else nothing here can be compared sample for sample)
		m1audio &b = fresh();
		Heard h;
		size_t same = 0, n = 0;
		for (long f = 0; f < std::min(frames, 3000L); f++) { std::vector<int16_t> out; h.frame(b, fr[size_t(f)], out); for (size_t i = 0; i < std::min(out.size(), ref[size_t(f)].size()); i++) same += out[i] == ref[size_t(f)][i]; n += ref[size_t(f)].size(); }
		printf("the from-start run again, a fresh board: %.1f%% of its samples the first's\n", 100.0 * double(same) / double(std::max<size_t>(n, 1)));
	}
	int worst[2] = { 100, 100 };
	for (long L = step; L + window <= frames; L += step) {
		for (int bound = 0; bound < 2; bound++)
		for (int pat = 0; pat < 5; pat++) {
			const long bursts[5] = { 0, 0, 0, 300, 600 };
			const double ratios[5] = { 1, 0.5, 0.1, 1, 1 };
			m1audio &b = fresh();
			vf1net::UnheardQueue q;
			q.most_timed = bound ? vf1net::UNHEARD_CATCH_UP : vf1net::UNHEARD_TIMED;
			vf1net::UnheardQueue::Piece piece;
			std::vector<uint8_t> uq;
			uint8_t status = 0;
			int owed = 0;
			double credit = 0;
			for (long f = 0; f < L; f++) {
				if (bound) {   // (the hotfix: the catch-up waits while the board has more than UNHEARD_AHEAD to run, all near the end)
					const size_t ahead = L > f + 600 ? vf1net::UNHEARD_AHEAD : 0;
					while (q.frames() > ahead && q.next(piece)) vf1net::run_unheard(b, piece, uq, status, owed);
				}
				q.frame(fr[size_t(f)]);
				credit += ratios[pat];
				if (bursts[pat] && (f + 1) % bursts[pat] != 0) continue;
				if (bursts[pat]) credit = 1e9;
				while (credit >= 1 && q.next(piece)) { vf1net::run_unheard(b, piece, uq, status, owed); if (piece.kind == piece.frame) credit -= 1; }
			}
			while (q.next(piece)) vf1net::run_unheard(b, piece, uq, status, owed);
			Heard h;
			h.bytes = uq;
			h.status = status;
			h.owed = owed;
			size_t same = 0, n = 0;
			std::vector<int16_t> all_a, all_b;
			for (long f = L; f < L + window; f++) {
				std::vector<int16_t> out;
				h.frame(b, fr[size_t(f)], out);
				const std::vector<int16_t> &r = ref[size_t(f)];
				for (size_t i = 0; i < std::min(out.size(), r.size()); i++) same += out[i] == r[i];
				n += r.size();
				all_a.insert(all_a.end(), r.begin(), r.end());
				all_b.insert(all_b.end(), out.begin(), out.end());
			}
			const int pct = n ? int(100.0 * double(same) / double(n)) : 100;
			worst[bound] = std::min(worst[bound], pct);
			printf("join at %5ld, %s, the sound thread at ratio %.2f burst %3ld: %5llu frames let go; after it %3d%% of the samples the from-start board's, loudness %.0f against %.0f\n", L,
			       bound ? "the hotfix" : "v1.6.1", ratios[pat], bursts[pat], (unsigned long long)q.dropped, pct, rms(all_b), rms(all_a));
			fflush(stdout);
		}
	}
	printf("the worst: v1.6.1 %d%%, the hotfix %d%% of the samples the from-start board's\n", worst[0], worst[1]);
	return worst[1] == 100 ? 0 : 1;
}
