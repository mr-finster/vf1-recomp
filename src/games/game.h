#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class model1;

namespace game {

// A game, by its short name ("vf", "vr"): the regions folder build/<name>, its assets/games/<name>.
// Each game's folder (src/games/<name>/) defines its own and registers its definition (below).
struct id
{
	std::string_view name;
	friend constexpr bool operator==(id, id) = default;
	explicit operator bool() const { return !name.empty(); }
};
enum class host_axis : int8_t { none = -1, left_x, left_y, right_x, right_y, left_trigger, right_trigger };
enum class analog_kind : uint8_t { centered, pedal };

struct profile
{
	id title;
	std::string_view display_name;
	std::string_view romset;
	std::string_view recompiler_map;
	std::string_view make_regions;   // how a development build gets the regions (src/host/regions.h)
	// work RAM (0x500000 on) the shared code reads: the camera pointer the game last sent (0 between
	// scenes, and until the boot's over), and the game's mode byte, test_menu_mode in its test menu
	uint32_t camera_ram;
	uint32_t mode_ram;
	uint8_t test_menu_mode;
};

struct io_profile
{
	// Active-low input bytes, ordered [system, player 1, player 2], with bit 0 first.
	std::array<std::string_view, 24> digital_controls;
	// ADC channels 0-7; the I/O board's secondary switch selects channels 4-7.
	std::array<std::string_view, 8> analog_controls;
	std::array<std::string_view, 8> analog_environment;
	std::array<uint8_t, 8> analog_neutral;
	std::array<host_axis, 8> host_axis_defaults;
	std::array<analog_kind, 8> analog_kinds;
};

struct tgp_hle_callbacks
{
	std::function<void()> fifo_push;
	std::function<void()> reset;
};

struct gameplay_state
{
	virtual ~gameplay_state() = default;
};

struct gameplay_callbacks
{
	std::shared_ptr<gameplay_state> state;
	std::function<void()> frame_begin;
	std::function<void()> frame_end;
	std::function<std::optional<uint16_t>(uint32_t)> backdrop_tile;
	std::function<std::vector<uint32_t>(std::string_view)> screen_hashes;
	std::function<std::optional<std::string_view>(uint32_t)> screen_name;
	std::function<bool(uint32_t, bool)> wide_scene;
};

// What the shared video code (src/machine) needs to know about a game's own data: none of it is
// the board's, each is where one game keeps something in its ROM or RAM.

// A stage sky's panorama record in the program ROM (see model1_sky.cpp). `top`: a one-row record
// the game streams over the panorama's top row (0: none).
struct sky_record
{
	std::string_view name;
	uint32_t rom;
	uint32_t top = 0;
};

// The RAM the game's sky-scroll routine reads (byte addresses in RAM B, see model1_interp.cpp):
// the camera's heading (1/32 pixel) and height, the last game frame's, and the byte whose bit 0
// marks the video frame between two game frames.
struct sky_camera
{
	uint32_t heading, height, last_heading, last_height, between;
};

// A texture block the game leaves unset (all FFFF) that the cabinet shows in one palette pen,
// for the polygons in [poly_first, poly_last] only.
struct unset_texture_pen
{
	uint32_t poly_first, poly_last, texture;
	uint16_t pen;
};

struct video_traits
{
	bool smooth_frames = false;      // the 3D blended between game frames (a 30 Hz game's)
	// polygon ROM addresses below this are the scenery a moving model's shadow falls on, which
	// above 1x has its shadows kept off the models (0: no such split, shadows drawn as the arcade)
	uint32_t scenery_polys_end = 0;
	std::vector<sky_record> skies;    // empty: no hi-res skies
	int sky_tile_map = 0, sky_first_row = 0;   // where the game streams the panoramas
	std::optional<sky_camera> sky_scroll;      // none: the sky's scroll as the tile chip has it
	std::vector<unset_texture_pen> unset_texture_pens;
};

class auto_play;
struct macro;

// What a game gives the shared code. Each game's folder has one (src/games/<name>/game.cpp) and
// registers it when the program starts, so a binary has the games it links and nothing here
// names them.
struct definition
{
	const profile *info = nullptr;
	const io_profile *io = nullptr;
	const video_traits *video = nullptr;
	tgp_hle_callbacks (*tgp_hle)(model1 &) = nullptr;           // none: the TGP runs its own program
	gameplay_callbacks (*gameplay)(model1 &) = nullptr;
	// scripted play (src/games/script.h): nullptr for a scenario the game hasn't
	std::unique_ptr<auto_play> (*play)(const std::string &scenario, uint32_t seed, bool checks) = nullptr;
	bool (*macros)(const std::string &cmd, std::istream &args, macro &out) = nullptr;   // nullptr: none
};

// in a game's game.cpp: static const game::registration REGISTERED{ { &PROFILE, ... } };
struct registration
{
	explicit registration(const definition &d);
};

// the game's definition; an unknown game ends the program with a message
const definition &get_definition(id title);
// the games in this binary, by name
std::vector<id> registered();

tgp_hle_callbacks make_tgp_hle(id title, model1 &machine);
gameplay_callbacks make_gameplay(id title, model1 &machine);
const video_traits &get_video(id title);

const profile &get(id title);
const io_profile &get_io_profile(id title);
// by its short name ("vf", "vr"), among the games in this binary
std::optional<id> find(std::string_view short_name);

}
