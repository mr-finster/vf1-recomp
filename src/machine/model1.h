// Sega Model 1, as Virtua Fighter uses it: memory map, interrupts, the TGP coprocessor as
// C functions (MAME 0.210's simulation) and the polygon rasterizer (MAME's model1_v.cpp).
// Ported from MAME's Model 1 driver, BSD-3-Clause, copyright-holders: Olivier Galibert.
#pragma once
#include <algorithm>
#include <cstdlib>
#include "parallel.h"
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <array>
#include <future>
#include <functional>
#include <string>

#include "mame_lite.h"
#include <map>
#include "s24tile.h"
#include "../gpu/gpu3d.h"
#include "m1audio.h"
#include "model1io.h"
#include "../core/v60_device.h"

#include <glm/vec3.hpp>

#define TGP_FUNCTION(name) void name()
enum { MAT_STACK_SIZE = 32 };

class model1 : public v60_bus
{
public:
	// Screen: 496x384, 60 Hz (MAME: 16 MHz pixel clock, 656x426 total)
	static constexpr int SCREEN_W = 496, SCREEN_H = 384;

	struct regions
	{
		std::vector<uint8_t> maincpu, copro_tables, polygons, sndcpu, pcm1, pcm2;
		std::vector<uint8_t> tgp_program;   // 315-5724.bin, when the set has it
		std::vector<uint8_t> ioboard;       // epr-14869b.25 (model1io), when there
	};

	// Testing (the host's VF_TRACE): the one-off work done since the counts were last cleared.
	struct work_trace {
		int tiles_full = 0, layers = 0, sky_build = 0, sky_scale = 0, sky_scale_sync = 0, padded = 0;
		uint64_t tiles_ns = 0, layers_ns = 0, sky_build_ns = 0, sky_scale_ns = 0, padded_ns = 0, edge_ns = 0;
	} m_work;
	static uint64_t trace_ns();

	explicit model1(regions &&r);
	void reset();
	// One video frame: CPU time, the vblank IRQ, the display-list scan and the render.
	void run_frame();
	const bitmap_rgb32 &frame() const { return m_bitmap; }
	// Runs the TGP from its own program (315-5724.bin) instead of MAME 0.210's simulation.
	bool use_tgp_program(const std::vector<uint8_t> &rom);
	bool tgp_is_lle() const { return bool(m_lle); }
	// Renders the 3D at `scale` times the arcade's 496x384 (1 = the arcade picture, exact).
	void set_render_scale(int scale);
	// Widescreen: `aspect` is the picture's width/height (4/3 = the arcade's). The 3D view and the
	// sky widen; the 2D text stays in the middle 4:3.
	void set_aspect(float aspect);
	float aspect() const { return m_aspect; }
	int render_scale() const { return m_scale; }
	int frame_w() const { return m_bitmap.width(); }
	int frame_h() const { return m_bitmap.height(); }
	v60_device &cpu() { return m_maincpu; }
	// Battery-backed RAM (0x400000-0x40ffff) and the I/O board's EEPROM (the real board's 93C46,
	// or the stand-in's image in the dual-port RAM at 0x100-0x17f).
	std::vector<uint8_t> &nvram() { return m_ram_a; }
	// The ranking's character names for random fighters and secret characters (the game keeps
	// Akira's there): kept by the host beside the backup, a line an entry ("<entry, hex> <name>")
	std::string ranking_names() const;
	void set_ranking_names(const std::string &text);
	uint32_t ranking_names_version() const { return m_rank_version; }
	std::vector<uint8_t> &work_ram() { return m_ram_b; }   // 500000-53ffff
	uint16_t tile_word(uint32_t o) const { return m_tiles.tile_r(o); }   // 700000 + 2*o
	uint16_t peek16(uint32_t a) { return bus_read16(a); }               // the V60's view (ROM, RAM)
	uint32_t bank_base() const { return m_bank_base; }                   // ROM offset of 100000-1fffff
	// Tile map entries (w x h) drawn with the current tiles and palette, for exporting artwork.
	void render_tiles(const uint16_t *e, int w, int h, uint32_t *out) { m_tiles.update(m_pens); m_tiles.render_entries(e, w, h, out); }
	// Tile map l (0-3) as a 512x512 picture in the current palette, for exporting the artwork.
	void tilemap_image(int l, std::vector<uint32_t> &out) { out.resize(512 * 512); m_tiles.update(m_pens); m_tiles.render_map(l, out.data()); }
	// Sets the game's coin/credit setting to FREE PLAY in the real I/O board's EEPROM (read at
	// boot: reset afterwards). False when it already was, or there are no settings yet.
	bool force_free_play() { return set_coin_setting(FREE_PLAY); }
	// The coin/credit setting itself (word 8's high byte, then word 10's): -1 before the game has
	// written its settings; set_ is false when nothing changed. 0: the game's own first setting
	// (1 coin 1 credit, "CREDIT 0" on screen).
	static constexpr int FREE_PLAY = 0x1901;
	int coin_setting();
	bool set_coin_setting(int v);
	// The test mode's GAME ASSIGNMENTS, in the same EEPROM (read at boot: reset after a change).
	// -1 when the game hasn't written its settings yet; set_ is false when nothing changed.
	enum class game_assignment { match_1p, match_vs, difficulty, advertise_sound, continues, language };
	int assignment(game_assignment a);
	bool set_assignment(game_assignment a, int v);
	// The ring's width (1200-1500) whatever the difficulty, or 0 for the arcade's (by difficulty,
	// which the test mode shows as STAGE WIDTH). From the next match; false if the table isn't VF's.
	bool set_ring_width(int width);
	// Cheats' Kage's mask: his hood off when knocked off, as after nine hours of play (the backup
	// RAM's 40e009 bit 0, set by the game's play clock at 40e004-8): held on while on; off puts back
	// what the clock earned
	void set_kage_unmasked(bool on);
	bool m_kage_unmasked = false;
	// The unused characters left in the game (TCRF: Jeff, Majido, Takeru, the Koskol brothers)
	// play in Akira's place: his entries in the character tables point at theirs. 0 is Akira; the
	// rest are UNUSED_CHARACTERS. Read as a fight loads, so a change wants a restart.
	static constexpr const char *UNUSED_CHARACTERS[] = { "Akira", "Siba (Majido)", "Jeff", "Takeru (early Akira)",
														 "Rekka (Koskol brother)", "Score (Koskol brother)", "Aeon (Koskol brother)", "Dural" };
	void set_akira_slot(int which);
	void akira_slot_heads(uint32_t record);   // (set_akira_slot: a select portrait for a head without one)
	// The select's icon for the character in Akira's place, in his icon's tiles (56 x 64 pixels,
	// xRGB, 15 colours at most; nullptr: his own). Returns false for a picture it can't show.
	bool set_akira_icon(const uint32_t *xrgb);
	static const char *const SECRET_ICON_NAMES[];   // (by UNUSED_CHARACTERS: assets/icons/NAME.png, nullptr: none)
	// Cheats, before each frame: energy kept full for the players in `infinite` (bit 0 player 1,
	// bit 1 player 2), the rest left with 1 when `one_hit` (any hit a knockout), and the round's
	// clock stopped when `no_time`.
	void fight_cheats(int infinite, bool one_hit, bool no_time);
	bool set_no_time_limit(bool on);   // (true: the game's code changed, to be recompiled)
	bool m_clock_held = false;
	struct secret_icon {
		bool on = false, active = false;   // a picture set; in the tiles now
		uint32_t at = 0;                   // where the icon's map is (tile RAM word)
		std::vector<uint16_t> chars, pal, map;            // ours: the tiles, the colours, the map
		std::vector<uint16_t> keep_chars, keep_pal, keep_map;   // VF's own, put back after
	} m_icon;
	void secret_icon_frame();
	// Cheats: a random fighter in Akira's place, a patchwork of the cast's (and the cut characters')
	// body parts, a pair of limbs from one of them, and a head of one, made afresh each time the
	// player select comes up. Both of Akira's costumes (player 1's and player 2's) get their own.
	void set_random_fighter(bool on, bool moves = false, bool cpu = false);
	// Cheats' fighter names: a name picks the players' random fighter (the same one every game, on
	// every machine): the first of seeds 1-RANDOM_SEEDS whose fighter has that name. Empty: none, a
	// new one each game. False (nothing changed) for a name no seed gives
	static constexpr uint32_t RANDOM_SEEDS = 32768;
	// Each player's fighter with Random fighter on: "" the select as ever (Akira is Akira), "*" a
	// random one, a new one each game, or a name's. False (nothing changed) for a name no seed gives.
	// Applied as the fighters are next made (set_random_fighter, the select)
	bool set_random_player(int pl, const std::string &setting);
	const std::map<std::string, uint32_t> &random_fighter_names();   // (every name, its seed; made once)
	// The strands' lookup (an instruction of the game's) to our list, and the special commands'
	// lookup to ours by record, for Random fighter: before the code is recompiled (true: it changed)
	void strand_log();   // TESTONLY
	bool set_strands_moved(bool on);   // moves: each command's move from a random fighter too; cpu: every opponent random too
	bool m_random_fighter = false, m_random_moves = false, m_random_cpu = false, m_select_seen = false;
	static constexpr uint32_t RECORD_SIZE = 0x176, MOVE_COUNT = 187;
	struct random_cast {
		bool made = false;
		std::map<uint32_t, std::vector<uint8_t>> orig;   // each record's own
		std::vector<uint32_t> recs, heads, tables;       // the records, those with a head list, the moves tables
		std::map<std::string, std::vector<uint32_t>> by_name;   // each character's records (costumes)
		std::vector<std::pair<uint32_t, uint32_t>> move_groups;   // the moves tables' commands in groups (first, end)
		std::map<uint32_t, uint8_t> flags;               // each record's flags byte (+0d) with its key
		std::unordered_map<uint16_t, float> move_lean;   // each move's character's head lean (NaN: shared by several)
	} m_random_cast;
	std::map<uint32_t, uint32_t> m_special_list;   // (each random record's special commands: its list)
	std::map<uint32_t, uint32_t> m_stance_from;    // (each random record's stance: the moves table it came from)
	std::map<uint32_t, uint8_t> m_voice_of;        // (each random record's voice: a character's number, VOICE_OURS)
	std::map<uint32_t, std::array<uint8_t, 6>> m_move_ids;   // (each random record's character number for VF's checks by character on its moves, MOVE_SITES: the one whose command it has)
	float m_look_share = 1;                         // (the fighter just made: the share of its looks one character's, random_fighter_make)
	bool m_look_same_body = false;                  // (... its face and torso one character's)
	std::map<uint32_t, uint32_t> m_moves_held;     // (the random records' moves tables, held back while the boot builds the stances' table)
	uint64_t m_boot_at = 0;                        // (the frame of the last reset)
	void random_fighter_hold_moves();
	void random_fighter_specials();
	uint32_t m_random_seen[2] = { 0, 0 };   // (each player's fighter last seen in their block)
	// (Random opponents: a credit's run, its select to GAME OVER (continues in it): the computer's fighters as they
	// were fought, kept through a continue's select and shown so in the ending's replays, not made again
	// nor built again)
	bool m_run_on = false;
	int m_run_idle = 0;                   // (frames with no one in)
	std::set<uint32_t> m_fought;          // (the computer's records this run)
	std::set<uint32_t> m_built_run;       // (the computer's records built in this run)
	struct cpu_kept {   // (a random record as it was: its bytes and what it's known by)
		std::vector<uint8_t> bytes; std::string name; float hue = 0, outfit = 0; int theme = 0; uint32_t stance = 0, list = 0;
		bool has_hue = false, has_outfit = false, has_stance = false, has_list = false, has_move_ids = false; int voice = -1; std::array<uint8_t, 6> move_ids{};
	};
	std::map<uint32_t, cpu_kept> m_cpu_akira;   // (the computer's Akira as fought this run, by record: put back at a continue, onto the one no player has)
	int m_dural_lm = -2;                  // (Dural's own polygons' light mode, her shine: -2 not looked for yet, -1 none)
	int dural_lightmode();
	uint32_t m_cpu_human[2] = { 0, 0 };    // (the records the players picked at the last select: random_fighter_cpu)
	bool cpu_keeps(uint32_t r) const;
	void random_fighter_ids();             // (a random record's character number: Akira's in a match, its own outside one)
	int m_akira_track = -1;                // (VF's stage music for Akira, fd04f8's entry 11: random_fighter_ids)
	random_cast &random_fighter_cast();
	uint32_t random_next(uint32_t n);
	void random_fighter_make(uint32_t rec);
	void random_fighter_make_named(uint32_t rec);   // (Random's: one of the named fighters, random_fighter_names)
	void random_fighter_colour(uint32_t rec);
	void random_fighter_strands();   // (the strands' list for every slot, as the records are now)
	std::vector<uint8_t> m_strands_orig;   // (the fight's swinging-strand list, fda917, as VF has it)
	bool m_strands_moved = false, m_specials_moved = false;
	bool m_random_applied = false;   // (records made random since the cast was last put back)
	std::map<uint32_t, std::string> m_random_names;   // (each random record's name, from what it's made of)
	static const char *record_name(uint32_t rec);
	static std::string blend_name(const std::map<std::string, int> &made_of);
	void random_fighter_hud();
	static constexpr uint32_t RANKING = 0xf800, RANK_ENTRIES = 18, RANK_ENTRY = 10;   // (in the backup RAM, 40f800: time, 0, character, initials, 0)
	uint8_t m_rank_prev[RANK_ENTRIES * RANK_ENTRY] = {};
	bool m_rank_seen = false;
	std::string m_rank_names[RANK_ENTRIES];
	uint32_t m_rank_version = 0;
	void ranking_frame();
	int m_akira_which = 0;   // (Cheats' Secret character: set_akira_slot's which, 0 none)
	uint8_t m_akira_ribbon[2] = { 0, 0 };   // (set_akira_slot's portrait slot bytes for Akira's places, fd2b60 and fd2b5b; 0 not set yet)
	void select_cover(bool select);
	void random_fighter_cpu();
	// Random fighter's round-1 build (random_fighter_build): as a match loads, a random fighter's
	// parts are built from the feet up during the intro's camera sweep, polygon by polygon along each
	// part's strip, each polygon a white flash as it lands, grey until all are on; then the colour
	// sweeps up the same way before READY. Each player's: its record, the frame it starts, each of its
	// models' group (0 feet ... 8 head) and polygon count
	struct build_model { int group, polys; };
	struct fighter_build { uint32_t rec = 0; int64_t start = -1; std::unordered_map<uint32_t, build_model> group; } m_build[2];
	std::unordered_set<uint32_t> m_build_models[2];   // (each player's record's models, built or not: a shared one's owner by the nearer torso)
	bool m_build_select = false;   // (a select showing last frame: its end builds every record afresh)
	void random_fighter_build(bool select);
	struct build_at { int64_t e; int appear, colour, polys; };   // (a model's: frames into the build, its polygons' start and colour's)
	bool build_info(uint32_t poly_adr, build_at &b) const;   // (model1_video.cpp: false for a model as ever)
	static int build_front(int64_t e, int start, int span, int polys) { return e < start ? 0 : int(std::min<int64_t>((e - start) * polys / span + 1, polys)); }   // (its polygons on by e)
	static constexpr int BUILD_DELAY = 8, BUILD_STEP = 6, BUILD_SPAN = 16, BUILD_FLASH = 4, BUILD_COLOUR = 70, BUILD_COLOUR_STEP = 2, BUILD_COLOUR_SPAN = 10;   // (frames: the sweep's close half, READY's start, colour fully in before the fight)
	void secret_akira_record(uint32_t rec, int which);
	void akira_portrait_of_head(uint32_t rec, uint32_t head);
	void akira_camera(int which);
	int akira_camera_for(uint32_t rec);
	int m_akira_cam_now = -1;
	std::map<uint32_t, std::string> m_secret_names;   // (Akira's records holding a player's secret character: its name)
	bool m_cpu_pending = false, m_rolling_at_select = false;   // (Random opponents' records to make when the select is over)
	bool select_showing() const;
	uint32_t m_secret_recs[2] = { 0, 0 };   // (Cheats' Secret character: its records in Akira's place, else 0)
	// Heads worn on another's body, turned to sit as on their own (by poly address: the cosine and
	// sine of a turn about the head's own side axis; see random_fighter_heads)
	// and the hue its hair, hats and bands take (skin kept; -1: as they are)
	struct head_style { float c = 1, s = 0, hue = -1, metal = -1, bright = 1; int pl = -1; bool strand = false; };   // (metal >= 0: Dural's head, tinted to that saturation, its brightness scaled; pl: whose; strand: hair that swings)
	// A head on another's torso: its neck's foot can sit above the torso's top (another's skeleton,
	// a shorter torso): the head (and its strands) brought down onto it, in the torso's own frame
	// (model1_video.cpp, neck_*). Each player's torso as last drawn, and the head's lowest point
	// near the neck this picture, for the next
	struct neck_fit {
		bool torso = false;              // (seen: a, inv, t, top, axis, sign hold)
		float a[9] = {}, inv[9] = {}, t[3] = {};   // its matrix (columns), the inverse of its 3x3, its place
		float lo[3] = {}, hi[3] = {};    // its model's extent, its own axes
		int axis = 1; float sign = 1;    // toward its head
		float head_low = 0; bool head_seen = false;   // (this picture's, along axis*sign)
		float drop = 0;                  // how far the head comes down (the last picture's gap)
		uint32_t torso_poly = 0;
		float pitch = 0, rel[9] = {}, view[9] = {}; int pitch_seen = 0;   // TESTONLY (VF_HEADLOG): the head against its torso, and as drawn
	};
	neck_fit m_neck[2];
	mutable int m_owner_last = -1;   // (the player whose model was drawn last, not a shared one: model_owner)
	uint32_t m_torso_poly[2] = { 0, 0 };
	int m_torso_seen = 0;
	std::unordered_map<uint32_t, std::vector<std::array<float, 3>>> m_model_points;   // (a model's own points, by its poly address)
	const std::vector<std::array<float, 3>> &model_points(const float *poly_data, uint32_t poly_adr);
	void neck_torso(int pl, const float *poly_data, uint32_t poly_adr);
	bool neck_head(int pl, bool strand, const float *poly_data, uint32_t poly_adr, float tc, float ts, float *off);
	void neck_settle();
	void dump_part(uint32_t tex_adr, uint32_t poly_adr, uint32_t size);   // TESTONLY
	std::unordered_map<uint32_t, head_style> m_head_turn;
	// (a model both players wear, the same head on both: the first drawn in a list is player 1's,
	// the second player 2's, with player 2's style here; the game draws player 1's first)
	std::unordered_map<uint32_t, head_style> m_head_turn2;
	std::unordered_map<uint32_t, int> m_head_seen;
	std::map<uint32_t, float> m_random_hue;   // (each random record's head's hue, 0-1)
	std::map<uint32_t, float> m_random_outfit;   // (each random record's clothes' turn round the colour wheel, 0-1)
	std::map<uint32_t, int> m_random_theme;      // (... and their theme: OUTFIT_THEMES)
	struct outfit_tones {
		uint32_t n[32 * 16] = {};   // (polygons drawn at each brightness (32) and hue (16): their median is the split)
		uint64_t at = ~0ull, below = 0, total = 0; int med = 0;   // (the split as the frame began: once a frame, not a polygon)
	};
	std::unordered_map<uint32_t, outfit_tones> m_outfit_tones;
	struct skin_sum { double h = 0, s = 0, v = 0; uint64_t n = 0; };
	std::unordered_map<uint32_t, skin_sum> m_skin_ref, m_skin_part;
	// (a player's record just changed: the last one's models and colours, as they were, for the
	// pictures of the old scene still drawn before the cut (its clothes and skin snapped back to the
	// cast's for a frame, or took the record's new ones as Random opponents remade it))
	struct build_prev { uint32_t rec = 0; int64_t until = -1; std::unordered_set<uint32_t> models; float outfit = -1; int theme = 0; outfit_tones tones; skin_sum skin; bool has_skin = false; } m_build_prev[2];
	static constexpr int BUILD_PREV_FRAMES = 12;   // (one skin a fighter: each record's face's skin, each body model's own)   // (each record's clothes' brightnesses seen: black and white's, black and gold's split)
	static constexpr int BUILD_SCENE_LAG = 4;   // (a record changed, the old scene's pictures go on for 3 frames: the new scene's from the 4th)
	enum { THEME_TURN, THEME_BW, THEME_BLACK_GOLD, THEME_COOL, THEME_WARM, THEME_OPPOSITE, THEME_NEON, THEME_PASTEL, OUTFIT_THEMES };
	int model_owner(uint32_t poly_adr) const;   // (a record's model: whose, by the nearer torso if both's; -1 neither's)
	// each player's styled models last frame, and those of the record before for a few frames after
	// it changes (the picture is a few frames behind the players' blocks)
	std::vector<std::pair<uint32_t, head_style>> m_head_last[2], m_head_lag[2];
	uint32_t m_head_rec[2] = { 0, 0 };
	float m_lean_body[2] = { NAN, NAN }, m_lean_turn[2] = { 0, 0 };   // (each player's: the move's character's lean, last known; the head's turn now, degrees)
	int m_head_lag_left[2] = { 0, 0 };
	void random_fighter_heads();
	void strand_polys(int pl, std::vector<uint32_t> &out) const;
	void random_fighter_strands_settle();
	std::map<uint32_t, std::vector<float>> m_strand_roots;   // (each strand task's chains' first points last frame: x y z, and 1 if still as the game started it)
	static float head_lean(uint32_t rec);
	int64_t m_pick_at = -1;   // (the frame the select came up: the random fighter is picked for the players)
	uint32_t m_random_state = 0;
	int m_player_mode[2] = { 1, 1 };                    // (each player's: 0 the select as ever, 1 random, 2 a name's)
	uint32_t m_player_seed[2] = { 0, 0 };
	std::map<uint32_t, std::pair<int, uint32_t>> m_akira_made;   // (each Akira record as made: mode, seed)
	void random_akira_record(uint32_t rec, int mode, uint32_t seed);
	void random_fighter_picks();
	uint32_t akira_record_of(int pl) const;
	uint32_t m_akira_held[2] = { 0, 0 };   // (the Akira record each random player picked, kept while it plays)
	void random_fighter_portrait_strands();
	std::map<uint32_t, std::vector<uint32_t>> m_portrait_strand_models;   // (each Akira record's portrait strands' models: its hue)
	bool picked_now(int pl) const;
	std::map<std::string, uint32_t> m_seed_names;
	void random_fighter_roll();
	void random_fighter_frame();
	// (the select hidden: one player alone at it, with a random fighter: who they got is a surprise
	// when the fight comes in. Two players in (from the start, or a challenger): it shows, portraits
	// and all)
	// (shown: the picture's question. The select's first frames, before the game marks who's in (its
	// words come a frame or more after it): hidden while a random fighter is set, as it may be the one
	// playing alone: it showed for a frame before. The pick's pace (run_frame) asks only once they're in)
	bool random_select_hidden(bool shown = true) const
	{
		if (!m_random_fighter || !m_select_seen || m_ram_a.size() < 0x7600) return false;
		const bool in0 = m_ram_a[0x7483] & 0x08, in1 = m_ram_a[0x7503] & 0x08;
		if (!in0 && !in1) return shown && (m_player_mode[0] || m_player_mode[1]);
		if (in0 == in1) return false;
		return m_player_mode[in0 ? 0 : 1] != 0;
	}
	static constexpr int64_t RANDOM_PREVIEW = 140;   // (frames the select shows a random fighter's portrait before its pick, none picking as ever)
	// Replaces n bytes of the main CPU program, if their CRC-32 is `crc` (guards against other sets).
	bool patch_program(uint32_t addr, size_t n, uint32_t crc, const uint8_t *to);
	// Blanks the FREE PLAY notices (the attract/title one and the fight's): for the PC build,
	// which is always free play. Returns how many it found.
	int hide_free_play_text();
	void show_free_play_text();          // (puts back what hide_free_play_text changed)
	// Hi-res replacements for the stage skies (model1_sky.cpp), drawn above 1x. The names are
	// the background export's (stage_<name>.png); false for a name it doesn't know.
	static std::vector<std::string> sky_names();
	bool load_sky(const std::string &name, std::vector<uint32_t> xrgb, int w, int h);
	// The stages' own skies above 1x: softened and smoothly enlarged, or the original pixels.
	void set_sky_filter(bool smooth);
	// Antialiasing's Low: the 3D's edges smoothed after it's drawn (edge_aa.cpp), at the render
	// scale, for about a tenth of the cost of drawing 4 times the pixels
	void set_edge_aa(bool on) { m_edge_aa = on; }
	// The host smooths Low's edges on the GPU (gpu_compose's edge pass, src/gpu/edge_aa.hlsl): the
	// 3D then goes straight into its texture with Low too, nothing read back for the CPU's pass
	void set_gpu_edge(bool on) { m_gpu_edge = on; }
	bool m_gpu_edge = false;
	bool edge_aa() const { return m_edge_aa; }
	uint32_t backdrop_hash() const;
	int sky_identity();                 // the stage sky in the map (-1: none known), see model1_sky.cpp
	// The full-screen 4:3 pictures (title, player select) with their sides filled for widescreen:
	// <name>.png at the size of the 16:9 canvas the export made (the arcade picture in the middle).
	// The middle stays the game's own pixels; the picture fills the sides. False for an unknown name.
	static std::vector<std::string> screen_names();
	bool load_screen(const std::string &name, std::vector<uint32_t> xrgb, int w, int h);

	// The boot is over (the game has sent its first camera: the attract has started).
	bool past_boot() const;
	// Frames run without drawing them (the display lists' uploads still happen, as MAME's skip).
	void set_drawing(bool on) { m_draw = on; }
	// Draws the picture last shown again, at the current settings (a change of scale, shape or
	// filter while paused or in the menu): the same moment, from what it was drawn from, even when
	// frames have run since without a new picture shown (a held scene change or camera cut). The
	// emulation doesn't move.
	void redraw();
	// Smooth motion (model1_interp.cpp): frames are kept instead of drawn, and render_smooth draws
	// the moment `now` (frame number and fraction), blending the last two game frames. `shown`:
	// the picture goes on screen (false: drawn only to look at, as for a held scene change).
	// smooth_cut forgets the older one (after a held scene change).
	void set_smooth(bool on);
	bool smooth() const { return m_smooth; }
	bool render_smooth(double now, bool shown = true);   // false: nothing new drawn (the picture before a cut stays)
	void smooth_cut();
	// frame() holds a picture that wasn't shown (a held scene change, or a picture put together by
	// the GPU): redraw() puts back the one that was, before a still (paused, the menu) shows frame().
	bool frame_unshown() const { return m_unshown; }
	bool frame_in_parts() const { return m_unshown && m_frame_parts; }   // frame() holds only the 3D of a picture in parts

	// The picture in parts, for the GPU to put together (model1_gpu.cpp): render_smooth's picture for
	// the same moment, with the 2D left at the arcade's resolution (uploaded once a game frame, when
	// it changed) and enlarged by the GPU, and only the 3D at the render scale. The parts are
	// images (KEY: nothing there) and ops, drawn in order onto a frame_w x frame_h target:
	// a fill (tex < 0), or an image's source rectangle (texels) to a destination one (pixels),
	// nearest texel, KEY left out (TEX_SKY: copied whole), columns outside [clip_x0, clip_x1) left.
	// Every op is a copy of whole texels (a scale by the render scale, or 1:1) except the widescreen
	// stretch of a 4:3 backdrop, whose texel boundaries the GPU works out in float as the CPU does.
	static constexpr uint32_t KEY = 0xff000000;     // drawn pixels are 0x00rrggbb: never this
	// a shadow over KEY (| half its colour): blended with the 2D beneath when the 3D is done, as
	// the CPU blends it with what's drawn (parts_below), if no polygon has covered it by then
	static constexpr uint32_t SHADOW_KEY = 0xfd000000;
	static constexpr int PAD = 32;                  // arcade pixels round a moved backdrop's image
	enum { TEX_L6, TEX_L4, TEX_L6M, TEX_L4M, TEX_MID, TEX_TOP, TEX_SKY, TEX_3D, TEX_SIDES, TEX_COUNT };
	struct gpu_image { const uint32_t *px = nullptr; int w = 0, h = 0; size_t stride = 0; uint64_t gen = 0; };
	struct gpu_op { int tex = -1; float sx = 0, sy = 0, sw = 0, sh = 0, dx = 0, dy = 0, dw = 0, dh = 0; int clip_x0 = 0, clip_x1 = 1 << 30; uint32_t colour = 0; };
	struct gpu_parts {
		gpu_image img[TEX_COUNT];
		std::vector<gpu_op> ops;
		struct rect { int x0, y0, x1, y1; };       // [x0, x1) x [y0, y1)
		std::vector<rect> d3_changed;               // what changed in TEX_3D (the rest stays as it was)
		bool d3_all = false;                        // all of it
		int d3_target = -1;                         // the 3D drawn by the GPU into the host's target texture
		                                            // this many (gpu3d::draw_target), not into TEX_3D's image
		bool d3_edge = false;                       // Low's edge smoothing still to do, by the host's GPU
	};
	// 1: drawn in parts; 2: drawn whole into frame() instead (a 4:3 screen's painted sides; the
	// host leaves antialiasing to the CPU on its own); 0: nothing new (a cut's hold: the picture
	// shown stays).
	// shown false: a picture only looked at (scene_sync's, during a hold), as render_smooth's
	int render_parts(double now, gpu_parts &p, bool shown = true);
	bool frame_blank() const;
	// A sky being scaled that's done but not yet in use (sky_ready takes it a few frames after it
	// started): the host can upload it ahead, a part a refresh, so taking it costs nothing then.
	bool sky_pending(gpu_image &im) const;
	const gpu_parts *m_last_parts = nullptr;         // what frame() holds the 3D of, when in parts
	// the host puts pictures together from parts: a frame with no 3D is drawn in parts too
	void set_parts_mode(bool on) { m_parts_mode = on; }
	// the host has the game stopped (paused, the menu): no frames run until it goes on
	void set_stopped(bool on) { m_stopped = on; }
	// testing (VF_TRACE): the last smooth picture's blend, and the two lists' frames it was between
	void smooth_state(float &alpha, int &older, int &newer, float *place = nullptr) const;
	float m_last_alpha = 1;
	float m_pic_place[3] = { 0, 0, 0 };                // testing: the 3D's mean place in the picture last drawn (the trace)
	bool m_stopped = false;
	bool m_parts_mode = false;
	gpu_parts m_own_parts;
	// The parts put together on the CPU, pixel by pixel, as the GPU must (for checking a renderer):
	// out is w x h, 0x00rrggbb. `upto`: stop at the first op with that image (-1: all of them).
	static void compose_cpu(const gpu_parts &p, int w, int h, std::vector<uint32_t> &out);
	static uint32_t compose_pixel(const gpu_parts &p, int x, int y, int upto = -1);

	// Polygons drawn in the last frame, and the camera the game last sent (0 between scenes).
	int frame_quads() const { return m_last_quads; }
	uint32_t camera() const { const uint8_t *p = &m_ram_b[0x17f0]; return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
	uint8_t game_mode() const { return m_ram_b[0x3002]; }   // 3 in its TEST MENU, but also for a moment in attract
	uint8_t *eeprom() { return m_ioboard ? reinterpret_cast<uint8_t *>(m_ioboard->eeprom) : &m_dpram[0x100]; }
	static constexpr size_t EEPROM_SIZE = 0x80;
	bool real_ioboard() const { return bool(m_ioboard); }
	const std::vector<uint8_t> &maincpu_rom() const { return m_regions.maincpu; }   // for the recompiler
	uint64_t frame_number() const { return m_frame; }

	// Hooks for tracing and the recompiler
	std::function<void(uint32_t pc)> on_instruction;
	// Runs the main CPU for a slice of cycles; the interpreter unless the recompiled code is attached.
	std::function<void(int cycles)> run_cpu;
	// Called after each scanline (lockstep checks).
	std::function<void(int line)> on_line;

	// Dual-port RAM shared with the I/O board (inputs, coin, EEPROM), offsets as the V60 sees them.
	uint8_t m_dpram[0x800]{};
	// I/O board inputs, active low, as MAME's vf ports: [0] coins/service/start, [1] P1, [2] P2.
	uint8_t m_inputs[3] = { 0xff, 0xff, 0xff };
	uint8_t m_lamps = 0;
	// Sound: the board runs alongside when enabled; its output (stereo, m1audio::OUT_RATE)
	// collects here until the host takes it.
	void enable_sound();
	void set_volumes(float music, float effects);   // music / effects and voices, 1 = as the cabinet
	std::vector<int16_t> m_audio_out;

	struct spoint_t
	{
		int32_t x = 0, y = 0;
	};

	struct point_t
	{
		float x = 0, y = 0, z = 0;
		float xx = 0, yy = 0;
		spoint_t s;
		float fx = 0, fy = 0;   // s before truncation, for the crack-free filler above 1x
	};

	class quad_t
	{
	public:
		quad_t() { }
		quad_t(int ccol, float cz, point_t* p0, point_t* p1, point_t* p2, point_t* p3)
			: p{ p0, p1, p2, p3 }
			, z(cz)
			, col(ccol)
		{
		}

		int compare(const quad_t* other) const;

		point_t *p[4] = { nullptr, nullptr, nullptr, nullptr };
		float z = 0;
		int col = 0;
		uint32_t obj = 0;      // the model it came from (its polygon data's address)
	};

	// v60_bus
	uint8_t read8(uint32_t a) override;
	void write8(uint32_t a, uint8_t v) override;
	uint16_t read16(uint32_t a) override;
	void write16(uint32_t a, uint16_t v) override;
	uint32_t read32(uint32_t a) override;
	void write32(uint32_t a, uint32_t v) override;
	uint8_t io_read8(uint32_t a) override { return uint8_t(io_read16(a & ~1) >> ((a & 1) * 8)); }
	void io_write8(uint32_t a, uint8_t v) override { io_write16(a & ~1, v); }
	uint16_t io_read16(uint32_t a) override;
	void io_write16(uint32_t a, uint16_t v) override;
	uint32_t io_read32(uint32_t a) override { return io_read16(a) | (uint32_t(io_read16(a + 2)) << 16); }
	void io_write32(uint32_t a, uint32_t v) override { io_write16(a, v); io_write16(a + 2, v >> 16); }

private:
	regions m_regions;
	v60_device m_maincpu;
	segas24_tile m_tiles;
	bitmap_rgb32 m_bitmap;
	int m_scale = 1;
	int m_ext = 0;                   // extra arcade pixels each side in widescreen
	bool m_wide_frame = true;        // this frame uses the extra width (the fight); else 4:3 in the middle
	bool m_fight_cam = true;         // the last camera sent was the fight's (kept at 4:3 too)
	bool fight_camera() const;
	struct slot {
		float mat[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
		std::vector<uint32_t> polys;
		float light[3] = { 0, 0, 0 };                // the list's light vector (command 0xa) where this slot is
		bool has_light = false;
	};
	struct logic_frame {
		std::vector<uint16_t> list;                  // the display list as the game left it
		uint64_t hash = 0;
		int frame = 0;
		uint16_t hscr = 0, vscr = 0;                 // the sky map's scroll
		std::vector<struct slot> slots;              // matrix commands and the objects drawn with each
		int objects = 0;
		double move = -1;                            // how far the parts moved from the list before, the median (-1: not the same parts)
		double carried = 0;                          // still: the move before it, carried on through it
		bool still = false;                          // nothing moved, a list after everything did: an update the game skipped
		uint16_t moves[2] = { 0, 0 };                // (Random fighter) each player's move, and their slots
		std::vector<int> mine[2];                    // (each random fighter's slots: its parts, head and strands)
		int root[2] = { -1, -1 };                    // (and the one of its hips, in those)
		int head[2] = { -1, -1 };                    // (its head's)
		int torso[2] = { -1, -1 };                   // (its torso's)
		std::vector<uint8_t> is_head[2];             // (which are its head's models)
		std::vector<uint8_t> strand[2];              // (which of those are strands)
		std::vector<int> limb[2];                    // (a limb's part: the one it hangs from, in those; else -1)
	};
	// Random fighter: a random fighter's change of move, its old pose eased into the new one (each
	// part's turn and place about the hips, the difference fading out): pose_offsets
	static double list_move(const logic_frame &a, const logic_frame &b);
	struct pose_offset {
		std::vector<std::array<float, 7>> o;          // a turn (quaternion) and a move per slot, about the hips
		std::vector<uint64_t> key;                   // each one's part: its first model, and which of that model's (the order the game draws them in can change)
		std::vector<std::array<float, 7>> lo;         // and a limb's part's about the part it hangs from
		std::array<float, 7> root = { 1, 0, 0, 0, 0, 0, 0 };   // and the hips' own
		double start = -1;
		int armed = 0;                               // (lists still to look at after a change of move)
	};
	pose_offset m_pose_off[2];
	void pose_slots(logic_frame &lf);
	void joint_log(const logic_frame &lf);   // TESTONLY (VF_JOINTLOG)
	void pose_transition(const logic_frame &a, const logic_frame &b);
	static std::vector<uint64_t> part_keys(const logic_frame &lf, int pl);
	void pose_apply(const logic_frame &b);
	logic_frame m_logic[2];                         // the game frame before, and the last
	logic_frame m_logic_prev;                       // and the one before those (a cut's older view goes on from it)
	bool m_replay_extra = false;                    // the older list drawn carried on past its frame (m_blend from the one before)
	bool m_smooth = false, m_replaying = false;
	float m_alpha = 1;
	int m_smooth_stats[3] = {};                     // per picture: unmatched, jumped, blended objects
	std::vector<std::array<float, 12>> m_blend;     // per slot of the newer list, the blended matrix
	std::vector<uint8_t> m_blend_ok;
	// per slot of the newer list, what each polygon's shade is blended from: the older and newer
	// frames' matrices and the older frame's light (the newer's is the list's own as it's drawn)
	std::vector<std::array<float, 12>> m_blend_from, m_blend_to;
	std::vector<std::array<float, 4>> m_blend_light;   // x, y, z, and 1 when known
	// while an object is drawn blended: its shades too (push_object)
	const float *m_shade_from = nullptr, *m_shade_to = nullptr, *m_shade_light = nullptr;
	int m_replay_slot = -1;                          // the slot being drawn while replaying
	double m_draw_time = 0;                          // the moment being drawn, in frames
	bool m_shown = false;                            // a smooth picture has been shown: its moment, the frame drawn in
	double m_shown_now = 0;
	uint64_t m_shown_frame = ~0ull;
	bool m_unshown = false;                          // m_bitmap holds a picture that wasn't shown
	bool m_frame_parts = false;                      // ... drawn in parts (only its 3D)
	bool m_redrawing = false;                        // redraw(): drawing the shown picture again
	void keep_still();
	void swap_still();
	static constexpr int SKY_HIST = 16;
	static constexpr double MAX_SKY_STEP = 160;     // the most smooth motion moves the sky between frames
	// the sky's scroll each frame, as the tile chip has it (h, v) and with the fractions the game
	// dropped (hx, vy: h & 0x1ff and v & 0x3ff plus those); nx, ny: the next frame's, worked out as
	// this one ends (next: when it could be)
	struct sky_sample { int frame = -1; uint16_t h = 0, v = 0; float hx = 0, vy = 0, nx = 0, ny = 0; bool next = false; int lands_h = 0, lands_v = 0; };
	sky_sample m_sky_hist[SKY_HIST];
	struct sky_scroll { uint16_t h = 0, v = 0; float hx = 0, vy = 0; };
	sky_scroll sky_scroll_from_ram(bool between) const;   // what the game's routine writes, from what it reads
	void sky_settled(int &h, int &v) const;              // where the sky lands for the camera in RAM now
	bool sky_reaches(const sky_sample &s) const;
	float m_sky_frac_h = 0, m_sky_frac_v = 0;        // the fractions of the scroll last written
	bool m_sky_read_h = false, m_sky_read_v = false; // and whether those writes were the routine's
	bool m_sky_between = false;                      // the frame between two of the game's
	uint64_t m_sky_written = ~0ull;                  // the frame they were written in
	void sky_scroll_written(uint32_t a);
	void blend_slots(const logic_frame &a, const logic_frame &b, double alpha);
	int repair_by_camera(const logic_frame &a, const logic_frame &b, std::vector<int> &pair, const std::vector<char> &changed_run);
	bool m_camera_cut = false;    // (repair_by_camera: the camera's move between the two lists is a cut)
	int m_still_carried = -1;            // (the skipped update carried through, render_smooth: the next list catches up)
	bool m_camera_sure = false;          // (the camera's move known from the pair before and no cut: a part jumped only off it)
	std::vector<float> m_cam_res;        // (each slot's move off the camera's, over its depth; -1 none)
	struct cam_move { int from = -1, to = -1; double d[3] = {}; double turn = 0, ratio = 0; };   // (between two lists: the scenery's mean shift, the turn, the shift over the depth)
	cam_move m_cam_move[16];       // (the last few pairs', by the newer list's frame: a shift like the last pair's is a swing, not a cut)
	int m_cam_move_next = 0;
	void index_objects(logic_frame &lf);
	void record_frame();
	bool smooth_matrix(uint32_t poly_adr, float *out);
	void smooth_sky_shift(float &dx, float &dy) const;
	// That shift in output pixels at scale s, rounded as the game rounds its scroll (the fraction
	// of the scroll dropped): at a frame's own moment, the game's own placing.
	static int sky_shift_x(float dx, int s) { return int(std::ceil(dx * s - 1e-3f)); }
	static int sky_shift_y(float dy, int s) { return int(std::floor(dy * s + 1e-3f)); }
	struct sky_art {
		std::string name;
		int rows = 0;                                // tile rows (8 pixels each)
		std::vector<uint16_t> entries;               // 256 x rows map entries, as the ROM has them
		std::unordered_map<uint64_t, int> unique;    // column hash -> column, for columns seen once
		std::vector<uint32_t> px;                    // the picture: a replacement, or built from the record
		int w = 0, h = 0;
		bool original = true;                        // no replacement: the game's own, built with its palette
		std::vector<int> lines;                      // palette lines the record uses
		std::vector<uint32_t> codes;                 // and tiles
		int period = 256;                            // columns before the panorama repeats
		uint64_t pen_sig = 0, pending_sig = 0;       // the palette it was built with; a new one, settling
		int pending_count = 0;
	};
	std::vector<sky_art> m_skies;
	struct screen_art { std::string name; std::vector<uint32_t> hashes; std::vector<uint32_t> px; int w = 0, h = 0; };
	std::vector<screen_art> m_screens;
	const screen_art *m_screen_now = nullptr;
	void draw_screen_sides(bitmap_rgb32 &bitmap);
	std::vector<uint32_t> m_tile_coord;             // where each m_tile_tmp pixel came from
	sky_art *m_sky_now = nullptr, *m_sky_last = nullptr;
	bool m_sky_smooth = false;
	bool m_blend_cut = false, m_replay_older = false;   // smooth motion at a cut: the older list shown
	int m_cut_frame = -1;                              // the newest cut (its list's frame)
	bool m_sky_settling_now = false;                   // this picture is just after a cut, the sky still moving into place
	void init_skies();
	bool prepare_sky(sky_art &a);
	using rows_fn = std::function<void(int, const std::function<void(int, int)> &)>;
	static std::vector<uint32_t> soften(const std::vector<uint32_t> &px, int w, int h, const rows_fn &rows);
	static void scale_sky_px(const sky_art &a, int s, bool smooth, std::vector<uint32_t> &out, const rows_fn &rows);
	bool sky_ready(const sky_art &a);
	uint64_t m_sky_job_gen = 0;
	std::vector<uint32_t> m_sky_job_px;             // (before the job: it goes after it, waited for)
	std::future<uint64_t> m_sky_job;                // a sky being scaled (sky_ready)
	const sky_art *m_sky_job_for = nullptr;
	int m_sky_job_s = 0;
	bool m_sky_job_smooth = false;
	uint64_t m_sky_job_version = 0, m_sky_job_frame = 0;
	int m_sky_base = 0, m_sky_last_tx0 = 0;         // panorama x of the picture's left edge
	std::vector<uint32_t> m_sky_scaled;             // the current sky at the render scale
	std::vector<int> m_col_src;                     // per output column, the arcade column (smooth sky)
	const sky_art *m_sky_scaled_for = nullptr;
	int m_sky_scaled_s = 0;
	uint32_t m_sky_top_colour = 0, m_sky_bottom_colour = 0;   // beyond the panorama's rows (draw_sky)
	const void *m_sky_edges_for = nullptr; const sky_art *m_sky_edges_art = nullptr; int m_sky_edges_s = 0;
	void find_sky();
	bool sky_agrees(const sky_art &a, int tx0, int base) const;
	void scale_sky(const sky_art &a);
	void draw_sky(bitmap_rgb32 &bitmap, float dx, float dy);   // dx, dy: smooth motion's shift
	bool m_draw = true;
	int m_frame_quads = 0, m_last_quads = 0;
	bool m_above_hud_seen = false;   // this frame's list has objects drawn over the HUD (VF has none)
	float m_aspect = 4.0f / 3.0f;
	void resize_frame();
	bitmap_rgb32 m_tile_tmp;         // 2D layers at arcade resolution when rendering scaled
	// each layer at arcade resolution, kept for the game frame it was drawn for: smooth motion draws
	// several pictures a frame and the tiles only change with the frame. With each row's drawn span,
	// so the scaling skips what's empty (the HUD and text layers are mostly empty).
	struct layer_cache { uint64_t frame = ~0ull; int flags = -1, ext = -1; bitmap_rgb32 px; std::vector<int16_t> x0, x1; uint64_t gen = 0; };
	// the sky layer as last drawn (once a game frame, as the others; its panorama placing every time)
	uint64_t m_l4_frame = ~0ull; int m_l4_ext = -1; bool m_l4_coords = false; uint64_t m_l4_gen = 0;
	// The GPU's parts (model1_gpu.cpp)
	gpu_parts *m_parts = nullptr;                    // being drawn in parts
	bool m_parts_whole = false;                     // this picture can't be: draw it whole
	bitmap_rgb32 m_mid, m_top, m_l6m, m_l4m;         // 1x: the 2D under the 3D, over it; backdrops padded
	// a backdrop stretched across a widescreen picture: its columns picked here, as draw_tiles picks
	// them, at the render width (a GPU's sampling at a texel's very edge isn't the CPU's floor)
	bitmap_rgb32 m_stretched[2];
	// a 4:3 screen's painted sides at the render scale (KEY between them), for the GPU: painted again
	// only when the picture, its fade (the gains) or the size changes
	bitmap_rgb32 m_sides;
	const void *m_sides_art = nullptr;
	float m_sides_gain[6] = {};
	uint64_t m_sides_gen = 0;
	uint64_t m_stretched_key[2] = { ~0ull, ~0ull }, m_stretched_gen[2] = { 0, 0 };
	const bitmap_rgb32 &stretched(int i, const bitmap_rgb32 &src, int y0, int rows, float sdx, bool moved, uint64_t src_gen);
	uint64_t m_mid_key = ~0ull, m_top_key = ~0ull, m_l6m_key = ~0ull, m_l4m_key = ~0ull;
	uint64_t m_mid_gen = 0, m_top_gen = 0, m_l6m_gen = 0, m_l4m_gen = 0, m_sky_gen = 0, m_sky_gen_next = 0;
	// a layer image as last handed over, so its number changes only when its pixels do
	struct kept_image { const void *src = nullptr; uint64_t src_gen = ~0ull, gen = 0; std::vector<uint32_t> px; };
	kept_image m_kept[2];
	uint64_t kept_gen(kept_image &k, const bitmap_rgb32 &b, uint64_t src_gen);
	int m_d3_y0 = 0, m_d3_y1 = -1;                   // the rows the 3D may have drawn in, this picture
	bool m_edge_aa = false;
	std::vector<std::vector<std::pair<uint32_t, uint32_t>>> m_aa_changes;   // edge_aa: each band's blends (pixel, colour)
	static constexpr uint32_t UNDER_3D = 0x40000000;  // edge_aa, drawn whole: a pixel the 3D hasn't drawn (yet)
	void edge_smooth(bitmap_rgb32 &bitmap, bool parts);
	std::vector<std::pair<int16_t, int16_t>> m_span; // m_bitmap: KEY but [first, second) of each row
	bool m_bitmap_whole = true;                      // (or none of it is KEY: a picture drawn whole)
	void parts_layer(int layer, int flags, const bitmap_rgb32 &px, bool stretch, float sdx, float sdy);
	void parts_composite(bitmap_rgb32 &out, uint64_t &key, uint64_t &gen, std::initializer_list<int> layers);
	const bitmap_rgb32 &padded_middle(const bitmap_rgb32 &src, bitmap_rgb32 &out, uint64_t &key, uint64_t &gen, uint64_t src_gen);
	void parts_sky(const sky_art &a, int x_off, int y_base, int oy);
	void parts_begin(bitmap_rgb32 &bitmap);
	uint32_t parts_below(int x, int y) const;
	void parts_end(bitmap_rgb32 &bitmap);
	layer_cache m_layer_cache[8];
	layer_cache &cached_layer(int layer, int flags);
	void prepare_layers();
	bool m_layers_together = false;               // prepare_layers: drawing several at once
	uint64_t m_tiles_updated = ~0ull;   // the game frame the tile graphics were last decoded for
	void draw_tiles(bitmap_rgb32 &bitmap, const rectangle &cliprect, int layer, int flags);
public:
	static bool s_moire_blend;
	static int s_mark_y1, s_mark_y2;   // rows holding shading marks this frame (y1 > y2: none)
	static bool s_hd_raster;       // crack-free polygon filling (render scale above 1)
	// Above 1x: which polygon painted each pixel (s_owner_base + its draw order + 1; s_owner_base
	// or less: none). Each pass draws above the last's numbers, so nothing is cleared.
	static std::vector<int32_t> s_owner;
	static int32_t s_owner_base;
	static int s_depth_w;
	static thread_local int s_cur_quad;   // per thread: the polygons are drawn in row bands at once
	static thread_local bool s_cur_fshadow;   // the polygon being drawn is a fighter's shadow
	static std::vector<uint8_t> s_qstage;   // each polygon: whether the stage's
	static bool stage_model(uint32_t poly_adr) { return (poly_adr & 0x7fffff) < 0x80000; }
	// (the stages' own see-through polygons keep the arcade's order: the floor covers them)
	static bool fighter_shadow(const quad_t &q) { return (q.col & 0x01000000) && !stage_model(q.obj); }   // 0x01000000: MOIRE
	static int s_depth_pass;       // 1 while painting with the shadow test

	// ---- the 3D on the GPU (src/gpu/gpu3d.h; the host's Display > 3D renderer) ----
	// While on, each picture's polygons are recorded as draw_quads would draw them and drawn by the
	// GPU into the same bitmap at the end of the 3D (gpu3d_end); anything it can't (a wireframe
	// line, a vertex that isn't a number) or a GPU error: that picture drawn by the CPU from the
	// recording. Off: the CPU's way, untouched.
	void set_gpu3d(gpu3d *g) { m_gpu3d = g; }
	void set_gpu3d_on(bool on) { m_gpu3d_on = on; }
	// The host's renderer is on the 3D's GPU device (SDL's "gpu" renderer): pictures put together in
	// parts have their 3D drawn straight into its textures (gpu_parts::d3_target), not into frame().
	void set_gpu3d_target(bool on) { m_g3_target = on; }
	bool gpu3d_target() const { return m_g3_target; }
	static constexpr int G3_TARGETS = 2;   // (one being drawn while the last is shown)
	bool gpu3d_on() const { return m_gpu3d && m_gpu3d_on; }
	struct gpu3d_stats {
		double ms = 0;          // the last picture's 3D drawing, wall time (CPU: the rasterizer's; GPU: recorded to drawn)
		double gpu_ms = 0;      // its GPU time
		double rec_ms = 0;      // its recording's (GPU)
		bool gpu = false;       // drawn by the GPU
		int polys = 0;
		uint64_t failed = 0, cpu_fallback = 0, checked = 0, differ = 0;   // errors; pictures the CPU drew instead; checks
		uint64_t pictures = 0;  // drawn by the GPU
		uint64_t handed = 0;    // of them, drawn straight into the host's texture (nothing waited for)
	};
	gpu3d_stats m_g3;
	// Auto's measure (model1_video.cpp); differ: also the test picture drawn once more each way and
	// compared, its pixels that differ
	bool bench3d(int reps, double &cpu_ms, double &gpu_ms, long long *differ = nullptr);
	int m_gpu3d_check = 0;     // tests (VF_GPU3D_CHECK=n): every n-th GPU picture drawn by the CPU too and compared
private:
	uint64_t m_frame = 0;

	// Memory
	std::vector<uint8_t> m_ram_a;      // 400000-40ffff (battery-backed)
	std::vector<uint8_t> m_ram_b;      // 500000-53ffff
	std::vector<uint16_t> m_display_list0_ram, m_display_list1_ram;
	uint64_t m_list_written[2] = { ~0ull, ~0ull };   // the game frame each display list was last written in
	const uint16_t *picture_list();                  // the display list to show (see model1_video.cpp)
	std::vector<uint16_t> m_paletteram16_ram, m_color_xlat_ram;
	uint16_t *m_display_list0, *m_display_list1, *m_paletteram16, *m_color_xlat;
	uint32_t *m_poly_rom, *m_copro_tables;
	uint32_t m_bank_base = 0x1000000;
	uint32_t m_pens[0x2000]{};

	uint16_t bus_read16(uint32_t a);
	// The main board's i8251 to the sound board: 31250 baud, 10 bits a byte.
	static constexpr int UART_BYTE_CYCLES = 5120;
	int m_uart_txbuf = -1, m_uart_shift = -1, m_uart_rx = -1;
	int m_uart_shift_left = 0, m_uart_rx_left = 0;
	std::unique_ptr<class m1audio> m_audio;
	void uart_write(uint8_t v);
	uint8_t uart_status();
	uint8_t uart_read();
	void uart_tick(int cycles);
	void sound_ready();
	// I/O board stand-in (MAME 0.190's io_r/io_w/network_ctl): its Z80 program isn't in this set.
	std::unique_ptr<model1io> m_ioboard;   // the real I/O board, when its program is there
	uint8_t io_board_r(int index);
	void io_board_w(int index, uint8_t data);
	void io_board_tick(int cycles);
	uint8_t m_io_command = 0;
	int64_t m_io_ack = 0;
	void bus_write16(uint32_t a, uint16_t v, uint16_t mem_mask);

	// Machine
	void bank_w(offs_t offset, u16 data, u16 mem_mask = ~0);
	u8 irq_callback();
	void irq_raise(int level);
	void irq_control_w(u8 data);
	u8 irq_mask_r();
	void irq_mask_w(u8 data);
	u16 timer_r(offs_t offset);
	void timer_period_w(offs_t offset, u16 data);
	void timer_tick(int cycles);
	uint16_t m_timer_period[2] = { 0, 0 };
	uint16_t m_timer_val[2] = { 0, 0 };
	int64_t m_timer_remain[2] = { 0, 0 };
	uint16_t m_timer_mode = 0;
	uint8_t m_irq_status = 0;
	uint8_t m_irq_mask = 0xff;
	int m_last_irq = 0;

	// TGP from its own program (model1_tgp_lle.cpp)
	struct tgp_lle;
	struct tgp_lle_deleter { void operator()(tgp_lle *) const; };
	std::unique_ptr<tgp_lle, tgp_lle_deleter> m_lle;
	void lle_reset();
	u16 lle_v60_r(uint32_t a);
	void lle_v60_w(uint32_t a, uint16_t data, uint16_t mem_mask);

	// TGP: V60 side
	u16 fifoin_status_r();
	u16 v60_copro_fifo_r(offs_t offset, u16 mem_mask);
	void v60_copro_fifo_w(offs_t offset, u16 data, u16 mem_mask);
	u16 v60_copro_ram_adr_r(offs_t offset, u16 mem_mask);
	void v60_copro_ram_adr_w(offs_t offset, u16 data, u16 mem_mask);
	u16 v60_copro_ram_r(offs_t offset, u16 mem_mask);
	void v60_copro_ram_w(offs_t offset, u16 data, u16 mem_mask);

	// TGP: table units (also reachable from the simulation)
	void copro_sincos_w(offs_t offset, u32 data, u32 mem_mask);
	u32 copro_sincos_r(offs_t offset, u32 mem_mask);
	void copro_inv_w(offs_t offset, u32 data, u32 mem_mask);
	u32 copro_inv_r(offs_t offset, u32 mem_mask);
	void copro_isqrt_w(offs_t offset, u32 data, u32 mem_mask);
	u32 copro_isqrt_r(offs_t offset, u32 mem_mask);
	void copro_atan_w(offs_t offset, u32 data, u32 mem_mask);
	u32 copro_atan_r(offs_t offset, u32 mem_mask);
	void copro_data_w(offs_t offset, u32 data, u32 mem_mask);
	u32 copro_data_r(offs_t offset, u32 mem_mask);
	void copro_ramadr_w(offs_t offset, u32 data, u32 mem_mask);
	u32 copro_ramadr_r(offs_t offset, u32 mem_mask);
	void copro_ramdata_w(offs_t offset, u32 data, u32 mem_mask);
	u32 copro_ramdata_r(offs_t offset, u32 mem_mask);
	void copro_reset();

	u32 m_copro_sincos_base = 0;
	u32 m_copro_inv_base = 0;
	u32 m_copro_isqrt_base = 0;
	u32 m_copro_atan_base[4]{};
	u32 m_copro_data_base = 0;
	u32 m_copro_ram_adr = 0;

	// Virtua Racing's track data; VF has none.
	struct data_region
	{
		std::vector<u32> d;
		u32 &as_u32(size_t i) { static u32 z; return i < d.size() ? d[i] : (z = 0); }
		size_t bytes() const { return d.size() * 4; }
	} m_copro_data_region;
	data_region *m_copro_data = &m_copro_data_region;

	// TGP FIFOs and simulation
	u32_fifo m_copro_fifo_in_q, m_copro_fifo_out_q;
	u32_fifo *m_copro_fifo_in = &m_copro_fifo_in_q, *m_copro_fifo_out = &m_copro_fifo_out_q;
	void    fifoout_push(uint32_t data);
	void    fifoout_push_f(float data);
	uint32_t  fifoin_pop();
	float   fifoin_pop_f();
	uint16_t  ram_get_i();
	float   ram_get_f();
	u32 m_v60_copro_fifo_r = 0, m_v60_copro_fifo_w = 0;
	offs_t m_pushpc = 0;
public:
	// TGP statistics: calls per function number, calls to missing ones, reads from an empty
	// result FIFO (by V60 address).
	std::map<int, uint64_t> m_tgp_calls, m_tgp_missing;
	std::map<uint32_t, uint64_t> m_tgp_underflow;
private:
	// MAME's simulation answers an unknown function (f100) with random numbers; per machine and
	// seeded, so runs repeat.
	uint32_t m_rng = 1;
	int m_rand() { m_rng = m_rng * 1103515245 + 12345; return int((m_rng >> 16) & 0x7fff); }
	u32 m_copro_hle_active_list_pos = 0, m_copro_hle_active_list_length = 0;
	void copro_hle_vf();

	typedef void (model1::*tgp_func)();
	struct function
	{
		tgp_func cb;
		int count;
	};
	static float tsin(s16 angle);
	static float tcos(s16 angle);
	static const struct function ftab_vf[];

	uint32_t  m_copro_hle_list_length = 0;
	float   m_cmat[12]{};
	float   m_mat_stack[MAT_STACK_SIZE][12]{};
	float   m_mat_vector[21][12]{};
	int32_t   m_mat_stack_pos = 0;
	float   m_acc = 0;
	float   m_tgp_vf_xmin = 0, m_tgp_vf_xmax = 0, m_tgp_vf_zmin = 0, m_tgp_vf_zmax = 0;
	float   m_tgp_vf_ygnd = 0, m_tgp_vf_yflr = 0, m_tgp_vf_yjmp = 0;
	float   m_tgp_vr_circx = 0, m_tgp_vr_circy = 0, m_tgp_vr_circrad = 0;
	float   m_tgp_vr_cbox[12]{};
	int     m_tgp_vr_select = 0;
	float   m_tgp_int_px = 0, m_tgp_int_py = 0, m_tgp_int_pz = 0;
	uint32_t  m_tgp_int_adr = 0;
	uint16_t  m_v60_copro_ram_adr = 0;
	uint16_t  m_v60_copro_ram_latch[2]{};
	uint16_t  m_copro_hle_ram_scan_adr = 0;
	std::unique_ptr<uint32_t[]> m_copro_ram_data;
	float   m_tgp_vr_base[4]{};
	int     m_ccount = 0;

	TGP_FUNCTION( fadd );
	TGP_FUNCTION( fsub );
	TGP_FUNCTION( fmul );
	TGP_FUNCTION( fdiv );
	TGP_FUNCTION( matrix_push );
	TGP_FUNCTION( matrix_pop );
	TGP_FUNCTION( matrix_write );
	TGP_FUNCTION( clear_stack );
	TGP_FUNCTION( matrix_mul );
	TGP_FUNCTION( anglev );
	TGP_FUNCTION( triangle_normal );
	TGP_FUNCTION( normalize );
	TGP_FUNCTION( acc_seti );
	TGP_FUNCTION( track_select );
	TGP_FUNCTION( load_base );
	TGP_FUNCTION( transpose );
	TGP_FUNCTION( anglep );
	TGP_FUNCTION( matrix_ident );
	TGP_FUNCTION( matrix_read );
	TGP_FUNCTION( matrix_trans );
	TGP_FUNCTION( matrix_scale );
	TGP_FUNCTION( matrix_rotx );
	TGP_FUNCTION( matrix_roty );
	TGP_FUNCTION( matrix_rotz );
	TGP_FUNCTION( track_read_quad );
	TGP_FUNCTION( intercept );
	TGP_FUNCTION( transform_point );
	TGP_FUNCTION( fcos_m1 );
	TGP_FUNCTION( fsin_m1 );
	TGP_FUNCTION( fcosm_m1 );
	TGP_FUNCTION( fsinm_m1 );
	TGP_FUNCTION( distance3 );
	TGP_FUNCTION( ftoi );
	TGP_FUNCTION( itof );
	TGP_FUNCTION( acc_set );
	TGP_FUNCTION( acc_get );
	TGP_FUNCTION( acc_add );
	TGP_FUNCTION( acc_sub );
	TGP_FUNCTION( acc_mul );
	TGP_FUNCTION( acc_div );
	TGP_FUNCTION( f42 );
	TGP_FUNCTION( xyz2rqf );
	TGP_FUNCTION( f43 );
	TGP_FUNCTION( f43_swa );
	TGP_FUNCTION( track_read_tri );
	TGP_FUNCTION( matrix_sdir );
	TGP_FUNCTION( fsqrt );
	TGP_FUNCTION( vlength );
	TGP_FUNCTION( f47 );
	TGP_FUNCTION( track_read_info );
	TGP_FUNCTION( colbox_set );
	TGP_FUNCTION( colbox_test );
	TGP_FUNCTION( f49_swa );
	TGP_FUNCTION( f50_swa );
	TGP_FUNCTION( f52 );
	TGP_FUNCTION( matrix_rdir );
	TGP_FUNCTION( track_lookup );
	TGP_FUNCTION( f56 );
	TGP_FUNCTION( int_normal );
	TGP_FUNCTION( matrix_readt );
	TGP_FUNCTION( acc_geti );
	TGP_FUNCTION( int_point );
	TGP_FUNCTION( col_setcirc );
	TGP_FUNCTION( col_testpt );
	TGP_FUNCTION( push_and_ident );
	TGP_FUNCTION( catmull_rom );
	TGP_FUNCTION( distance );
	TGP_FUNCTION( car_move );
	TGP_FUNCTION( cpa );
	TGP_FUNCTION( vmat_store );
	TGP_FUNCTION( vmat_restore );
	TGP_FUNCTION( vmat_mul );
	TGP_FUNCTION( vmat_read );
	TGP_FUNCTION( matrix_rtrans );
	TGP_FUNCTION( matrix_unrot );
	TGP_FUNCTION( f80 );
	TGP_FUNCTION( vmat_save );
	TGP_FUNCTION( vmat_load );
	TGP_FUNCTION( ram_setadr );
	TGP_FUNCTION( groundbox_test );
	TGP_FUNCTION( f89 );
	TGP_FUNCTION( f92 );
	TGP_FUNCTION( f93 );
	TGP_FUNCTION( f94 );
	TGP_FUNCTION( vmat_flatten );
	TGP_FUNCTION( vmat_load1 );
	TGP_FUNCTION( ram_trans );
	TGP_FUNCTION( f98_load );
	TGP_FUNCTION( f98 );
	TGP_FUNCTION( f99 );
	TGP_FUNCTION( f100 );
	TGP_FUNCTION( groundbox_set );
	TGP_FUNCTION( f102 );
	TGP_FUNCTION( f103 );
	TGP_FUNCTION( dump );
	TGP_FUNCTION( function_get_vf );
	TGP_FUNCTION( function_get_swa );

	// Rendering
	void video_start();
	u16 model1_listctl_r(offs_t offset);
	void model1_listctl_w(offs_t offset, u16 data, u16 mem_mask = ~0);
	void model1_paletteram_w(offs_t offset, u16 data, u16 mem_mask = ~0);
	void screen_vblank_model1(int state);

	struct lightparam_t
	{
		float a = 0;
		float d = 0;
		float s = 0;
		int p = 0;
	};

	class view_t
	{
	public:
		view_t() {
			light.x = 0;
			light.y = 0;
			light.z = 0;
		}

		void init_translation_matrix();

		void set_viewport(float xcenter, float ycenter, float xl, float xr, float yb, float yt);
		void set_lightparam(int index, float diffuse, float ambient, float specular, int power);
		void set_zoom(float x, float y);
		void set_light_direction(float x, float y, float z);
		static glm::vec3 light_vector(float x, float y, float z);
		void set_translation_matrix(float* mat);
		void set_view_translation(float x, float y);

		void project_point(point_t *p) const;
		void project_point_direct(point_t *p) const;

		void transform_vector(glm::vec3& p) const;
		void transform_point(point_t *p) const;

		void recompute_frustum();

		int xc = 0, yc = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;   // x1..y2: clip in render pixels
		float lx1 = 0, lx2 = 0, ly1 = 0, ly2 = 0;             // the clip as the game set it
		float scale = 1;                                      // render pixels per arcade pixel
		float ext = 0;                                        // widescreen: extra arcade pixels each side
		float zoomx = 0, zoomy = 0, viewx = 0, viewy = 0;
		float a_bottom = 0, a_top = 0, a_left = 0, a_right = 0;
		float vxx = 0, vyy = 0, vzz = 0, ayy = 0, ayyc = 0, ayys = 0;
		float translation[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		glm::vec3 light;
		lightparam_t lightparams[256];
		bool spec_enable = false;
	};

	class clipper_t
	{
	public:
		clipper_t() : m_isclipped(nullptr), m_clip(nullptr) {}
		clipper_t(std::function<bool(view_t*, point_t*)> isclipped, std::function<void(view_t*, point_t*, point_t*, point_t*)> clip)
			: m_isclipped(isclipped), m_clip(clip) {}
		std::function<bool(view_t*, point_t*)> m_isclipped;
		std::function<void(view_t*, point_t*, point_t*, point_t*)> m_clip;
	};

	std::unique_ptr<view_t> m_view;
	std::unique_ptr<point_t[]> m_pointdb;
	point_t *m_pointpt = nullptr;
	std::unique_ptr<quad_t[]> m_quaddb;
	quad_t      *m_quadpt = nullptr;
	std::unique_ptr<quad_t *[]> m_quadind;

	uint16_t  m_listctl[2]{};
	uint16_t  *m_glist = nullptr;
	bool    m_render_done = false;

	std::unique_ptr<uint16_t[]> m_tgp_ram;
	std::unique_ptr<uint32_t[]> m_poly_ram;

	uint32_t  readi(int adr) const;
	int16_t   readi16(int adr) const;
	float   readf(int adr) const;
	void    cross_product(point_t* o, const point_t* p, const point_t* q) const;
	float   view_determinant(const point_t *p1, const point_t *p2, const point_t *p3) const;

	static bool fclip_isc_bottom(view_t*, point_t*);
	static bool fclip_isc_top(view_t*, point_t*);
	static bool fclip_isc_left(view_t*, point_t*);
	static bool fclip_isc_right(view_t*, point_t*);
	static void fclip_clip_bottom(view_t*, point_t*, point_t*, point_t*);
	static void fclip_clip_top(view_t*, point_t*, point_t*, point_t*);
	static void fclip_clip_left(view_t*, point_t*, point_t*, point_t*);
	static void fclip_clip_right(view_t*, point_t*, point_t*, point_t*);

	enum render_pass { RENDER_BELOW_HUD, RENDER_ABOVE_HUD };
	void    tgp_render(bitmap_rgb32 &bitmap, const rectangle &cliprect, render_pass pass);
	void    tgp_scan();

	void        sort_quads() const;
	void        unsort_quads() const;
	void        draw_quads(bitmap_rgb32 &bitmap, const rectangle &cliprect);
	void        raster_quads(bitmap_rgb32 &bitmap, view_t *view, int count);   // draw_quads' drawing (the CPU's)
	// the GPU's recording (draw_quads in GPU mode)
	gpu3d *m_gpu3d = nullptr;
	bool m_g3_target = false;
	int m_g3_next = 0;                                // the target the next picture's 3D goes into
	bool m_gpu3d_on = false, m_rec = false, m_rec_bad = false;
	struct rec_batch { int x1, x2, y1, y2; size_t first, count; };
	std::vector<rec_batch> m_rec_batches;
	std::vector<quad_t> m_rec_quads;
	std::vector<point_t> m_rec_points;
	std::vector<gpu3d_poly> m_rec_polys;
	std::vector<gpu3d_span> m_rec_spans;
	int m_rec_y0 = 0, m_rec_y1 = -1, m_rec_m0 = 0, m_rec_m1 = -1;
	double m_raster_ms = 0, m_rec_ms = 0;
	uint32_t m_rec_rows = 0;
	bitmap_rgb32 m_check_cpu;
	void cpu_3d_let_go();   // (the GPU's pictures checked: the CPU's drawing buffers let go)
	void record_quads(view_t *view, int count);
	void gpu3d_begin(const bitmap_rgb32 &bitmap);
	int m_bitmap_rec_w = 0, m_bitmap_rec_h = 0;
	void gpu3d_end(bitmap_rgb32 &bitmap);
	void rec_replay(bitmap_rgb32 &bitmap);
	static void recompute_frustum(view_t *view);
	static void draw_hline(bitmap_rgb32 &bitmap, int x1, int x2, int y, int color);
	static void draw_hline_moired(bitmap_rgb32 &bitmap, int x1, int x2, int y, int color);
	static void fill_slope(bitmap_rgb32 &bitmap, view_t *view, int color, int32_t x1, int32_t x2, int32_t sl1, int32_t sl2, int32_t y1, int32_t y2, int32_t *nx1, int32_t *nx2);
	static void fill_line(bitmap_rgb32 &bitmap, view_t *view, int color, int32_t y, int32_t x1, int32_t x2);
	void        fill_quad(bitmap_rgb32 &bitmap, view_t *view, const quad_t& q) const;
	static void fill_quad_hd(bitmap_rgb32 &bitmap, view_t *view, const quad_t& q, int color);
	void        draw_line(bitmap_rgb32 &bitmap, view_t *view, int color, int x1, int y1, int x2, int y2) const;

	void    fclip_push_quad_next(int level, quad_t& q, point_t *p1, point_t *p2, point_t *p3, point_t *p4);
	void    fclip_push_quad(int level, quad_t& q);

	static float    min4f(float a, float b, float c, float d);
	static float    max4f(float a, float b, float c, float d);
	float   compute_specular(glm::vec3& normal, glm::vec3& light, float diffuse, int lmode);
	uint16_t scale_color(uint16_t color, float level) const;

	int push_direct(int list_offset);
	int draw_direct(bitmap_rgb32 &bitmap, const rectangle &cliprect, int list_offset);
	int skip_direct(int list_offset) const;
	void    push_object(uint32_t tex_adr, uint32_t poly_adr, uint32_t size, float &old_z);
	void    draw_objects(bitmap_rgb32 &bitmap, const rectangle &cliprect);

	void set_current_render_list();
	int     get_list_number();
	void    end_frame();

	// What the picture last shown was drawn from, kept as the next frame starts (the shown picture
	// is then the current one): its lists, 2D, palette and lights. When frames run on without a new
	// picture shown, redraw() swaps these in to draw that same moment again (model1_interp.cpp).
	struct still_state
	{
		uint64_t frame = ~0ull;
		logic_frame logic[2], logic_prev;
		int cut_frame = -1;
		sky_sample sky_hist[SKY_HIST];
		std::vector<uint16_t> tile_ram, char_ram, palette, color_xlat;
		uint64_t char_gen = ~0ull;
		std::array<uint32_t, 0x2000> pens{};
		std::array<lightparam_t, 256> lightparams{};
		glm::vec3 light{};
		bool spec_enable = false;
		uint8_t camera[4]{};
		bool fight_cam = true;
		sky_art *sky_now = nullptr, *sky_last = nullptr;
		int sky_base = 0, sky_last_tx0 = 0;
	} m_still;

	clipper_t m_clipfn[4];
	uint16_t* m_display_list_current = nullptr;

	int m_overlay_stride = 0;
	std::unique_ptr<uint8_t[]>  m_overlay_block;
	std::unique_ptr<uint32_t[]> m_overlay_hud_snapshot;
	void build_overlay_mask(bitmap_rgb32 &bitmap, const rectangle &cliprect);
	void apply_overlay_stencil(bitmap_rgb32 &bitmap, const rectangle &cliprect);
	void clear_moire_marks(bitmap_rgb32 &bitmap, const rectangle &cliprect);
	void screen_update(bitmap_rgb32 &bitmap, const rectangle &cliprect);

	// Framework bits the ported code calls
	struct palette_t
	{
		model1 &m;
		uint32_t pen(int i) const { return m.m_pens[i & 0x1fff]; }
		uint32_t black_pen() const { return 0; }
		void set_pen_color(int i, int r, int g, int b) { m.m_pens[i & 0x1fff] = (r << 16) | (g << 8) | b; }
	} m_palette_obj{*this};
	palette_t *m_palette = &m_palette_obj;
	struct screen_t
	{
		model1 &m;
		uint64_t frame_number() const { return m.m_frame; }
		int width() const { return SCREEN_W; }
		int height() const { return SCREEN_H; }
	} m_screen_obj{*this};
	screen_t *m_screen = &m_screen_obj;
public:
	// The game's own pictures still to check against the CPU's before the GPU's are trusted (one
	// every 60 with a fight's polygons): a difference is a failure (m_g3.failed: the CPU's then).
	// (last: the members before it where they were)
	int m_gpu3d_verify = 0;
	// When the GPU's picture differs from the CPU's, how wrong it is, not how many pixels: some
	// graphics cards' drivers (NVIDIA's on Direct3D 12) put pixels a hair off. Colours within
	// GPU_ROUNDING_COLOUR anywhere are rounding; past that, up to 1% of the pixels, and up to 5%
	// drawn on one and not the other (edges a hair apart), the GPU kept. When the player chose the
	// GPU (not Auto, m_gpu3d_chosen): 10% and 25%, only a picture mostly wrong (a GPU drawing
	// nothing, or garbage) putting the 3D on the CPU.
	static constexpr long long GPU_ROUNDING_PX = 64;
	static constexpr int GPU_ROUNDING_COLOUR = 8;
	static constexpr double GPU_WRONG_COLOUR = 0.01, GPU_WRONG_COVER = 0.05, GPU_WRONG_COLOUR_CHOSEN = 0.10, GPU_WRONG_COVER_CHOSEN = 0.25;
	bool m_gpu3d_chosen = false;
	struct gpu_diff { long long all = 0, cover = 0, big = 0; int most = 0; size_t first = 0; };
	static gpu_diff gpu_compare(const uint32_t *cpu, const uint32_t *gpu, size_t n)
	{
		gpu_diff d;
		for (size_t i = 0; i < n; i++) {
			const uint32_t a = cpu[i], g = gpu[i];
			if (a == g) continue;
			if (!d.all++) d.first = i;
			if ((a ^ g) >> 24 || !(a & 0xffffff) != !(g & 0xffffff)) { d.cover++; continue; }   // (drawn on one only, or marked apart)
			int off = 0;
			for (int sh = 0; sh < 24; sh += 8) off = std::max(off, std::abs(int((a >> sh) & 255) - int((g >> sh) & 255)));
			d.most = std::max(d.most, off);
			if (off > GPU_ROUNDING_COLOUR) d.big++;
		}
		return d;
	}
	bool gpu_diff_ok(const gpu_diff &d, long long px) const
	{
		const long long colour = std::max(GPU_ROUNDING_PX, (long long)(px * (m_gpu3d_chosen ? GPU_WRONG_COLOUR_CHOSEN : GPU_WRONG_COLOUR)));
		const long long cover = std::max(GPU_ROUNDING_PX, (long long)(px * (m_gpu3d_chosen ? GPU_WRONG_COVER_CHOSEN : GPU_WRONG_COVER)));
		return d.big <= colour && d.cover <= cover;
	}
	gpu_diff m_bench_diff;   // (bench3d's picture each way, compared)
	// tests: VF_GPU3D_TEST_DIFF=n, a check's picture counted n pixels more apart (-1: all of them)
	static long long gpu_test_diff(long long d, long long px) { const char *t = getenv("VF_GPU3D_TEST_DIFF"); if (!t) return d; const long long k = atoll(t); return k < 0 ? px : std::min(px, d + k); }
};
