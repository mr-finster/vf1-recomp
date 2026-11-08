#include "vf1_netplay.h"
#include "../rt/rec_rt.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>

namespace vf1net {

namespace {

// IN0's bits for each player (active low on the board)
constexpr uint8_t IN0_COIN[2] = { 0x01, 0x02 }, IN0_START[2] = { 0x10, 0x20 };

constexpr uint32_t BLOB_MAGIC = 0x42314656;   // "VF1B"
constexpr model1::game_assignment ASSIGNMENTS[] = {
	model1::game_assignment::match_1p, model1::game_assignment::match_vs, model1::game_assignment::difficulty,
	model1::game_assignment::advertise_sound, model1::game_assignment::continues, model1::game_assignment::language };
constexpr size_t N_ASSIGN = sizeof ASSIGNMENTS / sizeof ASSIGNMENTS[0];
constexpr size_t BACKUP_SIZE = 0x10000;

} // namespace

uint16_t pack(uint8_t in0, uint8_t port, int p)
{
	return uint16_t(uint8_t(~port) | (in0 & IN0_START[p] ? 0 : START_BUTTON) | (in0 & IN0_COIN[p] ? 0 : COIN_BUTTON));
}

void unpack(const uint16_t input[2], model1 &b)
{
	uint8_t in0 = 0xff;   // (test and service never pressed)
	for (int p = 0; p < 2; p++) {
		if (input[p] & START_BUTTON) in0 &= uint8_t(~IN0_START[p]);
		if (input[p] & COIN_BUTTON) in0 &= uint8_t(~IN0_COIN[p]);
	}
	b.m_inputs[0] = in0;
	b.m_inputs[1] = uint8_t(~input[0]);
	b.m_inputs[2] = uint8_t(~input[1]);
}

std::string online_version(const std::string &b)
{
	// a release's name: v, three numbers, and maybe a word after them (-alpha), not git describe's
	// -<commits>-g<commit> (a build between releases)
	size_t i = 0;
	auto number = [&] { const size_t at = i; while (i < b.size() && isdigit((unsigned char)b[i])) i++; return i > at; };
	bool release = !b.empty() && b[i++] == 'v' && number() && i < b.size() && b[i++] == '.' && number() && i < b.size() && b[i++] == '.' && number();
	if (release && i < b.size()) {
		release = b[i] == '-' && i + 1 < b.size() && islower((unsigned char)b[i + 1]);
		for (size_t k = i + 1; release && k < b.size(); k++)
			release = islower((unsigned char)b[k]) || isdigit((unsigned char)b[k]) || b[k] == '-';
		const size_t g = b.rfind("-g");
		if (release && g != std::string::npos && g > i && g + 2 < b.size()) {
			size_t d = b.rfind('-', g - 1);
			const std::string n = b.substr(d + 1, g - d - 1), h = b.substr(g + 2);
			const bool num = !n.empty() && n.find_first_not_of("0123456789") == std::string::npos;
			const bool hex = h.find_first_not_of("0123456789abcdef") == std::string::npos;
			if (num && hex) release = false;
		}
	}
	return release ? NET_VERSION : b;
}

uint64_t content_hash(const model1 &b) { return b.rom_hash(); }

std::vector<uint8_t> start_blob(model1 &b, const std::string &host_fighter, uint32_t seed)
{
	std::vector<uint8_t> out(4);
	memcpy(out.data(), &BLOB_MAGIC, 4);
	const std::vector<uint8_t> &backup = b.nvram();
	out.insert(out.end(), backup.begin(), backup.end());
	for (model1::game_assignment a : ASSIGNMENTS) {
		// (-1: never set, the game's own; two players' rounds always ONLINE_VS_ROUNDS, the host's own setting untouched)
		const int16_t v = int16_t(a == model1::game_assignment::match_vs ? ONLINE_VS_ROUNDS : b.assignment(a));
		out.push_back(uint8_t(v));
		out.push_back(uint8_t(uint16_t(v) >> 8));
	}
	for (int k = 0; k < 4; k++) out.push_back(uint8_t(seed >> (8 * k)));
	const std::string f = host_fighter.substr(0, vfnet::MAX_EXTRA);
	out.push_back(uint8_t(f.size()));
	out.insert(out.end(), f.begin(), f.end());
	return out;
}

std::string fighter_extra(const std::string &setting, model1 &b)
{
	if (setting.empty()) return "";
	if (setting == "*") return "R";
	if (setting[0] == '#') {   // (a secret character; one the game hasn't, from a settings.ini edited by hand: Normal, as the menu shows it)
		const int k = atoi(setting.c_str() + 1);
		return k >= 1 && k < int(std::size(model1::UNUSED_CHARACTERS)) ? "X" + std::to_string(k) : "";
	}
	std::string n;
	for (char ch : setting) if (ch != ' ') n += char(toupper(uint8_t(ch)));
	const auto &names = b.random_fighter_names();
	const auto it = names.find(n);
	return it == names.end() ? "R" : "S" + std::to_string(it->second);   // (a name no seed gives: Random, as offline)
}

bool fighter_setting(const std::string &x, model1 &b, std::string &setting)
{
	setting.clear();
	if (x.empty()) return true;
	if (x == "R") { setting = "*"; return true; }
	const bool digits = x.size() >= 2 && x.size() <= 6 && x.find_first_not_of("0123456789", 1) == std::string::npos;
	if (!digits) return false;
	const long v = strtol(x.c_str() + 1, nullptr, 10);
	if (x[0] == 'X') {
		if (v < 1 || v >= long(std::size(model1::UNUSED_CHARACTERS))) return false;
		setting = "#" + std::to_string(v);
		return true;
	}
	if (x[0] != 'S' || v < 1 || v > long(model1::RANDOM_SEEDS)) return false;
	setting = "S" + std::to_string(v);   // (a named fighter by its seed: set_players)
	return true;
}

std::string fighter_words(const std::string &x, model1 &b)
{
	if (x == "R") return "RANDOM";
	if (x.size() > 1 && x[0] == 'X') {
		const int k = atoi(x.c_str() + 1);
		if (k > 0 && k < int(std::size(model1::UNUSED_CHARACTERS))) {
			std::string n = model1::UNUSED_CHARACTERS[k];
			if (const size_t paren = n.find(" ("); paren != std::string::npos) n.resize(paren);   // (the name alone: "Siba")
			for (char &c : n) c = char(toupper(uint8_t(c)));
			return "SECRET: " + n;
		}
	}
	if (x.size() > 1 && x[0] == 'S') {
		const uint32_t seed = uint32_t(strtoul(x.c_str() + 1, nullptr, 10));
		for (const auto &[name, s] : b.random_fighter_names())
			if (s == seed) return name;
		return "SEED " + x.substr(1);
	}
	return "";
}

void set_players(model1 &b, const std::string fighters[2])
{
	for (int p = 0; p < 2; p++)
		if (fighters[p].size() > 1 && fighters[p][0] == 'S') b.set_random_player_seed(p, uint32_t(strtoul(fighters[p].c_str() + 1, nullptr, 10)));
		else b.set_random_player(p, fighters[p]);
}

namespace {
constexpr uint32_t WATCH_MAGIC = 0x57314656;   // "VF1W"
}

std::vector<uint8_t> watch_blob(const std::vector<uint8_t> &blob, const std::string &guest)
{
	const std::string g = guest.substr(0, vfnet::MAX_EXTRA);
	std::vector<uint8_t> out = blob;
	out.insert(out.end(), g.begin(), g.end());
	out.push_back(uint8_t(g.size()));
	for (int k = 0; k < 4; k++) out.push_back(uint8_t(WATCH_MAGIC >> (8 * k)));
	return out;
}

bool watch_parts(const std::vector<uint8_t> &w, std::vector<uint8_t> &blob, std::string fighters[2])
{
	constexpr size_t FIXED = 4 + BACKUP_SIZE + 2 * N_ASSIGN + 4 + 1;   // (start_blob's, before the host's fighter)
	uint32_t magic = 0;
	if (w.size() < FIXED + 5) return false;
	memcpy(&magic, w.data() + w.size() - 4, 4);
	const size_t glen = w[w.size() - 5];
	if (magic != WATCH_MAGIC || glen > vfnet::MAX_EXTRA || w.size() < FIXED + 5 + glen) return false;
	const size_t n = w.size() - 5 - glen;   // (the session's blob)
	if (n != FIXED + w[FIXED - 1]) return false;
	blob.assign(w.begin(), w.begin() + ptrdiff_t(n));
	fighters[0].assign(reinterpret_cast<const char *>(w.data()) + FIXED, w[FIXED - 1]);
	fighters[1].assign(reinterpret_cast<const char *>(w.data()) + n, glen);
	return true;
}

std::vector<uint8_t> Adapter::host_blob()
{
	uint32_t seed = uint32_t(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) * 2654435761u) ^ 0x9e3779b9u;
	if (!seed) seed = 1;
	return start_blob(board, own_extra, seed);
}

namespace {
// MIDI bytes into whole messages: status, running status, an unfinished message carried in `part`
void midi_messages(const std::vector<uint8_t> &bytes, uint8_t &status, std::vector<uint8_t> &part, std::vector<std::vector<uint8_t>> &out)
{
	for (const uint8_t x : bytes) {
		if (x >= 0xf8) { out.push_back({ x }); continue; }   // (a timing byte: alone, wherever it comes)
		if (x & 0x80) {   // (a message's start: one cut short before it is dropped)
			part.assign(1, x);
			status = x >= 0xf0 ? 0 : x;   // (a system message ends running status)
			if (!midi_data_bytes(x)) { out.push_back(part); part.clear(); }
			continue;
		}
		if (part.empty()) {
			if (!status) { out.push_back({ x }); continue; }   // (data with no status known: as it came)
			part.assign(1, status);   // (running status: another of the last kind)
		}
		part.push_back(x);
		if (int(part.size()) == 1 + midi_data_bytes(part[0])) { out.push_back(part); part.clear(); }
	}
}
} // namespace

// (whole messages, see the header)
void Adapter::take_sound(std::vector<uint8_t> &out)
{
	sound_taken = true;
	out.clear();
	if (frame == 0 && !boot_sound.empty()) {   // (the boot's, with the first frame's first run)
		out = std::move(boot_sound);
		boot_sound.clear();
	}
	sound_frame &f = sound_frames[frame];
	uint8_t status = f.status;
	std::vector<uint8_t> part = f.part;
	std::vector<std::vector<uint8_t>> msgs;
	midi_messages(pending, status, part, msgs);
	pending.clear();
	if (sound_log) {   // (this run's messages, whole: the frame's, once it's confirmed, for a snapshot)
		while (!logged.empty() && logged.back().first >= frame) logged.pop_back();   // (run again: from here, as it is now)
		std::vector<uint8_t> all;
		for (const auto &m : msgs) all.insert(all.end(), m.begin(), m.end());
		logged.push_back({ frame, std::move(all) });
		while (logged.size() > SOUND_TAIL + 64) logged.pop_front();
	}
	sound_frame &next = sound_frames[frame + 1];   // (where the next frame's bytes start: this run's, the latest)
	next.status = status;
	next.part = std::move(part);
	// the messages not sent for this frame yet (those sent that it doesn't have now can't be taken back: a wrong guess's
	// sound stays)
	std::vector<bool> matched(f.sent.size(), false);
	for (auto &m : msgs) {
		size_t k = 0;
		while (k < f.sent.size() && (matched[k] || f.sent[k] != m)) k++;
		if (k < f.sent.size()) { matched[k] = true; continue; }
		out.insert(out.end(), m.begin(), m.end());
		f.sent.push_back(std::move(m));
		matched.push_back(true);
	}
	while (!sound_frames.empty() && sound_frames.begin()->first + 64 < frame) sound_frames.erase(sound_frames.begin());
}

bool Adapter::begin(const std::vector<uint8_t> &given)
{
	// (a watcher's: the session's blob, and the guest's fighter after it, as the host sent it to the lobby)
	std::vector<uint8_t> watched;
	std::string wx[2];
	if (watching && !watch_parts(given, watched, wx)) return false;
	const std::vector<uint8_t> &blob = watching ? watched : given;
	uint32_t magic = 0;
	constexpr size_t FIXED = 4 + BACKUP_SIZE + 2 * N_ASSIGN + 4 + 1;
	if (blob.size() < FIXED || blob.size() != FIXED + blob[FIXED - 1] || board.nvram().size() != BACKUP_SIZE) return false;
	memcpy(&magic, blob.data(), 4);
	if (magic != BLOB_MAGIC) return false;
	uint32_t seed = 0;
	memcpy(&seed, blob.data() + FIXED - 5, 4);
	// each player's fighter: the host's (player 1's) in its blob, the guest's in its hello
	const std::string host_x(reinterpret_cast<const char *>(blob.data()) + FIXED, blob[FIXED - 1]);
	if (!fighter_setting(host_x, board, fighters[0]) || !fighter_setting(watching ? wx[1] : hosting ? their_extra : own_extra, board, fighters[1])) return false;
	const bool random = !fighters[0].empty() || !fighters[1].empty();
	// (a seed's fighter: every seed's name made now, before the boot, as a named fighter's own side does at the
	// start (fighter_extra), so the VS screen names it, fighter_words; the board as it was after)
	if ((fighters[0].size() > 1 && fighters[0][0] == 'S') || (fighters[1].size() > 1 && fighters[1][0] == 'S')) board.random_fighter_names();
	if (settle) settle();
	if (on_begin) on_begin();
	collecting = false;
	// the same game on both sides, whatever each plays offline: the cheats off (Random fighter, the secret
	// character, no time limit, the ring's width, Kage's mask: the host's play clock in the backup RAM has his),
	// the FREE PLAY notices hidden, the I/O board's stand-in, no sound board on the board
	board.set_random_fighter(false);
	board.set_akira_slot(0);
	board.set_ring_width(0);
	board.set_kage_unmasked(false);
	board.hide_free_play_text();
	bool code = board.set_no_time_limit(false);
	code |= board.set_strands_moved(false);
	board.set_random_fighter(false);   // (again, its moves off: its part of the ROM as VF's)
	// (the named fighters' list made now if it wasn't (every seed's fighter made: Random picks from it), so it's
	// never first made during the match on one side only; then everything Random fighter knew forgotten)
	if (random) board.random_fighter_names();
	board.random_fighter_forget();
	// the players' fighters (Random fighter, its moves too, only when someone has one: it patches the program),
	// made from the host's seed, the same on both sides
	if (random) {
		set_players(board, fighters);
		code |= board.set_strands_moved(true);
	}
	if (code && program_changed) program_changed();
	board.use_io_standin(true);
	board.on_uart_tx = [this](uint8_t b) { if (collecting) pending.push_back(b); };
	auto cold = [&](const uint8_t *eeprom) {
		board.power_on();
		memcpy(board.nvram().data(), blob.data() + 4, BACKUP_SIZE);
		if (eeprom) memcpy(board.eeprom(), eeprom, model1::EEPROM_SIZE);
		if (native) rec::attach(board.cpu());   // (a fresh game stack)
	};
	auto boot = [&] {
		for (int i = 0; i < BOOT_FRAMES && !board.past_boot(); i++) {
			board.run_frame();
			if (booting && i % 60 == 59) booting();
		}
	};
	// the stand-in's EEPROM starts empty: the first boot writes the game's settings; then free play and the host's
	// assignments, and a cold start again with them
	const bool drawing = board.drawing();
	cold(nullptr);
	board.set_drawing(false);
	boot();
	if (board.coin_setting() < 0) return false;
	board.set_coin_setting(model1::FREE_PLAY);
	for (size_t i = 0; i < N_ASSIGN; i++) {
		const int16_t v = int16_t(blob[4 + BACKUP_SIZE + 2 * i] | blob[4 + BACKUP_SIZE + 2 * i + 1] << 8);
		if (v >= 0) board.set_assignment(ASSIGNMENTS[i], v);
	}
	uint8_t eeprom[model1::EEPROM_SIZE];
	memcpy(eeprom, board.eeprom(), sizeof eeprom);
	cold(eeprom);
	// Random fighter on only now, from the cold start's RAM (it keeps a fighter a player has picked, by what the RAM
	// says: before, that was each player's own game's), made from the host's seed
	if (random) {
		board.set_random_seed(seed);
		board.set_random_fighter(true, true, false);
	}
	// (the boot's bytes too, from this cold start: the sound board needs its set-up; they go with the first frame's)
	collecting = true;
	pending.clear();
	boot();
	// the stages from the host's random place in their list, as VF3's (a cold start's at its start: every match's first
	// fight was on the same stage), from the host's seed: the same on both sides
	board.work_ram()[NEXT_STAGE] = uint8_t(((seed * 2654435761u) >> 16) % STAGES);
	board.set_drawing(drawing);
	frame = 0;
	if (const char *d = getenv("VF1_DUMP_BEGIN")) {   // (tests: the state the match starts from, and its parts)
		std::vector<uint8_t> st;
		std::vector<m1::StateSection> sec;
		board.save_state(st, &sec);
		if (FILE *f = fopen(d, "wb")) { fwrite(st.data(), 1, st.size(), f); fclose(f); }
		if (FILE *f = fopen((std::string(d) + ".txt").c_str(), "w")) { for (auto &x : sec) fprintf(f, "%s %zu\n", x.name, x.offset); fclose(f); }
	}
	sound_frames.clear();
	{   // (the boot's bytes as whole messages, sent with the first frame's first run; the match's first frame's bytes
		// start where they ended, a frame 0 run again too)
		sound_frame &f0 = sound_frames[0];
		std::vector<std::vector<uint8_t>> msgs;
		midi_messages(pending, f0.status, f0.part, msgs);
		pending.clear();
		boot_sound.clear();
		for (auto &m : msgs) boot_sound.insert(boot_sound.end(), m.begin(), m.end());
	}
	dropped = false;
	checkpoints.clear();
	failed_stack = false;
	return board.past_boot();
}

uint64_t Adapter::hash(const std::vector<uint8_t> &state)
{
	const size_t skip = std::min(state.size(), hash_skip());
	return vfnet::hash64(state.data() + skip, state.size() - skip);
}

// [STACK_ROOM: the stack's size and bytes][the board's state][the frame number]
void Adapter::save_into(std::vector<uint8_t> &state)
{
	state.resize(STACK_ROOM);
	stack_buf.clear();
	if (native) rec::save_stack(stack_buf);
	if (stack_buf.size() > STACK_ROOM) { failed_stack = true; stack_buf.clear(); }
	memset(state.data(), 0, sizeof(uint64_t));
	if (!stack_buf.empty()) memcpy(state.data(), stack_buf.data(), stack_buf.size());
	board.save_state(state, nullptr, true);
	const size_t n = state.size();
	state.resize(n + 4);
	memcpy(state.data() + n, &frame, 4);
}

uint32_t Adapter::snap_format() const
{
	const std::string k = std::string("vf1|") + NET_VERSION + "|" + std::to_string(STACK_ROOM) + "|" + std::to_string(board.nvram().size()) + "|" +
	                      std::to_string(board.work_ram().size()) + "|" + std::to_string(sizeof(void *));
	uint32_t h = 2166136261u;
	for (const char c : k) h = (h ^ uint8_t(c)) * 16777619u;
	return h ? h : 1;
}

// (the extra: "VF1T", the first frame, how many, each frame's bytes (u16 length, them), then where the frame's bytes'
// messages stand: the running status and an unfinished message's bytes (u8 length, them))
void Adapter::snap_extra(uint32_t at, std::vector<uint8_t> &out)
{
	out.clear();
	std::vector<const std::pair<uint32_t, std::vector<uint8_t>> *> v;
	for (const auto &e : logged)
		if (e.first < at && e.first + SOUND_TAIL >= at) v.push_back(&e);
	while (!v.empty() && v.front()->first + v.size() != at) v.erase(v.begin());   // (one frame after another up to it: none missing)
	auto u32 = [&](uint32_t x) { for (int k = 0; k < 4; k++) out.push_back(uint8_t(x >> (8 * k))); };
	u32(TAIL_MAGIC);
	u32(v.empty() ? at : v.front()->first);
	u32(uint32_t(v.size()));
	for (const auto *e : v) {
		const size_t n = std::min<size_t>(e->second.size(), 65535);
		out.push_back(uint8_t(n));
		out.push_back(uint8_t(n >> 8));
		out.insert(out.end(), e->second.begin(), e->second.begin() + ptrdiff_t(n));
	}
	const auto f = sound_frames.find(at);
	const uint8_t status = f != sound_frames.end() ? f->second.status : 0;
	const std::vector<uint8_t> part = f != sound_frames.end() ? f->second.part : std::vector<uint8_t>();
	out.push_back(status);
	out.push_back(uint8_t(std::min<size_t>(part.size(), 255)));
	out.insert(out.end(), part.begin(), part.begin() + ptrdiff_t(std::min<size_t>(part.size(), 255)));
}

bool Adapter::load_snap(const std::vector<uint8_t> &state, const std::vector<uint8_t> &extra)
{
	if (state.size() < STACK_ROOM + 4 || (native && !interpret)) return false;
	if (settle) settle();
	if (native) {   // (the board's part alone: the interpreter from here, the host's stack being its process's)
		interpret();
		native = false;
	}
	memcpy(&frame, state.data() + state.size() - 4, 4);
	pending.clear();
	if (!board.load_state(state.data() + STACK_ROOM, state.size() - STACK_ROOM - 4)) return false;
	sound_frames.clear();
	tail.clear();
	if (!boot_sound.empty()) tail.push_back(std::move(boot_sound));   // (the boot's set-up of the sound board first, as frame 0's run would send it)
	boot_sound.clear();
	dropped = false;
	// (the host's sound bytes before it, if they read whole; else none: the music from its next cue)
	size_t at = 0;
	auto need = [&](size_t n) { return extra.size() - at >= n; };
	auto u32 = [&] { uint32_t x = 0; for (int k = 0; k < 4; k++) x |= uint32_t(extra[at++]) << (8 * k); return x; };
	if (!need(12) || u32() != TAIL_MAGIC) return true;
	const uint32_t first = u32(), n = u32();
	if (n > SOUND_TAIL || uint64_t(first) + n != frame) return true;
	std::vector<std::vector<uint8_t>> got;
	for (uint32_t i = 0; i < n; i++) {
		if (!need(2)) return true;
		const size_t len = extra[at] | size_t(extra[at + 1]) << 8;
		at += 2;
		if (!need(len)) return true;
		got.emplace_back(extra.begin() + ptrdiff_t(at), extra.begin() + ptrdiff_t(at + len));
		at += len;
	}
	if (!need(2)) return true;
	const uint8_t status = extra[at], pl = extra[at + 1];
	at += 2;
	if (!need(pl)) return true;
	sound_frame &sf = sound_frames[frame];
	sf.status = status >= 0x80 && status < 0xf0 ? status : 0;
	sf.part.assign(extra.begin() + ptrdiff_t(at), extra.begin() + ptrdiff_t(at + pl));
	for (auto &b : got) tail.push_back(std::move(b));
	return true;
}

void Adapter::save(std::vector<uint8_t> &state)
{
	if (settle) settle();
	save_into(state);
}

bool Adapter::load(const std::vector<uint8_t> &state)
{
	if (state.size() < STACK_ROOM + 4 || failed_stack) return false;
	if (settle) settle();
	if (native) {
		uint64_t used = 0;
		memcpy(&used, state.data(), sizeof used);
		if (used + sizeof used > STACK_ROOM || !rec::load_stack(state.data(), size_t(sizeof used + used))) return false;
	}
	memcpy(&frame, state.data() + state.size() - 4, 4);
	pending.clear();
	return board.load_state(state.data() + STACK_ROOM, state.size() - STACK_ROOM - 4);
}

void Adapter::advance(const uint16_t input[2], bool resim)
{
	unpack(input, board);
	if (!tail.empty()) {   // (a watcher's first frame after a snapshot: the host's frames before it, for the sound board unheard)
		if (sound_unheard)
			for (const auto &b : tail) sound_unheard(b);
		tail.clear();
	}
	sound_taken = false;
	if (run) run(resim);
	else board.run_frame();
	if (!sound_taken) {
		std::vector<uint8_t> out;
		take_sound(out);
		if (mute && frame != 0) {   // (a watcher catching up: unheard, in time)
			if (sound_unheard) sound_unheard(out);
			else dropped = dropped || !out.empty();
		} else if (!out.empty() && sound_out) {
			if (dropped)   // (bytes dropped before these: from a status byte, their first data bytes no message's start)
				out.erase(out.begin(), std::find_if(out.begin(), out.end(), [](uint8_t x) { return x >= 0x80; }));
			dropped = false;
			if (!out.empty()) sound_out(out);
		}
	}
	frame++;
	if (checkpoint_every && frame % checkpoint_every == 0) {
		save_into(check_buf);
		checkpoints[frame] = hash(check_buf);
		if (keep_states) checkpoint_states[frame] = check_buf;
	}
}

void UnheardQueue::look(const std::vector<uint8_t> &b, bool is_frame)
{
	for (const uint8_t x : b) {
		if (x >= 0xf8) continue;
		if (x & 0x80) { st = x; need = midi_data_bytes(x); got = 0; continue; }
		if (!need) { need = midi_data_bytes(st); got = 0; }   // (running status)
		if (!need) continue;
		if (got++ == 0) d1 = x;
		if (--need == 0 && st == MUSIC_CUE[0] && d1 == MUSIC_CUE[1]) cue = is_frame ? frames_in : frames_in + 1;
	}
}

void UnheardQueue::heard(const std::vector<uint8_t> &b)
{
	const uint8_t st0 = st;
	look(b, false);
	q.push_back({ Piece::heard, b, 0, st0, std::move(carry_next) });
	carry_next.clear();
	if (strip) strip_front(q.size() - 1);
}

void UnheardQueue::frame(const std::vector<uint8_t> &b)
{
	const uint8_t st0 = st;
	look(b, true);
	q.push_back({ Piece::frame, b, frames_in++, st0, std::move(carry_next) });
	carry_next.clear();
	timed++;
	if (strip) strip_front(q.size() - 1);
	// past most_timed frames waiting (not counting those round the last cue), the oldest of the others let go: their
	// notes (sounds that would have ended by now), but not the rest, which go with the next one (carry)
	auto kept = [&](const Piece &p) { return cue != UINT64_MAX && p.n + before_cue >= cue && p.n < cue + cue_timed; };
	size_t kept_n = 0;
	for (const Piece &p : q) kept_n += p.kind == Piece::frame && kept(p);
	while (timed - kept_n > most_timed) {
		size_t i = 0;
		while (i < q.size() && (q[i].kind != Piece::frame || kept(q[i]))) i++;
		if (i == q.size()) break;
		std::vector<uint8_t> c = std::move(q[i].carry);
		carry(c, q[i].bytes, q[i].st0);
		q.erase(q.begin() + ptrdiff_t(i));
		timed--;
		dropped++;
		strip = true;
		if (i < q.size()) {
			carry(c, q[i].carry, 0);
			q[i].carry = std::move(c);
			strip_front(i);
		} else {
			carry(c, carry_next, 0);
			carry_next = std::move(c);
		}
	}
}

// (a frame let go: its messages put after carry's, but its notes on (sounds over by now) and the music's cues (the last
// one's never let go: those before it are over too); then each once, its last: a control's last value, a note's last
// off, a channel's last program. s: the running status its bytes start with)
void UnheardQueue::carry(std::vector<uint8_t> &c, const std::vector<uint8_t> &bytes, uint8_t s)
{
	struct M { uint8_t b[3]; int n; };
	std::vector<M> ms;
	auto read = [&](const std::vector<uint8_t> &v, uint8_t s) {
		int need = 0;
		M m{};
		for (const uint8_t x : v) {
			if (x >= 0xf8) continue;
			if (x & 0x80) { s = x; need = 0; continue; }
			if (!need) {
				need = midi_data_bytes(s);
				if (!need) continue;
				m = { { s, 0, 0 }, 1 };
			}
			m.b[m.n++] = x;
			if (--need == 0) ms.push_back(m);
		}
	};
	read(c, 0);
	read(bytes, s);
	auto note = [](const M &m) { return (m.b[0] & 0xf0) == 0x90 && m.b[2] != 0; };
	auto key = [](const M &m) {   // (a note off, either way it's sent, as one; a program, a pressure, a bend by its channel)
		const uint8_t k = m.b[0] & 0xf0;
		const uint8_t st = k == 0x90 ? uint8_t(0x80 | (m.b[0] & 0x0f)) : m.b[0];
		return k >= 0xc0 ? int(st) << 8 : int(st) << 8 | m.b[1];
	};
	std::vector<M> out;
	std::vector<int> seen;
	for (size_t i = ms.size(); i-- > 0;) {
		const M &m = ms[i];
		if (note(m) || (m.b[0] == MUSIC_CUE[0] && m.b[1] == MUSIC_CUE[1])) continue;
		if (std::find(seen.begin(), seen.end(), key(m)) != seen.end()) continue;
		seen.push_back(key(m));
		out.push_back(m);
	}
	c.clear();
	for (size_t i = out.size(); i-- > 0;) c.insert(c.end(), out[i].b, out[i].b + out[i].n);
}

// (after bytes let go: the next ones from a status byte, a message's data without its status byte being taken for
// another's)
void UnheardQueue::strip_front(size_t i)
{
	for (; i < q.size() && strip; i++) {
		std::vector<uint8_t> &u = q[i].bytes;
		const auto st = std::find_if(u.begin(), u.end(), [](uint8_t x) { return x >= 0x80; });
		strip = st == u.end();
		u.erase(u.begin(), st);
	}
}

bool UnheardQueue::next(Piece &p)
{
	if (q.empty()) return false;
	p = std::move(q.front());
	q.pop_front();
	if (p.kind == Piece::frame) timed--;
	return true;
}

void UnheardQueue::clear()
{
	q.clear();
	timed = 0;
	frames_in = 0;
	cue = UINT64_MAX;
	st = d1 = 0;
	need = got = 0;
	strip = false;
	carry_next.clear();
	dropped = 0;
}

void run_unheard(m1audio &b, const UnheardQueue::Piece &p, std::vector<uint8_t> &q, uint8_t &status, int &owed)
{
	q.insert(q.end(), p.carry.begin(), p.carry.end());
	q.insert(q.end(), p.bytes.begin(), p.bytes.end());
	if (p.kind == UnheardQueue::Piece::heard) return;
	constexpr int LINES = 424, BYTE_LINES = 8;   // (as the match's sound thread: a frame's scanlines, a byte every 8)
	const double line_us = 656 * 1e6 / 16e6;
	size_t whole = 0;   // (the bytes up to the last whole message, as the thread works it out)
	{
		uint8_t st = status;
		int need = owed;
		for (size_t i = 0; i < q.size(); i++) {
			if (q[i] >= 0xf8) { if (!need) whole = i + 1; continue; }
			if (q[i] & 0x80) { st = q[i]; need = midi_data_bytes(st); if (!need) whole = i + 1; continue; }
			if (!need) need = midi_data_bytes(st);
			if (!need || --need == 0) whole = i + 1;
		}
	}
	std::vector<int16_t> out;
	size_t sent = 0;
	for (int line = 0; line < LINES; line++) {
		if (line % BYTE_LINES == 0 && sent < whole) {
			const uint8_t x = q[sent++];
			if (x >= 0xf8) {}
			else if (x & 0x80) status = x, owed = midi_data_bytes(x);
			else if (owed || (owed = midi_data_bytes(status))) owed--;
			b.rx_byte(x);
		}
		b.run(line_us, out);
		out.clear();
		while (b.tx_pending()) b.tx_byte();
	}
	q.erase(q.begin(), q.begin() + ptrdiff_t(sent));
}

uint8_t Score::players_in(const model1 &board)
{
	const std::vector<uint8_t> &a = const_cast<model1 &>(board).nvram();
	// (0x10 and 0x08 both: 0x10 stays set after a player's lost a versus, the CPU taking their side, until the game's over)
	auto in = [&](uint32_t o) { return (a[o] & 0x18) == 0x18; };
	return uint8_t((in(PLAYING_P1) ? 1 : 0) | (in(PLAYING_P2) ? 2 : 0));
}

void Score::frame(const model1 &board)
{
	const std::vector<uint8_t> &ram = const_cast<model1 &>(board).work_ram();
	const uint8_t r[3] = { ram[ROUNDS_P1], ram[ROUNDS_P2], players_in(board) };
	if (memcmp(r, now, 3) != 0) { memcpy(now, r, 3); held = 0; return; }
	if (++held != 30) return;   // (once, when it's held long enough)
	playing = r[2];
	for (int p = 0; p < 2; p++)
		if (r[p] == seen[p] + 1) {
			round_ends++;
			if (best[0] + best[1] == 0) versus = playing == 3;   // (the match's first round)
			if (playing != 3) versus = false;
			if (versus) rounds[p]++;
		}
	for (int p = 0; p < 2; p++) best[p] = std::max(best[p], r[p]);
	if (r[0] == 0 && r[1] == 0 && best[0] + best[1] > 0) {   // (the match over: whoever reached more won it)
		if (versus && best[0] != best[1]) matches[best[0] > best[1] ? 0 : 1]++;
		versus = false;
		best[0] = best[1] = 0;
	}
	seen[0] = r[0];
	seen[1] = r[1];
}

} // namespace vf1net
