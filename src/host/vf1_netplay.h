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
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vf1net {

constexpr int BOOT_FRAMES = 900;   // the boot, unseen, up to the attract's first camera (about 420)
// The next fight's stage: work RAM 501160, 0-7 round the eight stages, one on after every fight, 1P or versus (found
// against each fight's sky over a run of versus fights; set, the next fight's stage is that one)
constexpr uint32_t NEXT_STAGE = 0x1160;
constexpr int STAGES = 8;

// A player's switches as the session's input word (the stick, Punch, Kick, Guard, Start and Coin; never test or
// service), from the board's ports (active low) and back. Start is the press the host made of it (the PC way's 6
// frames), not a key held: the word is what the game gets.
constexpr uint16_t START_BUTTON = 256, COIN_BUTTON = 512;
uint16_t pack(uint8_t in0, uint8_t player_port, int player);
void unpack(const uint16_t input[2], model1 &board);

// The online version both sides compare in the handshake (with the ROMs' hash), apart from the release's: changed
// only with what a match depends on (the board, the recompiler, their compiler flags, this adapter, the match's
// settings, src/net's protocol). Named after the release that brings it (docs/RELEASING-ALL.md).
inline constexpr const char *NET_VERSION = "v1.5.0";
// NET_VERSION for a release's build (v1.4.3, or with a word after it), else the build's own (a test build plays
// only the same build)
std::string online_version(const std::string &build);

uint64_t content_hash(const model1 &board);
// Every online match first to 3 rounds: the blob's Rounds to win (vs player) is this, whatever the host's own (kept for
// offline). A guest puts the blob's assignments in its match board as they are (Adapter::begin), so a guest of any
// version plays it, in step; an older host's blob has its own setting
inline constexpr int ONLINE_VS_ROUNDS = 3;
// The host's starting blob: its backup RAM and its GAME ASSIGNMENTS (the rounds, the difficulty: put in the match
// board's EEPROM as values, whichever I/O board each side has offline; Rounds to win (vs player) ONLINE_VS_ROUNDS),
// the match's random seed (Random fighter's fighters made alike on both sides) and the host's fighter (player 1's:
// fighter_extra)
std::vector<uint8_t> start_blob(model1 &board, const std::string &host_fighter = "", uint32_t seed = 1);

// A player's fighter as the handshake carries it (Identity::extra): "" the select as ever, "R" a random fighter, a
// new one at every select, "S<seed>" a named fighter's (model1::random_fighter_names: the same on any machine with
// the ROM), "X<k>" a secret character (1-7, model1::UNUSED_CHARACTERS). As model1::set_random_player's setting
// ("", "*", "#k", or "S<seed>": set_players); false for one that isn't any of them
std::string fighter_extra(const std::string &random_player_setting, model1 &board);
bool fighter_setting(const std::string &extra, model1 &board, std::string &setting);
void set_players(model1 &board, const std::string fighters[2]);
// A fighter as a word or two under a player's name (the VS reveal): "" for Normal, "RANDOM", "SECRET: SIBA", a seed's
// fighter by its name ("SAJACWOLF", from the board's names; "SEED 1234" for a seed no name has here)
std::string fighter_words(const std::string &extra, model1 &board);

// Watching a match (src/net/watch.h): its start as the host sends it to the lobby. A watcher has no hello, so the
// guest's fighter (the handshake's extra, not in the session's blob) goes after the blob: [the session's blob][the
// guest's fighter][its length, a byte]["VF1W"]. The session's blob and handshake are as ever (old and new builds
// play each other the same); only what goes to the lobby has it. watch_parts: the session's blob and both players'
// fighters (fighter_extra's), false if it isn't one
std::vector<uint8_t> watch_blob(const std::vector<uint8_t> &blob, const std::string &guest_fighter);
bool watch_parts(const std::vector<uint8_t> &watch, std::vector<uint8_t> &blob, std::string fighters[2]);

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

// The select in lockstep (Session::set_lockstep) or not: off, the select played on guesses, as VF3's every public
// release. On, it waited for the other side's inputs in the select, and after blips of its byte mid-match: VF3's
// v0.1.11-alpha check (28 Oct, 180 ms through a relay) dropped fights to 40 fps where guessing held 57.5. The inputs
// are the same either way (each side's own choice): the game, and the online version, unchanged
constexpr bool SELECT_LOCKSTEP = false;

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

// The match's sound thread's queue of a watcher's frames caught up unseen (Adapter::sound_unheard), for its sound
// board to run through unheard, in time, alongside the catch-up, as fast as it can (so the music a cue started is
// where the players' is), and the bytes sent heard while some wait (they go with the next). Bounded, as the thread can
// fall behind a long catch-up (a watcher come in an hour into a session of rematches: 200,000 frames): past
// UNHEARD_TIMED frames waiting, the oldest are let go (their notes, sounds long over; the rest, a control's last value
// and the like, go with the next), but for those round the music's last cue (MUSIC_CUE): the MUSIC_CUE_BEFORE before
// it (the scene's set-up, sent about 120 frames before a stage's song: without it, VF2's stage was silent; no more
// than UNHEARD_TIMED, the most kept as the cue comes) and the MUSIC_CUE_FRAMES from it. Bytes sent in a burst, out of
// time, instead lost VF2's song (its sound program takes its time over a song's start and its set-up), VF1's taken
// the same way. So the board's at most 420 frames' work behind when the picture's up, a fifth of a second's (VF1's
// sound board takes about 0.4 ms a frame), the heard sound after it; the music then from its song's start, not where
// the players are in it
constexpr size_t UNHEARD_TIMED = 180, MUSIC_CUE_BEFORE = 180, MUSIC_CUE_FRAMES = 60;
// A watcher catching up from the match's start lets none go: the catch-up waits while the sound board has more than
// UNHEARD_AHEAD frames to run (Adapter::sound_unheard_waiting; near the live point, while it has any), and the queue
// holds UNHEARD_CATCH_UP (more than that and a pass's frames, Spectator::Config::catch_up_frames). Letting frames go
// there (their notes dropped, the rest folded into the next) left the board in a state the game never made, out of step
// with the players' sound at every join point tried (tools/vf1watchsound): VF2 v0.1.20's watcher come 7 minutes late
// heard the music stuck on one note and no effects till the next round's song, and VF1's queue is the same. The board
// now runs every frame a from-the-start watcher's does, in order; only a snapshot's tail (the frames before it, given
// at once) is still cut down to the queue's bound, as it always was
constexpr size_t UNHEARD_AHEAD = 120, UNHEARD_CATCH_UP = 1200;
// (a snapshot's sound bytes: the frames before it the host keeps, about 4 minutes, past a fight's song's cue; "VF1T")
constexpr size_t SOUND_TAIL = 14400;
constexpr uint32_t TAIL_MAGIC = 0x54314656;
constexpr uint8_t MUSIC_CUE[2] = { 0xae, 0x10 };   // (AE 10 nn: the song, from the bytes a match sends: one at each scene, the stage's as its fight starts)
class UnheardQueue
{
public:
	struct Piece
	{
		enum Kind : uint8_t { frame, heard };   // (a frame's bytes, in its time; heard ones, with the next)
		Kind kind = frame;
		std::vector<uint8_t> bytes;
		uint64_t n = 0;                // (a frame's: which, from the first)
		uint8_t st0 = 0;               // (the running status its bytes start with)
		std::vector<uint8_t> carry;    // (what's kept of frames let go just before it, to send first: UnheardQueue::carry)
	};
	void frame(const std::vector<uint8_t> &bytes);   // (a frame unheard: its bytes, or none)
	void heard(const std::vector<uint8_t> &bytes);
	bool next(Piece &p);                             // (the oldest, taken off)
	bool empty() const { return q.empty(); }
	size_t frames() const { return timed; }          // (the frames waiting)
	void clear();
	size_t most_timed = UNHEARD_TIMED, before_cue = MUSIC_CUE_BEFORE, cue_timed = MUSIC_CUE_FRAMES;   // (tests may change them)
	uint64_t dropped = 0;                                                                         // (frames let go)
private:
	std::deque<Piece> q;
	size_t timed = 0;
	uint64_t frames_in = 0, cue = UINT64_MAX;   // (the frames put in; the last cue's frame)
	uint8_t st = 0, d1 = 0;                     // (the MIDI read so far, for the cues)
	int need = 0, got = 0;
	bool strip = false;                // (frames let go just now: the next bytes from a status byte)
	std::vector<uint8_t> carry_next;   // (the carry for the next piece put in)
	void look(const std::vector<uint8_t> &bytes, bool is_frame);
	void strip_front(size_t i);
	static void carry(std::vector<uint8_t> &c, const std::vector<uint8_t> &bytes, uint8_t st);
};
// The data bytes after a MIDI status byte (none for one without, or for a byte that isn't a status byte)
inline int midi_data_bytes(uint8_t st) { const int k = st >> 4; return k == 0xc || k == 0xd ? 1 : k >= 0x8 && k <= 0xe ? 2 : 0; }
// A piece of the queue run through the match's sound board unheard, its output thrown away, as the match's sound
// thread sends bytes: its bytes put after those the thread has still to send (bytes), then those sent a whole MIDI
// message at a time, a byte every 8 scanlines (those sent taken off): a frame's 424 scanlines, heard bytes none (they
// go with the next); a piece's carry before its bytes. status and owed: the running status and the data bytes
// still to come of the message sent last, as the thread keeps them
void run_unheard(m1audio &board, const UnheardQueue::Piece &piece, std::vector<uint8_t> &bytes, uint8_t &status, int &owed);

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
	// A watched match's snapshot (src/net/watch.h): this build's states (its online version, the state's size); with it,
	// the host's sound bytes of the frames before it (sound_log: each frame's whole messages as its last run had them,
	// the last SOUND_TAIL), and where its bytes' messages stood, so a watcher's sound board runs them unheard as one
	// catching up from the start would (the stage's song from its cue). The native code's stack in a state is the host
	// process's own: a watcher takes the board's part alone and goes on with the interpreter (interpret: the host
	// program's switch to it, for the rest of the match), which runs the same frames to the same states (vf1soak:
	// the interpreter's board and the native code's are the same, byte for byte, at every frame); one on the native
	// code with no interpret can't take one
	uint32_t snap_format() const override;
	void snap_extra(uint32_t frame, std::vector<uint8_t> &out) override;
	// (a snapshot's state, any host's: refused unless the board would make it (model1::state_sane, the reader's checks:
	// every memory its own size), its frame the snapshot's, and not a random fighters' match; refused, the board and
	// this as they were, on the interpreter)
	bool load_snap(const std::vector<uint8_t> &state, const std::vector<uint8_t> &extra, uint32_t frame) override;
	std::pair<size_t, size_t> snap_state_bytes() const override { return { STACK_ROOM + model1::STATE_HEADER + 4, STACK_ROOM + (4u << 20) }; }
	bool sound_log = false;   // (the host's, while its match may be watched)
	std::function<void()> interpret;
	// The frame's bytes for the sound board to send now, whole MIDI messages only, each with its status byte (a
	// message the frame's bytes end in the middle of goes with the frame that finishes it): the first time a frame
	// runs, all of them (the first frame's after the boot's); run again after a rollback, those it has that weren't
	// sent for it (late). A frame run again had sent nothing if its first run had sent anything (the right timeline's
	// sounds and music cues lost), and a message cut at a frame's end could meet another timeline's bytes (a status
	// byte with another's data, data bytes under another message's running status), as VF2's (issue #35); now only a
	// wrong guess's own sounds extra (the sound board can't take back what it played)
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
	// A watcher's (Online::watch): begin takes watch_blob's form, both fighters from it, as the players have them
	bool watching = false;
	// (a watcher catching up unseen: the frames' bytes for the sound board not played as they run, a burst, but given
	// to sound_unheard, a frame's at a time (none too), for the sound board to run through unheard, in time
	// (run_unheard): they're commands sent once (a stage's music is started by one); dropped, a watcher who came in
	// mid-fight heard its effects and voices but no music till the next cue. The first frame's, the boot's set-up of
	// the sound board, still go to sound_out. Only what's heard, never the game)
	bool mute = false;
	std::function<void(const std::vector<uint8_t> &)> sound_unheard;   // (none: they're dropped)
	// (and how many of them the sound board has still to run: a watcher catching up waits for it past
	// UNHEARD_AHEAD (Online::step), so none is let go (UnheardQueue's bound) and its board is a from-the-start
	// watcher's when the picture's up; 0 none waiting. None set: it doesn't wait)
	std::function<size_t()> sound_unheard_waiting;

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
	// per frame (the last 64): where its bytes' messages stood as it started (the running status, an unfinished
	// message's bytes), from the frame before's last run, and the messages the sound board was sent for it
	struct sound_frame { uint8_t status = 0; std::vector<uint8_t> part; std::vector<std::vector<uint8_t>> sent; };
	std::map<uint32_t, sound_frame> sound_frames;
	std::vector<uint8_t> boot_sound;   // (the match's boot's, whole messages, for the first frame's first run)
	bool dropped = false;   // (bytes for the sound board muted with no sound_unheard: none heard since)
	std::deque<std::pair<uint32_t, std::vector<uint8_t>>> logged;   // (sound_log's: frame, its messages)
	std::vector<std::vector<uint8_t>> tail;   // (a snapshot's sound bytes, for the sound board as its first frame after runs)
	void save_into(std::vector<uint8_t> &state);
};

} // namespace vf1net
