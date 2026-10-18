#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include "../game.h"

class model1;

namespace game {
struct gameplay_callbacks;

namespace vf {

extern const video_traits VIDEO;

inline constexpr const char *UNUSED_CHARACTERS[] = {
	"Akira", "Siba (Majido)", "Jeff", "Takeru (early Akira)",
	"Rekka (Koskol brother)", "Score (Koskol brother)", "Aeon (Koskol brother)", "Dural"
};
inline constexpr uint32_t RECORD_SIZE = 0x176;
inline constexpr uint32_t MOVE_COUNT = 187;

extern const char *const SECRET_ICON_NAMES[];

struct random_cast
{
	bool made = false;
	std::map<uint32_t, std::vector<uint8_t>> orig;
	std::vector<uint32_t> recs, heads, tables;
};

struct gameplay_state : game::gameplay_state
{
	bool clock_held = false;
	struct secret_icon {
		bool on = false, active = false;
		uint32_t at = 0;
		std::vector<uint16_t> chars, pal, map;
		std::vector<uint16_t> keep_chars, keep_pal, keep_map;
	} icon;
	bool random_fighter = false, random_moves = false, random_cpu = false, select_seen = false;
	random_cast random_cast;
	uint32_t random_seen[2] = { 0, 0 };
	std::vector<uint8_t> strands_orig;
	int64_t pick_at = -1;
	uint32_t random_state = 0;
};

class gameplay
{
public:
	explicit gameplay(model1 &machine) : m_machine(machine) {}

	static constexpr int FREE_PLAY = 0x1901;
	enum class assignment { match_1p, match_vs, difficulty, advertise_sound, continues };

	bool force_free_play() { return set_coin_setting(FREE_PLAY); }
	int coin_setting();
	bool set_coin_setting(int value);
	int assignment_value(assignment which);
	bool set_assignment(assignment which, int value);
	bool set_ring_width(int width);
	void set_akira_slot(int which);
	bool set_akira_icon(const uint32_t *xrgb);
	void fight_cheats(int infinite, bool one_hit, bool no_time);
	bool set_no_time_limit(bool on);
	void set_random_fighter(bool on, bool moves = false, bool cpu = false);
	int hide_free_play_text();
	void show_free_play_text();
	std::vector<std::string> screen_names() const;
	bool load_screen(const std::string &name, std::vector<uint32_t> xrgb, int w, int h);

private:
	model1 &m_machine;
};

gameplay_callbacks create_gameplay(model1 &machine);

}
}
