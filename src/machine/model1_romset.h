// A Model 1 game's MAME set: every ROM checked by CRC and laid out in the board's regions, as
// MAME's ROM_START does (and tools/romset.py, for development builds). The games give the tables
// (src/games/*/romset.cpp); the CPU board's own ROMs and the I/O board's are common to all of them.
#pragma once
#include "model1.h"
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace romset {

enum region_id { MAINCPU, COPRO_TABLES, COPRO_DATA, OTHER_DATA, OTHER_OTHER_DATA, SNDCPU, PCM1, PCM2, POLYGONS, TGP_PROGRAM, N_REGIONS };

// raw; byte16: every other byte; word32: 16-bit words into every other 32-bit slot;
// wordswap: 16-bit words byteswapped; byte32: every fourth byte
enum kind { RAW, BYTE16, WORD32, WORDSWAP, BYTE32 };

struct rom
{
	const char *name;
	uint32_t crc;
	region_id region;
	uint32_t offset;
	kind k;
	const char *alias = nullptr;   // the name some sets give it
};

struct region_def { size_t size = 0; uint8_t fill = 0; };   // size 0: the game has no such region

// A ROM outside the classic set that the game still needs (Virtua Fighter's TGP program, which
// only newer MAME sets have).
struct supplement { const char *name; uint32_t crc; const char *missing; };

struct definition
{
	game::id title;
	const char *zip;            // "vf.zip"
	const char *mame_name;      // "vf"
	const char *display_name;   // "Virtua Fighter"
	std::array<region_def, N_REGIONS> regions;
	std::span<const rom> roms;  // the game's own (the CPU board's are added)
	uint32_t main_crc;          // a ROM only this set has: tells it from another zip when picked
	const supplement *tgp_program = nullptr;   // when it is not in the classic set (and so not in roms)
};

// in a game's romset.cpp: static const romset::registration REGISTERED{ ROMSET };
struct registration
{
	explicit registration(const definition &d);
};
// the sets in this binary, by the game's short name ("vf"): nullptr for one it doesn't have
const definition *find(std::string_view game_name);
std::vector<const definition *> registered();

// The regions every Model 1 board has (the TGP's data ROMs, its program and the extra regions are
// per game).
constexpr region_def COMMON_REGIONS[N_REGIONS] = {
	{ 0x2000000, 0xff }, { 0x40000, 0 }, {}, { 0x80000, 0 }, { 0x20000, 0 },
	{ 0xc0000, 0 }, { 0x400000, 0 }, { 0x400000, 0 }, { 0x1000000, 0 }, {},
};

struct result
{
	bool ok = false;
	std::string error;              // for the player: what's missing or wrong
	model1::regions regions;
	bool has_tgp = false, has_ioboard = false;
};

// zips: the game's set, and optionally model1io.zip (the I/O board's program) or a newer set
// with more in it. Every file in them is looked at; the ROMs are found by name or by CRC.
result load(const definition &set, const std::vector<std::string> &zips);

// Whether a zip holds this game (enough to tell its set from model1io.zip or another game's).
bool is_game_set(const definition &set, const std::string &zip);

} // namespace romset
