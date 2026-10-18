// What a game adds to the headless tools (tools/model1run, model1script): its own pictures, its
// player's settings, and model1script's game-only options and commands. A game with none adds
// nothing (the defaults); one with some registers them (tools/games/<name>/tool.cpp).
#pragma once
#include "../src/machine/model1.h"
#include <istream>
#include <memory>
#include <string>

class tool_game
{
public:
	virtual ~tool_game() = default;
	// the game's own pictures in dir beside the skies (VF: the title and select's widescreen sides)
	virtual void load_pictures(const std::string &dir) {}
	// as the player's game (M1_PCMODE): VF hides its FREE PLAY notices; the value printed by model1script
	virtual int pc_mode() { return 0; }
	// model1script, before the reset: the game's own options (VF: free play, cheats, the icon's camera)
	virtual void script_setup() {}
	// model1script, after the reset (rec: on the recompiled code)
	virtual void script_started(bool rec) {}
	// model1script, after each frame (fno counts from 1)
	virtual void script_frame(int fno) {}
	// model1script: a script command of the game's (false: not one); out: the shot folder
	virtual bool script_command(const std::string &cmd, std::istream &args, const std::string &out) { return false; }
};

std::unique_ptr<tool_game> make_tool_game(game::id title, model1 &machine);

// in a game's tools/games/<name>/tool.cpp: static const tool_game_registration REGISTERED{ ID, make };
using tool_game_factory = std::unique_ptr<tool_game> (*)(model1 &);
struct tool_game_registration
{
	tool_game_registration(game::id title, tool_game_factory make);
};
