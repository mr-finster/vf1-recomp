// Plays a script of inputs headlessly, for operating the game's menus (test mode settings).
// Starts from the saved NVRAM/EEPROM in the regions directory and writes them back on `save`.
//   model1script [--game vf|vr] <regions dir> <script> <shot dir>
// Script lines:   wait N | press PORT MASK FRAMES | hold PORT MASK | release PORT | shot NAME |
//                 shots NAME N | dump NAME | poke16 ADDR VAL | watch ADDR | maps NAME |
//                 every NAME N K | save   (M1_SCALE / M1_ASPECT / M1_REC as for model1run)
//                 the game's own commands (src/games/script.h: VF's mash, brawl, fight; and
//                 tools/tool_game.h: VF's icon)
//                 rec NAME | stop: video of what's between, 60 pictures per second of the game's
//                 time, to NAME.mkv (lossless, ffmpeg), its sound to NAME.wav (M1_SOUND=1;
//                 M1_MUSIC / M1_EFFECTS levels), and NAME.txt: each picture's moment in game frames
//                 (M1_SMOOTH=1: smooth motion, as the game above 1x; M1_OUT=WxH scaled for an edit,
//                 M1_OUT_FLAGS its ffmpeg scaler, lanczos by default; M1_INLOG=file: each frame's
//                 inputs, for tools/mame_play.sh)
//   PORT 0: coin1 01 coin2 02 test 04 service 08 start1 10 start2 20; PORT 1/2: players
#include "../src/host/regions.h"
#include "../src/host/png.h"
#include "../src/host/backgrounds.h"
#include "../src/host/scene_sync.h"
#include "../src/games/script.h"
#include "../src/rt/rec_rt.h"
#include "tool_args.h"
#include "tool_game.h"
#include <fstream>
#include <sstream>

static bool load_blob(const std::string &path, uint8_t *dst, size_t n)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	bool ok = fread(dst, 1, n, f) == n;
	fclose(f);
	return ok;
}

static void save_blob(const std::string &path, const uint8_t *src, size_t n)
{
	if (FILE *f = fopen(path.c_str(), "wb")) { fwrite(src, 1, n, f); fclose(f); }
}

int main(int argc, char **argv)
{
	tool_args opt;
	if (!opt.take(argc, argv)) return 2;
	if (argc < 4) { fprintf(stderr, "usage: model1script [--game vf|vr] regions script shotdir\n"); return 1; }
	std::string dir = argv[1];
	const game::id title = opt.game_for(dir);
	model1 m(load_regions(dir, title));
	const std::unique_ptr<tool_game> game_part = make_tool_game(title, m);
	const std::string nv = dir + "/nvram.sav", ee = dir + (m.real_ioboard() ? "/eeprom_93c46.sav" : "/eeprom.sav");
	load_blob(nv, m.nvram().data(), m.nvram().size());
	load_blob(ee, m.eeprom(), model1::EEPROM_SIZE);
	game_part->script_setup();                       // the game's own options (VF: free play, cheats)
	if (getenv("M1_PCMODE")) printf("pc mode: %d\n", game_part->pc_mode());
	if (const char *bg = getenv("M1_BACKGROUNDS")) { load_backgrounds(m, bg); game_part->load_pictures(bg); }   // hi-res skies, the game's own
	if (getenv("M1_NODRAW")) m.set_drawing(false);   // faster when only exporting
	if (const char *sc = getenv("M1_SCALE")) m.set_render_scale(atoi(sc));
	if (getenv("M1_SKY_FILTER")) m.set_sky_filter(true);   // the menu's Background filter: Smooth
	if (const char *as = getenv("M1_ASPECT")) m.set_aspect(strtof(as, nullptr));
	const bool sound = getenv("M1_SOUND") != nullptr;
	if (sound) {
		m.enable_sound();
		m.set_volumes(getenv("M1_MUSIC") ? strtof(getenv("M1_MUSIC"), nullptr) : 1,
					  getenv("M1_EFFECTS") ? strtof(getenv("M1_EFFECTS"), nullptr) : 1);
	}
	const bool smooth = getenv("M1_SMOOTH") != nullptr;
	if (smooth) m.set_smooth(true);
	m.reset();
	if (getenv("M1_REC")) {                // the recompiled code instead of the interpreter
		rec::attach(m.cpu(), title.name);
		m.run_cpu = [](int n) { rec::run(n); };
	}
	game_part->script_started(getenv("M1_REC") != nullptr);
	std::ifstream in(argv[2]);
	std::string line;
	std::vector<unsigned> watch;          // work-RAM words whose changes are logged
	std::vector<int> last;
	int fno = 0;
	std::vector<std::pair<int, int>> ranges;   // M1_SHOTS=a-b,c-d: also shoot every frame in these
	if (const char *c = getenv("M1_SHOTS"))
		for (const char *q = c; *q;) {
			int a = 0, b = 0, used = 0;
			if (sscanf(q, "%d-%d%n", &a, &b, &used) < 2) break;
			ranges.push_back({a, b});
			q += used;
			if (*q == ',') q++;
		}
	// M1_SKYDIR=dir [M1_SKYEVERY=n]: every n frames, the frame as sky_NNNNNN.png plus a line in
	// dir/sky.txt (frame, the sky map's scroll, the camera pointer): for stitching the panoramas.
	// Run with M1_TILE_SKIP=cf M1_NO_POLYS=1 so the frame holds the background layers alone.
	const char *skydir = getenv("M1_SKYDIR");
	const int skyevery = getenv("M1_SKYEVERY") ? atoi(getenv("M1_SKYEVERY")) : 1;
	FILE *skylog = skydir ? fopen((std::string(skydir) + "/sky.txt").c_str(), "w") : nullptr;
	scene_sync sync;                                // M1_SCENE_SYNC=1: shots as the game shows them
	sync.enabled = getenv("M1_SCENE_SYNC") != nullptr;
	std::vector<uint32_t> shown;
	if (sync.enabled) sync.start();
	// recording: pictures at 60 per second of game time (the game runs at FRAME_HZ)
	constexpr double FRAME_HZ = 16000000.0 / (656.0 * 424.0);
	FILE *vid = nullptr, *moments = nullptr;
	std::vector<int16_t> rec_audio;
	std::string rec_name;
	double rec_t0 = 0;
	long rec_k = 0, rec_left = -1;                 // pictures still to take in a clip (-1: until stop)
	struct clip { std::string name; double t0; long pictures; };
	std::vector<clip> clips;                       // clip NAME FROM LEN: scheduled recordings
	auto open_rec = [&](const std::string &name, double t0, long pictures) {
		rec_name = name;
		const std::string base = std::string(argv[3]) + "/" + rec_name;
		char cmdl[1024];
		// M1_OUT=WxH: scaled (lanczos) and encoded for the edit; otherwise lossless at the game's size
		if (const char *o = getenv("M1_OUT"))
			snprintf(cmdl, sizeof cmdl, "ffmpeg -v error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 60 -i - "
					 "-vf scale=%s:flags=%s -c:v libx264 -crf 12 -preset fast -pix_fmt yuv420p '%s.mkv'",
					 m.frame_w(), m.frame_h(), o, getenv("M1_OUT_FLAGS") ? getenv("M1_OUT_FLAGS") : "lanczos", base.c_str());
		else
			snprintf(cmdl, sizeof cmdl, "ffmpeg -v error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 60 -i - "
					 "-c:v libx264rgb -crf 0 -preset ultrafast '%s.mkv'", m.frame_w(), m.frame_h(), base.c_str());
		vid = popen(cmdl, "w");
		moments = fopen((base + ".txt").c_str(), "w");
		rec_t0 = t0;
		rec_k = 0;
		rec_left = pictures;
		rec_audio.clear();
	};
	auto close_rec = [&] {
		if (vid) { pclose(vid); fclose(moments); vid = nullptr; }
		if (sound) {
			const std::string w = std::string(argv[3]) + "/" + rec_name + ".wav";
			FILE *f = fopen(w.c_str(), "wb");
			uint32_t n = uint32_t(rec_audio.size() * 2), rate = 48000;
			auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
			auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
			fwrite("RIFF", 1, 4, f); u32(36 + n); fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2);
			u32(rate); u32(rate * 4); u16(4); u16(16); fwrite("data", 1, 4, f); u32(n);
			fwrite(rec_audio.data(), 2, rec_audio.size(), f);
			fclose(f);
		}
		printf("%s: %ld pictures (%.2f s)\n", rec_name.c_str(), rec_k, rec_k / 60.0);
	};
	// M1_INLOG=file: the inputs each frame ran with (frame, then the three ports' pressed bits), for
	// playing the same inputs in MAME (tools/mame_play.sh)
	static FILE *inlog = getenv("M1_INLOG") ? fopen(getenv("M1_INLOG"), "w") : nullptr;
	auto step = [&] {
		if (inlog) fprintf(inlog, "%d %02x %02x %02x\n", fno + 1, unsigned(uint8_t(~m.m_inputs[0])), unsigned(uint8_t(~m.m_inputs[1])), unsigned(uint8_t(~m.m_inputs[2])));
		m.run_frame();
		fno++;
		// as the game: with smooth motion a frame is only drawn when shown, so a held scene change
		// is drawn anyway (not shown) for the hold to see the real picture
		if (m.smooth() && sync.enabled && sync.holding()) m.render_smooth(double(m.frame_number()), false);
		const size_t npx = size_t(m.frame_w()) * m.frame_h();
		const bool was_holding = sync.holding();
		const bool showing = sync.show(m);
		if (was_holding && !sync.holding()) m.smooth_cut();
		if (showing || shown.size() != npx) shown.assign(m.frame().data(), m.frame().data() + npx);
		if (!vid && !clips.empty() && double(m.frame_number()) > clips.front().t0) {
			open_rec(clips.front().name, clips.front().t0, clips.front().pictures);
			clips.erase(clips.begin());
		}
		if (sound) {
			if (vid) rec_audio.insert(rec_audio.end(), m.m_audio_out.begin(), m.m_audio_out.end());
			m.m_audio_out.clear();
		}
		if (vid) {
			// the pictures whose moments fall in this frame: [N-1, N)
			const double n = double(m.frame_number());
			for (;;) {
				const double t = rec_t0 + rec_k * FRAME_HZ / 60.0;
				if (t >= n) break;
				double moment = std::floor(t);
				if (smooth && (showing || !sync.enabled)) {
					m.render_smooth(t);
					moment = t - 2;                         // smooth motion shows a game frame behind
					shown.assign(m.frame().data(), m.frame().data() + npx);
				}
				fwrite(sync.enabled || smooth ? shown.data() : m.frame().data(), 4, npx, vid);
				fprintf(moments, "%ld %.4f %d\n", rec_k, moment, int(m.frame_quads()));
				rec_k++;
				if (rec_left > 0 && --rec_left == 0) { close_rec(); break; }
			}
		}
		game_part->script_frame(fno);
		if (skylog && fno % skyevery == 0) {
			fprintf(skylog, "%d %04x %04x %06x\n", fno, m.tile_word(0x5002), m.tile_word(0x5006), m.camera() & 0xffffff);
			char path[512];
			snprintf(path, sizeof path, "%s/sky_%06d.png", skydir, fno);
			write_png(path, m.frame().data(), m.frame_w(), m.frame_h());
		}
		static const char *dump_at = getenv("M1_DUMP_AT");      // M1_DUMP_AT=f1,f2: work RAM at those frames
		if (dump_at)
			for (const char *q = dump_at; *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : "")
				if (atoi(q) == fno)
{
					save_blob(std::string(argv[3]) + "/ram_" + std::to_string(fno) + ".bin", m.work_ram().data(), m.work_ram().size());
					save_blob(std::string(argv[3]) + "/ram_" + std::to_string(fno) + ".nv", m.nvram().data(), m.nvram().size());
				}
		for (auto &[a, b] : ranges)
			if (fno >= a && fno <= b) {
				char path[512];
				snprintf(path, sizeof path, "%s/f%06d.png", argv[3], fno);
				write_png(path, sync.enabled ? shown.data() : m.frame().data(), m.frame_w(), m.frame_h());
			}
		for (size_t i = 0; i < watch.size(); i++) {
			auto &r = m.work_ram();
			int v = r[watch[i] - 0x500000] << 8 | r[watch[i] - 0x500000 + 1];
			if (v != last[i]) { printf("frame %d: %06x = %04x\n", fno, watch[i], v); last[i] = v; }
		}
	};
	auto frame = [&](int n) { for (int i = 0; i < n; i++) step(); };
	while (std::getline(in, line)) {
		std::istringstream ss(line);
		std::string cmd;
		ss >> cmd;
		if (cmd.empty() || cmd[0] == '#') continue;
		if (cmd == "wait") { int n; ss >> n; frame(n); }
		else if (cmd == "press") {
			int port, n; unsigned mask;
			ss >> port >> std::hex >> mask >> std::dec >> n;
			m.m_inputs[port] = uint8_t(~mask);
			frame(n);
			m.m_inputs[port] = 0xff;
			frame(8);
		} else if (cmd == "shot") {
			std::string name; ss >> name;
			std::string path = std::string(argv[3]) + "/" + name + ".png";
			write_png(path.c_str(), m.frame().data(), m.frame_w(), m.frame_h());
		} else if (cmd == "shots") {         // shots NAME N: N consecutive frames, NAME_000..
			std::string name; int n; ss >> name >> n;
			for (int i = 0; i < n; i++) {
				step();
				char path[512];
				snprintf(path, sizeof path, "%s/%s_%03d.png", argv[3], name.c_str(), i);
				write_png(path, m.frame().data(), m.frame_w(), m.frame_h());
			}
		} else if (cmd == "hold") {          // hold PORT MASK: keep pressed until release
			int port; unsigned mask; ss >> port >> std::hex >> mask >> std::dec;
			m.m_inputs[port] = uint8_t(~mask);
		} else if (cmd == "release") {
			int port; ss >> port; m.m_inputs[port] = 0xff;
		} else if (cmd == "dump") {          // dump NAME: work RAM (500000-53ffff) to NAME.bin, 400000-40ffff to NAME.nv
			std::string name; ss >> name;
			save_blob(std::string(argv[3]) + "/" + name + ".bin", m.work_ram().data(), m.work_ram().size());
			save_blob(std::string(argv[3]) + "/" + name + ".nv", m.nvram().data(), m.nvram().size());
		} else if (cmd == "tiles") {         // tiles NAME: tile RAM (700000-70ffff) to NAME.tiles, words
			std::string name; ss >> name;
			std::vector<uint16_t> t(0x8000);
			for (uint32_t i = 0; i < t.size(); i++) t[i] = m.tile_word(i);
			save_blob(std::string(argv[3]) + "/" + name + ".tiles", reinterpret_cast<uint8_t *>(t.data()), t.size() * 2);
		} else if (cmd == "maps") {          // maps NAME: tile maps 2 and 3 as NAME_m2/_m3.png, scroll in NAME.txt
			std::string name; ss >> name;
			std::vector<uint32_t> img;
			for (int l = 2; l < 4; l++) {
				m.tilemap_image(l, img);
				std::string path = std::string(argv[3]) + "/" + name + "_m" + char('0' + l) + ".png";
				write_png(path.c_str(), img.data(), 512, 512);
			}
			if (FILE *f = fopen((std::string(argv[3]) + "/" + name + ".txt").c_str(), "w")) {
				for (int r = 0x5000; r < 0x5008; r++) fprintf(f, "%04x ", m.tile_word(r));
				fprintf(f, "\n");
				fclose(f);
			}
		} else if (cmd == "pokenv") {        // pokenv ADDR VALUE (hex): a byte of the battery RAM (400000-40ffff), for testing
			unsigned a, v; ss >> std::hex >> a >> v;
			m.nvram()[(a - 0x400000) & 0xffff] = uint8_t(v);
		} else if (cmd == "poke8") {         // poke8 ADDR VALUE (hex), work RAM: for testing only
			unsigned a, v; ss >> std::hex >> a >> v;
			m.work_ram()[a - 0x500000] = uint8_t(v);
		} else if (cmd == "poke16") {        // poke16 ADDR VALUE (hex), 400000- or 500000- RAM: for testing only
			unsigned a, v; ss >> std::hex >> a >> v;
			auto &r = a >= 0x500000 ? m.work_ram() : m.nvram();
			const uint32_t o = a - (a >= 0x500000 ? 0x500000 : 0x400000);
			r[o] = uint8_t(v); r[o + 1] = uint8_t(v >> 8);       // the V60 is little-endian
		} else if (cmd == "watch") {         // watch ADDR (hex): log the word's changes
			unsigned a; ss >> std::hex >> a; watch.push_back(a); last.push_back(-1);
		} else if (cmd == "every") {         // every NAME N K: K frames, a shot every N
			std::string name; int n, k; ss >> name >> n >> k;
			for (int i = 1; i <= k; i++) {
				step();
				if (i % n) continue;
				char path[512];
				snprintf(path, sizeof path, "%s/%s_%06d.png", argv[3], name.c_str(), fno);
				write_png(path, m.frame().data(), m.frame_w(), m.frame_h());
			}
		} else if (cmd == "rec") {
			std::string name; ss >> name;
			open_rec(name, double(m.frame_number()), -1);
		} else if (cmd == "stop") {
			if (vid) close_rec();
		} else if (cmd == "clip") {          // clip NAME FROM LEN: record LEN s starting FROM s from now
			std::string name; double from, len; ss >> name >> from >> len;
			clips.push_back({ name, double(m.frame_number()) + from * FRAME_HZ, long(std::lround(len * 60)) });
			std::sort(clips.begin(), clips.end(), [](const clip &a, const clip &b) { return a.t0 < b.t0; });
		} else if (game::macro mac; game::script_macro(title, cmd, ss, mac)) {   // the game's bots (VF: mash, brawl, fight)
			for (const auto &in : mac.frames) {
				for (int p = 0; p < 3; p++)
					if (mac.ports >> p & 1) m.m_inputs[p] = uint8_t(~in[p]);
				step();
			}
			for (int p = 0; p < 3; p++)
				if (mac.released >> p & 1) m.m_inputs[p] = 0xff;
		} else if (game_part->script_command(cmd, ss, argv[3])) {
		} else if (cmd == "save") {
			save_blob(nv, m.nvram().data(), m.nvram().size());
			save_blob(ee, m.eeprom(), model1::EEPROM_SIZE);
			printf("saved %s and %s\n", nv.c_str(), ee.c_str());
		}
	}
}
