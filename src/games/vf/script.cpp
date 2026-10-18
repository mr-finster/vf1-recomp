// Virtua Fighter's scripted play: the tools' run (attract, coins, a two-player game, random moves)
// and the bots a script can use (mash, brawl, fight).
#include "../script.h"

namespace game::vf {

namespace {

class play final : public auto_play
{
public:
	explicit play(uint32_t seed) : m_rng(seed) {}

	script_inputs frame(int f) override
	{
		script_inputs s;
		const int phase = f % 6000;
		if (phase >= 3000 && phase < 3006) s.pressed[0] |= 0x01;   // coin
		if (phase >= 3060 && phase < 3066) s.pressed[0] |= 0x01;   // coin
		if (phase >= 3120 && phase < 3126) s.pressed[0] |= 0x10;   // start
		if (phase >= 3200 && phase < 3206) s.pressed[0] |= 0x20;   // start 2 (versus)
		if (phase >= 3130 && (f % 7) == 0) {
			m_rng = m_rng * 1103515245 + 12345;
			s.pressed[1] = uint8_t((m_rng >> 16) & 0xf7);
			s.pressed[2] = uint8_t((m_rng >> 8) & 0xf7);
		}
		return s;
	}

private:
	uint32_t m_rng;
};

// punch 01 kick 02 guard 04 down 10 up 20 right 40 left 80 (the game's INPUT TEST)
void mash(int n, macro &out)
{
	for (int i = 0; i < n; i++)
		out.frames.push_back({ 0, uint8_t((i / 4) % 2 ? 0x02 : ((i / 4) % 3 ? 0x04 : 0x40)), 0 });
	out.ports = out.released = 0x02;
}

// random moves for N frames (both players, or 1 = P1)
void brawl(int n, unsigned seed, int players, macro &out)
{
	uint32_t rng = seed * 2654435761u + 1;
	auto rnd = [&](int k) { rng = rng * 1103515245 + 12345; return int((rng >> 16) % unsigned(k)); };
	static const uint8_t MOVES[] = { 0x40, 0x40, 0x80, 0x02, 0x02, 0x04, 0x04, 0x06, 0x03, 0x12, 0x14,
									 0x24, 0x42, 0x44, 0x82, 0x01, 0x46, 0x20, 0x10, 0x05, 0x00 };
	int left[2] = { 0, 0 };
	uint8_t cur[2] = { 0, 0 };
	for (int i = 0; i < n; i++) {
		std::array<uint8_t, 3> in{};
		for (int p = 0; p < players; p++) {
			if (--left[p] <= 0) {
				// a move, then let go a moment (taps register as presses)
				cur[p] = cur[p] ? 0 : MOVES[rnd(int(sizeof MOVES))];
				left[p] = cur[p] ? 3 + rnd(10) : 1 + rnd(4);
			}
			in[1 + p] = cur[p];
		}
		out.frames.push_back(in);
	}
	out.ports = players > 1 ? 0x06 : 0x02;
	out.released = 0x06;
}

// both players play real moves and combos for N frames
void fight(int n, unsigned seed, macro &out)
{
	uint32_t rng = seed * 2654435761u + 7;
	auto rnd = [&](int k) { rng = rng * 1103515245 + 12345; return int((rng >> 16) % unsigned(k)); };
	// G guard, P punch, K kick, D down, U up; F and B are forward and back for the player's side
	enum : uint8_t { P = 0x01, K = 0x02, G = 0x04, D = 0x10, U = 0x20, F = 0x40, B = 0x80 };
	struct step_t { uint8_t m; int frames; };
	static const std::vector<std::vector<step_t>> PLAYS = {
		{ { F, 14 } },                                                    // walk in
		{ { F, 2 }, { 0, 2 }, { F, 6 } },                                 // dash
		{ { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 4 }, { K, 3 }, { 0, 10 } },   // PPPK
		{ { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 8 } },   // PPP
		{ { P, 3 }, { 0, 4 }, { K, 3 }, { 0, 10 } },                      // PK
		{ { F | P, 3 }, { 0, 6 }, { F | K, 3 }, { 0, 10 } },              // elbow, knee
		{ { F | P, 3 }, { 0, 10 } },                                      // elbow
		{ { P | G, 4 }, { 0, 20 } },                                      // throw
		{ { F, 6 }, { P | G, 4 }, { 0, 20 } },                            // step in, throw
		{ { D | K, 3 }, { 0, 12 } },                                      // low kick
		{ { D | B | K, 3 }, { 0, 16 } },                                  // sweep
		{ { U | F, 3 }, { 0, 10 }, { K, 3 }, { 0, 24 } },                 // jump kick
		{ { F | K, 3 }, { 0, 12 } },                                      // knee / front kick
		{ { K, 3 }, { 0, 4 }, { K, 3 }, { 0, 10 } },                      // double kick
		{ { G, 18 } },                                                    // guard
		{ { D | G, 14 } },                                                // low guard
		{ { B, 10 } },                                                    // back off
	};
	static const int WEIGHTS[] = { 5, 3, 7, 3, 5, 5, 3, 3, 3, 3, 2, 2, 3, 2, 5, 3, 2 };
	int total = 0;
	for (int w : WEIGHTS) total += w;
	struct bot { const std::vector<step_t> *play = nullptr; size_t i = 0; int left = 0; uint8_t cur = 0; };
	bot bots[2];
	for (int f = 0; f < n; f++) {
		std::array<uint8_t, 3> in{};
		for (int p = 0; p < 2; p++) {
			bot &b = bots[p];
			if (b.left <= 0) {
				if (b.play && ++b.i < b.play->size()) {
					b.cur = (*b.play)[b.i].m; b.left = (*b.play)[b.i].frames;
				} else if (b.play) {
					b.play = nullptr; b.cur = 0; b.left = 2 + rnd(7);       // a breath between plays
				} else {
					int r = rnd(total), k = 0;
					while (r >= WEIGHTS[k]) r -= WEIGHTS[k++];
					b.play = &PLAYS[k]; b.i = 0;
					b.cur = PLAYS[k][0].m; b.left = PLAYS[k][0].frames + (k == 0 || k >= 14 ? rnd(12) : 0);
				}
			}
			b.left--;
			// forward is right for player 1 (on the left), left for player 2
			uint8_t bits = b.cur & 0x37;
			if (b.cur & F) bits |= p ? 0x80 : 0x40;
			if (b.cur & B) bits |= p ? 0x40 : 0x80;
			in[1 + p] = bits;
		}
		out.frames.push_back(in);
	}
	out.ports = out.released = 0x06;
}

}

std::unique_ptr<auto_play> make_auto_play(const std::string &scenario, uint32_t seed, bool)
{
	if (!scenario.empty()) return nullptr;
	return std::make_unique<play>(seed);
}

// mash N: P1 punches and kicks; brawl N SEED [PLAYERS]; fight N SEED
bool script_macro(const std::string &cmd, std::istream &args, macro &out)
{
	int n = 0;
	unsigned seed = 0;
	if (cmd == "mash") { args >> n; mash(n, out); }
	else if (cmd == "brawl") { int players = 2; args >> n >> seed; args >> players; brawl(n, seed, players, out); }
	else if (cmd == "fight") { args >> n >> seed; fight(n, seed, out); }
	else return false;
	return true;
}

}
