// Save states (model1.h's save_state): the field list, run both ways (state_io.h).
#include "model1.h"

namespace {
constexpr uint32_t STATE_MAGIC = 0x53314656;   // "VF1S"
constexpr uint32_t STATE_VERSION = 1;
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
