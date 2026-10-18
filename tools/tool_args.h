// The headless tools' common options, taken out of argv so what's left is each tool's own:
//   --game NAME       the game (else the regions folder's name when it's a game's, build/vr; else the
//                     tool's only game, when it has one)
//   --scenario NAME   a game's other scripted run (src/games/script.h), e.g. VR's "alternate"
#pragma once
#include "../src/games/game.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

struct tool_args
{
	std::optional<game::id> title;
	std::string scenario;

	// false (and a message) for a bad option
	bool take(int &argc, char **argv)
	{
		int out = 1;
		for (int i = 1; i < argc; i++) {
			const std::string a = argv[i];
			std::string value;
			bool has = false;
			for (const char *name : { "--game", "--scenario" }) {
				const size_t n = strlen(name);
				if (a == name) {
					if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", name); return false; }
					value = argv[++i];
					has = true;
				} else if (a.compare(0, n + 1, std::string(name) + "=") == 0) {
					value = a.substr(n + 1);
					has = true;
				}
				if (!has) continue;
				if (!strcmp(name, "--scenario")) scenario = value;
				else if (auto t = game::find(value)) title = t;
				else { fprintf(stderr, "--game: no game \"%s\" (%s)\n", value.c_str(), names().c_str()); return false; }
				break;
			}
			if (!has) argv[out++] = argv[i];
		}
		argc = out;
		argv[argc] = nullptr;
		return true;
	}

	// the game for a regions folder: --game's, else the folder's name when it's a game's, else the
	// only game; else the program ends with a message
	game::id game_for(const std::string &dir) const
	{
		if (title) return *title;
		std::string name = std::filesystem::path(dir).filename().string();
		if (name.empty()) name = std::filesystem::path(dir).parent_path().filename().string();   // (build/vr/)
		if (auto t = game::find(name)) return *t;
		if (const auto all = game::registered(); all.size() == 1) return all[0];
		fprintf(stderr, "which game is %s? say with --game (%s)\n", dir.empty() ? "it" : dir.c_str(), names().c_str());
		std::exit(2);
	}

	// the default regions folder: build/<game> (none when the game isn't known yet)
	std::string default_dir() const
	{
		if (title) return "build/" + std::string(title->name);
		if (const auto all = game::registered(); all.size() == 1) return "build/" + std::string(all[0].name);
		return {};
	}

	static std::string names()
	{
		std::string out;
		for (game::id t : game::registered()) out += (out.empty() ? "" : ", ") + std::string(t.name);
		return out;
	}
};
