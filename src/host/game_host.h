// What the desktop host (main.cpp) asks of the game it runs. The host itself is the Model 1's:
// the window, sound, pads, menu, saves and settings.ini; each game adds its own through this
// (src/games/<game>/host.cpp), and a host binary links one game's.
#pragma once
#include "controls.h"
#include "menu.h"
#include "../machine/model1.h"
#include "../machine/model1_romset.h"
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using settings_map = std::map<std::string, std::string>;

// The host's side, for the game's
struct host_context
{
	model1 &machine;
	std::string data_dir;                    // the player's (saves, settings.ini, backgrounds/)
	std::string assets_dir;
	const bool &pc_mode;                     // PC play style (else Arcade: the cabinet's boot screen, coins)
	std::function<void()> settings_changed;  // settings.ini is to be saved
	std::function<void(menu_page)> push_page;
	std::function<menu_item()> back_item;
	std::function<void()> service_menu;      // closes our menu; the game's own test menu opens
};

class game_host
{
public:
	virtual ~game_host() = default;

	virtual const romset::definition &romset() const = 0;   // the MAME set the player's zip is
	virtual const control_profile &controls() const = 0;

	// settings.ini's own keys (read before start)
	virtual void load_settings(const settings_map &) {}
	virtual void save_settings(FILE *) const {}

	// The machine is made, its code not yet recompiled (an instruction the game changes goes in it).
	virtual void start(host_context &) {}
	// Once, before the first boot: the game's pictures, the settings it reads at boot.
	virtual void before_boot() {}
	// After each boot (a reset); true: boot again (a setting the game reads at boot was put back).
	virtual bool booted() { return false; }

	// The Game page's play style (PC or Arcade): only for a game that has it.
	virtual bool has_play_style() const { return false; }
	// The game's own settings for Arcade mode, kept as they are now (before a restart and at quit).
	virtual void keep_arcade_settings() {}
	virtual void play_style_changed() {}

	// The main menu's own pages: after Game, and after Controls.
	virtual std::vector<menu_item> menu_items() { return {}; }
	virtual std::vector<menu_item> menu_items_after_controls() { return {}; }
	// The menu closed: whether the game restarts, and with its code changed (recompiled first).
	enum class restart { none, game, game_and_code };
	virtual restart menu_closed() { return restart::none; }

	// Before each game frame
	virtual void before_frame() {}

	// M1_TRACE's six columns of the game's own state (VF: the fighters' health, rounds, the fight's
	// state); zeros when the game has none
	virtual void trace_fields(unsigned (&fields)[6]) const {}
};

// The game this host binary runs (each game's host.cpp has it)
std::unique_ptr<game_host> make_game_host();
