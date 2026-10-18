#include "romset.h"
#include "profile.h"

namespace game::vf {

namespace {

using namespace romset;

// MAME's ROM_START(vf), past the CPU board's ROMs (as tools/romset.py)
const rom ROMS[] = {
	{ "epr-16082.14", 0xb23f22ee, MAINCPU, 0x200000, BYTE16 },
	{ "epr-16083.15", 0xd12c77f8, MAINCPU, 0x200001, BYTE16 },
	{ "epr-16080.4", 0x3662e1a5, MAINCPU, 0xfc0000, RAW },
	{ "epr-16081.5", 0x6dec06ce, MAINCPU, 0xfe0000, RAW },
	{ "mpr-16084.6", 0x483f453b, MAINCPU, 0x1000000, BYTE16 },
	{ "mpr-16085.7", 0x5fa01277, MAINCPU, 0x1000001, BYTE16 },
	{ "mpr-16086.8", 0xdeac47a1, MAINCPU, 0x1100000, BYTE16 },
	{ "mpr-16087.9", 0x7a64daac, MAINCPU, 0x1100001, BYTE16 },
	{ "mpr-16088.10", 0xfcda2d1e, MAINCPU, 0x1200000, BYTE16 },
	{ "mpr-16089.11", 0x39befbe0, MAINCPU, 0x1200001, BYTE16 },
	{ "mpr-16090.12", 0x90c76831, MAINCPU, 0x1300000, BYTE16 },
	{ "mpr-16091.13", 0x53115448, MAINCPU, 0x1300001, BYTE16 },
	{ "epr-16120.7", 0x2bff8378, SNDCPU, 0x00000, WORDSWAP },
	{ "epr-16121.8", 0xff6723f9, SNDCPU, 0x20000, WORDSWAP },
	{ "epr-16121.8", 0xff6723f9, SNDCPU, 0x80000, WORDSWAP },   // ROM_RELOAD
	{ "mpr-16122.32", 0x568bc64e, PCM1, 0x000000, RAW },
	{ "mpr-16123.33", 0x15d78844, PCM1, 0x200000, RAW },
	{ "mpr-16124.4", 0x45520ba1, PCM2, 0x000000, RAW },
	{ "mpr-16125.5", 0x9b4998b6, PCM2, 0x200000, RAW },
	{ "mpr-16096.26", 0xa92b0bf3, POLYGONS, 0x000000, WORD32 },
	{ "mpr-16097.27", 0x0232955a, POLYGONS, 0x000002, WORD32 },
	{ "mpr-16098.28", 0xcf2e1b84, POLYGONS, 0x400000, WORD32 },
	{ "mpr-16099.29", 0x20e46854, POLYGONS, 0x400002, WORD32 },
	{ "mpr-16100.30", 0xe13e983d, POLYGONS, 0x800000, WORD32 },
	{ "mpr-16101.31", 0x0dbed94d, POLYGONS, 0x800002, WORD32 },
	{ "mpr-16102.32", 0x4cb41fb6, POLYGONS, 0xc00000, WORD32 },
	{ "mpr-16103.33", 0x526d1c76, POLYGONS, 0xc00002, WORD32 },
};

// Required: without the TGP's own program the game falls back to MAME's stand-in for the chip,
// which gets the fighters' hair wrong (Sarah's ponytail and Pai's braid end up on the floor)
const supplement TGP = { "315-5724.bin", 0x4b4f330e,
	"Your vf.zip is missing 315-5724.bin (the TGP chip's program). Newer MAME sets include "
	"it: use one of those, or add the file to your vf.zip." };

constexpr std::array<region_def, N_REGIONS> regions()
{
	std::array<region_def, N_REGIONS> r{};
	for (int i = 0; i < N_REGIONS; i++) r[i] = COMMON_REGIONS[i];
	return r;
}

} // namespace

const romset::definition ROMSET = {
	ID, "vf.zip", "vf", "Virtua Fighter", regions(), ROMS,
	0x3662e1a5,   // epr-16080.4, the main program
	&TGP,
};

static const romset::registration REGISTERED{ ROMSET };

} // namespace game::vf
