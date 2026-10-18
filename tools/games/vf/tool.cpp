// Virtua Fighter's part of the headless tools (tools/tool_game.h).
//   M1_FREEPLAY=1   free play (model1script)
//   M1_AKIRA=n      UNUSED_CHARACTERS[n] in Akira's place, M1_ICONS=dir its select icon;
//   M1_CAM="x y z a b c"  Akira's place's select camera (tools/games/vf/select_icons)
//   M1_PANO=dir     the stages' sky panoramas as dir/sky_RECORD.png (M1_PANO_DEBUG: their first entries)
//   M1_LOG=file     a line a frame for finding an edit's marks (tools/montage/vflog.py)
//   icon NAME       script command: Akira's select icon (as on screen) to NAME.png
#include "../../tool_game.h"
#include "../../../src/games/vf/art.h"
#include "../../../src/games/vf/profile.h"
#include "../../../src/host/png.h"
#include <algorithm>
#include <cstring>

namespace {

class vf_tool final : public tool_game
{
public:
	explicit vf_tool(model1 &machine) : m(machine), vf(machine) {}

	void load_pictures(const std::string &dir) override { game::vf::load_screens(vf, dir); }

	int pc_mode() override { return vf.hide_free_play_text(); }

	void script_setup() override
	{
		if (getenv("M1_FREEPLAY")) printf("force_free_play: %d\n", vf.force_free_play());
		if (const char *a = getenv("M1_AKIRA")) vf.set_akira_slot(atoi(a));
		if (const char *ic = getenv("M1_ICONS")) game::vf::load_secret_icon(vf, getenv("M1_AKIRA") ? atoi(getenv("M1_AKIRA")) : 0, { ic });
		if (const char *c = getenv("M1_CAM")) {
			auto &rom = m.program_rom();
			float f[3]; int a[3];
			if (sscanf(c, "%f %f %f %d %d %d", &f[0], &f[1], &f[2], &a[0], &a[1], &a[2]) == 6) {
				memcpy(&rom[0xfd278b], f, 12);
				for (int i = 0; i < 3; i++) { const int16_t h = int16_t(a[i]); memcpy(&rom[0xfd278b + 12 + 2 * i], &h, 2); }
			}
		}
	}

	// M1_PANO: the streamer at ff4e0c (R20 = the stage's sky record) copies columns of 8 blocks,
	// each 32x48 map entries (256x384 pixels), into tile map 2 as the camera turns: 2048 pixels
	// for the full circle. The first time a record is used its blocks are copied out, and drawn
	// with that stage's tiles and palette a second later.
	void script_started(bool rec) override
	{
		panodir = getenv("M1_PANO");
		if (!panodir || rec) return;
		m.on_instruction = [this](uint32_t pc) {
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
	}

	void script_frame(int fno) override
	{
		if (pano_pending && int(m.frame_number()) >= pano_due) {
			std::vector<uint32_t> img(2048 * 8 * pano_rows);
			m.render_tiles(pano_entries.data(), 256, pano_rows, img.data());
			char path[512];
			snprintf(path, sizeof path, "%s/sky_%06x.png", panodir, pano_pending);
			write_png(path, img.data(), 2048, 8 * pano_rows);
			printf("frame %d: sky record %06x -> %s\n", fno, pano_pending, path);
			if (getenv("M1_PANO_DEBUG"))
				for (int y : { 0, 10, 20, 30, 40 }) {
					printf(" row %2d:", y);
					for (int x = 0; x < 8; x++) printf(" %04x", pano_entries[y * 256 + x]);
					printf("   map:");
					for (int x = 0; x < 8; x++) printf(" %04x", m.tile_word(0x2000 + (6 + y) * 64 + x));
					printf("\n");
				}
			pano_pending = 0;
		}
		// M1_LOG: the game frame, each fighter's health (0-192, battery RAM 4000b4 / 4000b6), two
		// round flags (400182: 0 from the replay on; 400202: the round is live), the shown
		// picture's polygon count and the rounds each has won (501281 / 501282)
		static FILE *glog = getenv("M1_LOG") ? fopen(getenv("M1_LOG"), "w") : nullptr;
		if (glog) {
			auto nv16 = [&](uint32_t a) { return unsigned(m.nvram()[a] | m.nvram()[a + 1] << 8); };
			fprintf(glog, "%d %u %u %u %u %u %d %u %u\n", fno, unsigned(m.frame_number()), nv16(0xb4), nv16(0xb6), nv16(0x182) >> 15, nv16(0x202) >> 15, int(m.frame_quads()), unsigned(m.work_ram()[0x1281]), unsigned(m.work_ram()[0x1282]));
		}
	}

	bool script_command(const std::string &cmd, std::istream &args, const std::string &out) override
	{
		if (cmd != "icon") return false;
		std::string name; args >> name;
		for (uint32_t i = 0; i < 0x4000 - 8 * 64; i++) if (m.tile_word(i) == 0xb080) {
			std::vector<uint16_t> e(7 * 8);
			for (int y = 0; y < 8; y++) for (int x = 0; x < 7; x++) e[y * 7 + x] = m.tile_word(i + y * 64 + x);
			std::vector<uint32_t> img(56 * 64);
			m.render_tiles(e.data(), 7, 8, img.data());
			write_png((out + "/" + name + ".png").c_str(), img.data(), 56, 64);
			break;
		}
		return true;
	}

private:
	model1 &m;
	game::vf::gameplay vf;
	const char *panodir = nullptr;
	std::vector<uint32_t> pano_seen;
	std::vector<uint16_t> pano_entries;            // 256 x 48, waiting to be drawn
	uint32_t pano_pending = 0;
	int pano_rows = 0;
	int pano_due = 0;                              // drawn a second later: the palette fades in
};

const tool_game_registration REGISTERED{ game::vf::ID, [](model1 &machine) -> std::unique_ptr<tool_game> {
	return std::make_unique<vf_tool>(machine);
} };

}
