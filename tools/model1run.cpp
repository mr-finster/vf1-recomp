// Shared Model 1 runner: boots a title on the interpreter and writes frames as PNGs.
//   model1run [--game vf|vr] [--scenario NAME] <regions dir> <frames> [every N] [outdir]
#include "../src/host/regions.h"
#include "../src/host/png.h"
#include "../src/host/backgrounds.h"
#include "../src/games/script.h"
#include "tool_args.h"
#include "tool_game.h"
#include <tuple>
#include <array>
#include "../src/host/scene_sync.h"
#include "../src/rt/rec_rt.h"
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>

static uint32_t g_ring[256];
static unsigned g_ring_pos;
static model1 *g_m;

static bool read_int_env(const char *name, int fallback, int min, int max, int &value)
{
	value = fallback;
	const char *text = getenv(name);
	if (!text) return true;
	char *end = nullptr;
	const long parsed = strtol(text, &end, 10);
	if (end == text || *end || parsed < min || parsed > max) {
		fprintf(stderr, "%s must be an integer from %d to %d\n", name, min, max);
		return false;
	}
	value = int(parsed);
	return true;
}

static void dump_ring(int)
{
	fprintf(stderr, "last instructions:\n");
	for (unsigned i = 0; i < 256; i++)
		fprintf(stderr, "%06x%s", g_ring[(g_ring_pos + i) & 255], (i & 15) == 15 ? "\n" : " ");
	if (g_m) {
		fprintf(stderr, "regs:");
		for (int r = 0; r < 32; r++) fprintf(stderr, " R%d=%08x", r, g_m->cpu().reg(r));
		fprintf(stderr, "\n");
	}
	_exit(1);
}

int main(int argc, char **argv)
{
	tool_args opt;
	if (!opt.take(argc, argv)) return 2;
	const std::string dir = argc > 1 ? argv[1] : opt.default_dir();
	const game::id title = opt.game_for(dir);
	int frames = argc > 2 ? atoi(argv[2]) : 600;
	int every = argc > 3 ? atoi(argv[3]) : 60;
	std::string out = argc > 4 ? argv[4] : "build/frames";
	model1 m(load_regions(dir, title));
	const std::unique_ptr<tool_game> game_part = make_tool_game(title, m);
	const auto &io = game::get_io_profile(title);
	std::array<bool, 8> pinned{};   // analog channels set here, which the scripted play leaves alone
	for (size_t channel = 0; channel < io.analog_environment.size(); channel++) {
		const std::string name(io.analog_environment[channel]);
		if (name.empty()) continue;
		const char *value = getenv(name.c_str());
		if (!value) continue;
		char *end = nullptr;
		const long parsed = strtol(value, &end, 10);
		if (end == value || *end || parsed < 0 || parsed > 255) {
			fprintf(stderr, "%s must be an integer from 0 to 255\n", name.c_str());
			return 1;
		}
		if (!m.set_analog_input(int(channel), uint8_t(parsed))) {
			fprintf(stderr, "%s requires the Model 1 I/O board ROM (model1io.zip)\n", name.c_str());
			return 1;
		}
		pinned[channel] = true;
	}
	// M1_SCALE: the render scale; M1_AA: antialiasing (n x the scale, 0: edge antialiasing);
	// M1_ASPECT: the picture's width / height
	int scale = 1, aa = 1;
	if (!read_int_env("M1_SCALE", 1, 1, 12, scale) || !read_int_env("M1_AA", 1, 0, 3, aa))
		return 1;
	if (aa == 0) m.set_edge_aa(true);
	else scale *= std::min(aa, std::max(1, 8 / scale));
	m.set_render_scale(scale);
	if (const char *aspect = getenv("M1_ASPECT")) {
		char *end = nullptr;
		const float parsed = strtof(aspect, &end);
		if (end == aspect || *end || !std::isfinite(parsed) || parsed < 4.0f / 3.0f || parsed > 32.0f / 9.0f) {
			fprintf(stderr, "M1_ASPECT must be a number from 1.3333 (4:3) to 3.5556 (32:9)\n");
			return 1;
		}
		m.set_aspect(parsed);
	}
	const char *wav = getenv("M1_WAV");
	if (wav) m.enable_sound();
	if (wav) m.set_volumes(getenv("M1_MUSIC") ? strtof(getenv("M1_MUSIC"), nullptr) : 1,    // stems: 0 mutes one
						   getenv("M1_EFFECTS") ? strtof(getenv("M1_EFFECTS"), nullptr) : 1);
	if (const char *bg = getenv("M1_BACKGROUNDS")) { load_backgrounds(m, bg); game_part->load_pictures(bg); }   // hi-res skies, the game's own (VF: the widescreen sides)
	if (getenv("M1_PCMODE")) {    // the saved settings (free play) and the game as the player has it (VF: no FREE PLAY notices)
		for (auto [file, dst, n] : { std::tuple{ "/nvram.sav", m.nvram().data(), m.nvram().size() },
									 std::tuple{ "/eeprom_93c46.sav", m.eeprom(), size_t(model1::EEPROM_SIZE) } })
			if (FILE *f = fopen((dir + file).c_str(), "rb")) { fread(dst, 1, n, f); fclose(f); }
		game_part->pc_mode();
	}
	m.reset();
	g_m = &m;
	if (getenv("M1_REC")) {
		rec::attach(m.cpu(), title.name);
		m.run_cpu = [](int n) { rec::run(n); };
	}
	std::vector<int16_t> audio;
	if (getenv("M1_TRACE")) {
		signal(SIGABRT, dump_ring);
		m.on_instruction = [](uint32_t pc) { g_ring[g_ring_pos++ & 255] = pc; };
	}
	auto t0 = std::chrono::steady_clock::now();
	std::vector<std::pair<int, int>> clips;
	if (const char *c = getenv("M1_CLIPS"))
		for (const char *q = c; *q;) {
			int a = 0, b = 0, used = 0;
			if (sscanf(q, "%d-%d%n", &a, &b, &used) < 2) break;
			clips.push_back({a, b});
			q += used;
			if (*q == ',') q++;
		}
	auto in_clip = [&](int f) {
		if (clips.empty()) return true;
		for (auto &[a, b] : clips) if (f >= a && f <= b) return true;
		return false;
	};
	FILE *raw = getenv("M1_RAW") ? fopen(getenv("M1_RAW"), "wb") : nullptr;
	scene_sync sync;                                // M1_SCENE_SYNC=1: clean scene changes, as the game
	sync.enabled = getenv("M1_SCENE_SYNC") != nullptr;
	// M1_SMOOTH=n: smooth motion, n pictures per frame (n x 57.5 fps out); PNGs get a suffix
	const int smooth = getenv("M1_SMOOTH") ? std::max(1, atoi(getenv("M1_SMOOTH"))) : 0;
	if (smooth) m.set_smooth(true);
	if (getenv("M1_BG_FILTER")) m.set_sky_filter(atoi(getenv("M1_BG_FILTER")) != 0);
	std::vector<uint32_t> shown(size_t(m.frame_w()) * m.frame_h(), 0);   // the picture on screen
	sync.start();
	// M1_SCRIPT=1: the game's scripted play (as model1cov: attract, coins, a game), M1_SEED its moves'
	// (an analog control set above stays as set)
	std::unique_ptr<game::auto_play> play;
	if (getenv("M1_SCRIPT")) {
		play = game::make_auto_play(title, opt.scenario, getenv("M1_SEED") ? uint32_t(atoi(getenv("M1_SEED"))) : 12345, false);
		if (!play) { fprintf(stderr, "%s has no scenario \"%s\"\n", game::get(title).display_name.data(), opt.scenario.c_str()); return 2; }
	}
	for (int f = 1; f <= frames; f++) {
		if (play) {
			const game::script_inputs in = play->frame(f);
			for (int c = 0; c < 3; c++)
				if (in.analog[c] >= 0 && !pinned[c] && !m.set_analog_input(c, uint8_t(in.analog[c]))) {
					fprintf(stderr, "%s scripted run requires the Model 1 I/O board ROM (model1io.zip)\n", game::get(title).display_name.data());
					return 1;
				}
			for (int p = 0; p < 3; p++) m.m_inputs[p] = uint8_t(~in.pressed[p]);
		}
		m.run_frame();
		const bool keep = in_clip(f);
		const size_t npx = size_t(m.frame_w()) * m.frame_h();
		const bool was_holding = sync.holding();
		const bool showing = sync.show(m);
		if (was_holding && !sync.holding()) m.smooth_cut();     // as the game: a new scene isn't blended from the old
		if (getenv("M1_LOG_FRAMES") && keep) {     // frame, polygons, camera, a 1/64 sample checksum of the picture
			uint64_t h = 0;
			const auto &b = m.frame();
			for (size_t i = 0; i < size_t(m.frame_w()) * m.frame_h(); i += 61) h = h * 31 + b.data()[i];
			fprintf(stderr, "F %d %d %06x %016llx\n", f, m.frame_quads(), m.camera(), (unsigned long long)h);
		}
		if (wav) { if (keep) audio.insert(audio.end(), m.m_audio_out.begin(), m.m_audio_out.end()); m.m_audio_out.clear(); }
		for (int k = 0; k < std::max(1, smooth); k++) {
			if (smooth && (showing || !sync.enabled)) m.render_smooth(double(m.frame_number() - 1) + double(k) / smooth);
			if (showing || !sync.enabled || shown.size() != npx) shown.assign(m.frame().data(), m.frame().data() + npx);
			const uint32_t *pic = sync.enabled ? shown.data() : m.frame().data();
			if (raw && keep) fwrite(pic, 4, npx, raw);
			if (every && f % every == 0 && keep) {
				char path[512];
				if (smooth > 1) snprintf(path, sizeof path, "%s/f%05d_%d.png", out.c_str(), f, k);
				else snprintf(path, sizeof path, "%s/f%05d.png", out.c_str(), f);
				write_png(path, pic, m.frame_w(), m.frame_h());
				printf("frame %d pc=%06x\n", f, m.cpu().pc());
			}
		}
	}
	if (raw) fclose(raw);
	if (wav) {
		FILE *w = fopen(wav, "wb");
		uint32_t n = uint32_t(audio.size() * 2), rate = m1audio::OUT_RATE;
		auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, w); };
		auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, w); };
		fwrite("RIFF", 1, 4, w); u32(36 + n); fwrite("WAVEfmt ", 1, 8, w); u32(16); u16(1); u16(2);
		u32(rate); u32(rate * 4); u16(4); u16(16); fwrite("data", 1, 4, w); u32(n);
		fwrite(audio.data(), 2, audio.size(), w);
		fclose(w);
		double sum = 0; int peak = 0;
		for (int16_t v : audio) { sum += double(v) * v; peak = std::max(peak, abs(int(v))); }
		printf("audio: %zu samples, rms %.0f, peak %d\n", audio.size() / 2, sqrt(sum / std::max<size_t>(1, audio.size())), peak);
	}
	if (getenv("M1_TGP_TRACE")) {
		const char *mode = m.tgp_is_lle() ? "ROM-backed LLE" :
			m.tgp_has_hle() ? "HLE simulation" : "no HLE handler";
		fprintf(stderr, "TGP mode: %s; MB86233 cycles executed: %llu\n",
			mode, (unsigned long long)m.tgp_cycles_executed());
	}
	double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	fprintf(stderr, "%d frames in %.2fs (%.0f fps)\n", frames, s, frames / s);
}
