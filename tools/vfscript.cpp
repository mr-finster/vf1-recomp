// Plays a script of inputs headlessly, for operating the game's menus (test mode settings).
// Starts from the saved NVRAM/EEPROM in the regions directory and writes them back on `save`.
//   vfscript <regions dir> <script> <shot dir>
// Script lines:   wait N | press PORT MASK FRAMES | hold PORT MASK | release PORT | shot NAME |
//                 shots NAME N | dump NAME | poke16 ADDR VAL | mash N | watch ADDR | maps NAME |
//                 every NAME N K | icon NAME | save   (VF_SCALE / VF_ASPECT / VF_REC as for vfrun)
//                 rec NAME | stop: video of what's between, 60 pictures per second of the game's
//                 time, to NAME.mkv (lossless, ffmpeg), its sound to NAME.wav (VF_SOUND=1;
//                 VF_MUSIC / VF_EFFECTS levels), and NAME.txt: each picture's moment in game frames
//                 (VF_SMOOTH=1: smooth motion, as the game above 1x; VF_OUT=WxH scaled for an edit,
//                 VF_OUT_FLAGS its ffmpeg scaler, lanczos by default; VF_LOG=file: a line a frame of
//                 health, rounds won and round flags, for finding marks: tools/montage/vflog.py;
//                 VF_INLOG=file: each frame's inputs, for tools/mame_play.sh)
//   PORT 0: coin1 01 coin2 02 test 04 service 08 start1 10 start2 20; PORT 1/2: players
#include "../src/host/regions.h"
#include "../src/host/png.h"
#include "../src/host/backgrounds.h"
#include "../src/host/scene_sync.h"
#include "../src/rt/rec_rt.h"
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
	if (argc < 4) { fprintf(stderr, "usage: vfscript regions script shotdir\n"); return 1; }
	std::string dir = argv[1];
	model1 m(load_regions(dir));
	const std::string nv = dir + "/nvram.sav", ee = dir + (m.real_ioboard() ? "/eeprom_93c46.sav" : "/eeprom.sav");
	load_blob(nv, m.nvram().data(), m.nvram().size());
	load_blob(ee, m.eeprom(), model1::EEPROM_SIZE);
	if (getenv("VF_FREEPLAY")) printf("force_free_play: %d\n", m.force_free_play());
	if (getenv("VF_PCMODE")) printf("hide_free_play_text: %d\n", m.hide_free_play_text());
	if (const char *bg = getenv("VF_BACKGROUNDS")) load_backgrounds(m, bg);   // hi-res skies
	if (getenv("VF_NODRAW")) m.set_drawing(false);   // faster when only exporting
	if (const char *sc = getenv("VF_SCALE")) m.set_render_scale(atoi(sc));
	if (getenv("VF_SKY_FILTER")) m.set_sky_filter(true);   // the menu's Background filter: Smooth
	if (const char *as = getenv("VF_ASPECT")) m.set_aspect(strtof(as, nullptr));
	const bool sound = getenv("VF_SOUND") != nullptr;
	if (sound) {
		m.enable_sound();
		m.set_volumes(getenv("VF_MUSIC") ? strtof(getenv("VF_MUSIC"), nullptr) : 1,
					  getenv("VF_EFFECTS") ? strtof(getenv("VF_EFFECTS"), nullptr) : 1);
	}
	const bool smooth = getenv("VF_SMOOTH") != nullptr;
	if (smooth) m.set_smooth(true);
	// Cheats: VF_AKIRA=n puts UNUSED_CHARACTERS[n] in Akira's place, VF_ICONS=dir its select icon;
	// VF_CAM="x y z a b c" sets Akira's place's select camera (tools/icons)
	if (const char *a = getenv("VF_AKIRA")) m.set_akira_slot(atoi(a));
	if (const char *ic = getenv("VF_ICONS")) load_secret_icon(m, getenv("VF_AKIRA") ? atoi(getenv("VF_AKIRA")) : 0, { ic });
	if (const char *c = getenv("VF_CAM")) {
		auto &rom = const_cast<std::vector<uint8_t> &>(m.maincpu_rom());
		float f[3]; int a[3];
		if (sscanf(c, "%f %f %f %d %d %d", &f[0], &f[1], &f[2], &a[0], &a[1], &a[2]) == 6) {
			memcpy(&rom[0xfd278b], f, 12);
			for (int i = 0; i < 3; i++) { const int16_t h = int16_t(a[i]); memcpy(&rom[0xfd278b + 12 + 2 * i], &h, 2); }
		}
	}
	m.reset();
	if (getenv("VF_REC")) {                // the recompiled code instead of the interpreter
		rec::attach(m.cpu());
		m.run_cpu = [](int n) { rec::run(n); };
	}
	// VF_PANO=dir: the stages' sky panoramas. The streamer at ff4e0c (R20 = the stage's sky record)
	// copies columns of 8 blocks, each 32x48 map entries (256x384 pixels), into tile map 2 as the
	// camera turns: 2048 pixels for the full circle. The first time a record is used its blocks
	// are copied out, and drawn with that stage's tiles and palette a second later.
	const char *panodir = getenv("VF_PANO");
	std::vector<uint32_t> pano_seen;
	std::vector<uint16_t> pano_entries;            // 256 x 48, waiting to be drawn
	uint32_t pano_pending = 0;
	int pano_rows = 0;
	int pano_due = 0;                              // drawn a second later: the palette fades in
	if (panodir && !getenv("VF_REC"))
		m.on_instruction = [&](uint32_t pc) {
			if (pc != 0xff4e0c) return;
			uint32_t r = m.cpu().reg(20);
			if (std::find(pano_seen.begin(), pano_seen.end(), r) != pano_seen.end()) return;
			pano_seen.push_back(r);
			pano_entries.assign(256 * 48, 0);
			pano_rows = 0;
			for (int b = 0; b < 8; b++) {
				uint32_t p = m.peek16(r + 8 + b * 4) | uint32_t(m.peek16(r + 10 + b * 4)) << 16;
				int rows = m.peek16(p + 4), stride = m.peek16(p + 8);
				pano_rows = std::max(pano_rows, std::min(rows, 48));
				for (int y = 0; y < std::min(rows, 48); y++)
					for (int x = 0; x < 32; x++)
						pano_entries[y * 256 + b * 32 + x] = m.peek16(p + 12 + 2 * (y * stride + x));
			}
			pano_pending = r;
			pano_due = int(m.frame_number()) + 60;
			printf("sky record %06x: ROM offset %07x (bank %07x)\n", r, m.bank_base() + (r & 0xfffff), m.bank_base());
		};
	std::ifstream in(argv[2]);
	std::string line;
	std::vector<unsigned> watch;          // work-RAM words whose changes are logged
	std::vector<int> last;
	int fno = 0;
	std::vector<std::pair<int, int>> ranges;   // VF_SHOTS=a-b,c-d: also shoot every frame in these
	if (const char *c = getenv("VF_SHOTS"))
		for (const char *q = c; *q;) {
			int a = 0, b = 0, used = 0;
			if (sscanf(q, "%d-%d%n", &a, &b, &used) < 2) break;
			ranges.push_back({a, b});
			q += used;
			if (*q == ',') q++;
		}
	// VF_SKYDIR=dir [VF_SKYEVERY=n]: every n frames, the frame as sky_NNNNNN.png plus a line in
	// dir/sky.txt (frame, the sky map's scroll, the camera pointer): for stitching the panoramas.
	// Run with VF_TILE_SKIP=cf VF_NO_POLYS=1 so the frame holds the background layers alone.
	const char *skydir = getenv("VF_SKYDIR");
	const int skyevery = getenv("VF_SKYEVERY") ? atoi(getenv("VF_SKYEVERY")) : 1;
	FILE *skylog = skydir ? fopen((std::string(skydir) + "/sky.txt").c_str(), "w") : nullptr;
	scene_sync sync;                                // VF_SCENE_SYNC=1: shots as the game shows them
	sync.enabled = getenv("VF_SCENE_SYNC") != nullptr;
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
		// VF_OUT=WxH: scaled (lanczos) and encoded for the edit; otherwise lossless at the game's size
		if (const char *o = getenv("VF_OUT"))
			snprintf(cmdl, sizeof cmdl, "ffmpeg -v error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 60 -i - "
					 "-vf scale=%s:flags=%s -c:v libx264 -crf 12 -preset fast -pix_fmt yuv420p '%s.mkv'",
					 m.frame_w(), m.frame_h(), o, getenv("VF_OUT_FLAGS") ? getenv("VF_OUT_FLAGS") : "lanczos", base.c_str());
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
	// VF_INLOG=file: the inputs each frame ran with (frame, then the three ports' pressed bits), for
	// playing the same inputs in MAME (tools/mame_play.sh)
	static FILE *inlog = getenv("VF_INLOG") ? fopen(getenv("VF_INLOG"), "w") : nullptr;
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
		if (pano_pending && int(m.frame_number()) >= pano_due) {
			std::vector<uint32_t> img(2048 * 8 * pano_rows);
			m.render_tiles(pano_entries.data(), 256, pano_rows, img.data());
			char path[512];
			snprintf(path, sizeof path, "%s/sky_%06x.png", panodir, pano_pending);
			write_png(path, img.data(), 2048, 8 * pano_rows);
			printf("frame %d: sky record %06x -> %s\n", fno, pano_pending, path);
			if (getenv("VF_PANO_DEBUG"))
				for (int y : { 0, 10, 20, 30, 40 }) {
					printf(" row %2d:", y);
					for (int x = 0; x < 8; x++) printf(" %04x", pano_entries[y * 256 + x]);
					printf("   map:");
					for (int x = 0; x < 8; x++) printf(" %04x", m.tile_word(0x2000 + (6 + y) * 64 + x));
					printf("\n");
				}
			pano_pending = 0;
		}
		if (skylog && fno % skyevery == 0) {
			const uint8_t *c = &m.work_ram()[0x17f0];
			fprintf(skylog, "%d %04x %04x %06x\n", fno, m.tile_word(0x5002), m.tile_word(0x5006),
					unsigned(c[0] | c[1] << 8 | c[2] << 16));
			char path[512];
			snprintf(path, sizeof path, "%s/sky_%06d.png", skydir, fno);
			write_png(path, m.frame().data(), m.frame_w(), m.frame_h());
		}
		// VF_LOG=path: a line a frame for finding the edit's marks: the game frame, each fighter's health
		// (0-192, battery RAM 4000b4 / 4000b6), two round flags (400182: 0 from the replay on; 400202: the
		// round is live), the shown picture's polygon count and the rounds each has won (501281 / 501282)
		static FILE *glog = getenv("VF_LOG") ? fopen(getenv("VF_LOG"), "w") : nullptr;
		if (glog) {
			auto nv16 = [&](uint32_t a) { return unsigned(m.nvram()[a] | m.nvram()[a + 1] << 8); };
			fprintf(glog, "%d %u %u %u %u %u %d %u %u\n", fno, unsigned(m.frame_number()), nv16(0xb4), nv16(0xb6), nv16(0x182) >> 15, nv16(0x202) >> 15, int(m.frame_quads()), unsigned(m.work_ram()[0x1281]), unsigned(m.work_ram()[0x1282]));
		}
		static const char *dump_at = getenv("VF_DUMP_AT");      // VF_DUMP_AT=f1,f2: work RAM at those frames
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
		} else if (cmd == "icon") {          // icon NAME: Akira's select icon (as on screen) to NAME.png (tools/icons)
			std::string name; ss >> name;
			for (uint32_t i = 0; i < 0x4000 - 8 * 64; i++) if (m.tile_word(i) == 0xb080) {
				std::vector<uint16_t> e(7 * 8);
				for (int y = 0; y < 8; y++) for (int x = 0; x < 7; x++) e[y * 7 + x] = m.tile_word(i + y * 64 + x);
				std::vector<uint32_t> img(56 * 64);
				m.render_tiles(e.data(), 7, 8, img.data());
				write_png((std::string(argv[3]) + "/" + name + ".png").c_str(), img.data(), 56, 64);
				break;
			}
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
		} else if (cmd == "mash") {          // mash N: P1 punches and kicks for N frames
			int n; ss >> n;
			for (int i = 0; i < n; i++) {
				m.m_inputs[1] = uint8_t(~((i / 4) % 2 ? 0x02 : ((i / 4) % 3 ? 0x04 : 0x40)));
				step();
			}
			m.m_inputs[1] = 0xff;
		} else if (cmd == "brawl") {         // brawl N SEED [PLAYERS]: random moves for N frames (both, or 1 = P1)
			int n, players = 2; unsigned seed; ss >> n >> seed; ss >> players;
			uint32_t rng = seed * 2654435761u + 1;
			auto rnd = [&](int k) { rng = rng * 1103515245 + 12345; return int((rng >> 16) % unsigned(k)); };
			// punch 01 kick 02 guard 04 down 10 up 20 right 40 left 80 (the game's INPUT TEST)
			static const uint8_t MOVES[] = { 0x40, 0x40, 0x80, 0x02, 0x02, 0x04, 0x04, 0x06, 0x03, 0x12, 0x14,
											 0x24, 0x42, 0x44, 0x82, 0x01, 0x46, 0x20, 0x10, 0x05, 0x00 };
			int left[2] = { 0, 0 };
			uint8_t cur[2] = { 0, 0 };
			for (int i = 0; i < n; i++) {
				for (int p = 0; p < players; p++) {
					if (--left[p] <= 0) {
						// a move, then let go a moment (taps register as presses)
						cur[p] = cur[p] ? 0 : MOVES[rnd(int(sizeof MOVES))];
						left[p] = cur[p] ? 3 + rnd(10) : 1 + rnd(4);
					}
					m.m_inputs[1 + p] = uint8_t(~cur[p]);
				}
				step();
			}
			m.m_inputs[1] = m.m_inputs[2] = 0xff;
		} else if (cmd == "fight") {         // fight N SEED: both players play real moves and combos for N frames
			int n; unsigned seed; ss >> n >> seed;
			uint32_t rng = seed * 2654435761u + 7;
			auto rnd = [&](int k) { rng = rng * 1103515245 + 12345; return int((rng >> 16) % unsigned(k)); };
			// G guard, P punch, K kick, D down, U up; F and B are forward and back for the player's side
			enum : uint8_t { P = 0x01, K = 0x02, G = 0x04, D = 0x10, U = 0x20, F = 0x40, B = 0x80 };
			struct step_t { uint8_t m; int frames; };
			static const std::vector<std::vector<step_t>> PLAYS = {
				{ { F, 14 } },                                                    // walk in
				{ { F, 2 }, { 0, 2 }, { F, 6 } },                                 // dash
				{ { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 4 }, { K, 3 }, { 0, 10 } },   // PPPK
				{ { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 4 }, { P, 3 }, { 0, 8 } },   // PPP
				{ { P, 3 }, { 0, 4 }, { K, 3 }, { 0, 10 } },                      // PK
				{ { F | P, 3 }, { 0, 6 }, { F | K, 3 }, { 0, 10 } },              // elbow, knee
				{ { F | P, 3 }, { 0, 10 } },                                      // elbow
				{ { P | G, 4 }, { 0, 20 } },                                      // throw
				{ { F, 6 }, { P | G, 4 }, { 0, 20 } },                            // step in, throw
				{ { D | K, 3 }, { 0, 12 } },                                      // low kick
				{ { D | B | K, 3 }, { 0, 16 } },                                  // sweep
				{ { U | F, 3 }, { 0, 10 }, { K, 3 }, { 0, 24 } },                 // jump kick
				{ { F | K, 3 }, { 0, 12 } },                                      // knee / front kick
				{ { K, 3 }, { 0, 4 }, { K, 3 }, { 0, 10 } },                      // double kick
				{ { G, 18 } },                                                    // guard
				{ { D | G, 14 } },                                                // low guard
				{ { B, 10 } },                                                    // back off
			};
			static const int WEIGHTS[] = { 5, 3, 7, 3, 5, 5, 3, 3, 3, 3, 2, 2, 3, 2, 5, 3, 2 };
			int total = 0;
			for (int w : WEIGHTS) total += w;
			struct bot { const std::vector<step_t> *play = nullptr; size_t i = 0; int left = 0; uint8_t cur = 0; };
			bot bots[2];
			for (int f = 0; f < n; f++) {
				for (int p = 0; p < 2; p++) {
					bot &b = bots[p];
					if (b.left <= 0) {
						if (b.play && ++b.i < b.play->size()) {
							b.cur = (*b.play)[b.i].m; b.left = (*b.play)[b.i].frames;
						} else if (b.play) {
							b.play = nullptr; b.cur = 0; b.left = 2 + rnd(7);       // a breath between plays
						} else {
							int r = rnd(total), k = 0;
							while (r >= WEIGHTS[k]) r -= WEIGHTS[k++];
							b.play = &PLAYS[k]; b.i = 0;
							b.cur = PLAYS[k][0].m; b.left = PLAYS[k][0].frames + (k == 0 || k >= 14 ? rnd(12) : 0);
						}
					}
					b.left--;
					// forward is right for player 1 (on the left), left for player 2
					uint8_t in = b.cur & 0x37;
					if (b.cur & F) in |= p ? 0x80 : 0x40;
					if (b.cur & B) in |= p ? 0x40 : 0x80;
					m.m_inputs[1 + p] = uint8_t(~in);
				}
				step();
			}
			m.m_inputs[1] = m.m_inputs[2] = 0xff;
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
		} else if (cmd == "save") {
			save_blob(nv, m.nvram().data(), m.nvram().size());
			save_blob(ee, m.eeprom(), model1::EEPROM_SIZE);
			printf("saved %s and %s\n", nv.c_str(), ee.c_str());
		}
	}
}
