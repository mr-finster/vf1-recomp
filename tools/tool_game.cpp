// The games' parts of the tools, registered by tools/games/<name>/tool.cpp at start-up; a game
// with none gets the defaults (nothing added).
#include "tool_game.h"
#include <vector>

namespace {

struct entry { game::id title; tool_game_factory make; };

std::vector<entry> &factories()
{
	static std::vector<entry> all;
	return all;
}

}

tool_game_registration::tool_game_registration(game::id title, tool_game_factory make)
{
	factories().push_back({ title, make });
}

std::unique_ptr<tool_game> make_tool_game(game::id title, model1 &machine)
{
	for (const entry &e : factories())
		if (e.title == title) return e.make(machine);
	return std::make_unique<tool_game>();
}
