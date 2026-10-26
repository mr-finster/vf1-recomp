// VF1 in a netplay session (src/net): the board as the session's Game. What both sides start from (the host's
// backup RAM: the rankings and the play clock), how they start (a cold start, the PC way on both: free play, the
// FREE PLAY notices hidden, the boot run unseen, the I/O board's stand-in), each player's switches as an input word,
// and the bytes for the sound board under rollback. VF3's vf3_netplay and VF2's vf2_netplay, for VF1's board.
//
// In a match the board has no sound board (the main board never hears back from it, the same on both sides); the
// bytes the game sends it go to the sound board taken off the board (take_sound), which plays to the real clock.
// With the recompiled code (native), each save state carries the game's coroutine stack (rec::save_stack) in a
// part the desync check leaves out (hash_skip): its bytes hold this process's addresses.
#pragma once
#include "../machine/model1.h"
#include "../net/session.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vf1net {

constexpr int BOOT_FRAMES = 900;   // the boot, unseen, up to the attract's first camera (about 420)

// A player's switches as the session's input word (the stick, Punch, Kick, Guard, Start and Coin; never test or
// service), from the board's ports (active low) and back. Start is the press the host made of it (the PC way's 6
// frames), not a key held: the word is what the game gets.
constexpr uint16_t START_BUTTON = 256, COIN_BUTTON = 512;
uint16_t pack(uint8_t in0, uint8_t player_port, int player);
void unpack(const uint16_t input[2], model1 &board);

// The online version both sides compare in the handshake (with the ROMs' hash), apart from the release's: changed
// only with what a match depends on (the board, the recompiler, their compiler flags, this adapter, the match's
// settings, src/net's protocol). Named after the release that brings it (docs/RELEASING-ALL.md).
inline constexpr const char *NET_VERSION = "vf1-online-1";
// NET_VERSION for a release's build (v1.4.3, or with a word after it), else the build's own (a test build plays
// only the same build)
std::string online_version(const std::string &build);

uint64_t content_hash(const model1 &board);
// The host's starting blob: its backup RAM and its GAME ASSIGNMENTS (the rounds, the difficulty: put in the match
// board's EEPROM as values, whichever I/O board each side has offline), the match's random seed (Random fighter's
// fighters made alike on both sides) and the host's fighter (player 1's: fighter_extra)
std::vector<uint8_t> start_blob(model1 &board, const std::string &host_fighter = "", uint32_t seed = 1);

// A player's fighter as the handshake carries it (Identity::extra): "" the select as ever, "R" a random fighter, a
// new one at every select, "S<seed>" a named fighter's (model1::random_fighter_names: the same on any machine with
// the ROM), "X<k>" a secret character (1-7, model1::UNUSED_CHARACTERS). As model1::set_random_player's setting
// ("", "*", "#k", or "S<seed>": set_players); false for one that isn't any of them
std::string fighter_extra(const std::string &random_player_setting, model1 &board);
bool fighter_setting(const std::string &extra, model1 &board, std::string &setting);
void set_players(model1 &board, const std::string fighters[2]);
// A fighter as a word or two under a player's name (the VS reveal): "" for Normal, "RANDOM", "SECRET: SIBA", "SEED 1234"
std::string fighter_words(const std::string &extra);

// The room at a save state's start for the coroutine's stack (its bytes, then the board's state): VF1's is a few
// hundred bytes at a frame's end
constexpr size_t STACK_ROOM = 16384;

// The character select on screen: work RAM 501138 (the game's mode; 501139 a copy) 7 from the first frame after
// Start to the VS screen, the challenger's select too, and never elsewhere (3 the title and the attract, 6 Start's
// frames, 8 the VS screen, 9 the fights, 10-14 the game over; found by probing every byte of RAM over a 1P and a
// 2P run, a challenger's way back in, held then frame by frame). A match plays it in lockstep
// (vfnet::Session::set_lockstep): no frame on a guess, so a face never flashes as a guess is put right. As VF3's.
constexpr uint32_t SCENE_BYTE = 0x1138;
inline int scene_byte(const model1 &board) { const std::vector<uint8_t> &r = const_cast<model1 &>(board).work_ram(); return r.size() > SCENE_BYTE ? r[SCENE_BYTE] : -1; }
inline bool in_select(const model1 &board) { return scene_byte(board) == 7; }

// The score between the two players, read from the game's own RAM after each frame (as Fightcade's detectors do):
// each player's rounds won in the match on now (both back to 0 when it's over), and which players are playing (a
// side not playing is the CPU's). A match counts only if both were playing at each round it was won (not the
// winner's fights against the CPU after it); each reading only once it's held 30 frames, longer than any rollback,
// so a guessed frame never scores. VF1's places (found against the HUD's round markers and runs with one player
// and two): work RAM 501281 and 501282, each player's rounds (going back down to 0 one at a time after the CPU wins:
// the winner is the one who reached more); bits 0x18 of the backup RAM's 407483 and 407503, a person playing that side (0x10 alone: they
// lost a versus and the CPU took their side).
struct Score
{
	int matches[2] = {}, rounds[2] = {};   // between the two players: matches won, and rounds won in them
	void frame(const model1 &board);
	int players() const { return playing; }          // (who's in the game, as held: bit 0 player 1, bit 1 player 2)
	uint8_t players_now() const { return now[2]; }   // (as the RAM has it this frame, not yet held)
	int round_ends = 0;   // (each round's end, any players')
	static constexpr uint32_t ROUNDS_P1 = 0x1281, ROUNDS_P2 = 0x1282;   // (work RAM offsets)
	static constexpr uint32_t PLAYING_P1 = 0x7483, PLAYING_P2 = 0x7503;  // (backup RAM offsets: bits 0x18)
	static constexpr bool known() { return true; }
	static uint8_t players_in(const model1 &board);   // (bit 0 player 1, bit 1 player 2)
private:
	uint8_t seen[2] = {}, best[2] = {}, now[3] = {}, playing = 0;
	int held = 0;
	bool versus = false;   // (the match on now: both playing at every round so far)
};

class Adapter : public vfnet::Game
{
public:
	explicit Adapter(model1 &board, bool native) : native(native), board(board) {}
	~Adapter() override { board.on_uart_tx = nullptr; }

	// Set by the host program:
	// one frame (the ports are set): resim, a frame run again (no picture, no sound needed). Default:
	// board.run_frame(). Whatever takes the frame's bytes for the sound board calls take_sound (else the adapter
	// does after the frame, and gives them to sound_out).
	std::function<void(bool resim)> run;
	std::function<void()> program_changed;     // the program was patched (the native code must be made again)
	std::function<void()> settle;              // before a save or load: anything running alongside the board waits
	std::function<void(const std::vector<uint8_t> &)> sound_out;   // bytes for the sound board
	std::function<void()> on_begin;            // a match's start, before the board is set up for it (keep the player's NVRAM and settings)
	std::function<void()> booting;             // now and then in the match's boot (seconds of frames: the window's events)

	bool begin(const std::vector<uint8_t> &blob) override;
	void save(std::vector<uint8_t> &state) override;
	bool load(const std::vector<uint8_t> &state) override;
	void advance(const uint16_t input[2], bool resim) override;
	uint64_t hash(const std::vector<uint8_t> &state);   // (as the session's check: past hash_skip)
	size_t hash_skip() const override { return STACK_ROOM + model1::STATE_HEADER; }
	// The frame's bytes for the sound board to send now: all of them the first time a frame runs; run again after a
	// rollback, none if its bytes went already (the sound board can't take back what it played), and if it sent
	// none and has some now, those (late)
	void take_sound(std::vector<uint8_t> &out);
	bool failed_stack = false;   // (a stack too deep for STACK_ROOM: the save is refused, the match ends)
	bool native;   // (the recompiled code runs the board: its stack in the states; the host's to set, the game falling back to the interpreter)
	// The fighters (Online sets these before the session): this side's choice (fighter_extra), and whether it hosts
	// (player 1, its choice in its blob) or joins (player 2)
	std::string own_extra;
	bool hosting = false;
	std::vector<uint8_t> host_blob();   // (start_blob with this side's fighter and a fresh seed)
	void peer_extra(const std::string &extra) override { their_extra = extra; }
	std::string fighters[2];   // (the match's, as begin set them: model1::set_random_player's settings)

	// tests: the state's hash after each frame a multiple of this, the last time it ran (0: none kept)
	uint32_t checkpoint_every = 0;
	std::map<uint32_t, uint64_t> checkpoints;
	bool keep_states = false;                              // (and the states themselves)
	std::map<uint32_t, std::vector<uint8_t>> checkpoint_states;

private:
	model1 &board;
	std::string their_extra;
	uint32_t frame = 0;   // the session's frame the board is at
	std::vector<uint8_t> pending, stack_buf, check_buf;
	bool collecting = false, sound_taken = false;
	std::map<uint32_t, std::vector<uint8_t>> sound_sent;
	void save_into(std::vector<uint8_t> &state);
};

} // namespace vf1net
