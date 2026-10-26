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
		const int16_t v = int16_t(b.assignment(a));   // (-1: never set, the game's own)
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
	if (setting[0] == '#') return "X" + std::to_string(atoi(setting.c_str() + 1));
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

std::string fighter_words(const std::string &x)
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
	if (x.size() > 1 && x[0] == 'S') return "SEED " + x.substr(1);
	return "";
}

void set_players(model1 &b, const std::string fighters[2])
{
	for (int p = 0; p < 2; p++)
		if (fighters[p].size() > 1 && fighters[p][0] == 'S') b.set_random_player_seed(p, uint32_t(strtoul(fighters[p].c_str() + 1, nullptr, 10)));
		else b.set_random_player(p, fighters[p]);
}

std::vector<uint8_t> Adapter::host_blob()
{
	uint32_t seed = uint32_t(std::chrono::steady_clock::now().time_since_epoch().count() * 2654435761u) ^ 0x9e3779b9u;
	if (!seed) seed = 1;
	return start_blob(board, own_extra, seed);
}

void Adapter::take_sound(std::vector<uint8_t> &out)
{
	sound_taken = true;
	out.clear();
	auto it = sound_sent.find(frame);
	const bool sent = it != sound_sent.end() && !it->second.empty();
	if (!pending.empty() && !sent) {
		out = pending;
		sound_sent[frame] = pending;
	} else if (it == sound_sent.end())
		sound_sent[frame] = {};
	pending.clear();
	while (!sound_sent.empty() && sound_sent.begin()->first + 64 < frame) sound_sent.erase(sound_sent.begin());
}

bool Adapter::begin(const std::vector<uint8_t> &blob)
{
	uint32_t magic = 0;
	constexpr size_t FIXED = 4 + BACKUP_SIZE + 2 * N_ASSIGN + 4 + 1;
	if (blob.size() < FIXED || blob.size() != FIXED + blob[FIXED - 1] || board.nvram().size() != BACKUP_SIZE) return false;
	memcpy(&magic, blob.data(), 4);
	if (magic != BLOB_MAGIC) return false;
	uint32_t seed = 0;
	memcpy(&seed, blob.data() + FIXED - 5, 4);
	// each player's fighter: the host's (player 1's) in its blob, the guest's in its hello
	const std::string host_x(reinterpret_cast<const char *>(blob.data()) + FIXED, blob[FIXED - 1]);
	if (!fighter_setting(host_x, board, fighters[0]) || !fighter_setting(hosting ? their_extra : own_extra, board, fighters[1])) return false;
	const bool random = !fighters[0].empty() || !fighters[1].empty();
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
	sound_sent.clear();
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
	sound_taken = false;
	if (run) run(resim);
	else board.run_frame();
	if (!sound_taken) {
		std::vector<uint8_t> out;
		take_sound(out);
		if (!out.empty() && sound_out) sound_out(out);
	}
	frame++;
	if (checkpoint_every && frame % checkpoint_every == 0) {
		save_into(check_buf);
		checkpoints[frame] = hash(check_buf);
		if (keep_states) checkpoint_states[frame] = check_buf;
	}
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
