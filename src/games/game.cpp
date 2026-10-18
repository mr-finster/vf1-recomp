// The games a binary has: each game's folder registers its definition at start-up
// (src/games/<name>/game.cpp), and the shared code looks it up by the game's id.
#include "game.h"
#include "script.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace game {

namespace {

// (a function's static: the games' registrations run before main, in any order)
std::vector<definition> &games()
{
	static std::vector<definition> all;
	return all;
}

}

registration::registration(const definition &d)
{
	if (!d.info || !d.io || !d.video || d.info->title.name.empty()) {
		fprintf(stderr, "game registration without a profile\n");
		std::abort();
	}
	for (const definition &other : games())
		if (other.info->title == d.info->title) {
			fprintf(stderr, "game \"%.*s\" registered twice\n", int(d.info->title.name.size()), d.info->title.name.data());
			std::abort();
		}
	games().push_back(d);
}

const definition &get_definition(id title)
{
	for (const definition &d : games())
		if (d.info->title == title) return d;
	std::string known;
	for (id t : registered()) known += (known.empty() ? "" : ", ") + std::string(t.name);
	fprintf(stderr, "no game \"%.*s\" in this program (it has: %s)\n", int(title.name.size()), title.name.data(),
			known.empty() ? "none" : known.c_str());
	std::exit(2);
}

std::vector<id> registered()
{
	std::vector<id> out;
	for (const definition &d : games()) out.push_back(d.info->title);
	std::sort(out.begin(), out.end(), [](id a, id b) { return a.name < b.name; });
	return out;
}

std::optional<id> find(std::string_view short_name)
{
	for (const definition &d : games())
		if (d.info->title.name == short_name) return d.info->title;
	return std::nullopt;
}

const profile &get(id title) { return *get_definition(title).info; }
const io_profile &get_io_profile(id title) { return *get_definition(title).io; }
const video_traits &get_video(id title) { return *get_definition(title).video; }

tgp_hle_callbacks make_tgp_hle(id title, model1 &machine)
{
	const definition &d = get_definition(title);
	return d.tgp_hle ? d.tgp_hle(machine) : tgp_hle_callbacks{};
}

gameplay_callbacks make_gameplay(id title, model1 &machine)
{
	const definition &d = get_definition(title);
	return d.gameplay ? d.gameplay(machine) : gameplay_callbacks{};
}

std::unique_ptr<auto_play> make_auto_play(id title, const std::string &scenario, uint32_t seed, bool checks)
{
	const definition &d = get_definition(title);
	return d.play ? d.play(scenario, seed, checks) : nullptr;
}

bool script_macro(id title, const std::string &cmd, std::istream &args, macro &out)
{
	const definition &d = get_definition(title);
	return d.macros && d.macros(cmd, args, out);
}

}
