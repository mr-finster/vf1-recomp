// Scripted play, each game's own: the run the headless tools share (tools/model1run, model1cov,
// model1lock: attract, coins, a game, then the title's play), the checks of where such a run got to,
// and the input macros a script can use (the host's M1_SCRIPT, tools/model1script: mash, fight...).
#pragma once
#include "game.h"
#include <array>
#include <istream>
#include <memory>
#include <string>
#include <vector>

class model1;

namespace game {

// One frame of a scripted run: the three input ports' pressed bits (1 = pressed, the machine's
// ports are active-low) and the analog channels to set (-1: left as they are).
struct script_inputs
{
	std::array<uint8_t, 3> pressed{};
	std::array<int, 3> analog{ -1, -1, -1 };
};

class auto_play
{
public:
	virtual ~auto_play() = default;
	// frame f's inputs (f from 1, each frame asked for once, in order)
	virtual script_inputs frame(int f) = 0;
	// with checks on: after frame f has run, a message when the run is not where it should be
	virtual std::string check(int f, model1 &m) { return {}; }
	// with checks on: at the end, a message when a checkpoint was never reached
	virtual std::string finish() { return {}; }
	// with checks on: the frames the checks need
	virtual int min_frames() const { return 0; }
};

// scenario: a game's other run ("" its usual one); nullptr when the game has no such scenario.
// seed: the random play's (VF's), checks: check() and finish() report (VR's race checkpoints).
std::unique_ptr<auto_play> make_auto_play(id title, const std::string &scenario, uint32_t seed, bool checks);

// A game's script command expanded into frames: each frame's pressed bits for the ports it plays
// (`ports`, a bit per port), the ports let go of after it (`released`).
struct macro
{
	std::vector<std::array<uint8_t, 3>> frames;
	uint8_t ports = 0, released = 0;
};

// false: not one of the game's commands
bool script_macro(id title, const std::string &cmd, std::istream &args, macro &out);

}
