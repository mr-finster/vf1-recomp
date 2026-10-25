// Save states (model1.h's save_state): the field list, run both ways (state_io.h).
#include "model1.h"

namespace {
constexpr uint32_t STATE_MAGIC = 0x53314656;   // "VF1S"
constexpr uint32_t STATE_VERSION = 1;

// The host-side containers Random fighter keeps (its fighters' names, colours, moves' sources...): written in key
// order (the same bytes for the same contents on any machine) and read back whole
template <class IO> void field(IO &io, std::string &v)
{
	uint64_t n = v.size();
	io.pod(n);
	if (IO::loading) { if (n > (1u << 20)) n = 0; v.resize(size_t(n)); }
	io.bytes(v.data(), v.size());
}
template <class IO, class T> std::enable_if_t<std::is_arithmetic_v<T>> field(IO &io, T &v) { io.raw(v); }
template <class IO, class T, size_t N> void field(IO &io, std::array<T, N> &v) { for (T &x : v) field(io, x); }
template <class IO, class A, class B> void field(IO &io, std::pair<A, B> &v) { field(io, v.first); field(io, v.second); }
template <class IO, class T> void field(IO &io, std::vector<T> &v)
{
	uint64_t n = v.size();
	io.pod(n);
	if (IO::loading) { if (n > (1u << 24)) n = 0; v.resize(size_t(n)); }
	for (T &x : v) field(io, x);
}
template <class IO, class K, class V> void field(IO &io, std::map<K, V> &m)
{
	uint64_t n = m.size();
	io.pod(n);
	if (!IO::loading) { for (auto &[k, v] : m) { K kk = k; field(io, kk); field(io, v); } return; }
	m.clear();
	for (uint64_t i = 0; i < n && i < (1u << 20); i++) { K k{}; V v{}; field(io, k); field(io, v); m.emplace(k, std::move(v)); }
}
template <class IO, class K> void field(IO &io, std::set<K> &m)
{
	uint64_t n = m.size();
	io.pod(n);
	if (!IO::loading) { for (K k : m) field(io, k); return; }
	m.clear();
	for (uint64_t i = 0; i < n && i < (1u << 20); i++) { K k{}; field(io, k); m.insert(k); }
}
// (a frame number, as frames before the board's own: the frame number is the header's)
template <class IO> void frame_ago(IO &io, int64_t &at, uint64_t now)
{
	int64_t ago = at < 0 ? -1 : int64_t(now) - at;
	io.pod(ago);
	if (IO::loading) at = ago < 0 ? -1 : int64_t(now) - ago;
}
}

template <class IO> void model1::state(IO &io)
{
	m_maincpu.state(io);

	io.section("ram");
	io.vec(m_ram_a);
	io.vec(m_ram_b);

	io.section("display lists");
	io.vec(m_display_list0_ram);
	io.vec(m_display_list1_ram);
	io.pod(m_listctl);
	// (the frames they were written in: kept as frames ago, as the frame number is the header's)
	for (uint64_t &w : m_list_written) {
		uint64_t ago = w == ~0ull ? ~0ull : m_frame - w;
		io.pod(ago);
		if (IO::loading) w = ago == ~0ull ? ~0ull : m_frame - ago;
	}

	io.section("palette");
	io.vec(m_paletteram16_ram);
	io.vec(m_color_xlat_ram);

	io.section("tiles");
	m_tiles.state(io);

	io.section("machine");
	io.pod(m_bank_base);
	io.pod(m_dpram);
	io.pod(m_inputs);
	io.pod(m_lamps);
	io.pod(m_io_command);
	io.pod(m_io_ack);
	io.pod(m_timer_period);
	io.pod(m_timer_val);
	io.pod(m_timer_remain);
	io.pod(m_timer_mode);
	io.pod(m_irq_status);
	io.pod(m_irq_mask);
	io.pod(m_last_irq);
	io.pod(m_uart_txbuf); io.pod(m_uart_shift); io.pod(m_uart_rx);
	io.pod(m_uart_shift_left); io.pod(m_uart_rx_left);

	io.section("tgp glue");
	io.pod(m_v60_copro_ram_adr);
	io.pod(m_v60_copro_ram_latch);
	io.pod(m_v60_copro_fifo_r);
	io.pod(m_v60_copro_fifo_w);
	if (m_lle) lle_state(io);
	else {
		// MAME 0.210's simulation (VF_TGP_SIM, or a set without the TGP's program)
		io.section("tgp simulation");
		io.pod(m_copro_sincos_base); io.pod(m_copro_inv_base); io.pod(m_copro_isqrt_base);
		io.pod(m_copro_atan_base); io.pod(m_copro_data_base); io.pod(m_copro_ram_adr);
		io.deq(m_copro_fifo_in_q.queue());
		io.deq(m_copro_fifo_out_q.queue());
		io.pod(m_pushpc);
		io.pod(m_rng);
		io.pod(m_copro_hle_active_list_pos); io.pod(m_copro_hle_active_list_length); io.pod(m_copro_hle_list_length);
		io.raw(m_cmat); io.raw(m_mat_stack); io.raw(m_mat_vector); io.pod(m_mat_stack_pos); io.raw(m_acc);
		io.raw(m_tgp_vf_xmin); io.raw(m_tgp_vf_xmax); io.raw(m_tgp_vf_zmin); io.raw(m_tgp_vf_zmax);
		io.raw(m_tgp_vf_ygnd); io.raw(m_tgp_vf_yflr); io.raw(m_tgp_vf_yjmp);
		io.raw(m_tgp_vr_circx); io.raw(m_tgp_vr_circy); io.raw(m_tgp_vr_circrad); io.raw(m_tgp_vr_cbox);
		io.pod(m_tgp_vr_select);
		io.raw(m_tgp_int_px); io.raw(m_tgp_int_py); io.raw(m_tgp_int_pz); io.pod(m_tgp_int_adr);
		io.pod(m_copro_hle_ram_scan_adr);
		io.bytes(m_copro_ram_data.get(), 0x8000 * sizeof(uint32_t));
		io.raw(m_tgp_vr_base);
		io.pod(m_ccount);
	}

	// Random fighter (netplay's random fighters: the same setting on both sides): its fighters as they're made, in
	// the ROM's last 512 KB (Akira's records, the moves tables, specials and strands lists it builds, the code it
	// patches there), and what it knows of them on the host's side. Only what the game's run reads or writes from:
	// the heads' turns, the poses' easing, the build's flashes (m_build, m_build_select, m_built_run) are the picture's.
	if (m_random_fighter) {
		io.section("random fighter: rom");
		auto &rom = m_regions.maincpu;
		if (rom.size() >= RANDOM_ROM_END) io.bytes(&rom[RANDOM_ROM_AT], RANDOM_ROM_END - RANDOM_ROM_AT);
		io.section("random fighter");
		io.pod(m_random_state);
		io.pod(m_player_mode);
		io.pod(m_player_seed);
		io.pod(m_select_seen);
		io.pod(m_random_applied);
		io.pod(m_cpu_pending);
		io.pod(m_rolling_at_select);
		io.pod(m_akira_held);
		io.pod(m_random_seen);
		io.pod(m_cpu_human);
		io.pod(m_secret_recs);
		io.pod(m_akira_ribbon);
		io.pod(m_akira_which);
		io.pod(m_akira_track);
		io.pod(m_akira_cam_now);
		io.pod(m_dural_lm);
		io.pod(m_run_on);
		io.pod(m_run_idle);
		io.raw(m_look_share);
		io.pod(m_look_same_body);
		frame_ago(io, m_pick_at, m_frame);
		field(io, m_random_names);
		field(io, m_secret_names);
		field(io, m_random_hue);
		field(io, m_random_outfit);
		field(io, m_random_theme);
		field(io, m_stance_from);
		field(io, m_special_list);
		field(io, m_voice_of);
		field(io, m_move_ids);
		field(io, m_akira_made);
		field(io, m_moves_held);
		field(io, m_portrait_strand_models);
		field(io, m_fought);
		field(io, m_strands_orig);
		field(io, m_strand_roots);   // (the strands' first points last frame: random_fighter_strands_settle writes the game's from them)
	}

	io.section("host");
	// (the stances' table's wait after a reset, and the ranking's names' bookkeeping: both read the board)
	uint64_t since_boot = m_frame - m_boot_at;
	io.pod(since_boot);
	if (IO::loading) m_boot_at = m_frame - since_boot;
	io.pod(m_rank_prev);
	io.pod(m_rank_seen);
	io.pod(m_clock_held);
}

void model1::save_state(std::vector<uint8_t> &out, std::vector<m1::StateSection> *sections, bool append)
{
	if (sections) sections->clear();
	m1::StateWriter w(out, sections, append);
	w.section("header");
	uint32_t magic = STATE_MAGIC, version = STATE_VERSION;
	w.pod(magic);
	w.pod(version);
	w.pod(m_frame);
	state(w);
}

bool model1::load_state(const uint8_t *p, size_t n)
{
	m1::StateReader r(p, n);
	uint32_t magic = 0, version = 0;
	uint64_t frame = 0;
	r.pod(magic);
	r.pod(version);
	r.pod(frame);
	if (!r.ok || magic != STATE_MAGIC || version != STATE_VERSION) return false;
	m_frame = frame;
	state(r);
	if (!r.done()) return false;
	// what's made from the board: the pens from the palette, and the 2D decoded again
	for (int i = 0; i < 0x2000; i++) model1_paletteram_w(offs_t(i), m_paletteram16[i], 0xffff);
	for (auto &c : m_layer_cache) c.frame = ~0ull;
	m_tiles_updated = ~0ull;
	return true;
}
