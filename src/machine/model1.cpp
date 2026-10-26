// Sega Model 1 glue: memory map, IRQ controller, timers, ROM banking and the per-frame loop.
// Follows MAME's src/mame/sega/model1.cpp (BSD-3-Clause, copyright-holders: Olivier Galibert).
#include <chrono>
#include "model1.h"
#include <zlib.h>

namespace {
constexpr int CPU_CLOCK = 16000000;          // 32 MHz XTAL / 2
constexpr int H_TOTAL = 656, V_TOTAL = 424;  // 16 MHz pixel clock: 57.52 Hz
constexpr int VBLANK_LINE = 384;
constexpr int CYCLES_PER_LINE = H_TOTAL;     // CPU and pixel clocks are both 16 MHz
}

model1::model1(regions &&r)
	: m_regions(std::move(r))
	, m_maincpu(*this, 0xfffffff0)
	, m_ram_a(0x10000, 0xff)                 // MAME: NVRAM default all 1s (VF's rankings need it)
	, m_ram_b(0x40000, 0)
	, m_display_list0_ram(0x8000), m_display_list1_ram(0x8000)
	, m_paletteram16_ram(0x2000), m_color_xlat_ram(0x6000)
{
	m_display_list0 = m_display_list0_ram.data();
	m_display_list1 = m_display_list1_ram.data();
	m_paletteram16 = m_paletteram16_ram.data();
	m_color_xlat = m_color_xlat_ram.data();
	m_poly_rom = reinterpret_cast<uint32_t *>(m_regions.polygons.data());
	m_copro_tables = reinterpret_cast<uint32_t *>(m_regions.copro_tables.data());
	m_copro_ram_data = std::make_unique<u32[]>(0x8000);
	m_maincpu.irq_vector = [this] { return irq_callback(); };
	m_copro_fifo_in_q.m_on_push = [this] { copro_hle_vf(); };
	m_bitmap.allocate(SCREEN_W, SCREEN_H);
	video_start();
	if (!m_regions.tgp_program.empty() && !getenv("VF_TGP_SIM"))
		use_tgp_program(m_regions.tgp_program);
	if (!m_regions.ioboard.empty() && !getenv("VF_IO_STANDIN"))
		m_ioboard = std::make_unique<model1io>(m_regions.ioboard, m_dpram);
	if (m_regions.maincpu.size() >= RANDOM_ROM_END)   // (VF's own, before anything's changed it: rom_pristine)
		m_rom_orig.assign(m_regions.maincpu.begin() + RANDOM_ROM_AT, m_regions.maincpu.begin() + RANDOM_ROM_END);
	uint64_t h = 0xcbf29ce484222325ull;
	for (const std::vector<uint8_t> *r : { &m_regions.maincpu, &m_regions.copro_tables, &m_regions.polygons, &m_regions.sndcpu,
	                                       &m_regions.pcm1, &m_regions.pcm2, &m_regions.tgp_program })
		h = m1::state_hash(r->data(), r->size()) ^ (h * 0x100000001B3ull);
	m_rom_hash = h;
}

void model1::power_on()
{
	std::fill(m_ram_a.begin(), m_ram_a.end(), 0xff);
	std::fill(m_ram_b.begin(), m_ram_b.end(), 0);
	std::fill(m_display_list0_ram.begin(), m_display_list0_ram.end(), 0);
	std::fill(m_display_list1_ram.begin(), m_display_list1_ram.end(), 0);
	std::fill(m_paletteram16_ram.begin(), m_paletteram16_ram.end(), 0);
	std::fill(m_color_xlat_ram.begin(), m_color_xlat_ram.end(), 0);
	for (int i = 0; i < 0x2000; i++) m_pens[i] = 0;
	m_tiles.clear();
	std::fill(std::begin(m_dpram), std::end(m_dpram), 0);
	std::fill(std::begin(m_inputs), std::end(m_inputs), 0xff);
	m_lamps = 0;
	m_io_command = 0;
	m_io_ack = 0;
	for (int t = 0; t < 2; t++) { m_timer_period[t] = m_timer_val[t] = 0; m_timer_remain[t] = 0; }
	m_timer_mode = 0;
	m_listctl[0] = m_listctl[1] = 0;
	m_v60_copro_fifo_r = m_v60_copro_fifo_w = 0;
	m_rng = 1;
	m_rank_seen = false;
	if (m_lle) use_tgp_program(m_regions.tgp_program);   // (its RAMs as at power-up: reset() keeps them)
	reset();
}

void model1::use_io_standin(bool on)
{
	if (on && m_ioboard) m_ioboard_off = std::move(m_ioboard);
	else if (!on && m_ioboard_off) m_ioboard = std::move(m_ioboard_off);
}

// (frames after a reset: random_fighter_hold_moves. The table is written on frames 203-204 and the
// attract's fighters are drawn from 211: given back at 240, on screen, every random fighter's pose
// jumped as its own moves table came back, the stance snap as the intro began)
static constexpr uint64_t STANCES_BUILT = 206;

void model1::reset()
{
	m_boot_at = m_frame;
	m_akira_held[0] = m_akira_held[1] = 0;   // (work RAM survives a reset: no one has picked)
	m_clock_held = false;                    // (No time limit: a round held at the reset is over)
	m_run_on = false; m_run_idle = 0; m_fought.clear(); m_built_run.clear(); m_cpu_akira.clear();   // (a new game: no run's fighters carried into it)
	random_fighter_hold_moves();
	for (auto &c : m_layer_cache) c.frame = ~0ull;   // a new game: nothing drawn is current
	m_shown = m_unshown = false;                      // nor shown
	m_tiles_updated = ~0ull;
	m_list_written[0] = m_list_written[1] = ~0ull;
	m_bank_base = 0x1000000;
	m_maincpu.set_irq_line(CLEAR_LINE);
	copro_reset();
	m_acc = 0;
	m_mat_stack_pos = 0;
	m_copro_hle_active_list_length = 0;
	m_copro_hle_active_list_pos = 0;
	m_copro_hle_list_length = 0;
	memset(m_cmat, 0, sizeof(m_cmat));
	m_cmat[0] = m_cmat[4] = m_cmat[8] = 1.0f;
	m_copro_fifo_in_q.clear();
	m_copro_fifo_out_q.clear();
	lle_reset();
	m_irq_status = 0;
	m_last_irq = 0;
	m_irq_mask = 0xff;
	m_uart_txbuf = m_uart_shift = m_uart_rx = -1;
	m_uart_shift_left = m_uart_rx_left = 0;
	if (m_audio) m_audio->reset();
	if (m_ioboard) {
		std::fill(std::begin(m_dpram), std::end(m_dpram), 0);
		m_ioboard->reset();
	}
	m_maincpu.reset();
}

void model1::copro_reset()
{
	m_v60_copro_ram_adr = 0;
	m_copro_ram_adr = 0;
	m_copro_sincos_base = 0;
	m_copro_inv_base = 0;
	m_copro_isqrt_base = 0;
	std::fill(std::begin(m_copro_atan_base), std::end(m_copro_atan_base), 0);
	std::fill(std::begin(m_v60_copro_ram_latch), std::end(m_v60_copro_ram_latch), 0);
	memset(m_copro_ram_data.get(), 0, 0x8000 * 4);
}

// ---- frame loop ----

void model1::run_frame()
{
	// Random fighter: the select picks for the players, Akira (whose place the fighter has), player
	// 1 first then player 2 (picking after player 1's same fighter gives player 2 Akira's other
	// costume, so a fighter of their own). The cursors are in the players' blocks (40749c, 4074b0,
	// 4074b3; player 2's 0x80 on); kick picks.
	const uint8_t inputs_were[3] = { m_inputs[0], m_inputs[1], m_inputs[2] };   // (the presses below are this frame's only)
	if (m_random_fighter && m_pick_at >= 0) {
		const int64_t f = int64_t(m_frame) - m_pick_at, ph = f % 40;
		// (only the players with a random fighter; the first of them at 10, the second at 30. A player
		// whose setting is "" picks as ever)
		// (a random fighter's player can't move or pick at the select themselves: their controls as
		// let go, but for the moves below: Left until its cursor (40749c, 0x80 on) is on Akira, the
		// row's first, so the select shows him (his place: its fighter's portrait) as a player's own
		// moves do; put there in memory, the pick took him but the portrait stayed on the one before)
		for (int pl = 0; pl < 2; pl++)
			if (m_player_mode[pl]) {
				m_inputs[1 + pl] = 0xff;
				if (m_ram_a.size() > 0x7600 && m_ram_a[0x749c + 0x80 * pl] != 0 && f % 8 < 3) m_inputs[1 + pl] &= uint8_t(~0x80);
			}
		// (two players in, all random: the pick waits RANDOM_PREVIEW, the fighters' portraits seen
		// first; one playing alone, the select hidden (a surprise: random_select_hidden) and the pick
		// at once; a player picking as ever there, at once, that one's pick the pace)
		bool normal_at = random_select_hidden(false);
		for (int pl = 0; pl < 2 && m_ram_a.size() > 0x7600; pl++)
			normal_at = normal_at || ((m_ram_a[0x7483 + 0x80 * pl] & 0x08) && !m_player_mode[pl]);
		int k = 0;
		for (int pl = 0; pl < 2; pl++)
			if (m_player_mode[pl]) {
				const int at = k++ ? 30 : 10;
				if (f >= (normal_at ? 0 : RANDOM_PREVIEW) && m_ram_a[0x749c + 0x80 * pl] == 0 && ph >= at && ph < at + 6) m_inputs[1 + pl] &= ~0x02;
			}
	}

	// the picture on screen was drawn from the state this frame is about to change: keep that
	if (m_smooth && m_shown && m_shown_frame == m_frame) keep_still();
	if (m_kage_unmasked && m_ram_a.size() > 0xe009) m_ram_a[0xe009] |= 1;
	for (int line = 0; line < V_TOTAL; line++) {
		if (line == VBLANK_LINE) {
			if (!BIT(m_irq_mask, 1))
				irq_raise(1);
			// vblank starts: the display lists are scanned and swapped
			screen_vblank_model1(1);
		}
		if (run_cpu)
			run_cpu(CYCLES_PER_LINE);
		else {
			m_maincpu.on_exec = on_instruction;
			m_maincpu.run(CYCLES_PER_LINE);
		}
		timer_tick(CYCLES_PER_LINE);
		io_board_tick(CYCLES_PER_LINE);
		uart_tick(CYCLES_PER_LINE);
		if (m_audio)
			m_audio->run(CYCLES_PER_LINE * 1e6 / CPU_CLOCK, m_audio_out);
		if (on_line) on_line(line);
	}
	if (m_icon.on) secret_icon_frame();
	if (m_random_fighter) random_fighter_frame();
	else if (m_akira_which > 0) select_cover(select_showing());
	if (!m_moves_held.empty() && m_frame - m_boot_at >= STANCES_BUILT) {   // (the stances' table built: random_fighter_hold_moves)
		for (const auto &[r, t] : m_moves_held) for (int b = 0; b < 4; b++) m_regions.maincpu[r + 4 + b] = uint8_t(t >> (8 * b));
		m_moves_held.clear();
	}
	else if (m_secret_recs[0]) random_fighter_hud();
	if (static const bool sl = getenv("VF_STRANDLOG") != nullptr; sl) strand_log();   // TESTONLY
	ranking_frame();
	memcpy(m_inputs, inputs_were, sizeof m_inputs);
	list_select_settle();   // (the list selected, as the picture's drawing had it brought up to date: drawn or not)
	// MAME draws the screen when vblank ends (VIDEO_UPDATE_AFTER_VBLANK): the 2D layers are
	// as the vblank handler left them
	if (m_draw) {
		if (m_smooth) record_frame();     // drawn later, for the moment shown (render_smooth)
		else screen_update(m_bitmap, m_bitmap.cliprect());
	}
	m_frame++;
}

// ---- IRQs (MAME: irq_raise, irq_callback, irq_control_w, irq_mask_w) ----

void model1::irq_raise(int level)
{
	m_irq_status |= (1 << level);
	m_maincpu.set_irq_line(ASSERT_LINE);
}

u8 model1::irq_callback()
{
	for (int i = 0; i < 8; i++)
		if (BIT(m_irq_status, i)) {
			m_last_irq = i;
			break;
		}
	return m_last_irq;
}

void model1::irq_control_w(u8 data)
{
	switch (data) {
	case 0x10:
		m_irq_status = 0;
		m_maincpu.set_irq_line(CLEAR_LINE);
		break;
	case 0x20:
		m_irq_status &= ~(1 << m_last_irq);
		if (m_irq_status == 0)
			m_maincpu.set_irq_line(CLEAR_LINE);
		break;
	}
}

u8 model1::irq_mask_r() { return m_irq_mask; }

void model1::irq_mask_w(u8 data)
{
	m_irq_mask = data;
	sound_ready();
}

// MAME's sound_ready_w: the UART ready to send or holding a byte raises level 3 (VF's sound
// queue pump) unless it's masked.
void model1::sound_ready()
{
	if ((m_uart_txbuf < 0 || m_uart_rx >= 0) && !BIT(m_irq_mask, 3))
		irq_raise(3);
}

void model1::uart_write(uint8_t v)
{
	m_uart_txbuf = v;
	if (m_uart_shift < 0) {           // the shift register takes it at once
		m_uart_shift = m_uart_txbuf;
		m_uart_txbuf = -1;
		m_uart_shift_left = UART_BYTE_CYCLES;
	}
	sound_ready();
}

uint8_t model1::uart_status()
{
	uint8_t st = 0x80;                                    // DSR
	if (m_uart_txbuf < 0) st |= 0x01;                     // TxRDY
	if (m_uart_rx >= 0) st |= 0x02;                       // RxRDY
	if (m_uart_txbuf < 0 && m_uart_shift < 0) st |= 0x04; // TxEMPTY
	return st;
}

uint8_t model1::uart_read()
{
	uint8_t v = m_uart_rx >= 0 ? uint8_t(m_uart_rx) : 0;
	m_uart_rx = -1;
	return v;
}

void model1::uart_tick(int cycles)
{
	if (m_uart_shift >= 0 && (m_uart_shift_left -= cycles) <= 0) {
		if (static const bool sl = getenv("VF_SNDLOG") != nullptr; sl) fprintf(stderr, "SND %llu %02x\n", (unsigned long long)m_frame, m_uart_shift & 0xff);   // TESTONLY
		if (m_audio) m_audio->rx_byte(uint8_t(m_uart_shift));
		else if (on_uart_tx) on_uart_tx(uint8_t(m_uart_shift));
		m_uart_shift = -1;
		if (m_uart_txbuf >= 0) {
			m_uart_shift = m_uart_txbuf;
			m_uart_txbuf = -1;
			m_uart_shift_left += UART_BYTE_CYCLES;
			sound_ready();
		}
	}
	// replies from the sound board, at the same rate
	if (m_uart_rx_left > 0) m_uart_rx_left -= cycles;
	if (m_audio && m_audio->tx_pending() && m_uart_rx < 0 && m_uart_rx_left <= 0) {
		m_uart_rx = m_audio->tx_byte();
		m_uart_rx_left = UART_BYTE_CYCLES;
		sound_ready();
	}
}

void model1::enable_sound()
{
	m_audio = std::make_unique<m1audio>(m_regions.sndcpu, m_regions.pcm1, m_regions.pcm2);
	m_audio->reset();
}

void model1::set_volumes(float music, float effects)
{
	if (m_audio) m_audio->set_volumes(music, effects);
}

// ---- timers (MAME: 0x800 CPU clocks per count; VF leaves them stopped) ----

u16 model1::timer_r(offs_t offset)
{
	if (m_timer_period[offset])
		m_timer_val[offset] = uint16_t(m_timer_remain[offset] / 0x800);
	return m_timer_val[offset];
}

void model1::timer_period_w(offs_t offset, u16 data)
{
	m_timer_period[offset] = data;
	m_timer_remain[offset] = int64_t(0x800) * data;
}

void model1::timer_tick(int cycles)
{
	for (int t = 0; t < 2; t++) {
		if (!m_timer_period[t])
			continue;
		m_timer_remain[t] -= cycles;
		while (m_timer_remain[t] <= 0) {
			if (!BIT(m_irq_mask, 0))
				irq_raise(0);
			m_timer_remain[t] += int64_t(0x800) * m_timer_period[t];
		}
	}
}

void model1::bank_w(offs_t, u16 data, u16 mem_mask)
{
	if ((mem_mask & 0xff) && (data & 0xf) == 1)
		m_bank_base = 0x1000000 + 0x100000 * ((data >> 4) & 0x7);
}

// ---- I/O board stand-in ----
// Byte offsets in the dual-port RAM (V60 address = 0xc00000 + 2 * offset), from MAME 0.190:
//   00-07 analog inputs (VF has none), 08-0a digital inputs, 0f lamps,
//   20 command to the board (cleared when done; 3 = EEPROM write), 21 status (0x40),
//   100-17f the EEPROM image, kept in the shared RAM.

uint8_t model1::io_board_r(int index)
{
	m_maincpu.adjust_icount(-1);    // MAME: a wait state on every dual-port RAM read
	if (m_ioboard) return m_dpram[index];
	if (index < 0x08) return 0x00;
	if (index < 0x0b) return m_inputs[index - 0x08];
	if (index < 0x10) return index == 0x0f ? m_lamps : 0xff;
	if (index == 0x20) return m_io_command;
	if (index == 0x21) return 0x40;
	return m_dpram[index];
}

void model1::io_board_w(int index, uint8_t data)
{
	if (m_ioboard) { m_dpram[index] = data; return; }
	if (index == 0x0f) { m_lamps = data; return; }
	if (index == 0x20) {
		m_io_command = data;
		// MAME's guess: 10 us, or 100 ms for an EEPROM write
		m_io_ack = data == 3 ? CPU_CLOCK / 10 : CPU_CLOCK / 100000;
		return;
	}
	m_dpram[index] = data;
}

void model1::io_board_tick(int cycles)
{
	if (m_ioboard) {
		std::copy(std::begin(m_inputs), std::end(m_inputs), m_ioboard->inputs);
		m_ioboard->run(cycles * 1e6 / CPU_CLOCK);
		m_lamps = m_ioboard->outputs;
		return;
	}
	if (m_io_command && (m_io_ack -= cycles) <= 0)
		m_io_command = 0;
}

// ---- TGP status ----

u16 model1::fifoin_status_r() { return 0xffff; }

// ---- memory map (MAME: model1_mem / model1_io) ----

uint16_t model1::bus_read16(uint32_t a)
{
	a &= 0xfffffe;
	const uint8_t *rom = m_regions.maincpu.data();
	auto rd = [](const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); };
	switch (a >> 20) {
	case 0x0: return rd(rom + a);                                        // ROMA (unpopulated)
	case 0x1: return rd(rom + m_bank_base + (a & 0xfffff));             // ROMO, banked data
	case 0x2: return rd(rom + a);                                        // ROMX
	case 0xf: return a >= 0xf80000 ? rd(rom + a) : 0xffff;              // ROM0
	}
	if (a >= 0x400000 && a < 0x410000) return rd(&m_ram_a[a - 0x400000]);
	if (a >= 0x500000 && a < 0x540000) return rd(&m_ram_b[a - 0x500000]);
	if (a >= 0x600000 && a < 0x610000) return m_display_list0[(a - 0x600000) >> 1];
	if (a >= 0x610000 && a < 0x620000) return m_display_list1[(a - 0x610000) >> 1];
	if (a >= 0x680000 && a < 0x680004) return model1_listctl_r((a - 0x680000) >> 1);
	if (a >= 0x700000 && a < 0x710000) return m_tiles.tile_r((a - 0x700000) >> 1);
	if (a >= 0x780000 && a < 0x800000) return m_tiles.char_r((a - 0x780000) >> 1);
	if (a >= 0x900000 && a < 0x904000) return m_paletteram16[(a - 0x900000) >> 1];
	if (a >= 0x910000 && a < 0x91c000) return m_color_xlat[(a - 0x910000) >> 1];
	if (a >= 0xc00000 && a < 0xc01000) return 0xff00 | io_board_r(((a - 0xc00000) >> 1) & 0x7ff);
	if (a >= 0xc40000 && a < 0xc40004) return 0xff00 | ((a & 2) ? uart_status() : uart_read());
	if (m_lle && a >= 0xd00000 && a < 0xde0000) return lle_v60_r(a);
	if (a >= 0xd00000 && a < 0xd20000) return v60_copro_ram_adr_r(0, 0xffff);
	if (a >= 0xd20000 && a < 0xd40000) return v60_copro_ram_r((a >> 1) & 1, 0xffff);
	if (a >= 0xd80000 && a < 0xda0000) return v60_copro_fifo_r((a >> 1) & 1, 0xffff);
	if (a >= 0xdc0000 && a < 0xde0000) return fifoin_status_r();
	if (a == 0xe00002) return m_irq_mask;
	if (a >= 0xe0000c && a < 0xe00010) return timer_r((a >> 1) & 1);
	return 0xffff;
}

void model1::bus_write16(uint32_t a, uint16_t v, uint16_t mask)
{
	a &= 0xfffffe;
	static uint32_t watch_lo = 1, watch_hi = 0;      // debug: VF_WATCHW=addr or lo-hi (hex)
	static const bool watch_init = [] {
		if (const char *w = getenv("VF_WATCHW")) {
			char *end;
			watch_lo = watch_hi = strtoul(w, &end, 16);
			if (*end == '-') watch_hi = strtoul(end + 1, nullptr, 16);
		}
		return true;
	}();
	(void)watch_init;
	if (a >= watch_lo && a <= watch_hi)
		fprintf(stderr, "frame %llu: write %06x = %04x/%04x pc %06x\n", (unsigned long long)m_frame, a, v, mask, m_maincpu.pc());
	auto wr = [&](uint8_t *p) {
		if (mask & 0x00ff) p[0] = uint8_t(v);
		if (mask & 0xff00) p[1] = uint8_t(v >> 8);
	};
	auto combine = [&](uint16_t &d) { d = (d & ~mask) | (v & mask); };
	if (a >= 0x400000 && a < 0x410000) { wr(&m_ram_a[a - 0x400000]); return; }
	if (a >= 0x500000 && a < 0x540000) { wr(&m_ram_b[a - 0x500000]); return; }
	if (a >= 0x600000 && a < 0x610000) { combine(m_display_list0[(a - 0x600000) >> 1]); m_list_written[0] = m_frame; return; }
	if (a >= 0x610000 && a < 0x620000) { combine(m_display_list1[(a - 0x610000) >> 1]); m_list_written[1] = m_frame; return; }
	if (a >= 0x680000 && a < 0x680004) { model1_listctl_w((a - 0x680000) >> 1, v, mask); return; }
	if (a >= 0x700000 && a < 0x710000) {
		m_tiles.tile_w((a - 0x700000) >> 1, v, mask);
		if (a == 0x70a004 || a == 0x70a00c) sky_scroll_written(a);   // the sky's scroll (model1_interp.cpp)
		return;
	}
	if (a >= 0x780000 && a < 0x800000) { m_tiles.char_w((a - 0x780000) >> 1, v, mask); return; }
	if (a >= 0x900000 && a < 0x904000) { model1_paletteram_w((a - 0x900000) >> 1, v, mask); return; }
	if (a >= 0x910000 && a < 0x91c000) { combine(m_color_xlat[(a - 0x910000) >> 1]); return; }
	if (a >= 0xc00000 && a < 0xc01000) { if (mask & 0xff) io_board_w(((a - 0xc00000) >> 1) & 0x7ff, uint8_t(v)); return; }
	if (a == 0xc40000) { if (mask & 0xff) uart_write(uint8_t(v)); return; }
	if (a == 0xc40002) return;                                   // i8251 mode/command
	if (m_lle && a >= 0xd00000 && a < 0xde0000) { lle_v60_w(a, v, mask); return; }
	if (a >= 0xd00000 && a < 0xd20000) { v60_copro_ram_adr_w(0, v, mask); return; }
	if (a >= 0xd20000 && a < 0xd40000) { v60_copro_ram_w((a >> 1) & 1, v, mask); return; }
	if (a >= 0xd80000 && a < 0xda0000) { v60_copro_fifo_w((a >> 1) & 1, v, mask); return; }
	if (a == 0xe00000) { if (mask & 0xff) irq_control_w(uint8_t(v)); return; }
	if (a == 0xe00002) { if (mask & 0xff) irq_mask_w(uint8_t(v)); return; }
	if (a == 0xe00004) { bank_w(0, v, mask); return; }
	if (a == 0xe00006) { combine(m_timer_mode); return; }
	if (a >= 0xe00008 && a < 0xe0000c) { timer_period_w((a >> 1) & 1, v); return; }
}

uint8_t model1::read8(uint32_t a) { return uint8_t(bus_read16(a) >> ((a & 1) * 8)); }
void model1::write8(uint32_t a, uint8_t v) { bus_write16(a, uint16_t(v) << ((a & 1) * 8), (a & 1) ? 0xff00 : 0x00ff); }

uint16_t model1::read16(uint32_t a)
{
	if (a & 1) return read8(a) | (read8(a + 1) << 8);
	return bus_read16(a);
}

void model1::write16(uint32_t a, uint16_t v)
{
	if (a & 1) { write8(a, uint8_t(v)); write8(a + 1, uint8_t(v >> 8)); return; }
	bus_write16(a, v, 0xffff);
}

uint32_t model1::read32(uint32_t a) { return read16(a) | (uint32_t(read16(a + 2)) << 16); }
void model1::write32(uint32_t a, uint32_t v) { write16(a, uint16_t(v)); write16(a + 2, uint16_t(v >> 16)); }

uint16_t model1::io_read16(uint32_t a) { return bus_read16(a); }
void model1::io_write16(uint32_t a, uint16_t v) { bus_write16(a, v, 0xffff); }

// ---- render scale ----

void model1::set_render_scale(int scale)
{
	m_scale = std::clamp(scale, 1, 12);       // up to 6x, or less with antialiasing on top
	s_moire_blend = m_scale > 1;
	s_hd_raster = m_scale > 1 && !getenv("VF_MAME_RASTER");
	resize_frame();
}

void model1::set_aspect(float aspect)
{
	m_aspect = std::clamp(aspect, 4.0f / 3.0f, 32.0f / 9.0f);
	// the arcade's pixels are slightly wide: 496x384 fills a 4:3 tube
	float width = SCREEN_H * m_aspect * float(SCREEN_W) / (SCREEN_H * 4.0f / 3.0f);
	m_ext = std::max(0, int(std::lround((width - SCREEN_W) / 2)));
	resize_frame();
}

void model1::resize_frame()
{
	// the view's clip is kept in render pixels: undo the old widening before changing it
	view_t *v = m_view.get();
	float gx1 = v->lx1, gx2 = v->lx2;
	if (v->ext > 0 && v->lx1 < 0) { gx1 += v->ext; gx2 -= v->ext; }
	m_bitmap.allocate((SCREEN_W + 2 * m_ext) * m_scale, SCREEN_H * m_scale);
	m_bitmap_whole = true;                    // (not KEY yet, for the GPU's parts)
	m_tile_tmp.allocate(SCREEN_W + 2 * m_ext, SCREEN_H);
	m_l4_frame = ~0ull;                       // (made again empty: layer 4 drawn into it again, a
	                                          // stopped game's too, or a 4:3 screen's painted sides go black)
	m_tiles.m_ox = 0;
	v->scale = float(m_scale);
	v->ext = float(m_ext);
	v->set_viewport(v->xc, v->yc, gx1, gx2, v->ly1, v->ly2);
	// the above-HUD pass's buffers: made when first needed (never in VF; they'd be two frames' worth)
	m_overlay_stride = m_bitmap.width();
	m_overlay_block.reset();
	m_overlay_hud_snapshot.reset();
}

// VF's settings in the 93C46: "SEGA" in words 0-1, a CRC-16 (XMODEM, of words 8-63 high byte
// first) in word 4 stored low byte first, and the coin/credit setting in word 8's high byte:
// setting #26 (0x19) is FREE PLAY, with word 10's high byte set.
// edit(e) changes the words and says whether it did; false too when the game hasn't written its
// settings yet (its first boot does).
template <class F>
static bool edit_settings(uint16_t *ioboard, uint8_t *standin, F edit)
{
	// The real I/O board's 93C46 holds 16-bit words; the stand-in's image (without model1io.zip)
	// is the same settings with each word's bytes the other way round.
	uint16_t words[64];
	if (standin)
		for (int i = 0; i < 64; i++) words[i] = uint16_t(standin[2 * i] << 8 | standin[2 * i + 1]);
	uint16_t *e = ioboard ? ioboard : words;
	if (e[0] != 0x5345 || e[1] != 0x4741 || !edit(e)) return false;
	uint16_t crc = 0;
	for (int i = 8; i < 64; i++)
		for (uint8_t byte : { uint8_t(e[i] >> 8), uint8_t(e[i]) }) {
			crc ^= uint16_t(byte << 8);
			for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
		}
	e[4] = uint16_t(crc << 8 | crc >> 8);
	if (standin)
		for (int i = 0; i < 64; i++) { standin[2 * i] = uint8_t(words[i] >> 8); standin[2 * i + 1] = uint8_t(words[i]); }
	return true;
}

int model1::coin_setting()
{
	int v = -1;
	edit_settings(m_ioboard ? m_ioboard->eeprom : nullptr, m_ioboard ? nullptr : eeprom(), [&](uint16_t *e) {
		v = (e[8] & 0xff00) | (e[10] >> 8);
		return false;
	});
	return v;
}

// (both bytes: a coin setting of #26 with word 10's left as it was isn't free play, and was taken
// for it, so PC mode kept the credits)
bool model1::set_coin_setting(int v)
{
	return edit_settings(m_ioboard ? m_ioboard->eeprom : nullptr, m_ioboard ? nullptr : eeprom(), [&](uint16_t *e) {
		if (((e[8] & 0xff00) | (e[10] >> 8)) == v) return false;
		e[8] = uint16_t((e[8] & 0x00ff) | (v & 0xff00));
		e[10] = uint16_t((e[10] & 0x00ff) | ((v & 0xff) << 8));
		return true;
	});
}

// GAME ASSIGNMENTS in the 93C46 (found by changing each in test mode): the word, and the byte in it
static constexpr struct { int word, shift, lo, hi; } ASSIGNMENTS[] = {
	{ 24, 8, 2, 5 },   // MATCH COUNT (1P): rounds to win
	{ 24, 0, 2, 5 },   // MATCH COUNT (VS)
	{ 16, 8, 0, 3 },   // DIFFICULTY: EASY, NORMAL, HARD, HARDEST
	{ 29, 8, 0, 1 },   // ADVERTISE SOUND
	{ 27, 0, 0, 1 },   // CONTINUE
	{ 13, 0, 0, 1 },   // LANGUAGE: JAPANESE, ENGLISH (the rules between demos, its text screens; found by trying each byte)
};

int model1::assignment(game_assignment a)
{
	const auto &s = ASSIGNMENTS[int(a)];
	int v = -1;
	edit_settings(m_ioboard ? m_ioboard->eeprom : nullptr, m_ioboard ? nullptr : eeprom(), [&](uint16_t *e) {
		v = std::clamp((e[s.word] >> s.shift) & 0xff, s.lo, s.hi);
		return false;
	});
	return v;
}

bool model1::set_assignment(game_assignment a, int v)
{
	const auto &s = ASSIGNMENTS[int(a)];
	v = std::clamp(v, s.lo, s.hi);
	return edit_settings(m_ioboard ? m_ioboard->eeprom : nullptr, m_ioboard ? nullptr : eeprom(), [&](uint16_t *e) {
		if (((e[s.word] >> s.shift) & 0xff) == v) return false;
		e[s.word] = uint16_t((e[s.word] & ~(0xff << s.shift)) | v << s.shift);
		return true;
	});
}

bool model1::patch_program(uint32_t addr, size_t n, uint32_t crc, const uint8_t *to)
{
	auto &rom = m_regions.maincpu;
	if (addr + n > rom.size() || ::crc32(0, &rom[addr], uInt(n)) != crc) return false;
	memcpy(&rom[addr], to, n);
	return true;
}

// The ring's width for EASY..HARDEST in 20s (1500, 1400, 1300, 1200), read as each match starts;
// the energy for each (16-bit) follows it.
bool model1::set_ring_width(int width)
{
	constexpr uint32_t TABLE = 0xfcc9f6;
	static const uint8_t ARCADE[4] = { 75, 70, 65, 60 }, ENERGY[8] = { 0xdc, 0, 0xc8, 0, 0xb4, 0, 0xa0, 0 };
	auto &rom = m_regions.maincpu;
	if (TABLE + 12 > rom.size() || memcmp(&rom[TABLE + 4], ENERGY, 8)) return false;
	for (int i = 0; i < 4; i++)
		if (rom[TABLE + i] < 25 || rom[TABLE + i] > 250) return false;   // not VF's table (or ours: Cheats' rings, 600 and 5000)
	for (int i = 0; i < 4; i++) rom[TABLE + i] = width ? uint8_t(width / 20) : ARCADE[i];
	return true;
}

void model1::set_kage_unmasked(bool on)
{
	const bool was = m_kage_unmasked;
	m_kage_unmasked = on;
	if (m_ram_a.size() <= 0xe009) return;
	// (turned off: the flag as the play clock has it, its hours on: set from the third 3-hour step on.
	// Off from the start, never on here: the board's own flag left as it is, the normal game's)
	if (!on && was) m_ram_a[0xe009] = uint8_t((m_ram_a[0xe009] & ~1) | (m_ram_a[0xe007] >= 3 || m_ram_a[0xe008] ? 1 : 0));
}

// Akira's entries (from mountainmanjed's MAME cheats, tcrf.net/Virtua_Fighter_(Arcade)): his two
// slots in the table of characters (fd2e8f: 26 pointers into an array of 0x176-byte records), his
// select portrait, his name on the select screen for each side, and his bio. An unused character
// takes as many as it has; the rest stay Akira's.
// The strands that swing (hair, braids, Akira's headband ribbon) are listed by slot, not by record,
// for the select's portrait (fd2b33, read at fce947) and for the fight (fda917, read at ffc4b6, by
// the player's slot): a slot number and its strands' data, to ff. A character in Akira's slots
// would wear his ribbon, so his entries there get a slot no player has (0x19, the last boss's).
void model1::set_akira_slot(int which)
{
	random_fighter_cast();   // (Random fighter's copy of the cast as VF has them, before this changes any)
	enum { MODEL1, MODEL2, PORTRAIT, NAME_P1, NAME_P2, BIO, RIBBON1, RIBBON2, RIBBON3, RIBBON4, N };
	static const struct { uint32_t addr; int size; } AT[N] = {
		{ 0xfd2ee7, 2 }, { 0xfd2ebb, 2 }, { 0xfd2a73, 4 }, { 0xfd2a93, 2 }, { 0xfd2ab3, 2 }, { 0xfd2ad3, 2 },
		{ 0xfd2b60, 1 }, { 0xfd2b5b, 1 }, { 0xfda944, 1 }, { 0xfda93f, 1 } };
	constexpr int NO = 0x19;   // (no ribbon)
	static const int64_t TO[std::size(UNUSED_CHARACTERS)][N] = {     // -1: Akira's own
		{ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
		{ 0x33af, 0x4843, 0x001d1a8f, 0x0b37, 0x14f7, 0xd773, NO, NO, NO, NO },     // Majido (Siba)
		{ 0x3c7b, 0x4ca5, -1, 0x09ff, 0x11eb, 0xc95b, NO, NO, NO, NO },             // Jeff (TCRF's has Majido's icon: a made one instead)
		{ 0x3df9, 0x4e23, -1, -1, -1, 0xb437, -1, -1, -1, -1 },                     // Takeru, the early Akira (his ribbon)
		// the Koskol brothers, one each, in both of Akira's slots: TCRF's cheats put two in at once
		// (Rekka + Score, Aeon + Dural), one per slot
		{ 0x510f, 0x510f, -1, -1, -1, -1, NO, NO, NO, NO },                         // Rekka
		{ 0x5285, 0x5285, -1, -1, -1, -1, NO, NO, NO, NO },                         // Score
		{ 0x53fb, 0x53fb, -1, -1, -1, -1, NO, NO, NO, NO },                         // Aeon
		{ 0x369b, 0x369b, -1, -1, -1, -1, NO, NO, NO, NO },                         // Dural (TCRF's second with Aeon)
	};
	auto &rom = m_regions.maincpu;
	if (AT[0].addr + 4 > rom.size()) return;
	static uint8_t akira[N][4];
	static bool kept = false, ribbon = false;
	if (!kept) {
		for (int i = 0; i < N; i++) memcpy(akira[i], rom_pristine(AT[i].addr), AT[i].size);
		// (VF's lists as expected: by the fight's (RIBBON3/4), which nothing else changes; the
		// portraits' (RIBBON1/2) may have been changed already by Random fighter's portrait strands,
		// so VF's own values taken for them)
		ribbon = akira[RIBBON3][0] == 0x16 && akira[RIBBON4][0] == 0x0b;
		if (ribbon) { akira[RIBBON1][0] = 0x16; akira[RIBBON2][0] = 0x0b; }
		kept = true;
	}
	which = std::clamp(which, 0, int(std::size(UNUSED_CHARACTERS)) - 1);
	for (int i = 0; i < N; i++) {
		uint8_t *p = &rom[AT[i].addr];
		if (TO[which][i] < 0 || (i >= RIBBON1 && !ribbon)) memcpy(p, akira[i], AT[i].size);
		else for (int b = 0; b < AT[i].size; b++) p[b] = uint8_t(TO[which][i] >> (8 * b));   // (little-endian)
	}
	m_akira_ribbon[0] = rom[AT[RIBBON1].addr];   // (the portraits' slot bytes for Akira's places: random_fighter_portrait_strands keeps them)
	m_akira_ribbon[1] = rom[AT[RIBBON2].addr];
	random_fighter_portrait_strands();   // (a random fighter's own over them)
	akira_slot_heads(TO[which][MODEL1] < 0 ? 0 : 0xfd0000 | uint32_t(TO[which][MODEL1]));
	// (their names under the energy bars: their own, by random_fighter_hud, not Akira's or the ROM's
	// MAJIDO and WILLIE)
	for (int k = 0; k < 2; k++) m_secret_recs[k] = which > 0 && TO[which][MODEL1 + k] >= 0 ? 0xfd0000 | uint32_t(TO[which][MODEL1 + k]) : 0;
	m_akira_which = which;
	// (with Random opponents, its records as they are: made random at the last select, put back)
	if (m_random_applied)
		for (int k = 0; k < 2; k++)
			if (m_secret_recs[k] && m_random_names.count(m_secret_recs[k])) { random_akira_record(m_secret_recs[k], 0, 0); random_fighter_strands(); random_fighter_specials(); }
	// Dural is the game's own in Akira's place, not her record in Akira's slots: the fight looks a
	// fighter up by slot (the select's character, 0-8, through fd2f1b for player 1 and fd2f24 for
	// player 2: Akira's slots 0x16 and 0x0b, Dural's 5), and the game checks for her by slot, as
	// for the metal clank of a hit on her (ffb8d8: 9e0f7f), which a record in Akira's slots never
	// made. Akira's entry is her slot instead.
	static const uint32_t SLOT_TABLES[2] = { 0xfd2f1b, 0xfd2f24 };
	static const uint8_t AKIRA_SLOT[2] = { 0x16, 0x0b };
	for (int t = 0; t < 2; t++) {
		uint8_t *e = &rom[SLOT_TABLES[t]];
		if (SLOT_TABLES[t] + 9 > rom.size() || e[8] != 5 || (e[0] != AKIRA_SLOT[t] && e[0] != 5)) continue;   // (not VF's)
		e[0] = which == 7 ? 5 : AKIRA_SLOT[t];
	}
	// Siba's pounce (Up + Punch at a downed opponent): his moves table (fc78c4, his record's +4)
	// has move 259 there, a placeholder's record (fc2722, five other moves': a jump with no jump in
	// it), so the game planned a jump of 0 + 2 frames, the whole way in 2, and he flew on at that
	// speed for his pounce's 26: across the ring, often out of it. Lau's pounce (381) in its place,
	// as his table has it (Siba's moves are mostly Lau's).
	constexpr uint32_t SIBA_POUNCE = 0xfc78c4 + 130 * 4;
	if (SIBA_POUNCE + 4 <= rom.size() && (rom[SIBA_POUNCE + 1] == 0x01) && (rom[SIBA_POUNCE] == 0x03 || rom[SIBA_POUNCE] == 0x7d))
		rom[SIBA_POUNCE] = which == 1 || (m_random_fighter && ((m_player_mode[0] == 3 && m_player_seed[0] == 1) || (m_player_mode[1] == 3 && m_player_seed[1] == 1))) ? 0x7d : 0x03;   // (381 = 0x17d; 259 = 0x103; a player's Siba too)
	akira_camera(which);
}

// The select's camera for Akira's spot (fd278b: x, y, z, then 3 angles, a camera per place in
// the row): each head sits differently in its own frame (its origin, how it's turned), so each
// has its own: the eyes at the cast's height (a little below the middle), the face across the
// middle, the head the cast's size, the neck running off the bottom. Dural's head (the fight's)
// is turned and tilted down besides: hers turns it to face as the rest. (Found by rendering
// each against the cast's.) 0: Akira's own, as VF has it
void model1::akira_camera(int which)
{
	auto &rom = m_regions.maincpu;
	struct cam { float x, y, z; int16_t a, b, c; };
	static const cam CAMS[std::size(UNUSED_CHARACTERS)] = {
		{ 0, 0, 0, 0, 0, 0 },                                   // Akira: his own
		{ -0.028f, -0.075f, 1.0f, 17400, 32000, -23549 },        // Siba
		{ -0.019f, -0.103f, 1.0f, 17400, 32000, -23549 },        // Jeff
		{ -0.025f, -0.1f, 0.9f, 17400, 32000, -23549 },          // Takeru
		{ -0.003f, -0.085f, 0.95f, 17400, 32000, -23549 },       // Rekka
		{ -0.003f, -0.085f, 0.95f, 17400, 32000, -23549 },       // Score
		{ -0.018f, -0.0995f, 0.95f, 17400, 32000, -23549 },      // Aeon
		{ -0.025f, -0.08f, 0.88f, 15400, 32000, -22800 },        // Dural
	};
	constexpr uint32_t CAMERA = 0xfd278b;
	if (CAMERA + 18 > rom.size()) return;
	static uint8_t akira_cam[18];
	static bool cam_kept = false;
	if (!cam_kept) { memcpy(akira_cam, rom_pristine(CAMERA), sizeof akira_cam); cam_kept = true; }
	which = std::clamp(which, 0, int(std::size(UNUSED_CHARACTERS)) - 1);
	if (which == m_akira_cam_now && which) return;
	m_akira_cam_now = which;
	memcpy(&rom[CAMERA], akira_cam, sizeof akira_cam);
	if (which) {
		const cam &c = CAMS[which];
		const float f[3] = { c.x, c.y, c.z };
		const int16_t h[3] = { c.a, c.b, c.c };
		memcpy(&rom[CAMERA], f, sizeof f);       // (little-endian, as VF's)
		memcpy(&rom[CAMERA + 12], h, sizeof h);
	}
}

// Which camera a random or secret fighter's portrait in Akira's spot wants (akira_camera): its
// head's character's (Siba's, Jeff's, Takeru's, a portrait made of Dural's or a Koskol's head),
// else Akira's (the main cast's heads sit as his)
int model1::akira_camera_for(uint32_t rec)
{
	auto &rom = m_regions.maincpu;
	auto rd = [&](uint32_t a) { return uint32_t(rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | uint32_t(rom[a + 3]) << 24); };
	if (auto it = m_akira_made.find(rec); it != m_akira_made.end() && it->second.first == 3) return int(it->second.second);
	if (!m_random_names.count(rec)) return 0;
	const uint32_t head = rd(rec + 0x72);
	if ((head >> 16) == 0xf8) return 7;   // (Dural's: the only single-model head a random fighter wears)
	random_cast &c = random_fighter_cast();
	for (const auto &[r, b] : c.orig) {
		if (uint32_t(b[0x72] | b[0x73] << 8 | b[0x74] << 16 | uint32_t(b[0x75]) << 24) != head) continue;
		const std::string n = record_name(r);
		if (n == "SIBA") return 1;
		if (n == "JEFF") return 2;
		if (n == "TAKERU") return 3;
		return 0;
	}
	return 0;
}

// A select portrait for the characters without one (Dural and the Koskol brothers), made from
// their own head. A record's +72 is its portrait: 9 parts (lists of objects: the face, the eyes and
// mouth with their frames), at +24 the face's animations by number (ff06ac), at +3c the select's
// blinking. The fight draws a head from it too (ff06ff), or without one the record's +16, a single
// object, as those characters have. So the portrait here is that object alone as its first part
// (a list of it, whatever frame is asked for), no animations, Akira's blinking for the select's
// (it only moves parts it hasn't): in the fight the same head as before, on the select their face.
// It goes in the main CPU's empty f80000-fbffff (no ROM there, nothing reads it). Only the
// character in Akira's place has one; the rest stay as the game made them.
void model1::akira_slot_heads(uint32_t record)
{
	static const uint32_t NO_PORTRAIT[] = { 0xfd369b, 0xfd510f, 0xfd5285, 0xfd53fb };   // Dural, Rekka, Score, Aeon
	constexpr uint32_t AT = 0xf80000, LIST = AT + 0x80, BLINK_OF = 0xfd3f6f;   // (Akira's record)
	auto &rom = m_regions.maincpu;
	if (LIST + 0x80 > rom.size()) return;
	auto rd = [&](uint32_t a) { return uint32_t(rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | rom[a + 3] << 24); };
	auto wr = [&](uint32_t a, uint32_t v) { for (int b = 0; b < 4; b++) rom[a + b] = uint8_t(v >> (8 * b)); };
	static bool kept = false, ours[std::size(NO_PORTRAIT)];
	if (!kept) {
		for (size_t i = 0; i < std::size(NO_PORTRAIT); i++) { const uint8_t *q = rom_pristine(NO_PORTRAIT[i] + 0x72); ours[i] = (q[0] | q[1] | q[2] | q[3]) == 0; }   // (VF's: none)
		kept = true;
	}
	const uint32_t blink = rd(rd(BLINK_OF + 0x72) + 0x3c);
	bool made = false;
	for (size_t i = 0; i < std::size(NO_PORTRAIT); i++) {
		if (!ours[i] || m_random_names.count(NO_PORTRAIT[i])) continue;   // (a random fighter made there: its own head)
		const bool here = NO_PORTRAIT[i] == record && blink >> 16 == 0xfd;
		wr(NO_PORTRAIT[i] + 0x72, here ? AT : 0);
		if (!here) continue;
		std::fill(&rom[AT], &rom[LIST + 0x80], 0);
		wr(AT, LIST);                                     // the first part: the head
		wr(AT + 0x3c, blink);
		const uint32_t head = rd(record + 0x16);
		for (uint32_t k = 0; k < 0x80; k += 4) wr(LIST + k, head);   // (every frame the same)
		made = true;
	}
	if (!made) std::fill(&rom[AT], &rom[LIST + 0x80], 0xff);   // (as it was: no ROM)
}

// Cheats' random fighter. A fighter's record (0x176 bytes) holds its body parts' models: torso
// +12, hips +32, the near arm +1a/+1e, the far one +26/+2a, the front leg +36/+3a/+3e, the back
// one +42/+46/+4a, the hands' poses +5e, +62 and +76 on (64 of them); and at +72 the list of its
// head's models (the blinks), which the select's portrait draws too. The cast share a skeleton,
// so any part sits on any body: the animation stays Akira's (or his place's), the parts are
// anyone's. Akira's records: fd4f99 (player 1's costume) and fd3f6f (player 2's).
void model1::set_random_fighter(bool on, bool moves, bool cpu)
{
	m_random_fighter = on;
	m_random_moves = on && moves;
	m_random_cpu = on && cpu;
	m_select_seen = false;
	random_fighter_roll();
	// (off: the heads' turns, colours and neck fits and the poses' easing go with it, not left on
	// the models the last random fighters wore, the cast's own heads)
	random_fighter_heads();
	m_pose_off[0] = m_pose_off[1] = pose_offset{};
	m_neck[0] = m_neck[1] = neck_fit{};
	for (int pl = 0; pl < 2; pl++) { m_build[pl] = fighter_build{}; m_build_models[pl].clear(); m_build_prev[pl] = build_prev{}; }
	m_build_select = false;
	// (all off: what was made in the main CPU's empty f80000-fbffff (moves tables, specials, the
	// portraits' code and heads, the strands and stances lists) cleared back to the 0xff it was,
	// nothing pointing at it any more: the ROM as VF's, byte for byte)
	auto &rom = m_regions.maincpu;
	if (!on && !m_strands_moved && rom.size() >= 0xfc0000) std::fill(rom.begin() + 0xf80000, rom.begin() + 0xfc0000, uint8_t(0xff));
	// (and Akira's stage music, random_fighter_ids' computer's character's: VF's own back)
	if (!on && m_akira_track >= 0 && 0xfd04f8 + 4 * 11 < rom.size()) rom[0xfd04f8 + 4 * 11] = uint8_t(m_akira_track);
}

void model1::random_fighter_forget()
{
	m_random_names.clear(); m_secret_names.clear();
	m_random_hue.clear(); m_random_outfit.clear(); m_random_theme.clear();
	m_stance_from.clear(); m_special_list.clear(); m_voice_of.clear(); m_move_ids.clear();
	m_akira_made.clear(); m_moves_held.clear(); m_portrait_strand_models.clear(); m_strand_roots.clear();
	m_fought.clear(); m_built_run.clear(); m_cpu_akira.clear();
	for (int pl = 0; pl < 2; pl++) { m_akira_held[pl] = m_random_seen[pl] = m_cpu_human[pl] = 0; m_build[pl] = fighter_build{}; }
	m_pick_at = -1;
	m_select_seen = m_cpu_pending = m_rolling_at_select = m_build_select = m_run_on = false;
	m_run_idle = 0;
	m_random_applied = false;
	m_look_share = 1;
	m_look_same_body = false;
}

// (the special commands' lists by character, fdb820, and ours: see random_fighter_make)
static constexpr uint32_t SPECIALS = 0xfdb820, SPECIALS_OURS = 0xf80c00, SPECIALS_LOOKUP = 0xfeab54;
// VF's moves code tells some characters' commands apart by the record's character number (+0c,
// mov.b C[-50[R19]], R0 then cmp.b #n, R0): up-back and kick (command 148, a backflip kick) taken
// by all but Akira, Jeffry and Wolf (theirs command 48), Kage's own three by the distance, and
// Jeffry's punch, kick and guard (his move 10b, his command 129) and a special command of his. A
// random fighter wears Akira's in a match (random_fighter_ids): none had the backflip kick. Ours:
// by the record's flags byte (its key), the character's number whose command it has, in a table of
// each place's (MOVEID_OURS, 256 bytes), a few instructions in the ROM's empty space (MOVEID_CODE)
// called in place of the two
struct move_site { uint32_t at; uint8_t is; uint32_t command; bool special; };   // (special: the command's in the special commands' list)
static constexpr int MOVE_SITE_COUNT = 6;
static constexpr move_site MOVE_SITES[MOVE_SITE_COUNT] = {
	{ 0xfe9d24, 0x0b, 148 },   // (up-back and kick: Akira's, Jeffry's and Wolf's command 48 instead)
	{ 0xfe9da3, 0x02, 130 },   // (Kage's by the distance, in place of command 130's)
	{ 0xfe9e6b, 0x02, 158 },   // (and of 158's)
	{ 0xfea423, 0x02, 127 },   // (Kage's command 3 where 127 has none)
	{ 0xfe9255, 0x08, 129 },   // (Jeffry's punch, kick and guard: his move 10b, his command 129)
	{ 0xfe94f3, 0x08, 34, true } };   // (Jeffry's special command 34, as his own: not taken in some states)
static constexpr uint32_t MOVEID_OURS = 0xf90000, MOVEID_CODE = 0xf90800;

// The records random fighters are made from and into: every record the slots point at, Akira's
// two (fd4f99 player 1's costume, fd3f6f player 2's) and the Koskol brothers'. Their own data is
// kept: the parts always come from the cast as they are, and Off puts them all back.
model1::random_cast &model1::random_fighter_cast()
{
	static constexpr uint32_t SLOTS = 0xfd2e8f, KOSKOLS[3] = { 0xfd510f, 0xfd5285, 0xfd53fb };
	random_cast &c = m_random_cast;
	if (c.made) return c;
	c.made = true;
	auto &rom = m_regions.maincpu;
	if (SLOTS + 26 * 4 > rom.size()) return c;
	auto rd = [&](uint32_t a) { return uint32_t(rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | uint32_t(rom[a + 3]) << 24); };
	auto model = [&](uint32_t r, uint32_t o) { return (rd(r + o) >> 16) == 0x2d; };
	auto add = [&](uint32_t r) {
		if ((r >> 16) != 0xfd || r + RECORD_SIZE > rom.size() || c.orig.count(r)) return;
		if (!model(r, 0x12) || !model(r, 0x1a) || !model(r, 0x36)) return;
		c.orig[r].assign(&rom[r], &rom[r] + RECORD_SIZE);
		c.recs.push_back(r);
		if ((rd(r + 0x72) >> 16) == 0xfd) c.heads.push_back(r);
		const uint32_t t = rd(r + 4);
		if ((t >> 16) == 0xfc && t + MOVE_COUNT * 4 <= rom.size() && std::find(c.tables.begin(), c.tables.end(), t) == c.tables.end()) c.tables.push_back(t);
	};
	// Akira's two by name: the slots may hold a secret character in his place just now (Cheats'
	// Secret character, set before Random fighter was turned on), and this list is made once
	add(0xfd4f99);
	add(0xfd3f6f);
	for (int i = 0; i < 26; i++) add(rd(SLOTS + 4 * i));
	for (uint32_t r : KOSKOLS) add(r);
	for (uint32_t r : c.recs) c.by_name[record_name(r)].push_back(r);
	if (getenv("VF_CASTLOG"))   // TESTONLY: the cast's records, their names, heads and moves tables
		for (uint32_t r : c.recs) fprintf(stderr, "CAST %06x %s head %06x moves %06x\n", r, record_name(r), rd(r + 0x72), rd(r + 4));
	if (getenv("VF_PARTLOG")) {   // TESTONLY: each record's parts' polygon counts
		const float *pd = (const float *)m_poly_rom;
		auto polys = [&](uint32_t m) {
			if ((m >> 16) != 0x2d || m + 12 > rom.size()) return -1;
			const uint32_t pa = rd(m + 8) & 0x7fffff;
			int n = 0;
			for (uint32_t a = pa + 6; n < 4096 && (*reinterpret_cast<const uint32_t *>(pd + a) & 3); a += 10) n++;
			return n;
		};
		for (uint32_t r : c.recs) {
			fprintf(stderr, "REC %06x %-7s", r, record_name(r));
			for (uint32_t o : { 0x12u, 0x32u, 0x1au, 0x26u, 0x1eu, 0x2au, 0x36u, 0x42u, 0x3au, 0x46u, 0x3eu, 0x4au }) fprintf(stderr, " %02x:%d", o, polys(rd(r + o)));
			fprintf(stderr, "\n");
		}
	}
	// the moves tables' commands in groups: runs between commands no one has (a combo's steps and
	// a move's variants sit together)
	for (uint32_t k = 0, first = 0; k <= MOVE_COUNT; k++) {
		bool none = k == MOVE_COUNT;
		if (!none) {
			none = true;
			for (uint32_t t : c.tables) none = none && rd(t + 4 * k) == 0;
		}
		if (none) { if (k > first) c.move_groups.push_back({ first, k }); first = k + 1; }
	}
	// each move's character's head lean (random_fighter_heads), from the moves tables and special
	// commands each character's records have: NaN for a move characters of different leans share
	for (uint32_t r : c.recs) {
		const float lean = head_lean(r);
		auto note = [&](uint32_t m) {
			if (!(m &= 0xffff)) return;
			auto [it, fresh] = c.move_lean.emplace(uint16_t(m), lean);
			if (!fresh && it->second != lean) it->second = NAN;
		};
		const uint32_t t = rd(r + 4);
		if ((t >> 16) == 0xfc && t + MOVE_COUNT * 4 <= rom.size())
			for (uint32_t k = 0; k < MOVE_COUNT; k++) note(rd(t + 4 * k));
		if (const uint8_t id = rom[r + 0x0c]; id < 12)
			for (uint32_t k = 0, l = rd(SPECIALS + 4 * id); k < 50 && l + 7 * k + 4 <= rom.size(); k++)
				if (rd(l + 7 * k) != 0xffffffff) note(rd(l + 7 * k));
	}
	// each record's key for its special commands (random_fighter_specials): in its flags byte's
	// bits the game doesn't look at (it tests 1 and 2 only, ffd35a and ff882d; the rest only
	// copied), one a record: bit 0 and bits 3-7
	for (size_t k = 0; k < c.recs.size() && k < 64; k++) {
		const uint32_t r = c.recs[k];
		c.flags[r] = uint8_t((rom[r + 0x0d] & 0x06) | (k & 1) | (k >> 1) << 3);
	}
	return c;
}

uint32_t model1::random_next(uint32_t n)
{
	uint32_t &x = m_random_state;   // xorshift, seeded from the frame and the clock
	if (!x && getenv("VF_SEED")) x = uint32_t(atoi(getenv("VF_SEED")));   // TESTONLY
	if (!x) x = uint32_t(m_frame * 2654435761u) ^ uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x9e3779b9u;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	return n ? x % n : 0;
}

// One record made a random fighter: each group of parts (a pair of limbs as one) from a random
// fighter's own, a random head (+72, the select's portrait too), and with random moves a moves
// table of its own (in the ROM's empty space, f81000 on: 0x300 bytes a record), each command's
// entry from a random fighter's table. The moves are numbered alike for everyone (Siba's are
// mostly Lau's) and the cast share a skeleton, so any part and move fits anyone.
// The special commands, by character (record +0c): fdb820, a list's address each; looked up by
// fea54 on (movz.bw C[-50[R19]], R3; movea.w FDB820(R3), R3). Ours, by a key in each record's
// flags byte (+0d) instead, so each random fighter has its own: random_fighter_specials.

void model1::random_fighter_make(uint32_t rec)
{
	random_cast &c = random_fighter_cast();
	auto &rom = m_regions.maincpu;
	// Made on Akira's record as it is (player 2's on his own, his second costume), whichever record
	// it goes in: a seed the same fighter as the computer's as a player's. On the record's own (the
	// computer's, Pai's), the parts its layout has, its name, its size, weight and character number
	// (VF's own code for that character's moves (fe6711), its voice) were that record's character's
	const uint32_t base = rec == 0xfd3f6f ? rec : 0xfd4f99;
	if (!c.orig.count(rec) || !c.orig.count(base) || c.recs.empty() || c.heads.empty()) return;
	auto orig = [&](uint32_t r, uint32_t o) { const auto &b = c.orig.at(r); return uint32_t(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24); };
	auto wr = [&](uint32_t a, uint32_t v) { for (int b = 0; b < 4; b++) rom[a + b] = uint8_t(v >> (8 * b)); };
	auto model = [&](uint32_t r, uint32_t o) { return (orig(r, o) >> 16) == 0x2d; };
	auto rd32 = [&](uint32_t a) { return uint32_t(rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | uint32_t(rom[a + 3]) << 24); };
	memcpy(&rom[rec], c.orig.at(base).data(), RECORD_SIZE);
	for (uint32_t t : { 0x5eu, 0x62u })   // (its hands' tables, in the record: pointed at in this one)
		if (const uint32_t p = orig(base, t); p >= base && p < base + RECORD_SIZE) wr(rec + t, p - base + rec);
	rom[rec + 0x0d] = c.orig.at(rec)[0x0d];   // (its flags byte its own: random_fighter_specials)
	static const std::vector<std::vector<uint32_t>> GROUPS = { { 0x12 }, { 0x32 }, { 0x1a, 0x26 }, { 0x1e, 0x2a }, { 0x36, 0x42 }, { 0x3a, 0x46 }, { 0x3e, 0x4a } };
	// The three it's made of: three of the eight in the game's own select (their moves, and its
	// name, theirs blended), the first's head, stance and special commands leading. Its looks are
	// mostly theirs, now and then anyone's (the cut characters and Dural too: their looks, never
	// their moves)
	static const char *const MAIN[] = { "AKIRA", "PAI", "LAU", "WOLF", "JEFFRY", "KAGE", "SARAH", "JACKY" };
	std::vector<std::string> main, all;
	for (const auto &[n, rs] : c.by_name) {
		all.push_back(n);
		if (std::find_if(std::begin(MAIN), std::end(MAIN), [&](const char *m) { return n == m; }) != std::end(MAIN)) main.push_back(n);
	}
	if (main.size() < 3) return;
	std::string three[3];
	for (int i = 0; i < 3; i++)
		do three[i] = main[random_next(uint32_t(main.size()))]; while ((i > 0 && three[i] == three[0]) || (i == 2 && three[2] == three[1]));
	// One fighter in three a mismatch (VF_RANDOM_FUNNY: every one, testing): a small head on a big
	// upper body or a big head on a small one. The body's character first: its stance's skeleton holds
	// its own torso and arms (a small one's narrow shoulders hid a big torso's upper arms inside its
	// chest); the head the second's
	static const bool all_funny = getenv("VF_RANDOM_FUNNY") != nullptr;   // TESTONLY
	const bool funny = all_funny || random_next(3) == 0;
	if (funny) {
		static const char *const SMALL[] = { "PAI", "SARAH" }, *const BIG[] = { "WOLF", "JEFFRY" };
		const std::string sm = SMALL[random_next(2)], bg = BIG[random_next(2)];
		const bool small_head = random_next(2) == 0;
		three[0] = small_head ? bg : sm;
		three[1] = small_head ? sm : bg;
		do three[2] = main[random_next(uint32_t(main.size()))]; while (three[2] == three[0] || three[2] == three[1]);
	}
	auto costume = [&](int who) { const auto &rs = c.by_name[three[who]]; return rs[random_next(uint32_t(rs.size()))]; };
	auto anyones = [&] { const auto &rs = c.by_name[all[random_next(uint32_t(all.size()))]]; return rs[random_next(uint32_t(rs.size()))]; };
	// who gives what: each of the three at least once
	auto deal = [&](std::vector<int> &owner, size_t from) {
		const auto b = owner.begin() + long(from);
		for (auto i = b; i != owner.end(); ++i) *i = int(random_next(3));
		if (owner.end() - b < 3) return;
		for (int who = 0; who < 3; who++)
			if (std::find(b, owner.end(), who) == owner.end()) {   // (in place of one given more than once)
				auto i = b;
				do i = b + long(random_next(uint32_t(owner.end() - b))); while (std::count(b, owner.end(), *i) < 2);
				*i = who;
			}
	};
	// (for its name: the head counts 4, the torso 2, every other part and group of moves 1)
	std::map<std::string, int> made_of;
	static const int WEIGHT[] = { 2, 1, 1, 1, 1, 1, 1 };
	std::vector<int> part_of(GROUPS.size());
	deal(part_of, 0);
	// (the torso and arms the body's. Otherwise too, a big torso (Wolf's, Jeffry's) on a small
	// one's skeleton (Pai's, Sarah's), or a small one on a big, comes with its arms from the skeleton's
	// own character: the upper arms were inside the chest, or out in the air)
	auto size_of = [](const std::string &n) { return n == "WOLF" || n == "JEFFRY" ? 2 : n == "PAI" || n == "SARAH" ? 1 : 0; };
	if (funny || (size_of(three[part_of[0]]) && size_of(three[0]) && size_of(three[part_of[0]]) != size_of(three[0])))
		part_of[0] = part_of[2] = part_of[3] = 0;
	std::string log_torso, log_head;   // TESTONLY (VF_RANDLOG)
	// (its looks, by whose they are: the face counts 4, the torso 3, the hips 2, each other part and
	// the hands 1; a part a source hasn't left as Akira's record has it, Akira's: random_fighter_look)
	std::map<std::string, float> look;
	static const float LOOK_WEIGHT[] = { 3, 2, 1, 1, 1, 1, 1 };
	uint32_t torso_src = 0;
	for (size_t gi = 0; gi < GROUPS.size(); gi++) {
		bool other = random_next(4) == 0;   // (one part in four anyone's)
		if (funny && (gi == 0 || gi == 2 || gi == 3)) other = false;
		uint32_t src = other ? anyones() : costume(part_of[gi]);
		if (gi == 0) torso_src = src;
		bool used = false;
		for (uint32_t o : GROUPS[gi])
			if (model(src, o) && model(base, o)) { wr(rec + o, orig(src, o)); used = true; }
		if (used && !other) made_of[three[part_of[gi]]] += WEIGHT[gi];
		if (gi == 0) log_torso = record_name(src);
		look[used ? record_name(src) : record_name(base)] += LOOK_WEIGHT[gi];
		if (static const bool pl = getenv("VF_PARTLOG") != nullptr; pl)   // TESTONLY
			fprintf(stderr, "PART %06x group %zu from %06x %s%s:", rec, gi, src, record_name(src), used ? "" : " (not used)"),
			[&] {
				auto polys = [&](uint32_t m) {   // (a model's polygons, from the poly ROM)
					if ((m >> 16) != 0x2d || m + 12 > rom.size()) return -1;
					const uint32_t pa = rd32(m + 8) & 0x7fffff;
					const float *pd = (const float *)m_poly_rom;
					int n = 0;
					for (uint32_t a = pa + 6; n < 4096 && (*reinterpret_cast<const uint32_t *>(pd + a) & 3); a += 10) n++;
					return n;
				};
				for (uint32_t o : GROUPS[gi]) fprintf(stderr, " +%02x %08x (%d polys)", o, orig(src, o), polys(orig(src, o)));
				fprintf(stderr, "\n");
			}();
	}
	// The hands: two tables of 32 poses in the record, where +5e and +62 point (+76 and +f6 for
	// most; +7e and +fe for Pai, Kage and Jeff, whose +76 lists two models of their own, +56): pose
	// by pose from one fighter's, each table found by its pointer (by offset, the poses came two
	// off and those two models went: hands gone or wrong). (Pai's sort's second table runs past the
	// record's end: its last two poses left as they are.)
	{
		const uint32_t src = costume(int(random_next(3)));
		look[record_name(src)] += 1;
		for (uint32_t t : { 0x5eu, 0x62u }) {
			const uint32_t so = orig(src, t) - src, ro = orig(base, t) - base;
			if (so >= RECORD_SIZE || ro >= RECORD_SIZE) continue;
			for (uint32_t k = 0; k < 32; k++) {
				const uint32_t s4 = so + 4 * k, r4 = ro + 4 * k;
				if (s4 + 4 > RECORD_SIZE || r4 + 4 > RECORD_SIZE) break;
				if (model(src, s4) && model(base, r4)) wr(rec + r4, orig(src, s4));
			}
		}
	}
	if ((orig(base, 0x72) >> 16) == 0xfd) {
		uint32_t h;
		const bool other = !funny && random_next(5) == 0;   // (one head in five anyone's with one)
		int tries = 0;
		do h = other ? anyones() : costume(funny ? 1 : 0); while (++tries < 100 && std::find(c.heads.begin(), c.heads.end(), h) == c.heads.end());
		if (std::find(c.heads.begin(), c.heads.end(), h) == c.heads.end()) do h = costume(funny ? 1 : 0); while (std::find(c.heads.begin(), c.heads.end(), h) == c.heads.end());
		// (the cut characters' heads, Siba's, Jeff's and Takeru's, about as often as Dural's: one in
		// twelve; picked among everyone's they came one in a hundred)
		if (random_next(12) == 0) {
			std::vector<uint32_t> cut;
			for (const char *n : { "SIBA", "JEFF", "TAKERU" })
				if (auto it = c.by_name.find(n); it != c.by_name.end())
					for (uint32_t r : it->second)
						if (std::find(c.heads.begin(), c.heads.end(), r) != c.heads.end()) cut.push_back(r);
			if (!cut.empty()) h = cut[random_next(uint32_t(cut.size()))];
		}
		wr(rec + 0x72, orig(h, 0x72));
		wr(rec + 0x16, orig(base, 0x16));
		log_head = record_name(h);
		// (Dural's head, one in five of anyone's: she has no head list (+72 none), her head the model
		// at +16, which the game draws as the head when there's no list)
		constexpr uint32_t DURAL_HEAD = 0xfd369b;   // (Dural's record)
		if ((funny ? random_next(6) == 0 : other && random_next(5) == 0) && c.orig.count(DURAL_HEAD) && model(DURAL_HEAD, 0x16) && model(base, 0x16)) {
			wr(rec + 0x72, 0);
			wr(rec + 0x16, orig(DURAL_HEAD, 0x16));
			log_head = "DURAL";
			akira_portrait_of_head(rec, orig(DURAL_HEAD, 0x16));   // (a player's: a select portrait of it, else none)
		}
		made_of[three[funny ? 1 : 0]] += 4;
		look[log_head] += 4;
	}
	if (const char *fh = getenv("VF_FORCE_HEAD")) {   // TESTONLY (the head, the rest made as ever; "dural": hers)
		if (!strcmp(fh, "dural")) { wr(rec + 0x72, 0); wr(rec + 0x16, orig(0xfd369b, 0x16)); }
		else wr(rec + 0x72, uint32_t(strtoul(fh, nullptr, 16)));
	}
	m_random_hue[rec] = float(random_next(1000)) / 1000.0f;
	m_voice_of[rec] = c.orig.at(c.by_name[three[0]].front())[0x0c];   // (the first of the three's voice: random_fighter_specials)
	// Its moves: the moves table (+4, one move a command, numbered alike for everyone) of its own
	// (in the ROM's empty space, f81000 on: 0x300 bytes a record), each group of commands (a combo's
	// steps together) from one of the three, the first's its stance and basics; and its special
	// commands (by character, fdb820: 50 of 7 bytes, a move and its conditions; Akira's forward,
	// forward and punch among them), each from one of the three that has it (f88000 on: 0x160
	// a record; random_fighter_specials).
	const uint32_t index = uint32_t(std::find(c.recs.begin(), c.recs.end(), rec) - c.recs.begin());
	const uint32_t at = 0xf81000 + 0x300 * index, list = 0xf88000 + 0x160 * index;
	if (m_random_moves && !c.move_groups.empty() && list + 0x160 <= 0xfc0000 && list + 0x160 <= rom.size()) {
		// the groups whose moves differ between characters (most are everyone's): dealt round the three
		// in turn, in a random order, so each one's style comes through as much as the others'
		std::vector<int> move_of(c.move_groups.size(), 0);
		std::vector<size_t> differ;
		for (size_t gi = 1; gi < c.move_groups.size(); gi++) {
			bool d = false;
			for (uint32_t k = c.move_groups[gi].first; k < c.move_groups[gi].second && !d; k++)
				for (uint32_t t : c.tables) d = d || rd32(t + 4 * k) != rd32(c.tables[0] + 4 * k);
			if (d) differ.push_back(gi); else move_of[gi] = int(random_next(3));
		}
		for (size_t i = differ.size(); i > 1; i--) std::swap(differ[i - 1], differ[random_next(uint32_t(i))]);
		const int first = int(random_next(3));
		for (size_t i = 0; i < differ.size(); i++) move_of[differ[i]] = int((first + i) % 3);
		// Every command filled (a gap: a command its giver hasn't, though another has; nothing came
		// out): a group from one of the three whose version has no gaps (the one dealt it, if it's
		// one of them), else the one with the fewest; its gaps from another of the three that has
		// the command, else from anyone's (a throw's command always a throw)
		uint32_t table[3];
		for (int who = 0; who < 3; who++) table[who] = orig(costume(who), 4);
		auto usable = [&](uint32_t t) { return (t >> 16) == 0xfc && t + MOVE_COUNT * 4 <= rom.size(); };
		auto gaps = [&](uint32_t t, const std::pair<uint32_t, uint32_t> &g) {
			int n = 0;
			for (uint32_t k = g.first; k < g.second; k++) n += rd32(t + 4 * k) == 0;
			return n;
		};
		// (whose command each of the commands VF checks by character is: MOVE_SITES. One none of the
		// three has, a gap filled from anyone's, is the group's giver's: an Akira, Jeffry and Wolf's
		// up-back and kick theirs, not another's backflip kick)
		std::array<uint8_t, MOVE_SITE_COUNT> ids;
		ids.fill(0xff);
		uint8_t table_id[3] = { 0x0b, 0x0b, 0x0b };
		for (int who = 0; who < 3; who++)
			for (const auto &[r, b] : c.orig)
				if (uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24) == table[who]) { table_id[who] = b[0x0c]; break; }
		auto put = [&](uint32_t k, uint32_t t) {
			memcpy(&rom[at + 4 * k], &rom[t + 4 * k], 4);
			for (int i = 0; i < MOVE_SITE_COUNT; i++)
				if (!MOVE_SITES[i].special && k == MOVE_SITES[i].command) {
					ids[i] = 0xfe;   // (not the three's: the group's giver's, below)
					for (int who = 0; who < 3; who++) if (t == table[who]) ids[i] = table_id[who];
				}
			// (Siba's pounce, move 259, is a placeholder that flies its fighter across the ring:
			// Lau's, 381, as set_akira_slot gives Siba himself)
			if (rom[at + 4 * k] == 0x03 && rom[at + 4 * k + 1] == 0x01) rom[at + 4 * k] = 0x7d;
		};
		for (size_t gi = 0; gi < c.move_groups.size(); gi++) {
			const auto &g = c.move_groups[gi];
			int best = -1, best_gaps = 1 << 30;
			for (int i = 0; i < 3; i++) {
				const int who = (move_of[gi] + i) % 3;   // (the dealt one first)
				if (!usable(table[who])) continue;
				const int n = gaps(table[who], g);
				if (n < best_gaps) { best = who; best_gaps = n; }
			}
			if (best < 0) continue;
			// (a parent's entry for a command that only repeats a move its own table gives another
			// command, outside this group, is its stand-in for not having one: Akira's, Wolf's and
			// Jeffry's back jump for a tap of up-back, where the others backflip. Another of the three's
			// own move for it taken instead, the first in the dealt order: no draw, seeds the same)
			auto stand_in = [&](uint32_t t, uint32_t k) {
				const uint32_t v = rd32(t + 4 * k);
				if (!v) return true;
				for (uint32_t j = 0; j < MOVE_COUNT; j++)
					if ((j < g.first || j >= g.second) && rd32(t + 4 * j) == v) return true;
				return false;
			};
			for (uint32_t k = g.first; k < g.second; k++) {
				put(k, table[best]);
				if (rd32(at + 4 * k) && stand_in(table[best], k))
					for (int i = 1; i < 3; i++) {
						const int who = (best + i) % 3;
						if (usable(table[who]) && !stand_in(table[who], k)) { put(k, table[who]); break; }
					}
				if (rd32(at + 4 * k)) continue;
				uint32_t have[32];
				int n = 0;
				for (int who = 0; who < 3; who++)
					if (usable(table[who]) && rd32(table[who] + 4 * k)) have[n++] = table[who];
				if (!n)
					for (uint32_t t : c.tables)
						if (n < 32 && rd32(t + 4 * k)) have[n++] = t;
				// (a command over half the cast leave empty stays empty, as theirs: a few characters' own
				// extra, tied to their own moves in the group. A punch during the kick (136) is Lau's
				// punch and Pai's crouching punch, everyone else's an ordinary punch; filled from Pai's,
				// a random fighter crouched for a punch after any kick. Drawn all the same: the seeds'
				// other moves as before)
				int owners = 0;
				for (uint32_t t : c.tables) owners += rd32(t + 4 * k) != 0;
				if (n) { const uint32_t pick = have[random_next(uint32_t(n))]; if (owners * 2 >= int(c.tables.size())) put(k, pick); }
			}
			for (int i = 0; i < MOVE_SITE_COUNT; i++)
				if (!MOVE_SITES[i].special && ids[i] == 0xfe && MOVE_SITES[i].command >= g.first && MOVE_SITES[i].command < g.second) ids[i] = table_id[best];
			made_of[three[best]] += 1;
		}
		for (uint32_t k = 0; k < MOVE_COUNT; k++) {   // (commands no one has: none)
			bool in = false;
			for (const auto &g : c.move_groups) in = in || (k >= g.first && k < g.second);
			if (!in) memset(&rom[at + 4 * k], 0, 4);
		}
		wr(rec + 4, at);
		// (whose stance it is: command 0, the stance, from which of the three)
		for (int who = 0; who < 3; who++)
			if (usable(table[who]) && rd32(table[who]) == rd32(at)) { m_stance_from[rec] = table[who]; break; }
		uint32_t lists[3];
		uint8_t list_id[3];
		for (int who = 0; who < 3; who++) {
			// (Siba's list is empty, his moves Lau's; Jeff's and Takeru's Jacky's)
			uint8_t id = c.orig.at(costume(who))[0x0c];
			id = id == 3 ? 0 : id == 9 || id == 10 ? 1 : id;
			lists[who] = rd32(SPECIALS + 4 * (id < 12 ? id : 9));
			list_id[who] = id;
		}
		memcpy(&rom[list], &rom[rd32(SPECIALS + 4 * 9)], 50 * 7);   // (an empty list to fill: Jeff's)
		for (uint32_t k = 1; k < 50; k++) {
			// (Dural's list, the Koskols' too, is everyone's moves, Akira's and Wolf's most: it made
			// every fighter with one of them in it an Akira and a Wolf. Not taken from)
			int have[3], n = 0;
			for (int who = 0; who < 3; who++) {
				const uint32_t v = rd32(lists[who] + 7 * k);
				if (v != 0 && v != 0xffffffff && lists[who] != rd32(SPECIALS + 4 * 5)) have[n++] = who;
			}
			if (!n) continue;
			const int who = have[random_next(uint32_t(n))];
			memcpy(&rom[list + 7 * k], &rom[lists[who] + 7 * k], 7);
			for (int i = 0; i < MOVE_SITE_COUNT; i++)
				if (MOVE_SITES[i].special && k == MOVE_SITES[i].command) ids[i] = list_id[who];
		}
		m_special_list[rec] = list;
		m_move_ids[rec] = ids;
	}
	m_random_names[rec] = blend_name(made_of);
	{   // (its looks: the most of them one character's, and whether its face and torso are one's)
		float total = 0, most = 0; std::string top;
		for (const auto &[n, w] : look) { total += w; if (w > most) { most = w; top = n; } }
		m_look_share = total > 0 ? most / total : 1;
		m_look_same_body = !log_head.empty() && log_head == log_torso;
		if (getenv("VF_LOOKLOG")) fprintf(stderr, "LOOK %s %.2f %s head %s torso %s\n", m_random_names[rec].c_str(), m_look_share, top.c_str(), log_head.c_str(), log_torso.c_str());   // TESTONLY
	}
	if (getenv("VF_RANDLOG")) {   // TESTONLY
		int empty = 0;
		if (m_random_moves)
			for (const auto &g : c.move_groups)
				for (uint32_t k = g.first; k < g.second; k++) empty += rd32(at + 4 * k) == 0;
		fprintf(stderr, "MADE %06x %s+%s+%s %s, %d commands empty, head %s torso %s\n", rec, three[0].c_str(), three[1].c_str(), three[2].c_str(), m_random_names[rec].c_str(), empty, log_head.c_str(), log_torso.c_str());
		fprintf(stderr, "MADEOF %06x", rec);
		for (const auto &[n, k] : made_of) fprintf(stderr, " %s:%d", n.c_str(), k);
		fprintf(stderr, "\n");
	}
}

// All of the cast as they are, then Akira's two made random (Akira's place), and with random
// opponents every other record too (the computer's next one is made afresh as it comes in:
// random_fighter_frame).
void model1::random_fighter_roll()
{
	static constexpr uint32_t AKIRA[2] = { 0xfd4f99, 0xfd3f6f };
	random_cast &c = random_fighter_cast();
	auto &rom = m_regions.maincpu;
	// Akira's two records, each side's (akira_record_of): made by that player's setting, or Akira's
	// own. One a player has now (picked at an earlier select: a
	// challenger's select doesn't pick again) is kept as it is
	struct kept { uint32_t rec; std::vector<uint8_t> bytes; std::pair<int, uint32_t> made; std::string name; float hue, outfit; int theme; uint32_t stance, list; bool has_hue, has_outfit, has_stance, has_list; int voice; std::array<uint8_t, MOVE_SITE_COUNT> move_ids; bool has_move_ids; };
	std::vector<kept> keep;
	if (m_random_applied && m_random_fighter)
		for (int pl = 0; pl < 2; pl++) {
			const uint32_t r = akira_record_of(pl);
			const auto made = m_akira_made.find(r);
			if (!r || !picked_now(pl) || m_ram_a[0x6b5 + 0x1800 * pl] % 9 || made == m_akira_made.end()
				|| made->second != std::pair<int, uint32_t>(m_player_mode[pl], m_player_mode[pl] >= 2 ? m_player_seed[pl] : 0)) continue;
			keep.push_back({ r, std::vector<uint8_t>(&rom[r], &rom[r] + RECORD_SIZE), made->second, m_random_names.count(r) ? m_random_names[r] : std::string(),
							 m_random_hue.count(r) ? m_random_hue[r] : 0.0f, m_random_outfit.count(r) ? m_random_outfit[r] : 0.0f, m_random_theme.count(r) ? m_random_theme[r] : 0,
							 m_stance_from.count(r) ? m_stance_from[r] : 0, m_special_list.count(r) ? m_special_list[r] : 0,
							 m_random_hue.count(r) > 0, m_random_outfit.count(r) > 0, m_stance_from.count(r) > 0, m_special_list.count(r) > 0,
							 m_voice_of.count(r) ? int(m_voice_of[r]) : -1, m_move_ids.count(r) ? m_move_ids[r] : std::array<uint8_t, MOVE_SITE_COUNT>{}, m_move_ids.count(r) > 0 });
		}
	// (Random opponents, a run's continue: the computer's fighters fought so far kept as fought, for
	// the ending's replays)
	auto keep_rec = [&](uint32_t r, std::pair<int, uint32_t> made) {
		keep.push_back({ r, std::vector<uint8_t>(&rom[r], &rom[r] + RECORD_SIZE), made, m_random_names.count(r) ? m_random_names[r] : std::string(),
						 m_random_hue.count(r) ? m_random_hue[r] : 0.0f, m_random_outfit.count(r) ? m_random_outfit[r] : 0.0f, m_random_theme.count(r) ? m_random_theme[r] : 0,
						 m_stance_from.count(r) ? m_stance_from[r] : 0, m_special_list.count(r) ? m_special_list[r] : 0,
						 m_random_hue.count(r) > 0, m_random_outfit.count(r) > 0, m_stance_from.count(r) > 0, m_special_list.count(r) > 0,
						 m_voice_of.count(r) ? int(m_voice_of[r]) : -1, m_move_ids.count(r) ? m_move_ids[r] : std::array<uint8_t, MOVE_SITE_COUNT>{}, m_move_ids.count(r) > 0 });
	};
	if (m_random_applied && m_random_fighter && m_random_cpu && m_run_on)
		for (uint32_t r : m_fought)
			if (r != AKIRA[0] && r != AKIRA[1] && c.orig.count(r) && m_random_names.count(r) && std::none_of(keep.begin(), keep.end(), [&](const kept &x) { return x.rec == r; })) {
				const auto made = m_akira_made.find(r);
				keep_rec(r, made != m_akira_made.end() ? made->second : std::pair<int, uint32_t>(0, 0));
			}
	// (and the computer's Akira as fought: put back once the picks are known, random_fighter_cpu)
	if (m_random_applied && m_random_fighter && m_random_cpu && m_run_on)
		for (uint32_t r : AKIRA)
			if (m_fought.count(r) && m_random_names.count(r) && !m_cpu_akira.count(r)) {
				cpu_kept &k = m_cpu_akira[r];
				k.bytes.assign(&rom[r], &rom[r] + RECORD_SIZE);
				k.name = m_random_names[r];
				if ((k.has_hue = m_random_hue.count(r) > 0)) k.hue = m_random_hue[r];
				if ((k.has_outfit = m_random_outfit.count(r) > 0)) { k.outfit = m_random_outfit[r]; k.theme = m_random_theme.count(r) ? m_random_theme[r] : 0; }
				if ((k.has_stance = m_stance_from.count(r) > 0)) k.stance = m_stance_from[r];
				if ((k.has_list = m_special_list.count(r) > 0)) k.list = m_special_list[r];
				if (m_voice_of.count(r)) k.voice = m_voice_of[r];
				if ((k.has_move_ids = m_move_ids.count(r) > 0)) k.move_ids = m_move_ids[r];
			}
	// (the cast as they were, if random ones were made: Off leaves alone what the Secret character
	// cheat set, as Akira's place's portrait head)
	if (m_random_applied) for (const auto &[r, b] : c.orig) memcpy(&rom[r], b.data(), RECORD_SIZE);
	m_random_applied = false;
	m_random_names.clear();   // (the records are the cast's own again)
	m_secret_names.clear();
	m_stance_from.clear();
	m_voice_of.clear();
	m_move_ids.clear();
	m_special_list.clear();
	m_moves_held.clear();
	m_akira_made.clear();
	if (!m_random_fighter) { random_fighter_strands(); random_fighter_specials(); if (m_akira_which > 0) akira_slot_heads(m_secret_recs[0]); return; }
	m_random_applied = true;
	for (const kept &k : keep) {   // (the computer's kept: as they were)
		if (k.rec == AKIRA[0] || k.rec == AKIRA[1]) continue;
		const uint32_t r = k.rec;
		memcpy(&rom[r], k.bytes.data(), RECORD_SIZE);
		if (!k.name.empty()) m_random_names[r] = k.name;
		if (k.has_hue) m_random_hue[r] = k.hue;
		if (k.has_outfit) { m_random_outfit[r] = k.outfit; m_random_theme[r] = k.theme; }
		if (k.has_stance) m_stance_from[r] = k.stance;
		if (k.voice >= 0) m_voice_of[r] = uint8_t(k.voice);
		if (k.has_move_ids) m_move_ids[r] = k.move_ids;
		if (k.has_list) m_special_list[r] = k.list;
	}
	for (uint32_t r : AKIRA) {
		const auto k = std::find_if(keep.begin(), keep.end(), [&](const kept &x) { return x.rec == r; });
		if (k != keep.end()) {
			memcpy(&rom[r], k->bytes.data(), RECORD_SIZE);
			m_akira_made[r] = k->made;
			if (!k->name.empty()) m_random_names[r] = k->name;
			if (k->made.first == 3) secret_akira_record(r, int(k->made.second));   // (its name, moves and portrait again)
			if (k->has_hue) m_random_hue[r] = k->hue;
			if (k->has_outfit) { m_random_outfit[r] = k->outfit; m_random_theme[r] = k->theme; }
			if (k->has_stance) m_stance_from[r] = k->stance;
			if (k->voice >= 0) m_voice_of[r] = uint8_t(k->voice);
			if (k->has_move_ids) m_move_ids[r] = k->move_ids;
			if (k->has_list) m_special_list[r] = k->list;
			continue;
		}
		int owner = -1;
		for (int pl = 0; pl < 2; pl++) if (akira_record_of(pl) == r) owner = pl;
		// (no one at a select: the attract's demo, a boot: by side, player 1's fd4f99, player 2's
		// fd3f6f, so the demo's Akira is a random fighter too; a select makes them again as who's in)
		const bool anyone = m_ram_a.size() > 0x7600 && ((m_ram_a[0x7483] & 0x08) || (m_ram_a[0x7503] & 0x08));
		if (owner < 0 && !anyone) { const int side = r == 0xfd4f99 ? 0 : 1; if (m_random_fighter && m_player_mode[side]) owner = side; }
		random_akira_record(r, owner < 0 ? 0 : m_player_mode[owner], owner < 0 ? 0 : m_player_seed[owner]);
	}
	// (Random opponents: the computer's made random once the select is over, not now: a player
	// picking as ever picked from these records, and took a random fighter for Lau
	// (random_fighter_cpu); at once outside a select, a restart's)
	m_cpu_pending = m_random_cpu;
	if (m_cpu_pending && !m_rolling_at_select) random_fighter_cpu();
	random_fighter_strands();
	random_fighter_specials();
	random_fighter_hold_moves();
	if (m_akira_which > 0) akira_slot_heads(m_secret_recs[0]);   // (the Secret character's portrait head: the records put back took it)
}

// A match loading (a player's block naming another record, or the select ending: every record
// then, the ones made at it and the computer's carried from the attract): a random fighter's
// models listed by group, to be built as the intro's camera sweeps in (BUILD_DELAY frames on: the
// sweep's start). None at the select itself, none in the attract
void model1::random_fighter_build(bool select)
{
	static const int delay = getenv("VF_BUILD_DELAY") ? atoi(getenv("VF_BUILD_DELAY")) : 0;   // TESTONLY
	static const bool off = getenv("VF_NO_BUILD") != nullptr;                                  // TESTONLY
	auto &rom = m_regions.maincpu;
	if (m_ram_a.size() < 0x1f00) return;
	auto rd = [&](uint32_t a) { return a + 4 <= rom.size() ? uint32_t(rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | uint32_t(rom[a + 3]) << 24) : 0; };
	auto poly_of = [&](uint32_t m) -> uint32_t { return (m >> 16) == 0x2d && m + 12 <= rom.size() ? rd(m + 8) & 0x7fffff : 0; };
	const bool select_over = m_build_select && !select;
	// (a match only: someone in (the select word's 0x0800), not the attract's demo)
	const bool playing = m_ram_a.size() > 0x7504 && ((m_ram_a[0x7483] | m_ram_a[0x7503]) & 0x08);
	m_build_select = select;
	for (int pl = 0; pl < 2; pl++) {
		fighter_build &b = m_build[pl];
		const uint32_t o = 0x6b0 + 0x1800 * uint32_t(pl);
		const uint32_t rec = uint32_t(m_ram_a[o] | m_ram_a[o + 1] << 8 | m_ram_a[o + 2] << 16 | uint32_t(m_ram_a[o + 3]) << 24);
		if (select_over) b.rec = 0;
		if (b.start >= 0 && int64_t(m_frame) - b.start > BUILD_COLOUR + 8 * BUILD_COLOUR_STEP + BUILD_COLOUR_SPAN + BUILD_FLASH) { b.start = -1; b.group.clear(); }
		if (rec == b.rec) continue;
		if (b.rec && !select_over) {   // (the last record's, as it was: build_prev; a cast one's too, its colours its own)
			build_prev &p = m_build_prev[pl];
			p.rec = b.rec;
			p.until = int64_t(m_frame) + BUILD_PREV_FRAMES;
			p.models = m_build_models[pl];
			const auto o = m_random_outfit.find(b.rec);
			p.outfit = o != m_random_outfit.end() ? o->second : -1;
			const auto t = m_random_theme.find(b.rec);
			p.theme = t != m_random_theme.end() ? t->second : 0;
			const auto tn = m_outfit_tones.find(b.rec);
			p.tones = tn != m_outfit_tones.end() ? tn->second : outfit_tones{};
			const auto sk = m_skin_ref.find(b.rec);
			p.has_skin = sk != m_skin_ref.end() && sk->second.n;
			if (p.has_skin) p.skin = sk->second;
		}
		if (getenv("VF_RANDLOG") && (rec >> 16) == 0xfd) fprintf(stderr, "BLOCK pl%d %06x %s frame %llu\n", pl, rec, m_random_names.count(rec) ? m_random_names[rec].c_str() : "-", (unsigned long long)m_frame);   // TESTONLY
		b.rec = rec;
		b.start = -1;
		b.group.clear();
		m_build_models[pl].clear();
		if ((rec >> 16) != 0xfd) continue;
		const bool build = !off && playing && m_random_names.count(rec);
		// (+0e, a few polygons at the waist (a belt's knot), with the torso: left out, it was drawn from
		// the build's first frame, a dark spot at chest height before anything else)
		static const std::vector<std::vector<uint32_t>> GROUPS = {
			{ 0x3e, 0x4a }, { 0x3a, 0x46 }, { 0x36, 0x42 }, { 0x32 }, { 0x12, 0x0e }, { 0x1a, 0x26 }, { 0x1e, 0x2a } };
		auto polys = [&](uint32_t p) {   // (a model's polygons: 10 words each after its first two points, to a type 0)
			int n = 0;
			for (uint32_t a = p + 6; n < 4096 && (a + 1) * 4 <= m_regions.polygons.size() && (m_poly_rom[a] & 3); a += 10) n++;
			return n;
		};
		auto put = [&](uint32_t p, int g) { if (!p) return; m_build_models[pl].insert(p); if (build) b.group.emplace(p, build_model{ g, polys(p) }); };
		for (size_t g = 0; g < GROUPS.size(); g++)
			for (uint32_t off_ : GROUPS[g]) put(poly_of(rd(rec + off_)), int(g));
		for (uint32_t t : { 0x5eu, 0x62u }) {   // (the hands: two tables of 32 poses)
			const uint32_t tab = rd(rec + t);
			for (uint32_t k = 0; k < 32 && (tab >> 16) == 0xfd; k++) put(poly_of(rd(tab + 4 * k)), 7);
		}
		for (uint32_t h : { 0x22u, 0x2eu, 0x76u, 0x7au, 0x7eu }) put(poly_of(rd(rec + h)), 7);   // (the record's own hands: some not in its tables)
		const uint32_t head = rd(rec + 0x72);
		if ((head >> 16) == 0xfd || (head >> 16) == 0xf8)
			for (uint32_t k = 0; k < 121; k++) { const uint32_t p = poly_of(rd(head + 0x40 + 4 * k)); if (!p) break; put(p, 8); }
		put(poly_of(rd(rec + 0x16)), 8);
		std::vector<uint32_t> strands;
		strand_polys(pl, strands);
		for (uint32_t p : strands) put(p, 8);
		if (!build || select) continue;
		// (the computer's side: built once a run, not again in a continue's rematch or the ending's replays)
		const bool cpu_side = m_ram_a.size() > 0x7600 && !(m_ram_a[0x7483 + 0x80 * pl] & 0x08);
		if (cpu_side && m_run_on) { if (m_built_run.count(rec)) continue; m_built_run.insert(rec); }
		b.start = int64_t(m_frame) + delay + BUILD_DELAY;
		if (getenv("VF_RANDLOG")) fprintf(stderr, "BUILD pl%d %06x frame %llu: %zu models\n", pl, rec, (unsigned long long)m_frame, b.group.size());   // TESTONLY
	}
}

// Whose a model is: the player whose record has it; both (a shared model: Jacky's legs on a random
// fighter against Jacky), the one whose parts were just drawn (the game draws each fighter's parts
// in a run), unless it's more than twice as far from that one's torso as from the other's (the first
// of the next fighter's run): by the nearer torso alone, a random fighter's Jacky hair took the other
// fighter's colours mid-throw, its head nearer the thrower's torso; -1 neither's
int model1::model_owner(uint32_t poly_adr) const
{
	int pl = -1, owners = 0;
	for (int k = 0; k < 2; k++) {
		if (!m_build_models[k].count(poly_adr)) continue;
		owners++;
		if (pl >= 0 && m_neck[0].torso && m_neck[1].torso) {
			const float *T = m_view->translation;
			auto d2 = [&](const neck_fit &f) { float s = 0; for (int i = 0; i < 3; i++) s += (T[9 + i] - f.t[i]) * (T[9 + i] - f.t[i]); return s; };
			pl = d2(m_neck[k]) < d2(m_neck[pl]) ? k : pl;
			if (const int l = m_owner_last; l >= 0 && d2(m_neck[l]) <= 4 * d2(m_neck[l ^ 1])) pl = l;
			continue;
		}
		if (pl < 0) pl = k;
	}
	if (owners < 2) m_owner_last = pl;   // (a model only one has, or scenery: whose run this is)
	return pl;
}

bool model1::build_info(uint32_t poly_adr, build_at &at) const
{
	const int pl = model_owner(poly_adr);
	if (pl < 0 || m_build[pl].start < 0) return false;
	const auto it = m_build[pl].group.find(poly_adr);
	if (it == m_build[pl].group.end()) return false;
	const int g = it->second.group;
	at.e = int64_t(m_frame) - m_build[pl].start;
	at.appear = g * BUILD_STEP;
	at.colour = BUILD_COLOUR + g * BUILD_COLOUR_STEP;
	at.polys = std::max(it->second.polys, 1);
	return true;
}

// Every name a random fighter can have, and the first seed that gives it: each seed's fighter made
// in player 1's record as at a game's start, on a copy of the ROM and the fighters' state, both put
// back after (nothing on screen changes). A few hundred milliseconds, once
const std::map<std::string, uint32_t> &model1::random_fighter_names()
{
	if (!m_seed_names.empty()) return m_seed_names;
	random_cast &c = random_fighter_cast();
	if (c.recs.empty() || !c.orig.count(0xfd4f99)) return m_seed_names;
	auto &rom = m_regions.maincpu;
	const std::vector<uint8_t> rom_was = rom;
	const auto names = m_random_names;
	const auto hue = m_random_hue;
	const auto outfit = m_random_outfit;
	const auto theme = m_random_theme;
	const auto stance = m_stance_from;
	const auto voice = m_voice_of;
	const auto move_ids = m_move_ids;
	const auto specials = m_special_list;
	const uint32_t state = m_random_state;
	const bool moves = m_random_moves;
	m_random_moves = true;
	for (uint32_t seed = 1; seed <= RANDOM_SEEDS; seed++) {
		m_random_state = seed;
		random_fighter_make(0xfd4f99);
		// (only a fighter that looks its own: not one character's face on its own torso, not over 60%
		// of its looks one character's (an Akira with another's legs and hands); the first such seed
		// for each name, every name but two kept)
		static const bool any_look = getenv("VF_ANY_LOOK") != nullptr;   // TESTONLY (as before: every fighter)
		if (!any_look && (m_look_same_body || m_look_share > 0.6f)) continue;
		m_seed_names.emplace(m_random_names[0xfd4f99], seed);   // (the first seed for each)
	}
	rom = rom_was;
	m_random_names = names;
	m_random_hue = hue;
	m_random_outfit = outfit;
	m_random_theme = theme;
	m_stance_from = stance;
	m_voice_of = voice;
	m_move_ids = move_ids;
	m_special_list = specials;
	m_random_state = state;
	m_random_moves = moves;
	return m_seed_names;
}

bool model1::set_random_player(int pl, const std::string &setting)
{
	if (pl < 0 || pl > 1) return false;
	std::string n;
	for (char ch : setting) if (ch != ' ') n += char(toupper(uint8_t(ch)));
	if (n.empty() || n == "*") { m_player_mode[pl] = n.empty() ? 0 : 1; m_player_seed[pl] = 0; return true; }
	if (n[0] == '#') {   // (a secret character, "#1"-"#7": UNUSED_CHARACTERS)
		const int k = atoi(n.c_str() + 1);
		if (k < 1 || k >= int(std::size(UNUSED_CHARACTERS))) return false;
		m_player_mode[pl] = 3;
		m_player_seed[pl] = uint32_t(k);
		return true;
	}
	const auto &names = random_fighter_names();
	const auto it = names.find(n);
	if (it == names.end()) return false;
	m_player_mode[pl] = 2;
	m_player_seed[pl] = it->second;
	return true;
}

// One of Akira's records made for a player's setting: Akira's own (0), random (1), a name's (2:
// its seed, then the clock's stream as it was)
void model1::random_akira_record(uint32_t rec, int mode, uint32_t seed)
{
	random_cast &c = random_fighter_cast();
	auto it = c.orig.find(rec);
	if (it == c.orig.end()) return;
	m_akira_made[rec] = { mode, mode >= 2 ? seed : 0 };
	m_secret_names.erase(rec);
	if (!mode || mode == 3) {
		memcpy(&m_regions.maincpu[rec], it->second.data(), RECORD_SIZE);
		m_random_names.erase(rec);
		m_random_hue.erase(rec);
		m_random_outfit.erase(rec);
		m_random_theme.erase(rec);
		m_outfit_tones.erase(rec);
		m_skin_ref.erase(rec);
		m_stance_from.erase(rec);
		m_voice_of.erase(rec);
		m_move_ids.erase(rec);
		m_special_list.erase(rec);
		if (mode == 3) secret_akira_record(rec, int(seed));
		return;
	}
	if (mode == 1) { random_fighter_make_named(rec); return; }
	const uint32_t was = m_random_state;
	m_random_state = seed;
	random_fighter_make(rec);
	m_random_state = was;
	random_fighter_colour(rec);
}

// Random: one of the named fighters (the first seed of each name, random_fighter_names), so the
// name on its energy bar, typed as a seed, is that same fighter; its colour new each game
void model1::random_fighter_make_named(uint32_t rec)
{
	if (!random_fighter_cast().orig.count(rec)) return;   // (no record: a block's first look, 0 or RAM's ff)
	const auto &names = random_fighter_names();   // (first: it makes fighters of its own, all put back)
	if (names.empty()) { random_fighter_make(rec); return; }
	auto it = names.begin();
	std::advance(it, random_next(uint32_t(names.size())));
	const uint32_t was = m_random_state;
	m_random_state = it->second;
	random_fighter_make(rec);
	m_random_state = was;
	random_fighter_colour(rec);
	if (getenv("VF_RANDLOG") && m_random_names[rec] != it->first)   // TESTONLY (a record that names the seed's fighter otherwise)
		fprintf(stderr, "NAMED %06x seed %u: %s, made %s\n", rec, it->second, it->first.c_str(), m_random_names[rec].c_str());
}

// A random fighter's colours (its hair, hat and bands: m_random_hue; its clothes' turn round the
// colour wheel: m_random_outfit), from the game's own draw, not its seed's: a seed is the same build
// and moves every game, its colours new each time
void model1::random_fighter_colour(uint32_t rec)
{
	if (m_random_hue.count(rec)) m_random_hue[rec] = float(random_next(1000)) / 1000.0f;
	m_random_outfit[rec] = float(random_next(1000)) / 1000.0f;   // (its clothes' turn round the colour wheel)
	// (and their theme, by weight: a free turn most; cool, warm, two opposite colours; neon, pastel;
	// black and white, black and gold the rare ones)
	static const int WEIGHT[OUTFIT_THEMES] = { 6, 1, 1, 4, 4, 4, 3, 3 };
	int w = int(random_next(26)), t = 0;
	while (t < OUTFIT_THEMES - 1 && w >= WEIGHT[t]) w -= WEIGHT[t++];
	m_random_theme[rec] = t;
	m_outfit_tones.erase(rec);
	m_skin_ref.erase(rec);   // (its face's skin measured again: a new record's head)
}

// A secret character for a player (Player 1 / 2 fighter): its record copied into that player's Akira
// record (fd4f99 takes its first costume, fd3f6f its second), picked for them at the select as a
// random fighter is, so the other player keeps Akira. Its own moves (+4), special commands (by its
// character, +0c) and stance; its name on the energy bar (m_secret_names). Those with no portrait
// (Rekka, Score, Aeon, Dural: no head list, their head the model at +16) get one made from that
// head, as akira_slot_heads, a place each for Akira's two records (f80200, f80300: free ROM space).
// Not Dural's metal clank (the game checks for her by slot, 5): left
// (each secret character's records, its first costume's and its second's)
static const uint32_t SECRET[8][2] = { { 0, 0 }, { 0xfd33af, 0xfd4843 }, { 0xfd3c7b, 0xfd4ca5 }, { 0xfd3df9, 0xfd4e23 },
									   { 0xfd510f, 0xfd510f }, { 0xfd5285, 0xfd5285 }, { 0xfd53fb, 0xfd53fb }, { 0xfd369b, 0xfd369b } };

void model1::secret_akira_record(uint32_t rec, int which)
{
	random_cast &c = random_fighter_cast();
	auto &rom = m_regions.maincpu;
	if (which < 1 || which > 7) return;
	const uint32_t src = SECRET[which][rec == 0xfd4f99 ? 0 : 1];
	const auto so = c.orig.find(src), ao = c.orig.find(rec);
	if (so == c.orig.end() || ao == c.orig.end()) return;
	auto rd = [](const std::vector<uint8_t> &b, size_t o) { return uint32_t(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24); };
	auto wr = [&](uint32_t a, uint32_t v) { for (int b = 0; b < 4; b++) rom[a + b] = uint8_t(v >> (8 * b)); };
	memcpy(&rom[rec], so->second.data(), RECORD_SIZE);
	m_secret_names[rec] = record_name(src);
	m_stance_from[rec] = rd(so->second, 4);
	const uint8_t id = so->second[0x0c];
	m_special_list[rec] = uint32_t(rom[SPECIALS + 4 * (id < 12 ? id : 9)] | rom[SPECIALS + 4 * (id < 12 ? id : 9) + 1] << 8 | rom[SPECIALS + 4 * (id < 12 ? id : 9) + 2] << 16 | uint32_t(rom[SPECIALS + 4 * (id < 12 ? id : 9) + 3]) << 24);
	if (which == 1) {   // (Siba's pounce: Lau's, as set_akira_slot gives him)
		constexpr uint32_t SIBA_POUNCE = 0xfc78c4 + 130 * 4;
		if (rom[SIBA_POUNCE + 1] == 0x01 && rom[SIBA_POUNCE] == 0x03) rom[SIBA_POUNCE] = 0x7d;
	}
	if (rd(so->second, 0x72) == 0) akira_portrait_of_head(rec, rd(so->second, 0x16));   // (no portrait: one of its head)
}

// A portrait for one of Akira's records whose head is a single model (+16, no head list: Dural's,
// the Koskols'), as akira_slot_heads: a list of that model as its first part (every frame), Akira's
// blinking, at a place each for Akira's two records (f80200, f80300: free ROM space). The fight
// draws its head from the list then: the same model
void model1::akira_portrait_of_head(uint32_t rec, uint32_t head)
{
	random_cast &c = random_fighter_cast();
	auto &rom = m_regions.maincpu;
	const auto ao = c.orig.find(rec);
	if ((rec != 0xfd4f99 && rec != 0xfd3f6f) || ao == c.orig.end() || (head >> 16) != 0x2d) return;
	auto rd = [](const std::vector<uint8_t> &b, size_t o) { return uint32_t(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24); };
	auto wr = [&](uint32_t a, uint32_t v) { for (int b = 0; b < 4; b++) rom[a + b] = uint8_t(v >> (8 * b)); };
	const uint32_t at = rec == 0xfd4f99 ? 0xf80200 : 0xf80300, list = at + 0x80;
	if (list + 0x80 > rom.size()) return;
	const uint32_t blink_list = rd(ao->second, 0x72);   // (Akira's own record's blinking)
	const uint32_t blink = (blink_list >> 16) == 0xfd ? uint32_t(rom[blink_list + 0x3c] | rom[blink_list + 0x3d] << 8 | rom[blink_list + 0x3e] << 16 | uint32_t(rom[blink_list + 0x3f]) << 24) : 0;
	std::fill(&rom[at], &rom[list + 0x80], 0);
	wr(at, list);
	wr(at + 0x3c, blink);
	for (uint32_t k = 0; k < 0x80; k += 4) wr(list + k, head);
	wr(rec + 0x72, at);
}

// (the select's word for each player, 407482 and 0x80 on: 0x8000 at the select, 0x1000 picked)
bool model1::picked_now(int pl) const
{
	const uint32_t o = 0x7482 + 0x80 * uint32_t(pl);
	return o + 2 <= m_ram_a.size() && (m_ram_a[o + 1] & 0x10);
}

// Which of Akira's records a player's random fighter is, as the game gives them to who's in: one
// player alone at the select, fd4f99 (the character 0 on either side); both, player 1's fd4f99 and
// player 2's fd3f6f (the select's +9 for player 2's side, fcf578); one who picked at an earlier
// select and is still in (a challenger's select doesn't pick again) keeps theirs, the other the
// other. 0 for a player picking as ever, or not in. (Player 1's portrait of Akira with player 2
// holding fd4f99 from alone shows that one's head: rare, left)
uint32_t model1::akira_record_of(int pl) const
{
	if (!m_random_fighter || !m_player_mode[pl] || m_ram_a.size() < 0x7600) return 0;
	auto in = [&](int p) { return (m_ram_a[0x7483 + 0x80 * p] & 0x08) != 0; };   // (0x0800: at the select, or picked and playing)
	if (!in(pl)) return 0;
	const int other = pl ^ 1;
	if (m_akira_held[pl] && picked_now(pl)) return m_akira_held[pl];
	if (m_akira_held[other] && picked_now(other)) return m_akira_held[other] == 0xfd4f99 ? 0xfd3f6f : 0xfd4f99;
	if (!in(other)) return 0xfd4f99;
	return pl ? 0xfd3f6f : 0xfd4f99;
}

// At the select: each random player's Akira record made for them as who's in becomes known (the
// players' words come a few frames after the select), the one no random player has Akira's own;
// and a random player who has picked (the select's word: 0x8000 the select, 0x1000 picked) has
// its character byte (-4B in its block, read as the match loads: Akira 0, his other costume 9)
// set to that record's, and keeps it
void model1::random_fighter_picks()
{
	if (m_ram_a.size() < 0x7600) return;
	static constexpr uint32_t AKIRA[2] = { 0xfd4f99, 0xfd3f6f };
	bool remade = false;
	uint32_t wanted[2] = { 0, 0 };
	for (int pl = 0; pl < 2; pl++) wanted[pl] = akira_record_of(pl);
	for (uint32_t r : AKIRA) {
		int owner = -1;
		for (int pl = 0; pl < 2; pl++) if (wanted[pl] == r) owner = pl;
		const std::pair<int, uint32_t> want = owner < 0 ? std::pair<int, uint32_t>{ 0, 0 }
			: std::pair<int, uint32_t>{ m_player_mode[owner], m_player_mode[owner] >= 2 ? m_player_seed[owner] : 0 };
		const auto made = m_akira_made.find(r);
		const bool same = made != m_akira_made.end() && made->second.first == want.first && (want.first < 2 || made->second.second == want.second);
		if (same || (owner < 0 && made == m_akira_made.end())) continue;
		if (owner < 0 && ((m_akira_held[0] == r && picked_now(0)) || (m_akira_held[1] == r && picked_now(1)))) continue;
		random_akira_record(r, want.first, want.second);
		remade = true;
	}
	if (remade) { random_fighter_strands(); random_fighter_specials(); }
	for (int pl = 0; pl < 2; pl++) {
		if (!wanted[pl]) continue;
		const uint32_t o = 0x7482 + 0x80 * uint32_t(pl);
		const uint16_t f = uint16_t(m_ram_a[o] | m_ram_a[o + 1] << 8);
		if ((f & 0x9000) != 0x9000) continue;
		m_akira_held[pl] = wanted[pl];
		uint8_t &ch = m_ram_a[0x6b5 + 0x1800 * pl];
		const uint8_t want = wanted[pl] == 0xfd4f99 ? 0 : 9;
		if ((ch == 0 || ch == 9) && ch != want) {
			if (getenv("VF_RANDLOG")) fprintf(stderr, "PICK pl%d character %d -> %d\n", pl, ch, want);   // TESTONLY
			ch = want;
		}
	}
	// a Normal player picking Akira where the random player has his record (a challenger to a random
	// player playing alone, who has player 1's side's): his other costume, Akira's own
	for (int pl = 0; pl < 2; pl++) {
		const uint32_t theirs = wanted[pl ^ 1];
		if (wanted[pl] || !theirs || m_player_mode[pl]) continue;
		const uint32_t o = 0x7482 + 0x80 * uint32_t(pl);
		const uint16_t f = uint16_t(m_ram_a[o] | m_ram_a[o + 1] << 8);
		if ((f & 0x9800) != 0x9800) continue;
		uint8_t &ch = m_ram_a[0x6b5 + 0x1800 * pl];
		if ((ch == 0 && theirs == 0xfd4f99) || (ch == 9 && theirs == 0xfd3f6f)) {
			if (getenv("VF_RANDLOG")) fprintf(stderr, "PICK pl%d (normal) character %d -> %d\n", pl, ch, 9 - ch);   // TESTONLY
			ch = uint8_t(9 - ch);
		}
	}
}

// The stances' table (STANCE_LOOKUP's, 53b000) is built once as the game boots (ff5fbf, frames 203-204),
// each character's stance as a difference from player 1's own: its record's moves table, at the
// boot Dural's (fd369b). Dural made a random fighter (random opponents) made every entry that much
// off, the real characters' too: each move ended blending toward a stance that wasn't its own, and
// it snapped into the real one as it began. So the records keep their own moves tables until the
// boot has built it (and random_fighter_frame puts the random ones back)
void model1::random_fighter_hold_moves()
{
	if (!m_random_applied || m_frame - m_boot_at >= STANCES_BUILT || getenv("VF_STANCE_VF")) return;
	random_cast &c = random_fighter_cast();
	auto &rom = m_regions.maincpu;
	for (const auto &[r, b] : c.orig) {
		if (r + 8 > rom.size() || !memcmp(&rom[r + 4], &b[4], 4)) continue;
		if (!m_moves_held.count(r)) m_moves_held[r] = uint32_t(rom[r + 4] | rom[r + 5] << 8 | rom[r + 6] << 16 | uint32_t(rom[r + 7]) << 24);
		memcpy(&rom[r + 4], &b[4], 4);
		if (getenv("VF_HOLDLOG")) fprintf(stderr, "HOLD %06x frame %llu boot %llu: %06x held, %02x%02x%02x back\n", r, (unsigned long long)m_frame, (unsigned long long)m_boot_at, m_moves_held[r], rom[r + 6], rom[r + 5], rom[r + 4]);   // TESTONLY
	}
}

// The strands that swing (Sarah's ponytail, Pai's and Lau's braids, Akira's headband ribbon,
// Jeffry's dreadlocks) are listed by the player's slot, not by record (fda917: a slot byte and a
// pointer to its strands, 5 bytes each, to ff), and looked up once, as a match loads (ffc49d, at
// ffc4b6, by the block's slot, -4c), the strands' data kept from then on. With random heads each
// slot needs the strands of the head its record wears, and every slot's must be there before a
// match loads (made as the fighter came in, one frame late, the strands began on another's data:
// Kage's tails huge, Lau's braid flying, Sarah's ponytail over her face; or none, the hair gone).
// VF's list has room for its 14, so ours, for all 26 slots, is at f80800 (the ROM's empty space;
// f80000-f800ff is the secret characters' portrait heads, akira_slot_heads),
// the lookup's address moved there (set_strands_moved, before the code is recompiled), and made
// afresh whenever a fighter is.
static constexpr uint32_t STRAND_LIST = 0xfda917, STRAND_LIST_OURS = 0xf80800, STRAND_LOOKUP = 0xffc4b6, KAGE_SHRINK = 0xffc7e6, KAGE_TAILS = 0xfdac50;
// The stance a move ends into: ff60f0 looks its fighter's record's moves table (+4) up in VF's list
// of the ten (ff5f6f: Dural, Lau, Jacky, Kage, Siba, Wolf, Sarah, Pai, Jeffry, Akira) for its place
// in a table of their stances (53b000, 0x170 each), and blends the end of every move toward that
// one. A random fighter's table is none of the ten: not found, it was Dural's (the first), another
// lead's stance, and every move ended blending toward it, the body turning and the head looking
// up, then snapped into its own stance as that began. Ours: by the record's flags byte (its key,
// random_fighter_specials), a place in that list, the stance's own character's
static constexpr uint32_t STANCE_LOOKUP = 0xff60f0, STANCE_OURS = 0xf80b00, STANCE_LIST = 0xff5f6f;
static const uint8_t STANCE_VF[15] = { 0x2d, 0x62, 0x19, 0xb0, 0x04, 0x2d, 0x21, 0xea, 0x44, 0x60, 0xc1, 0xf1, 0x77, 0xfe, 0xbc };
static const uint8_t STANCE_PATCH[15] = { 0x0d, 0x61, 0x19, 0xb0, 0x0d,                                   // movz.bw 0D[-50[R25]], R1 (the flags byte)
										  0x0d, 0x61, 0xc1, 0xf3, 0x00, 0x0b, 0xf8, 0x00,                 // movz.bw F80B00(R1), R1 (its place)
										  0x6a, 0x08 };                                                   // br FF6105

// The select's portraits look their strands up (fce947, in fd2b33) by the character's slot through
// fd2f1b without player 2's side's +9 (fce87d: mov.b [R10](R11), R0; bsr fce947), so both sides'
// Akira took slot 16's: Akira's own two are alike (his ribbon) and it never showed, but a random
// fighter in each place is its own. Ours: the call goes to a few instructions in the ROM's empty
// space (f80700) that add the side's 9 (the head's own test, fce800) and look up from there
static constexpr uint32_t PORTRAIT_STRANDS = 0xfce87d, PORTRAIT_OURS = 0xf80700;
// A move's voice (the move's script, feddfe: a kind of shout, its sound by the record's character
// number, +0c, in a table of each kind's, fda6d7). A random fighter is made on Akira's record
// (random_fighter_make), its number his: every one had his voice. Ours: by the record's flags byte
// (its key, random_fighter_specials), a character's number in our table (VOICE_OURS, 256 bytes),
// a few instructions in the ROM's empty space (VOICE_CODE) called in place of the two that read it
static constexpr uint32_t VOICE_LOOKUP = 0xfeddfe, VOICE_CODE = 0xf80740, VOICE_OURS = 0xf80a00;
static const uint8_t VOICE_VF[8] = { 0x0d, 0x60, 0x89, 0x0d, 0x62, 0x19, 0xb0, 0x0c };   // movz.bw [R9+], R0; movz.bw C[-50[R25]], R2
static const uint8_t VOICE_CALL[8] = { 0xe8, 0xf2, 0x42, 0x29, 0xf9, 0xff, 0xcd, 0xcd };   // jsr F80740[PC]; nop; nop
static const uint8_t VOICE_PATCH[17] = {
	0x0d, 0x60, 0x89,                                           // movz.bw [R9+], R0
	0x0d, 0x62, 0x19, 0xb0, 0x0d,                               // movz.bw D[-50[R25]], R2 (the flags byte)
	0x0d, 0x62, 0xc2, 0xf3, 0x00, 0x0a, 0xf8, 0x00,             // movz.bw F80A00(R2), R2 (its voice)
	0xca };                                                     // rsr
static const uint8_t PORTRAIT_VF[7] = { 0x09, 0x60, 0xcb, 0x6a, 0x48, 0xc6, 0x00 };   // mov.b [R10](R11), R0; bsr FCE947
static const uint8_t PORTRAIT_CALL[7] = { 0xe8, 0xf2, 0x83, 0x1e, 0xfb, 0xff, 0xcd };   // jsr F80700[PC]; nop
static const uint8_t PORTRAIT_CODE[0x1c] = {
	0x2d, 0x4b, 0x60,                                           // mov.w R11, R0
	0x87, 0x80, 0xf4, 0x19, 0x00, 0x00, 0x00, 0x13, 0x80,       // test1 #19, -80[R19] (player 2's side)
	0x64, 0x05,                                                 // be +5
	0x84, 0x20, 0xe9,                                           // add.w #9, R0
	0x09, 0x60, 0xc0, 0x6a,                                     // mov.b [R10](R0), R0
	0xe8, 0xf2, 0x32, 0xe2, 0x04, 0x00,                         // jsr FCE947[PC]
	0xca };                                                     // rsr

bool model1::set_strands_moved(bool on)
{
	auto &rom = m_regions.maincpu;
	if (STRAND_LOOKUP + 7 > rom.size() || rom[STRAND_LOOKUP] != 0x40 || rom[STRAND_LOOKUP + 1] != 0x26 || rom[STRAND_LOOKUP + 2] != 0xf2) return false;
	const uint32_t want = (on ? STRAND_LIST_OURS : STRAND_LIST) - STRAND_LOOKUP;   // (PC-relative)
	const uint32_t was = uint32_t(rom[STRAND_LOOKUP + 3] | rom[STRAND_LOOKUP + 4] << 8 | rom[STRAND_LOOKUP + 5] << 16 | uint32_t(rom[STRAND_LOOKUP + 6]) << 24);
	if (was != STRAND_LIST - STRAND_LOOKUP && was != STRAND_LIST_OURS - STRAND_LOOKUP) return false;   // (not VF's)
	// the special commands' lookup: by the record's flags byte (+0d, with its key), in our table
	static const uint8_t VF_LOOKUP[13] = { 0x0d, 0x63, 0x13, 0xb0, 0x0c, 0x44, 0x63, 0xc3, 0xf3, 0x20, 0xb8, 0xfd, 0x00 };
	uint8_t ours[13];
	memcpy(ours, VF_LOOKUP, sizeof ours);
	ours[4] = 0x0d;
	for (int b = 0; b < 4; b++) ours[9 + b] = uint8_t(SPECIALS_OURS >> (8 * b));
	const bool specials = SPECIALS_LOOKUP + 13 <= rom.size() && (!memcmp(&rom[SPECIALS_LOOKUP], VF_LOOKUP, 13) || !memcmp(&rom[SPECIALS_LOOKUP], ours, 13));
	m_strands_moved = on;
	m_specials_moved = on && specials;
	random_fighter_strands();
	random_fighter_specials();
	bool changed = false;
	if (specials && memcmp(&rom[SPECIALS_LOOKUP], on ? ours : VF_LOOKUP, 13)) { memcpy(&rom[SPECIALS_LOOKUP], on ? ours : VF_LOOKUP, 13); changed = true; }
	// Kage's tails are drawn at a tenth (ffc7e6: the first two groups of the strands of the player
	// whose slot is Kage's, 2, scaled by 0.1 on the TGP), whatever strands that slot has: Sarah's
	// ponytail on Kage's record was a stub and specks, Kage's head on another's, its tails full size.
	// By the strands themselves instead: the task's byte -60 (unused, always 0), 2 while its strands
	// are Kage's (random_fighter_strands_settle)
	static const uint8_t SHRINK_VF[5] = { 0xb8, 0x80, 0x13, 0xb4, 0xe2 }, SHRINK_OURS[5] = { 0xb8, 0x80, 0x19, 0xa0, 0xe2 };
	static const bool vf_shrink = getenv("VF_KAGE_SLOT_SHRINK") != nullptr;   // TESTONLY (VF's own: by the slot)
	if (!vf_shrink && KAGE_SHRINK + 5 <= rom.size() && (!memcmp(&rom[KAGE_SHRINK], SHRINK_VF, 5) || !memcmp(&rom[KAGE_SHRINK], SHRINK_OURS, 5))
		&& memcmp(&rom[KAGE_SHRINK], on ? SHRINK_OURS : SHRINK_VF, 5)) {
		memcpy(&rom[KAGE_SHRINK], on ? SHRINK_OURS : SHRINK_VF, 5);
		changed = true;
	}
	// (the portraits' strands by side)
	if (PORTRAIT_OURS + sizeof PORTRAIT_CODE <= rom.size() && PORTRAIT_STRANDS + 7 <= rom.size()
		&& (!memcmp(&rom[PORTRAIT_STRANDS], PORTRAIT_VF, 7) || !memcmp(&rom[PORTRAIT_STRANDS], PORTRAIT_CALL, 7))) {
		if (on) memcpy(&rom[PORTRAIT_OURS], PORTRAIT_CODE, sizeof PORTRAIT_CODE);
		const uint8_t *to = on ? PORTRAIT_CALL : PORTRAIT_VF;
		if (memcmp(&rom[PORTRAIT_STRANDS], to, 7)) { memcpy(&rom[PORTRAIT_STRANDS], to, 7); changed = true; }
	}
	// (the voices' lookup, with the same keys)
	if (VOICE_LOOKUP + 8 <= rom.size() && VOICE_CODE + sizeof VOICE_PATCH <= rom.size()
		&& (!memcmp(&rom[VOICE_LOOKUP], VOICE_VF, 8) || !memcmp(&rom[VOICE_LOOKUP], VOICE_CALL, 8))) {
		const bool ours = on && m_specials_moved;
		if (ours) memcpy(&rom[VOICE_CODE], VOICE_PATCH, sizeof VOICE_PATCH);
		const uint8_t *to = ours ? VOICE_CALL : VOICE_VF;
		if (memcmp(&rom[VOICE_LOOKUP], to, 8)) { memcpy(&rom[VOICE_LOOKUP], to, 8); changed = true; }
	}
	// (the checks by character on the moves, with the same keys)
	for (int i = 0; i < MOVE_SITE_COUNT; i++) {
		const move_site &ms = MOVE_SITES[i];
		const uint32_t code = MOVEID_CODE + 0x20 * i, table = MOVEID_OURS + 0x100 * i;
		const uint8_t vf[8] = { 0x09, 0x60, 0x13, 0xb0, 0x0c, 0xb8, 0x20, uint8_t(0xe0 | ms.is) };   // mov.b C[-50[R19]], R0; cmp.b #n, R0
		const uint32_t to_code = code - ms.at;
		const uint8_t call[8] = { 0xe8, 0xf2, uint8_t(to_code), uint8_t(to_code >> 8), uint8_t(to_code >> 16), uint8_t(to_code >> 24), 0xcd, 0xcd };   // jsr code[PC]; nop; nop
		const uint8_t patch[17] = {
			0x0d, 0x60, 0x13, 0xb0, 0x0d,                                                                 // movz.bw D[-50[R19]], R0 (the flags byte)
			0x09, 0x60, 0xc0, 0xf3, uint8_t(table), uint8_t(table >> 8), uint8_t(table >> 16), uint8_t(table >> 24),   // mov.b table(R0), R0 (its number here)
			0xb8, 0x20, uint8_t(0xe0 | ms.is),                                                            // cmp.b #n, R0
			0xca };                                                                                       // rsr
		if (ms.at + 8 > rom.size() || code + sizeof patch > rom.size() || (memcmp(&rom[ms.at], vf, 8) && memcmp(&rom[ms.at], call, 8))) continue;
		const bool ours = on && m_specials_moved;
		if (ours) memcpy(&rom[code], patch, sizeof patch);
		const uint8_t *to = ours ? call : vf;
		if (memcmp(&rom[ms.at], to, 8)) { memcpy(&rom[ms.at], to, 8); changed = true; }
	}
	// (the stances' lookup, with the special commands' keys)
	if (STANCE_LOOKUP + 15 <= rom.size() && (!memcmp(&rom[STANCE_LOOKUP], STANCE_VF, 15) || !memcmp(&rom[STANCE_LOOKUP], STANCE_PATCH, 15))) {
		const uint8_t *to = on && m_specials_moved && !getenv("VF_STANCE_VF") ? STANCE_PATCH : STANCE_VF;   // (VF_STANCE_VF: testing, as VF has it)
		if (memcmp(&rom[STANCE_LOOKUP], to, 15)) { memcpy(&rom[STANCE_LOOKUP], to, 15); changed = true; }
	}
	if (was == want) return changed;
	for (int b = 0; b < 4; b++) rom[STRAND_LOOKUP + 3 + b] = uint8_t(want >> (8 * b));
	return true;
}

// Every record's flags byte with its key, and our table (by flags byte: a list's address) of
// their special commands: a random fighter's own, the rest their character's. Off: VF's bytes.
void model1::random_fighter_specials()
{
	auto &rom = m_regions.maincpu;
	if (SPECIALS_OURS + 0x400 > rom.size() || SPECIALS + 0x30 > rom.size()) return;
	random_cast &c = random_fighter_cast();
	auto rd = [&](uint32_t a) { return uint32_t(rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | uint32_t(rom[a + 3]) << 24); };
	auto wr = [&](uint32_t a, uint32_t v) { for (int b = 0; b < 4; b++) rom[a + b] = uint8_t(v >> (8 * b)); };
	auto rd32_of = [](const std::vector<uint8_t> &b, size_t o) { return uint32_t(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24); };
	// (flag 1: Pai's, Kage's and Jeff's records have a thing a hit knocks off their heads, ffd35a,
	// its models listed at +56: off with another's head on, it took that head's hair with it and left
	// it bald; not then, nor on a random fighter, made on Akira's record (none at +56))
	auto flags_of = [&](uint32_t r, const std::vector<uint8_t> &b) {
		uint8_t f = m_specials_moved && c.flags.count(r) ? c.flags[r] : b[0x0d];
		if (rd(r + 0x72) != rd32_of(b, 0x72) || m_random_names.count(r)) f &= uint8_t(~0x02);
		// (a secret character in Akira's record: its own thing to knock off, Jeff's hat, by its list)
		if (m_secret_names.count(r) && !m_random_names.count(r) && rd(r + 0x56)) f |= 0x02;
		return f;
	};
	for (const auto &[r, b] : c.orig) rom[r + 0x0d] = flags_of(r, b);
	if (!m_specials_moved) return;
	// each record's stance's place in VF's list (STANCE_LOOKUP): its own table's, a random one's
	// stance's character's; a key no record has, VF's not found (0)
	if (STANCE_OURS + 0x100 <= rom.size() && STANCE_LIST + 40 <= rom.size()) {
		memset(&rom[STANCE_OURS], 0, 0x100);
		for (const auto &[r, b] : c.orig) {
			const auto st = m_stance_from.find(r);
			const uint32_t t = st != m_stance_from.end() ? st->second : rd32_of(b, 4);
			for (uint32_t i = 0; i < 10; i++) if (rd(STANCE_LIST + 4 * i) == t) { rom[STANCE_OURS + flags_of(r, b)] = uint8_t(i); break; }
		}
	}
	// each record's voice: a random one's (the first of the three it's made of), the rest their own
	if (VOICE_OURS + 0x100 <= rom.size()) {
		memset(&rom[VOICE_OURS], 0x0b, 0x100);   // (a key no record has: Akira's)
		for (const auto &[r, b] : c.orig) {
			const auto v = m_voice_of.find(r);
			rom[VOICE_OURS + flags_of(r, b)] = v != m_voice_of.end() && m_random_names.count(r) ? v->second : rom[r + 0x0c];
		}
	}
	// each record's number for VF's checks by character (MOVE_SITES): a random one's, the character's
	// whose command it has (else Akira's, as it wears in a match), the rest their own (as the record
	// has it now: a secret character in Akira's record, its own)
	if (MOVEID_OURS + 0x100 * MOVE_SITE_COUNT <= rom.size()) {
		memset(&rom[MOVEID_OURS], 0x0b, 0x100 * MOVE_SITE_COUNT);   // (a key no record has: Akira's)
		for (const auto &[r, b] : c.orig) {
			const auto m = m_move_ids.find(r);
			for (int i = 0; i < MOVE_SITE_COUNT; i++)
				rom[MOVEID_OURS + 0x100 * i + flags_of(r, b)] = !m_random_names.count(r) ? rom[r + 0x0c] : m != m_move_ids.end() && m->second[i] != 0xff ? m->second[i] : 0x0b;
		}
	}
	for (uint32_t f = 0; f < 0x100; f++) wr(SPECIALS_OURS + 4 * f, rd(SPECIALS + 4 * 9));   // (none: Jeff's, empty)
	for (const auto &[r, b] : c.orig) {
		auto it = m_special_list.find(r);
		wr(SPECIALS_OURS + 4 * flags_of(r, b), it != m_special_list.end() ? it->second : rd(SPECIALS + 4 * (b[0x0c] < 12 ? b[0x0c] : 9)));
	}
}

void model1::random_fighter_strands()
{
	static constexpr uint32_t SLOTS = 0xfd2e8f;
	auto &rom = m_regions.maincpu;
	if (STRAND_LIST + 80 > rom.size() || STRAND_LIST_OURS + 26 * 5 + 1 > rom.size()) return;
	if (m_strands_orig.empty()) {
		size_t n = 0;
		while (n < 75 && rom[STRAND_LIST + n] != 0xff) n += 5;
		if (n >= 75) return;   // (not VF's list)
		m_strands_orig.assign(&rom[STRAND_LIST], &rom[STRAND_LIST] + n + 1);
	}
	if (!m_strands_moved) { random_fighter_portrait_strands(); return; }   // (the portraits' put back as VF has them)
	auto rd = [&](const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); };
	std::vector<uint8_t> list;
	if (!m_random_fighter) list = m_strands_orig;   // (as VF has it)
	else {
		random_cast &c = random_fighter_cast();
		auto orig_strands = [&](uint32_t slot) -> uint32_t {   // a slot's own, or 0
			for (size_t i = 0; i + 5 <= m_strands_orig.size(); i += 5)
				if (m_strands_orig[i] == slot) return rd(&m_strands_orig[i + 1]);
			return 0;
		};
		// a head list -> the strands of the slot whose own record wears it
		auto strands_of_head = [&](uint32_t head) -> uint32_t {
			for (uint32_t s = 0; s < 26; s++) {
				auto it = c.orig.find(rd(&rom[SLOTS + 4 * s]));
				if (it != c.orig.end() && rd(&it->second[0x72]) == head) return orig_strands(s);
			}
			for (const auto &[r, b] : c.orig)   // (Akira's two, not in the slots while a secret character is)
				if (rd(&b[0x72]) == head) return orig_strands(r == 0xfd4f99 ? 0x16 : r == 0xfd3f6f ? 0x0b : 0xff);
			return 0;
		};
		for (uint32_t s = 0; s < 26; s++) {
			const uint32_t rec = rd(&rom[SLOTS + 4 * s]);
			auto it = c.orig.find(rec);
			if (it == c.orig.end()) { if (uint32_t st = orig_strands(s)) { list.push_back(uint8_t(s)); for (int b = 0; b < 4; b++) list.push_back(uint8_t(st >> (8 * b))); } continue; }
			const uint32_t head = rd(&rom[rec + 0x72]);
			uint32_t st = (head >> 16) == 0xfd ? strands_of_head(head) : (head >> 16) == 0xf8 ? 0 : orig_strands(s);   // (f8xxxx: a portrait made of a head with none)
			if (!head && rd(&it->second[0x72])) st = 0;   // (Dural's head: no strands)
			// Kage's first costume's tails (fdac50) come out huge on any other record: his second's
			// (fdaee2), the same tails, sit right on anyone
			if (st == 0xfdac50 && (head != rd(&it->second[0x72]) || m_random_names.count(rec))) st = 0xfdaee2;   // (a random one's made on Akira's)
			if (!st) continue;
			list.push_back(uint8_t(s));
			for (int b = 0; b < 4; b++) list.push_back(uint8_t(st >> (8 * b)));
		}
		list.push_back(0xff);
	}
	memcpy(&rom[STRAND_LIST_OURS], list.data(), list.size());
	random_fighter_portrait_strands();
}

// The select's portraits have their own strands list (fd2b33, by slot, read at fce947): Akira's
// places (slot 16, player 1's side, fd2b60; 0b, player 2's, fd2b5b, looked up by side with
// PORTRAIT_OURS) wore his headband's ribbon on any random fighter's head. A random fighter's: the
// strands of the slot whose own record wears its head (Sarah's ponytail, Lau's braid; none for a
// head without, the slot byte one no player has, 0x19, as set_akira_slot's); Akira's own as VF has
void model1::random_fighter_portrait_strands()
{
	static constexpr uint32_t LIST = 0xfd2b33, SLOTS = 0xfd2e8f;
	auto &rom = m_regions.maincpu;
	if (LIST + 14 * 5 + 1 > rom.size()) return;
	static uint8_t orig[14 * 5];
	static bool kept = false;
	if (!kept) {
		// (not VF's list; Akira's places' slot bytes may be set_akira_slot's already: none, 0x19)
		const uint8_t *vf = rom_pristine(LIST);
		if (vf[14 * 5] != 0xff || (vf[9 * 5] != 0x16 && vf[9 * 5] != 0x19) || (vf[8 * 5] != 0x0b && vf[8 * 5] != 0x19)) return;
		memcpy(orig, vf, sizeof orig);
		orig[9 * 5] = 0x16;
		orig[8 * 5] = 0x0b;
		kept = true;
	}
	auto rd = [&](const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); };
	auto portrait_of = [&](uint32_t slot) -> uint32_t {
		for (int i = 0; i < 14; i++) if (orig[5 * i] == slot) return rd(&orig[5 * i + 1]);
		return 0;
	};
	auto poly_of = [&](uint32_t m) -> uint32_t { return (m >> 16) == 0x2d && m + 12 <= rom.size() ? rd(&rom[m + 8]) & 0x7fffff : 0; };
	random_cast &c = random_fighter_cast();
	m_portrait_strand_models.clear();
	for (const auto &[at, rec] : { std::pair<int, uint32_t>{ 9, 0xfd4f99 }, { 8, 0xfd3f6f } }) {
		uint8_t *e = &rom[LIST + 5 * at];
		memcpy(e + 1, &orig[5 * at + 1], 4);   // (Akira's ribbon, its slot byte set_akira_slot's: VF's, or none for a secret character)
		e[0] = m_akira_ribbon[at == 9 ? 0 : 1] ? m_akira_ribbon[at == 9 ? 0 : 1] : orig[5 * at];
		if (!m_strands_moved || !m_random_fighter || (!m_random_names.count(rec) && !m_secret_names.count(rec))) continue;
		const uint32_t head = rd(&rom[rec + 0x72]);
		uint32_t st = 0;
		if ((head >> 16) == 0xfd)
			for (uint32_t sl = 0; sl < 26 && !st; sl++) {
				auto it = c.orig.find(rd(&rom[SLOTS + 4 * sl]));
				if (it != c.orig.end() && rd(&it->second[0x72]) == head) st = portrait_of(sl);
			}
		if (!st) for (const auto &[r, b] : c.orig) if (rd(&b[0x72]) == head && (r == 0xfd4f99 || r == 0xfd3f6f)) st = portrait_of(orig[5 * at]);   // (Akira's head: his)
		e[0] = st ? orig[5 * at] : 0x19;
		for (int b = 0; b < 4 && st; b++) e[1 + b] = uint8_t(st >> (8 * b));
		// (its strands' models, to the next strands' data: in its hair's hue, random_fighter_heads)
		uint32_t end = st + 0x200;
		for (int i = 0; i < 14; i++) if (const uint32_t q = rd(&orig[5 * i + 1]); q > st && q < end) end = q;
		for (uint32_t a = st; st && a + 4 <= end && a + 4 <= rom.size(); a++)
			if (const uint32_t m = poly_of(rd(&rom[a]))) m_portrait_strand_models[rec].push_back(m);
	}
}

// A record's character, by name (the HUD's: VF's own, and the cut ones' as the cheat names them)
const char *model1::record_name(uint32_t rec)
{
	static const std::pair<uint32_t, const char *> NAMES[] = {
		{ 0xfd2f45, "LAU" }, { 0xfd46cd, "LAU" }, { 0xfd30bb, "JACKY" }, { 0xfd49b9, "JACKY" }, { 0xfd3231, "KAGE" }, { 0xfd454f, "KAGE" },
		{ 0xfd33af, "SIBA" }, { 0xfd4843, "SIBA" }, { 0xfd3525, "WOLF" }, { 0xfd4b2f, "WOLF" }, { 0xfd369b, "DURAL" },
		{ 0xfd3811, "SARAH" }, { 0xfd43d9, "SARAH" }, { 0xfd3987, "PAI" }, { 0xfd40e5, "PAI" }, { 0xfd3b05, "JEFFRY" }, { 0xfd4263, "JEFFRY" },
		{ 0xfd3c7b, "JEFF" }, { 0xfd4ca5, "JEFF" }, { 0xfd3df9, "TAKERU" }, { 0xfd4e23, "TAKERU" }, { 0xfd3f6f, "AKIRA" }, { 0xfd4f99, "AKIRA" },
		{ 0xfd510f, "REKKA" }, { 0xfd5285, "SCORE" }, { 0xfd53fb, "AEON" } };
	for (const auto &[r, n] : NAMES) if (r == rec) return n;
	return "???";
}

// The three it's most made of, in one name of 6 to 10 letters, by their syllables: the first's start
// (two syllables of a long name), a syllable of the second and the third's last (JACKY, PAI and
// WOLF: JACPAIWOLF; WOLF, LAU and SARAH: WOLFLAURAH; over 10, the first's start and the third's
// end), a letter shared at a join written once. Of two: the first's start and the rest of the
// second; of one, its own. (VF_SHORT_NAMES, testing: the old 5-7 letters, the first's start, the
// second's middle and the third's end: SARACAI, AKIOLAU)
std::string model1::blend_name(const std::map<std::string, int> &made_of)
{
	std::vector<std::pair<int, std::string>> by;
	for (const auto &[n, k] : made_of) by.push_back({ -k, n });
	std::sort(by.begin(), by.end());
	if (by.empty()) return "???";
	static const bool shorter = getenv("VF_SHORT_NAMES") != nullptr;   // testing
	if (!shorter) {
		// each name's syllables
		static const std::map<std::string, std::vector<std::string>> SYL = {
			{ "AKIRA", { "A", "KI", "RA" } }, { "JACKY", { "JAC", "KY" } }, { "PAI", { "PAI" } }, { "WOLF", { "WOLF" } },
			{ "LAU", { "LAU" } }, { "SARAH", { "SA", "RAH" } }, { "JEFFRY", { "JEF", "FRY" } }, { "KAGE", { "KA", "GE" } },
			{ "DURAL", { "DU", "RAL" } }, { "SIBA", { "SI", "BA" } }, { "JEFF", { "JEFF" } }, { "TAKERU", { "TA", "KE", "RU" } } };
		auto syl = [&](const std::string &n) { auto it = SYL.find(n); return it != SYL.end() ? it->second : std::vector<std::string>{ n }; };
		auto join = [](std::string a, std::string b) { if (!a.empty() && !b.empty() && a.back() == b.front()) b.erase(0, 1); return a + b; };
		const std::string &a = by[0].second;
		if (by.size() == 1) return a;
		const auto sa = syl(a), sb = syl(by[1].second);
		const std::string start = sa.size() >= 3 ? sa[0] + sa[1] : sa[0];   // (the heaviest's start)
		if (by.size() == 2) {
			std::string rest;
			for (size_t i = sb.size() > 1 ? 1 : 0; i < sb.size(); i++) rest += sb[i];
			return join(start, rest);
		}
		std::string mid = sb[0];
		for (const auto &x : sb) if (x.size() >= 2) { mid = x; break; }
		const std::string end = syl(by[2].second).back();
		std::string n = join(join(start, mid), end);
		return n.size() > 10 ? join(start, end) : n;
	}
	auto join = [](std::string a, std::string b) { if (!a.empty() && !b.empty() && a.back() == b.front()) b.erase(0, 1); return a + b; };
	const std::string &a = by[0].second;
	if (by.size() == 1) return a;
	const std::string &b = by[1].second;
	if (by.size() == 2) {
		if (a.size() + b.size() <= 6) return a + b;
		return join(a.substr(0, std::min(a.size(), (a.size() + 1) / 2 + 1)), b.substr(b.size() / 2));
	}
	const std::string &c = by[2].second;
	const std::string start = a.size() > 3 ? a.substr(0, std::min<size_t>(3, (a.size() + 1) / 2)) : a.substr(0, 2);
	const size_t m = b.size() / 2;
	const std::string middle = b.substr(m > 0 ? m - 1 : 0, 2);
	const std::string end = c.size() > 2 ? c.substr(c.size() - 2) : c;
	return join(join(start, middle), end);
}

// The names under the energy bars, a random fighter's own (m_random_names), or a secret
// character's (Cheats, in Akira's place: SIBA, JEFF, TAKERU, REKKA, SCORE, AEON, DURAL): the HUD's map (tile
// layer page 0, rows 6-7) has each player's name boxed, player 1's a picture of its name (tiles
// 1a80-1aff), player 2's spelt in the font (1b00 on, two tiles a letter, ' ' 0x20 on, 16 a row,
// top above bottom; framed by 1b03/1b04 above, 1b13/1b14 below). Each frame a random fighter's box
// is spelt anew in the font: player 1's from the left of its picture, player 2's to its box's right.
// names: netplay's, each player's own name in their box instead (set_hud_names), with undo: what each cell held
// before, so the picture can be drawn with them and the tiles put back as the game left them.
void model1::random_fighter_hud(const std::string *names, std::vector<std::pair<uint32_t, uint16_t>> *undo)
{
	if (m_ram_a.size() < 0x1f00) return;
	auto rd = [&](const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); };
	constexpr int ROW = 6, W = 64;
	auto at = [&](int x, int y) { return m_tiles.tile_r(uint32_t(y * W + x)); };
	auto put = [&](int x, int y, uint16_t v) {
		if (x < 0 || x >= W || at(x, y) == v) return;
		if (undo) undo->push_back({ uint32_t(y * W + x), at(x, y) });
		m_tiles.tile_w(uint32_t(y * W + x), v, 0xffff);
	};
	// only the fight's HUD: its WIN markers (8900-8902 below each player's bar, row 7) there, so a
	// title or select screen's tiles that look alike are never touched (its picture unrecognised,
	// the widescreen sides came late)
	auto win_at = [&](int x) { return (at(x, ROW + 1) & 0x7fff) == 0x0900 && (at(x + 1, ROW + 1) & 0x7fff) == 0x0901 && (at(x + 2, ROW + 1) & 0x7fff) == 0x0902; };
	bool win[2] = { false, false };
	for (int x = 0; x + 2 < W; x++) if (win_at(x)) win[x >= 32] = true;
	auto label = [](uint16_t e) { return (e & 0x1f80) == 0x1a80; };
	auto font = [](uint16_t e) { return (e & 0x1f80) >= 0x1b00 && (e & 0x1f80) < 0x1b80; };
	for (int pl = 0; pl < 2; pl++) {
		const uint32_t rec = rd(&m_ram_a[0x700 + 0x1800 * pl - 0x50]);
		std::string name;
		if (names) name = names[pl];
		else if (m_random_fighter) { if (auto it = m_random_names.find(rec); it != m_random_names.end()) name = it->second; }
		if (auto it = m_secret_names.find(rec); !names && name.empty() && it != m_secret_names.end()) name = it->second;   // (a player's secret character)
		// (Cheats' Secret character, with Random opponents too: the ROM's picture of its name was another's)
		if (!names && name.empty() && rec && (rec == m_secret_recs[0] || rec == m_secret_recs[1])) name = record_name(rec);
		if (names) m_hud_flag_x[pl] = -1;
		if (name.empty() || !win[pl]) continue;
		// the box: its cells on this player's half of the row
		int x0 = -1, x1 = -1;
		for (int x = pl ? 32 : 0; x < (pl ? W : 32); x++)
			if (label(at(x, ROW)) || (font(at(x, ROW)) && font(at(x, ROW + 1)))) { if (x0 < 0) x0 = x; x1 = x; }
		if (x0 < 0) continue;
		const uint16_t blank = at(pl ? x0 - 1 : x1 + 1, ROW), blank2 = at(pl ? x0 - 1 : x1 + 1, ROW + 1);
		if (label(blank) || font(blank)) continue;
		const bool flag = names && m_hud_flag_on[pl];   // (netplay's: two cells for the flag, out from the middle)
		const int n = int(name.size()) + 2 + (flag ? 2 : 0);
		const int from = pl ? x1 - n + 1 : x0;
		for (int x = std::min(x0, from); x <= std::max(x1, from + n - 1); x++) { put(x, ROW, blank); put(x, ROW + 1, blank2); }
		auto glyph = [](char ch) { const int c = std::clamp(int(ch), 0x20, 0x5f) - 0x20; return uint16_t(0x9b00 + (c >> 4) * 0x20 + (c & 15)); };
		put(from, ROW, 0x9b03); put(from, ROW + 1, 0x9b13);
		const int text = from + 1 + (flag && !pl ? 2 : 0), flag_at = pl ? from + 1 + int(name.size()) : from + 1;
		if (flag) {
			for (int k = 0; k < 2; k++) { put(flag_at + k, ROW, glyph(' ')); put(flag_at + k, ROW + 1, uint16_t(glyph(' ') + 0x10)); }
			m_hud_flag_x[pl] = m_ext + 8 * flag_at;
			m_hud_flag_y[pl] = 8 * ROW;
		}
		for (int i = 0; i < int(name.size()); i++) { put(text + i, ROW, glyph(name[i])); put(text + i, ROW + 1, uint16_t(glyph(name[i]) + 0x10)); }
		put(from + n - 1, ROW, 0x9b04); put(from + n - 1, ROW + 1, 0x9b14);
	}
	if (names && undo) hud_edges(*undo);
}

// The font's letters carry the box's edges in their own tiles (a dark line along the top of the upper tile, a light
// one along the bottom of the lower), its space and its signs ('-', '.') not: in netplay's names (and the flag's
// cells, spaces) the box's lines broke under them. Each of those in the names' cells is shown as a copy of itself with
// a letter's top and bottom lines, put in the tiles of a character in the font no tile on the screen shows, for
// this picture only (the undo puts the tile graphics back too: entries with CHAR_UNDO set).
void model1::hud_edges(std::vector<std::pair<uint32_t, uint16_t>> &undo)
{
	constexpr int ROW = 6, W = 64;
	constexpr uint32_t FONT = 0x1b00, A = FONT + 2 * 0x20 + 1;   // (the font's tiles; 'A''s upper tile)
	auto font_char = [](uint32_t code) { return code >= FONT && code < FONT + 0x80 && (code & 0x10) == 0; };   // (an upper tile)
	auto words = [&](uint32_t code) { return code * 16; };
	auto edged = [&](uint32_t code) {   // (its upper tile's top line and its lower's bottom line as a letter's)
		for (int w = 0; w < 2; w++)
			if (m_tiles.char_r(words(code) + w) != m_tiles.char_r(words(A) + w) || m_tiles.char_r(words(code + 0x10) + 14 + w) != m_tiles.char_r(words(A + 0x10) + 14 + w)) return false;
		return true;
	};
	// the font's characters any tile shows now (the names' included)
	bool shown[0x80] = {};
	for (uint32_t e = 0; e < 0x4000; e++) {
		const uint32_t code = m_tiles.tile_code(m_tiles.tile_r(e));
		if (code >= FONT && code < FONT + 0x80) shown[code - FONT] = true;
	}
	int spare = 0x7f;
	auto next_spare = [&]() -> int {
		for (; spare >= 0; spare--)
			if (!(spare & 0x10) && !shown[spare] && !shown[spare + 0x10]) return FONT + spare--;
		return -1;
	};
	std::map<uint32_t, uint32_t> copies;   // (a character's upper tile: its edged copy's)
	for (int pl = 0; pl < 2; pl++) {
		if (m_hud_flag_x[pl] < 0 && m_hud_names[pl].empty()) continue;
		for (int x = pl ? 32 : 0; x < (pl ? W : 32); x++) {
			const uint16_t e = m_tiles.tile_r(uint32_t(ROW * W + x)), e2 = m_tiles.tile_r(uint32_t((ROW + 1) * W + x));
			const uint32_t code = m_tiles.tile_code(e);
			if (!font_char(code) || m_tiles.tile_code(e2) != code + 0x10 || code == FONT + 3 || code == FONT + 4 || edged(code)) continue;   // (the frame's ends, 1b03 and 1b04, as they are)
			auto it = copies.find(code);
			if (it == copies.end()) {
				const int to = next_spare();
				if (to < 0) return;
				for (uint32_t t = 0; t < 2; t++)
					for (uint32_t w = 0; w < 16; w++) {
						uint16_t v = m_tiles.char_r(words(code + 0x10 * t) + w);
						if (t == 0 && w < 2) v = m_tiles.char_r(words(A) + w);
						if (t == 1 && w >= 14) v = m_tiles.char_r(words(A + 0x10) + w);
						const uint32_t o = words(uint32_t(to) + 0x10 * t) + w;
						if (m_tiles.char_r(o) == v) continue;
						undo.push_back({ CHAR_UNDO | o, m_tiles.char_r(o) });
						m_tiles.char_w(o, v, 0xffff);
					}
				it = copies.emplace(code, uint32_t(to)).first;
			}
			const uint16_t keep = uint16_t(e & ~0x3fff);   // (the entry's other bits: its colour's from the code's)
			undo.push_back({ uint32_t(ROW * W + x), e });
			m_tiles.tile_w(uint32_t(ROW * W + x), uint16_t(keep | it->second), 0xffff);
			undo.push_back({ uint32_t((ROW + 1) * W + x), e2 });
			m_tiles.tile_w(uint32_t((ROW + 1) * W + x), uint16_t((e2 & ~0x3fff) | (it->second + 0x10)), 0xffff);
		}
	}
}

// The ranking (backup RAM 40f800: 18 entries of 10 bytes, the time, 0, the character's number,
// the initials, 0) keeps a character by number: a random fighter or a secret character, in
// Akira's place, went in as AKIRA. Each entry's name is kept here, moving with it as entries go
// in below and above: an entry that comes in (as the name entry starts, its initials then typed
// in) with Akira's number gets the name of the random fighter or secret character in a player's
// block. The ranking screen's CHARACTER column (tile layer page 0: each letter 2 x 3 tiles, A at
// 8546 and 0 at 84e0 a letter 6 on; the rank's digits at x 3 and 5, the character from x 45) is
// spelt with it on rows whose rank has one.
void model1::ranking_frame()
{
	if (m_ram_a.size() < RANKING + RANK_ENTRIES * RANK_ENTRY) return;
	const uint8_t *now = &m_ram_a[RANKING];
	constexpr uint32_t N = RANK_ENTRIES, E = RANK_ENTRY;
	if (!m_rank_seen) { memcpy(m_rank_prev, now, sizeof m_rank_prev); m_rank_seen = true; }
	if (memcmp(m_rank_prev, now, sizeof m_rank_prev)) {
		// each entry now: the same entry before (moved, or its initials being typed in at its place), or new
		auto same = [&](const uint8_t *a, const uint8_t *b, bool initials) { return !memcmp(a, b, 6) && (!initials || !memcmp(a + 6, b + 6, 4)); };
		std::string names[N];
		bool changed = false;
		for (uint32_t j = 0; j < N; j++) {
			const uint8_t *e = now + E * j;
			int from = -1;
			for (uint32_t i = 0; i < N && from < 0; i++) if (same(m_rank_prev + E * i, e, true)) from = int(i);
			if (from < 0 && same(m_rank_prev + E * j, e, false)) from = int(j);
			if (from >= 0) { names[j] = m_rank_names[from]; continue; }
			changed = true;
			for (int pl = 0; pl < 2 && names[j].empty(); pl++) {
				const uint32_t o = 0x6b0 + 0x1800 * pl;
				const uint32_t rec = uint32_t(m_ram_a[o] | m_ram_a[o + 1] << 8 | m_ram_a[o + 2] << 16 | uint32_t(m_ram_a[o + 3]) << 24);
				// (the entry's character number the record's: Akira's for a random fighter, a secret
				// character's its own (Jeff 09: VF's name for it WILLIE))
				if (rec + RECORD_SIZE > m_regions.maincpu.size() || m_regions.maincpu[rec + 0x0c] != e[5]) continue;
				if (m_random_fighter) { if (auto it = m_random_names.find(rec); it != m_random_names.end() && (rec == 0xfd4f99 || rec == 0xfd3f6f)) names[j] = it->second; }
				if (auto it = m_secret_names.find(rec); names[j].empty() && it != m_secret_names.end()) names[j] = it->second;
				if (names[j].empty() && rec && (rec == m_secret_recs[0] || rec == m_secret_recs[1])) names[j] = record_name(rec);
			}
		}
		// (a named entry's own bytes changed too: its initials typed in after it came in, saved again
		// so ranking_names.txt holds the entry as the backup does)
		for (uint32_t j = 0; j < N; j++)
			changed = changed || names[j] != m_rank_names[j] || (!names[j].empty() && memcmp(m_rank_prev + E * j, now + E * j, E));
		for (uint32_t j = 0; j < N; j++) m_rank_names[j] = names[j];
		memcpy(m_rank_prev, now, sizeof m_rank_prev);
		if (changed) m_rank_version++;
	}
	bool any = false;
	for (const auto &n : m_rank_names) any = any || !n.empty();
	if (!any) return;
	// (a name of up to 8 letters at the column's own place; a longer one, up to 10, from further left to
	// the same end, in the gap after the time (x 39-44 empty): from 45 the last two were cut off)
	constexpr int W = 64, LETTERS = 0x8546, DIGITS = 0x84e0, NAME_X = 45, NAME_END = 61, LONG_X = 41;
	auto at = [&](int x, int y) { return m_tiles.tile_r(uint32_t(y * W + x)); };
	auto put = [&](int x, int y, uint16_t v) { if (at(x, y) != v) m_tiles.tile_w(uint32_t(y * W + x), v, 0xffff); };
	auto digit = [&](uint16_t v) { const int k = int(v) - DIGITS; return k >= 0 && k < 60 && k % 6 == 0 ? k / 6 : -1; };
	auto letter = [&](uint16_t v) { const int k = int(v) - LETTERS; return k >= 0 && k < 26 * 6 && k % 6 == 0; };
	auto glyph = [&](uint16_t v) { const int k = int(v) - LETTERS; return k >= 0 && k < 26 * 6; };   // (any of a letter's 6 tiles)
	for (int y = 0; y + 2 < 64; y++) {
		// a ranking row: its rank (a digit at x 5, another at 3 for 10 on), the minutes' last digit
		// at 25, a letter at 45 (the character's name)
		const int d5 = digit(at(5, y)), d3 = digit(at(3, y));
		if (d5 < 0 || digit(at(25, y)) < 0 || !(letter(at(NAME_X, y)) || at(NAME_X, y) == 0x20)) continue;
		const int rank = (d3 > 0 ? d3 * 10 : 0) + d5;
		// (a long name's start, left on the row as the page turned to another rank's: cleared)
		for (int x = LONG_X; x < NAME_X; x++) for (int r = 0; r < 3; r++) if (glyph(at(x, y + r))) put(x, y + r, 0x20);
		if (rank < 1 || rank > int(N) || now[E * (rank - 1) + 5] != 0x0b) continue;
		const std::string &name = m_rank_names[rank - 1];
		if (name.empty()) continue;
		int x = std::max(LONG_X, std::min(NAME_X, NAME_END - 2 * int(name.size())));
		for (char ch : name) {
			if (x + 1 >= NAME_END) break;
			if (ch >= 'A' && ch <= 'Z') {
				const uint16_t g = uint16_t(LETTERS + (ch - 'A') * 6);
				for (int r = 0; r < 3; r++) { put(x, y + r, uint16_t(g + 2 * r)); put(x + 1, y + r, uint16_t(g + 2 * r + 1)); }
			} else for (int r = 0; r < 3; r++) { put(x, y + r, 0x20); put(x + 1, y + r, 0x20); }
			x += 2;
		}
		for (; x < NAME_END; x++) for (int r = 0; r < 3; r++) put(x, y + r, 0x20);
		y += 2;
	}
}

std::string model1::ranking_names() const
{
	std::string out;
	if (m_ram_a.size() < RANKING + RANK_ENTRIES * RANK_ENTRY) return out;
	for (uint32_t j = 0; j < RANK_ENTRIES; j++) {
		if (m_rank_names[j].empty()) continue;
		char hex[2 * RANK_ENTRY + 1];
		for (uint32_t b = 0; b < RANK_ENTRY; b++) snprintf(hex + 2 * b, 3, "%02x", m_ram_a[RANKING + RANK_ENTRY * j + b]);
		out += std::string(hex) + " " + m_rank_names[j] + "\n";
	}
	return out;
}

// (each line's entry found in the ranking as it is, else one with its time and character (a file
// saved before the entry's initials were typed in, as before 1.4.0's fix): an entry not there any
// more is dropped)
void model1::set_ranking_names(const std::string &text)
{
	if (m_ram_a.size() < RANKING + RANK_ENTRIES * RANK_ENTRY) return;
	for (auto &n : m_rank_names) n.clear();
	std::vector<std::pair<std::array<uint8_t, RANK_ENTRY>, std::string>> left;
	size_t p = 0;
	while (p < text.size()) {
		size_t e = text.find('\n', p);
		if (e == std::string::npos) e = text.size();
		const std::string line = text.substr(p, e - p);
		p = e + 1;
		if (line.size() < 2 * RANK_ENTRY + 2 || line[2 * RANK_ENTRY] != ' ') continue;
		uint8_t entry[RANK_ENTRY];
		bool ok = true;
		for (uint32_t b = 0; b < RANK_ENTRY && ok; b++) {
			char *end;
			const std::string h = line.substr(2 * b, 2);
			entry[b] = uint8_t(strtoul(h.c_str(), &end, 16));
			ok = *end == 0;
		}
		std::string name;
		for (char ch : line.substr(2 * RANK_ENTRY + 1)) if ((ch >= 'A' && ch <= 'Z') || ch == '?') name += ch;
		if (!ok || name.empty()) continue;
		bool found = false;
		for (uint32_t j = 0; j < RANK_ENTRIES && !found; j++)
			if (m_rank_names[j].empty() && !memcmp(&m_ram_a[RANKING + RANK_ENTRY * j], entry, RANK_ENTRY)) { m_rank_names[j] = name.substr(0, 10); found = true; }
		if (!found) { std::array<uint8_t, RANK_ENTRY> a; memcpy(a.data(), entry, RANK_ENTRY); left.push_back({ a, name.substr(0, 10) }); }
	}
	for (const auto &[entry, name] : left)
		for (uint32_t j = 0; j < RANK_ENTRIES; j++)
			if (m_rank_names[j].empty() && !memcmp(&m_ram_a[RANKING + RANK_ENTRY * j], entry.data(), 6)) { m_rank_names[j] = name; m_rank_version++; break; }
	memcpy(m_rank_prev, &m_ram_a[RANKING], sizeof m_rank_prev);
	m_rank_seen = true;
}

// Each fighter's head is modelled for its own animation: the head bone of Akira's sits leant back
// (his neck model slants forward to meet it), Pai's and Sarah's upright, and so on. A head on
// another's body would nod with that body's lean: Pai's on Akira's looks up. head_lean is how far
// back a record's own animation holds the head (degrees, from the cast's head and neck models),
// and each head the two players wear is turned by its own lean less the body's.
float model1::head_lean(uint32_t rec)
{
	static const char *const test = getenv("VF_LEAN");   // testing: rec:deg,... (hex record: degrees)
	if (const char *e = test)
		for (const char *p = e; *p;) {
			const uint32_t r = uint32_t(strtoul(p, (char **)&p, 16));
			const float d = *p == ':' ? strtof(p + 1, (char **)&p) : 0;
			if (r == rec) return d;
			while (*p == ',') p++;
		}
	// (each head on Akira's body in the intro's close-up, against his own: tools/vftest VF_FORCE_HEAD
	// and VF_LEAN; the rest sit as his, 0)
	switch (rec) {
	case 0xfd3987: case 0xfd40e5: return -8;    // Pai
	case 0xfd3811: case 0xfd43d9: return -6;    // Sarah
	case 0xfd3b05: case 0xfd4263: return -10;   // Jeffry
	default: return 0;
	}
}

// A player's strands (ponytail, braids, ribbon): the list at f80800 (random_fighter_strands) by the
// slot its block holds (-0x4c); their data runs to the next strands' (the models in it, 2dxxxx):
// their polygons' addresses
void model1::strand_polys(int pl, std::vector<uint32_t> &out) const
{
	const auto &rom = m_regions.maincpu;
	if (!m_strands_moved || m_ram_a.size() < 0x1f00) return;
	auto rd = [&](const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); };
	auto poly_of = [&](uint32_t m) -> uint32_t { return (m >> 16) == 0x2d && m + 12 <= rom.size() ? rd(&rom[m + 8]) & 0x7fffff : 0; };
	const uint8_t slot = m_ram_a[0x700 + 0x1800 * pl - 0x4c];
	uint32_t sp = 0;
	for (uint32_t i = STRAND_LIST_OURS; i + 5 <= rom.size() && rom[i] != 0xff && i < STRAND_LIST_OURS + 26 * 5; i += 5)
		if (rom[i] == slot) { sp = rd(&rom[i + 1]); break; }
	if ((sp >> 16) != 0xfd) return;
	uint32_t end = sp + 0x300;
	for (size_t i = 0; i + 5 <= m_strands_orig.size(); i += 5)
		if (const uint32_t q = rd(&m_strands_orig[i + 1]); q > sp && q < end) end = q;
	for (uint32_t a = sp; a + 4 <= end && a + 4 <= rom.size(); a++)
		if (const uint32_t m = rd(&rom[a]); poly_of(m)) out.push_back(poly_of(m));
}

// A strand's points stay where they were when its fighter is put somewhere else at once (a
// replay's start: the loser lying on the floor one frame, standing the next): its first point goes
// with the head and the rest are yanked after it, up past the head, and with a random fighter's
// strands (another's lengths and weights) they stay standing there for a while (VF's own do it
// for a moment). A chain whose first point jumps without the game starting it afresh (its points
// moving: the game's start puts them at rest) is put back to hang as it starts, from its first
// point. Each player's strands are a task of the game's (resumed at ffc57b, its address at -4c,
// its player -7c, its data -48, its points from -1a: 0x18 bytes each, place then speed, floats),
// the data's chains in groups (a group: 0x46 bytes then its chains; a chain: 0xd bytes, its
// count of points, then 0x18 bytes each, the place at +0c; player 2's mirrored, x and z).
void model1::random_fighter_strands_settle()
{
	const auto &rom = m_regions.maincpu;
	if (!m_strands_moved || m_ram_a.size() < 0x10000) return;
	auto rd = [&](const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); };
	auto rdf = [&](const uint8_t *p) { const uint32_t v = rd(p); float f; memcpy(&f, &v, 4); return f; };
	auto wrf = [&](uint8_t *p, float f) { uint32_t v; memcpy(&v, &f, 4); for (int b = 0; b < 4; b++) p[b] = uint8_t(v >> (8 * b)); };
	constexpr uint32_t START = 0xffc49d, RESUME = 0xffc57b, IDLE = 0xffc577;
	auto listed = [&](uint8_t slot) -> uint32_t {   // (the strands our list has for a slot, 0: none)
		for (uint32_t a = STRAND_LIST_OURS; a + 5 <= rom.size() && rom[a] != 0xff && a < STRAND_LIST_OURS + 26 * 5; a += 5)
			if (rom[a] == slot) return rd(&rom[a + 1]);
		return 0;
	};
	std::map<uint32_t, std::vector<float>> now;
	for (uint32_t i = 0x4c + 0x7c; i + 0x200 < m_ram_a.size(); i += 2) {
		const uint32_t at = rd(&m_ram_a[i]);
		if (at != RESUME && at != IDLE) continue;
		const uint32_t task = i + 0x4c, data = rd(&m_ram_a[task - 0x48]);
		const int pl = m_ram_a[task - 0x7c] & 1;
		m_ram_a[task - 0x60] = data == KAGE_TAILS ? 2 : 0;   // (shrunk as Kage's: set_strands_moved)
		// The strands are looked up as a match loads; the attract's next demo starts without that,
		// so a fighter made afresh since (another head) kept the last one's strands (one whose rest
		// points up stood straight up on the new head as the round began). Started again: the
		// lookup, by the slot, in the list as it is now.
		if (const uint32_t want = listed(m_ram_a[0x700 + 0x1800 * pl - 0x4c]); at == IDLE ? want != 0 : want != data) {
			for (int b = 0; b < 4; b++) m_ram_a[i + b] = uint8_t(START >> (8 * b));
			continue;
		}
		if (at != RESUME || (data >> 16) != 0xfd || data + 0x400 > rom.size()) continue;
		const bool mirrored = pl != 0;
		const auto was = m_strand_roots.find(task);
		auto &roots = now[task];
		uint32_t q = data + 4, pt = task - 0x1a;
		const int groups = rom[q++];
		for (int g = 0; g < groups && q < data + 0x400; g++) {
			q += 0x46;
			const int chains = rom[q++];
			for (int c = 0; c < chains && q < data + 0x400; c++) {
				q += 0xd;
				const int n = rom[q++];
				if (n < 1 || pt + 0x18 * uint32_t(n) > m_ram_a.size()) return;
				uint8_t *p = &m_ram_a[pt];
				const float x = rdf(p), y = rdf(p + 4), z = rdf(p + 8);
				bool moving = false;
				for (int j = 0; j < n && !moving; j++)
					moving = rd(p + 0x18 * j + 0xc) || rd(p + 0x18 * j + 0x10) || rd(p + 0x18 * j + 0x14);
				const size_t k = roots.size();
				roots.insert(roots.end(), { x, y, z, moving ? 0.0f : 1.0f });
				if (n > 1 && was != m_strand_roots.end() && k + 4 <= was->second.size()) {
					const float dx = x - was->second[k], dy = y - was->second[k + 1], dz = z - was->second[k + 2];
					// (or its first step since the game started it: its rest places are for the
					// strands' own fighter where a round starts, the first point then goes to the head
					// where it is, another's height, a demo's own start, and yanked the rest up)
					const bool started = was->second[k + 3] != 0;
					if (moving && dx * dx + dy * dy + dz * dz > (started ? 0.0025f : 1.0f)) {
						const float s = mirrored ? -1.0f : 1.0f;
						const float x0 = rdf(&rom[q + 0xc]), y0 = rdf(&rom[q + 0x10]), z0 = rdf(&rom[q + 0x14]);
						for (int j = 1; j < n; j++) {
							const uint8_t *d = &rom[q + 0x18 * j];
							wrf(p + 0x18 * j, x + s * (rdf(d + 0xc) - x0));
							wrf(p + 0x18 * j + 4, y + (rdf(d + 0x10) - y0));
							wrf(p + 0x18 * j + 8, z + s * (rdf(d + 0x14) - z0));
						}
						for (int j = 0; j < n; j++) for (int o = 0xc; o < 0x18; o += 4) wrf(p + 0x18 * j + o, 0);
					}
				}
				// (the strands spring back to a shape set by the head's turn, not by weight: on another's
				// body, whose head sits otherwise, that shape can stand up, and a braid stood over the
				// head through a whole pose. A point above its chain's first one falls a little more)
				if (n > 1 && moving)
					for (int j = 1; j < n; j++) {
						uint8_t *pj = p + 0x18 * j;
						if (rdf(pj + 4) > y - 0.02f * float(j)) wrf(pj + 0x10, rdf(pj + 0x10) - 0.012f);
					}
				q += 0x18 * uint32_t(n);
				pt += 0x18 * uint32_t(n);
			}
		}
	}
	m_strand_roots = std::move(now);
}

void model1::random_fighter_heads()
{
	m_head_turn.clear();
	m_head_turn2.clear();
	m_torso_poly[0] = m_torso_poly[1] = 0;
	if (!m_random_fighter || m_ram_a.size() < 0x1f00) return;
	random_cast &c = random_fighter_cast();
	auto &rom = m_regions.maincpu;
	auto rd = [&](const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); };
	auto lean_of_head = [&](uint32_t head) -> float {   // (the lean of the record whose own head it is)
		for (const auto &[r, b] : c.orig)
			if (rd(&b[0x72]) == head) return head_lean(r);
		return 0;
	};
	// a model (12 bytes in the 2dxxxx table: its poly address at +8) -> its poly address, or 0
	auto poly_of = [&](uint32_t m) -> uint32_t { return (m >> 16) == 0x2d && m + 12 <= rom.size() ? rd(&rom[m + 8]) & 0x7fffff : 0; };
	// a player's strands (ponytail, braids, ribbon): the list at f80800 (random_fighter_strands) by the
	// slot its block holds (-0x4c); their data runs to the next strands' (the models in it, 2dxxxx)
	for (int pl = 0; pl < 2; pl++) {
		const uint32_t rec = rd(&m_ram_a[0x700 + 0x1800 * pl - 0x50]);
		std::vector<std::pair<uint32_t, head_style>> now;
		// At the select (shown when a player picks there as ever): each side's portrait, the record
		// its cursor is on (the character, 40749c and 0x80 on, through its side's table: fd2f1b,
		// fd2f24), a random fighter's head in its hue (not turned, no neck: the portrait's own
		// framing), anyone else's as VF has it, so a head both show is told apart (drawn in order)
		if (m_select_seen) {
			static const uint32_t SIDE[2] = { 0xfd2f1b, 0xfd2f24 };
			const uint8_t ch = m_ram_a[0x749c + 0x80 * pl];
			const uint32_t slot = ch < 9 && SIDE[pl] + 9 <= rom.size() ? rom[SIDE[pl] + ch] : 0xff;
			const uint32_t shown = slot < 26 ? rd(&rom[0xfd2e8f + 4 * slot]) : 0;
			const uint32_t list = shown && shown + 0x76 <= rom.size() ? rd(&rom[shown + 0x72]) : 0;
			const auto hue = m_random_names.count(shown) ? m_random_hue.find(shown) : m_random_hue.end();
			if ((list >> 16) == 0xfd && list + 0x224 <= rom.size())
				for (uint32_t k = 0; k < 121; k++) {
					const uint32_t m = poly_of(rd(&rom[list + 0x40 + 4 * k]));
					if (!m) break;
					head_style st;
					if (hue != m_random_hue.end()) st.hue = hue->second;
					now.push_back({ m, st });
				}
			if (auto sm = m_portrait_strand_models.find(shown); hue != m_random_hue.end() && sm != m_portrait_strand_models.end())   // (its strands: random_fighter_portrait_strands)
				for (uint32_t m : sm->second) { head_style ss; ss.hue = hue->second; ss.strand = true; now.push_back({ m, ss }); }
			m_head_last[pl] = std::move(now);
			m_head_lag_left[pl] = 0;
			continue;
		}
		// (a random fighter's only: the computer's own characters, with random opponents off, as VF has them)
		auto it = m_random_names.count(rec) ? c.orig.find(rec) : c.orig.end();
		const uint32_t head = it != c.orig.end() ? rd(&rom[rec + 0x72]) : 0;
		// (made on Akira's record, random_fighter_make: his head its own, his lean its body's)
		const uint32_t lay = rec == 0xfd3f6f ? rec : 0xfd4f99;
		const uint32_t own = it == c.orig.end() ? 0 : c.orig.count(lay) ? rd(&c.orig.at(lay)[0x72]) : rd(&it->second[0x72]);
		// (its torso: the neck's fit, model1_video.cpp; anyone else's too, the torsos told apart as drawn
		// when both wear the same one: else the other's, drawn second, was the random one's fit)
		m_torso_poly[pl] = rec < rom.size() && rom.size() - rec >= 0x16 ? poly_of(rd(&rom[rec + 0x12])) : 0;
		if (it != c.orig.end() && (head >> 16) == 0xfd && head + 0x224 <= rom.size()) {
			// (the head's lean less the lean of the character whose move is playing: the moves come
			// from three characters; a move several of different leans share, as the head's own)
			const float hl = lean_of_head(head);
			const uint16_t move = uint16_t(m_ram_a[0x6c8 + 0x1800 * pl] | m_ram_a[0x6c9 + 0x1800 * pl] << 8);
			// (a move none of the tables has, a move's own follow-on, or one characters of different
			// leans share: the last known lean held, not the head's own, which turned the head back and
			// forth within a move. The turn eases toward its aim: it looked up, then down again)
			// (a move one character has: that one's lean; one several have, the stance among them:
			// the fighter's own character's, its record's (the shared moves play as the record's own:
			// after a move the head kept that move's lean, or the stance's group's, looking up); a
			// move no table has, a move's own follow-on: the last lean held)
			float lean = NAN;
			const auto ml = c.move_lean.find(move);
			if (ml != c.move_lean.end()) lean = std::isnan(ml->second) ? head_lean(lay) : ml->second;
			if (rec != m_head_rec[pl]) m_lean_body[pl] = head_lean(lay);
			if (!std::isnan(lean)) m_lean_body[pl] = lean;
			// (one lean a fighter, its record's: by the character whose move was playing, the lean
			// changed at every change of move and the head nodded to it over a few frames, looking up
			// 10 degrees in a move and back down as the stance came, up to 89 times a fight)
			static const bool by_move = getenv("VF_LEAN_BY_MOVE") != nullptr;   // testing: as it was
			const float body = by_move && m_random_moves ? (!std::isnan(m_lean_body[pl]) ? m_lean_body[pl] : hl) : head_lean(lay);
			const float aim = head == own && !m_random_moves ? 0.0f : hl - body;
			m_lean_turn[pl] = rec != m_head_rec[pl] ? aim : m_lean_turn[pl] + (aim - m_lean_turn[pl]) * 0.35f;
			const float d = m_lean_turn[pl] * float(M_PI / 180);
			const auto hue = m_random_hue.find(rec);
			head_style st{ std::cos(d), std::sin(d), hue != m_random_hue.end() ? hue->second : -1.0f };
			st.pl = pl;
			if (std::fabs(d) < 1e-4f) st.c = 1, st.s = 0;
			if (st.s != 0 || st.hue >= 0)
				for (uint32_t k = 0; k < 121; k++) {   // (the head's models: its list after 16 words)
					const uint32_t m = poly_of(rd(&rom[head + 0x40 + 4 * k]));
					if (!m) break;
					now.push_back({ m, st });
				}
			if (st.hue >= 0) {   // (the strands in the hair's hue, not turned: they hang as they swing)
				std::vector<uint32_t> strands;
				strand_polys(pl, strands);
				for (uint32_t m : strands) { head_style ss{ 1, 0, st.hue }; ss.pl = pl; ss.strand = true; now.push_back({ m, ss }); }
			}
		}
		// (Dural's head, +16 with no head list: a metal of its own, from its hue: chrome, gold,
		// bronze, copper, blue steel, gunmetal)
		if (it != c.orig.end() && (head == 0 || (head >> 16) == 0xf8) && own) {
			static constexpr float METALS[][3] = { { 0, 0, 1 }, { 0.13f, 0.55f, 1 }, { 0.08f, 0.6f, 0.85f }, { 0.04f, 0.65f, 0.9f }, { 0.6f, 0.35f, 1 }, { 0.6f, 0.08f, 0.6f } };
			const auto hue = m_random_hue.find(rec);
			const float *mt = METALS[hue != m_random_hue.end() ? std::min(5, int(hue->second * 6)) : 0];
			head_style ms{ 1, 0, mt[0], mt[1], mt[2] };
			ms.pl = pl;
			if (const uint32_t m = poly_of(rd(&rom[rec + 0x16]))) now.push_back({ m, ms });
		}
		// (anyone else, a computer's own character: its head's models and strands as VF has them, so a
		// random fighter wearing the same head (Jeffry's on both) styles only its own, the first or
		// second drawn; with only the random one's listed, both were drawn in its hue and turn)
		if (it == c.orig.end() && rec) {
			const uint32_t own_head = rec < rom.size() && rom.size() - rec >= 0x76 ? rd(&rom[rec + 0x72]) : 0;
			if ((own_head >> 16) == 0xfd && own_head + 0x224 <= rom.size())
				for (uint32_t k = 0; k < 121; k++) {
					const uint32_t m = poly_of(rd(&rom[own_head + 0x40 + 4 * k]));
					if (!m) break;
					now.push_back({ m, head_style{} });
				}
			else if (own_head == 0 && rec < rom.size() && rom.size() - rec >= 0x1a)
				if (const uint32_t m = poly_of(rd(&rom[rec + 0x16]))) now.push_back({ m, head_style{} });
			std::vector<uint32_t> strands;
			strand_polys(pl, strands);
			for (uint32_t m : strands) { head_style ss; ss.strand = true; now.push_back({ m, ss }); }
		}
		if (rec != m_head_rec[pl]) { m_head_lag[pl] = std::move(m_head_last[pl]); m_head_lag_left[pl] = 4; m_head_rec[pl] = rec; }
		m_head_last[pl] = std::move(now);
	}
	// (player 2's style for a model player 1 has too goes aside: each is drawn with its own)
	std::unordered_map<uint32_t, head_style> mine[2];
	for (int pl = 0; pl < 2; pl++) {
		for (const auto &[m, st] : m_head_last[pl]) mine[pl].emplace(m, st);
		// (the record before's, still on screen, over the new one's)
		if (m_head_lag_left[pl] > 0) {
			m_head_lag_left[pl]--;
			for (const auto &[m, st] : m_head_lag[pl]) mine[pl][m] = st;
		}
	}
	m_head_turn = mine[0];
	for (const auto &[m, st] : mine[1])
		if (m_head_turn.count(m)) m_head_turn2[m] = st;
		else m_head_turn.emplace(m, st);
}

// TESTONLY (VF_STRANDLOG): each strands task as it stands: where it resumes, its player and data,
// its slot's, its first chain's points and whether they move
void model1::strand_log()
{
	auto rd = [&](const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); };
	auto rdf = [&](const uint8_t *p) { const uint32_t v = rd(p); float f; memcpy(&f, &v, 4); return f; };
	if (m_ram_a.size() < 0x10000) return;
	for (uint32_t i = 0x4c + 0x7c; i + 0x200 < m_ram_a.size(); i += 2) {
		const uint32_t at = rd(&m_ram_a[i]);
		if (at != 0xffc57b && at != 0xffc577 && at != 0xffc49d) continue;
		const uint32_t task = i + 0x4c, data = rd(&m_ram_a[task - 0x48]);
		const int pl = m_ram_a[task - 0x7c] & 1;
		fprintf(stderr, "STRAND f%lld task %05x at %06x pl%d data %06x slot %02x rec %06x blk50 %.3f len %.3f:", (long long)m_frame, task, at, pl, data,
				m_ram_a[0x700 + 0x1800 * pl - 0x4c], rd(&m_ram_a[0x700 + 0x1800 * pl - 0x50]), rdf(&m_ram_a[0x700 + 0x1800 * pl + 0x50]), rdf(&m_ram_a[task - 0x40]));
		// (any chain whose points stand above its root: how far, the most)
		if (at == 0xffc57b && (data >> 16) == 0xfd) {
			const auto &rom = m_regions.maincpu;
			uint32_t q = data + 4, pt = task - 0x1a;
			float most = 0;
			int worst = -1;
			const int groups = rom[q++];
			for (int g = 0; g < groups && g < 8; g++) {
				q += 0x46;
				const int chains = rom[q++];
				for (int c = 0; c < chains && c < 16; c++) {
					q += 0xd;
					const int n = rom[q++];
					if (n < 1 || n > 32) break;
					const float y0 = rdf(&m_ram_a[pt + 4]);
					for (int j = 1; j < n; j++) { const float up = rdf(&m_ram_a[pt + 0x18 * j + 4]) - y0; if (up > most) { most = up; worst = g * 16 + c; } }
					q += 0x18 * uint32_t(n);
					pt += 0x18 * uint32_t(n);
				}
			}
			fprintf(stderr, " up %.3f (chain %d)", most, worst);
		}
		for (int j = 0; j < 4; j++) {
			const uint8_t *p = &m_ram_a[task - 0x1a + 0x18 * j];
			fprintf(stderr, " (%.2f %.2f %.2f%s)", rdf(p), rdf(p + 4), rdf(p + 8), rd(p + 0xc) || rd(p + 0x10) || rd(p + 0x14) ? " m" : "");
		}
		fprintf(stderr, "\n");
	}
}

// The select's covers: random_fighter_frame's, or each frame with a Secret character alone
void model1::select_cover(bool select)
{
	// A select that shows (a player at it picks as ever): a random fighter's name there (AKIRA, tile
	// layer 0, rows 31-33 under its portrait: a strip of tiles each side, 9e03 on and 9fd3 on, the
	// name drawn into their pictures with the gold) gilded over with the plain gold's own three
	// tiles a row (player 1's left of its strip, 9e00, 9e14, 9e28 on; player 2's the first three of its
	// strip's right end, 9fe5, 9ffd, a015 on, which no name reaches: the gold's shade there), repeating
	if (!select || m_ram_a.size() <= 0x7600) return;
	for (int pl = 0; pl < 2; pl++) {
		if (!(m_ram_a[0x7483 + 0x80 * pl] & 0x08)) continue;
		const bool random = m_random_fighter && m_player_mode[pl];
		// (a secret character in Akira's place, the cursor on it: its name and profile where VF's are
		// Akira's: Takeru's name, and Rekka's, Score's, Aeon's and Dural's name and profile; Siba's
		// and Jeff's are their own, set_akira_slot)
		const bool secret = !random && m_akira_which > 0 && m_ram_a[0x749c + 0x80 * pl] == 0;
		if (!random && !secret) continue;
		const bool cover_name = random || m_akira_which >= 3, cover_profile = random || m_akira_which >= 4;
		// (nor its cursor on Akira's icon, the row's first: the blinking 1P / 2P frame, tiles over
		// blank ones (0020), columns 3-9, rows 37-44; left while a player picking as ever has its
		// cursor there too, whose it is then)
		const int other = pl ^ 1;
		if (random && !((m_ram_a[0x7483 + 0x80 * other] & 0x08) && !m_player_mode[other] && m_ram_a[0x749c + 0x80 * other] == 0))
			for (int y = 37; y <= 44; y++)
				for (int x = 3; x <= 9; x++)
					if (m_tiles.tile_r(uint32_t(y * 64 + x)) != 0x0020) m_tiles.tile_w(uint32_t(y * 64 + x), 0x0020, 0xffff);
		// (a one-player select: its profile, Akira's name, country, job..., the panel's text over its
		// plain tile, 9800, beside the portrait: player 1's rows 7-29 columns 27-58, player 2's rows
		// 6-33 columns 3-33; told by that tile at the panel's corners)
		{
			const int y0 = pl ? 6 : 7, y1 = pl ? 33 : 29, x0 = pl ? 3 : 27, x1 = pl ? 33 : 58;
			auto at = [&](int x, int y) { return m_tiles.tile_r(uint32_t(y * 64 + x)); };
			if (cover_profile && at(x0, y0) == 0x9800 && at(x1, y1) == 0x9800 && at(x1, y0) == 0x9800)
				for (int y = y0; y <= y1; y++)
					for (int x = x0; x <= x1; x++)
						if (at(x, y) != 0x9800) m_tiles.tile_w(uint32_t(y * 64 + x), 0x9800, 0xffff);
		}
		static const uint16_t GOLD[2][3] = { { 0x9e00, 0x9e14, 0x9e28 }, { 0x9fe5, 0x9ffd, 0xa015 } };
		for (int y = 31; y <= 33 && cover_name; y++)
			for (int x = pl ? 41 : 9; x <= (pl ? 52 : 19); x++) {   // (where the names are; the gold either side the game's)
				// (player 2's gold shades across: its left half from the three just left of the name,
				// its right half from the three just right)
				const uint16_t v = pl ? uint16_t(GOLD[1][y - 31] + (x < 47 ? -15 : 0) + (x - 41) % 3) : uint16_t(GOLD[0][y - 31] + (x - 4) % 3);
				if (m_tiles.tile_r(uint32_t(y * 64 + x)) != v) m_tiles.tile_w(uint32_t(y * 64 + x), v, 0xffff);
			}
	}
}

// A new player select: a new fighter (the select's own Akira icon, b080, comes into the tiles)
bool model1::select_showing() const
{
	for (uint32_t i = 0; i < 0x4000 - 8 * 64; i++)
		if (m_tiles.tile_r(i) == 0xb080 && m_tiles.tile_r(i + 1) == 0xb081 && m_tiles.tile_r(i + 64) == 0xb087) return true;
	return false;
}

// Dural's shine (her polygons' light mode: the game's light parameters with a specular term), for a
// random fighter made in her record (Random opponents' last fight): the most of her own models' polygons'
int model1::dural_lightmode()
{
	if (m_dural_lm != -2) return m_dural_lm;
	m_dural_lm = -1;
	random_cast &c = random_fighter_cast();
	const auto it = c.orig.find(0xfd369b);
	if (it == c.orig.end()) return m_dural_lm;
	auto &rom = m_regions.maincpu;
	const std::vector<uint8_t> &b = it->second;
	auto rd = [&](const uint8_t *q) { return uint32_t(q[0] | q[1] << 8 | q[2] << 16 | uint32_t(q[3]) << 24); };
	std::map<int, int> n;
	for (uint32_t off : { 0x0eu, 0x12u, 0x16u, 0x1au, 0x1eu, 0x26u, 0x2au, 0x32u, 0x36u, 0x3au, 0x3eu, 0x42u, 0x46u, 0x4au }) {
		if (off + 4 > b.size()) continue;
		const uint32_t m = rd(&b[off]);
		if ((m >> 16) != 0x2d || m + 12 > rom.size()) continue;
		const uint32_t p = rd(&rom[m + 8]) & 0x7fffff;
		for (uint32_t a = p + 6, k = 0; k < 4096 && (a + 1) * 4 <= m_regions.polygons.size() && (m_poly_rom[a] & 3); a += 10, k++) {
			const uint32_t f = m_poly_rom[a];
			n[int(((f >> 17) & 15) | ((f & 0x00400000) ? 0x80 : 0))]++;
		}
	}
	int most = 0;
	for (const auto &[lm, k] : n) if (k > most) { most = k; m_dural_lm = lm; }
	return m_dural_lm;
}

// Random opponents: every record but Akira's, a Secret character's and those the players picked
// (their character, -4B in their blocks, through player 1's side's table, fd2f1b: player 2's
// side's +9 in it) made random
// Random opponents: the records the computer's fighters are never made from: the Secret character's;
// a player's secret character's own (their copy in Akira's record reads its hands there, +5e/+62);
// and the ones the players picked at the last select
bool model1::cpu_keeps(uint32_t r) const
{
	if (r == m_secret_recs[0] || r == m_secret_recs[1] || r == m_cpu_human[0] || r == m_cpu_human[1]) return true;
	// (fought this run: kept as fought, for the ending's replays; not Akira's two, a player's at a select)
	if (m_run_on && m_fought.count(r) && r != 0xfd4f99 && r != 0xfd3f6f) return true;
	for (int pl = 0; pl < 2; pl++)
		if (m_random_fighter && m_player_mode[pl] == 3 && m_player_seed[pl] >= 1 && m_player_seed[pl] <= 7)
			if (r == SECRET[m_player_seed[pl]][0] || r == SECRET[m_player_seed[pl]][1]) return true;
	return false;
}

void model1::random_fighter_cpu()
{
	static constexpr uint32_t AKIRA[2] = { 0xfd4f99, 0xfd3f6f };
	m_cpu_pending = false;
	random_cast &c = random_fighter_cast();
	auto &rom = m_regions.maincpu;
	uint32_t human[2] = { 0, 0 };
	for (int pl = 0; pl < 2 && m_ram_a.size() > 0x7600; pl++) {
		if (!(m_ram_a[0x7483 + 0x80 * pl] & 0x10)) continue;   // (picked: in the game)
		const uint8_t ch = m_ram_a[0x6b5 + 0x1800 * pl];
		if (ch >= 18 || 0xfd2f1b + 18 > rom.size()) continue;
		const uint8_t slot = rom[0xfd2f1b + ch];
		if (slot < 26) human[pl] = uint32_t(rom[0xfd2e8f + 4 * slot] | rom[0xfd2e90 + 4 * slot] << 8 | rom[0xfd2e91 + 4 * slot] << 16 | uint32_t(rom[0xfd2e92 + 4 * slot]) << 24);
	}
	m_cpu_human[0] = human[0];
	m_cpu_human[1] = human[1];
	for (uint32_t r : c.recs)
		if (r != AKIRA[0] && r != AKIRA[1] && !cpu_keeps(r))
			random_fighter_make_named(r);
	// (and Akira's record no player has, a match's: the computer's Akira, else the real one; none
	// outside a match, the attract's demo keeping the players' settings, random_fighter_roll)
	const bool anyone = m_ram_a.size() > 0x7600 && ((m_ram_a[0x7483] | m_ram_a[0x7503]) & 0x08);
	for (uint32_t r : AKIRA)
		if (anyone && !cpu_keeps(r) && r != akira_record_of(0) && r != akira_record_of(1)) {
			const auto k = m_cpu_akira.find(r);
			if (!m_run_on || k == m_cpu_akira.end()) { random_fighter_make_named(r); continue; }
			// (a continue: the computer's Akira as fought this run, for the ending's replays)
			const cpu_kept &kk = k->second;
			memcpy(&rom[r], kk.bytes.data(), RECORD_SIZE);
			m_random_names[r] = kk.name;
			if (kk.has_hue) m_random_hue[r] = kk.hue;
			if (kk.has_outfit) { m_random_outfit[r] = kk.outfit; m_random_theme[r] = kk.theme; }
			if (kk.has_stance) m_stance_from[r] = kk.stance;
			if (kk.has_list) m_special_list[r] = kk.list;
			if (kk.voice >= 0) m_voice_of[r] = uint8_t(kk.voice);
			if (kk.has_move_ids) m_move_ids[r] = kk.move_ids;
		}
	random_fighter_strands();
	random_fighter_specials();
}

void model1::random_fighter_frame()
{
	const bool select = select_showing();
	if (!select && m_select_seen && m_cpu_pending) random_fighter_cpu();   // (the select over: the players' picks known)
	if (select && !m_select_seen) { m_rolling_at_select = true; random_fighter_roll(); m_rolling_at_select = false; m_pick_at = int64_t(m_frame); }
	if (select) random_fighter_picks();
	// (the camera for Akira's spot: the portrait shown there's, a random or secret fighter's. One
	// camera serves both sides' portraits of the spot: when they'd want two (a Dural-headed one
	// beside a Normal player's cursor on Akira, or two secret characters), Akira's own)
	if (select && m_akira_which == 0 && m_ram_a.size() > 0x7600) {
		static const uint32_t SIDE[2] = { 0xfd2f1b, 0xfd2f24 };
		auto &rom = m_regions.maincpu;
		int cam = -1;
		bool mixed = false;
		for (int pl = 0; pl < 2; pl++) {
			int want = -1;
			if (const uint32_t r = akira_record_of(pl)) want = akira_camera_for(r);
			else if (m_ram_a[0x7483 + 0x80 * pl] & 0x08) {   // (a Normal player's cursor on Akira's spot)
				const uint8_t ch = m_ram_a[0x749c + 0x80 * pl];
				const uint32_t slot = ch < 9 && SIDE[pl] + 9 <= rom.size() ? rom[SIDE[pl] + ch] : 0xff;
				const uint32_t at = 0xfd2e8f + 4 * slot;
				const uint32_t r = slot < 26 && at + 4 <= rom.size() ? uint32_t(rom[at] | rom[at + 1] << 8 | rom[at + 2] << 16 | uint32_t(rom[at + 3]) << 24) : 0;
				if (r == 0xfd4f99 || r == 0xfd3f6f) want = 0;
			}
			if (want < 0) continue;
			if (cam < 0) cam = want; else if (want != cam) mixed = true;
		}
		akira_camera(cam > 0 && !mixed ? cam : 0);
	}   // (at a select only: outside one, as the attract's demo, no one is in and it put Akira back)
	select_cover(select);
	random_fighter_heads();
	random_fighter_strands_settle();
	random_fighter_hud();
	random_fighter_build(select);
	// A random player gone (lost, not continuing) while the other plays on: their Akira record back to
	// Akira's own once it's off screen, so the computer's Akira on that side isn't them
	if (!select && m_random_fighter && m_ram_a.size() > 0x7600) {
		auto in = [&](int p) { return (m_ram_a[0x7483 + 0x80 * p] & 0x08) != 0; };
		auto block = [&](int p) { const uint32_t o = 0x6b0 + 0x1800 * uint32_t(p); return uint32_t(m_ram_a[o] | m_ram_a[o + 1] << 8 | m_ram_a[o + 2] << 16 | uint32_t(m_ram_a[o + 3]) << 24); };
		if (in(0) || in(1))
			for (const uint32_t r : { 0xfd4f99u, 0xfd3f6fu }) {
				const auto made = m_akira_made.find(r);
				if (made == m_akira_made.end() || made->second.first == 0) continue;
				if ((in(0) && m_akira_held[0] == r) || (in(1) && m_akira_held[1] == r) || block(0) == r || block(1) == r) continue;
				if (getenv("VF_RANDLOG")) fprintf(stderr, "GONE %06x back to Akira's own, frame %llu\n", r, (unsigned long long)m_frame);   // TESTONLY
				random_akira_record(r, 0, 0);
				if (m_random_cpu) random_fighter_make_named(r);   // (Random opponents: the computer's Akira random too)
				random_fighter_strands();
				random_fighter_specials();
			}
	}
	// Random opponents: each player's fighter is in its block (player 1's at 4006b0, player 2's
	// 0x1800 on), set as a match loads. Every record was made random at the select; one the
	// computer is done with (another in its place) is made afresh then, so the same character
	// twice comes in new, and none changes during its match (a record made as it comes in would:
	// the match's first moments are drawn before its block names it).
	// (both blocks read first: Dural and the Koskols have one record for both sides, and the other
	// block taking it in the same frame keeps it)
	if (m_random_cpu && m_ram_a.size() > 0x1eb4) {
		uint32_t now[2];
		for (int pl = 0; pl < 2; pl++) {
			const uint32_t o = 0x6b0 + 0x1800 * pl;
			now[pl] = uint32_t(m_ram_a[o] | m_ram_a[o + 1] << 8 | m_ram_a[o + 2] << 16 | uint32_t(m_ram_a[o + 3]) << 24);
		}
		const bool in0 = m_ram_a.size() > 0x7600 && (m_ram_a[0x7483] & 0x08), in1 = m_ram_a.size() > 0x7600 && (m_ram_a[0x7503] & 0x08);
		if (in0 || in1) {
			if (!m_run_on) { m_fought.clear(); m_built_run.clear(); m_cpu_akira.clear(); }
			m_run_on = true;
			m_run_idle = 0;
			// (the computer's side's record, in a match: fought)
			for (int pl = 0; pl < 2; pl++)
				if (!select && !(pl ? in1 : in0) && (now[pl] >> 16) == 0xfd) m_fought.insert(now[pl]);
		} else if (m_run_on && !select && ++m_run_idle > 120) {   // (no one in for 2 s: GAME OVER's (a continue's countdown keeps the player in), the run over)
			m_run_on = false;
			m_fought.clear();
			m_built_run.clear();
			m_cpu_akira.clear();
		}
		for (int pl = 0; pl < 2; pl++) {
			if (now[pl] == m_random_seen[pl]) continue;
			const uint32_t gone = m_random_seen[pl];
			m_random_seen[pl] = now[pl];
			if (gone != 0xfd4f99 && gone != 0xfd3f6f && !cpu_keeps(gone) && gone != now[pl ^ 1] && !select && !m_run_on) {
				random_fighter_make_named(gone);
				random_fighter_strands();
				random_fighter_specials();
				random_fighter_hold_moves();   // (during the boot's stances build: its own moves until then)
			}
		}
	}
	if (!select) m_pick_at = -1;
	m_select_seen = select;
	random_fighter_ids();
}

// A random fighter is made on Akira's record, his character number its own (random_fighter_make):
// in a match, so a seed plays as the same fighter for the computer as for a player. Outside one
// (the boot, the attract) each record wears its own character's: a fresh backup's ranking is made
// at boot from the records' (ff6a70, 40f800: AKIRA on every row), and the attract's demos fight
// the ranking's characters, each by its own number's demo (the same two of Akira's each time)
void model1::random_fighter_ids()
{
	auto &rom = m_regions.maincpu;
	random_cast &c = random_fighter_cast();
	if (!c.orig.count(0xfd4f99) || m_ram_a.size() < 0x7504) return;
	const bool playing = (m_ram_a[0x7483] | m_ram_a[0x7503]) & 0x08;
	for (const auto &[r, b] : c.orig)
		if (m_random_names.count(r) && r + 0x0d <= rom.size())
			rom[r + 0x0c] = playing ? c.orig.at(r == 0xfd3f6f ? r : 0xfd4f99)[0x0c] : b[0x0c];
	// The stage's music is the computer's character's, by its number (fe20d8: fd04f8's commands, one
	// a character): Akira's for every random one. Akira's entry is the computer's own character's
	// while it's a random one, his own otherwise
	constexpr uint32_t MUSIC = 0xfd04f8, AKIRA_ID = 11, KINDS = 21;
	if (MUSIC + 4 * KINDS > rom.size()) return;
	if (m_akira_track < 0) m_akira_track = rom[MUSIC + 4 * AKIRA_ID];
	int track = m_akira_track;
	for (int pl = 0; pl < 2 && playing; pl++) {
		if (m_ram_a[0x7483 + 0x80 * pl] & 0x08) continue;   // (a player's side: the other is the computer's)
		const uint32_t o = 0x6b0 + 0x1800 * uint32_t(pl);
		const uint32_t r = uint32_t(m_ram_a[o] | m_ram_a[o + 1] << 8 | m_ram_a[o + 2] << 16 | uint32_t(m_ram_a[o + 3]) << 24);
		const auto own = c.orig.find(r);
		if (m_random_names.count(r) && own != c.orig.end() && own->second[0x0c] < KINDS && own->second[0x0c] != AKIRA_ID)
			track = rom[MUSIC + 4 * own->second[0x0c]];
	}
	rom[MUSIC + 4 * AKIRA_ID] = uint8_t(track);
}

// The select's icons for the characters without one (all but Siba, whose is in the ROM): pictures
// made from their heads (tools/icons), shown in Akira's icon's tiles while one is in his place.
// His icon (map at 2ff5ec: 7 x 8 tiles, 8 x 8 pixels at 4 bits, colour 0x61, its own) is drawn on the
// select as entries b080-b0b1, two of them used more than once (the left edge's b087, and b0a5):
// those cells get tiles of their own, b0b2-b0b7 (unused there). While the icon is on screen its
// tiles, colours and map are ours, VF's own kept and put back as it goes; when the game writes
// them again, what it wrote is kept instead.
const char *const model1::SECRET_ICON_NAMES[] = { nullptr, nullptr, "jeff", "takeru", "rekka", "score", "aeon", "dural" };

namespace {
constexpr int ICON_W = 7, ICON_H = 8, ICON_COLOUR = 0x61;
constexpr uint16_t ICON_FIRST = 0xb080;
// VF's map of the icon, and ours (the repeated cells moved to b0b2-b0b7)
uint16_t icon_entry(int x, int y, bool ours)
{
	static const uint8_t VF[ICON_H][ICON_W] = {
		{ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 }, { 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d },
		{ 0x07, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13 }, { 0x07, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19 },
		{ 0x07, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f }, { 0x07, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25 },
		{ 0x07, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x25 }, { 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31 } };
	if (ours && x == 0 && y >= 2 && y <= 6) return ICON_FIRST + 0x32 + (y - 2);
	if (ours && x == 6 && y == 6) return ICON_FIRST + 0x37;
	return ICON_FIRST + VF[y][x];
}
}

bool model1::set_akira_icon(const uint32_t *xrgb)
{
	if (m_icon.active) { m_icon.on = false; secret_icon_frame(); }   // (VF's own back first)
	if (!xrgb) { m_icon.on = false; return true; }
	constexpr int W = ICON_W * 8, H = ICON_H * 8;
	std::vector<uint32_t> cols;
	std::vector<uint8_t> idx(W * H);
	for (int i = 0; i < W * H; i++) {
		const uint32_t c = xrgb[i] & 0xffffff;
		auto it = std::find(cols.begin(), cols.end(), c);
		if (it == cols.end()) { if (cols.size() == 15) return false; cols.push_back(c); it = cols.end() - 1; }
		idx[i] = uint8_t(1 + (it - cols.begin()));   // (0: see-through, unused)
	}
	m_icon.pal.assign(16, 0x8000);
	for (size_t i = 0; i < cols.size(); i++) {
		const uint32_t c = cols[i];
		m_icon.pal[1 + i] = uint16_t(0x8000 | ((c & 0xff) >> 3) << 10 | ((c >> 8 & 0xff) >> 3) << 5 | (c >> 16 & 0xff) >> 3);   // (xBGR 555)
	}
	m_icon.map.resize(ICON_W * ICON_H);
	m_icon.chars.assign(0x38 * 16, 0);
	for (int ty = 0; ty < ICON_H; ty++)
		for (int tx = 0; tx < ICON_W; tx++) {
			const uint16_t e = icon_entry(tx, ty, true);
			m_icon.map[ty * ICON_W + tx] = e;
			uint16_t *c = &m_icon.chars[(e - ICON_FIRST) * 16];
			for (int y = 0; y < 8; y++) {
				uint32_t row = 0;
				for (int x = 0; x < 8; x++) row |= uint32_t(idx[(ty * 8 + y) * W + tx * 8 + x]) << (28 - 4 * x);
				c[y * 2] = uint16_t(row >> 16); c[y * 2 + 1] = uint16_t(row);
			}
		}
	m_icon.on = true;
	return true;
}

void model1::secret_icon_frame()
{
	// where the icon is: its first row, VF's or ours
	uint32_t at = ~0u;
	if (m_icon.on)
		for (uint32_t i = 0; i < 0x4000 - ICON_H * 64 && at == ~0u; i++) {
			if (m_tiles.tile_r(i) != ICON_FIRST) continue;
			bool row = true;
			for (int x = 1; x < ICON_W && row; x++) row = m_tiles.tile_r(i + x) == ICON_FIRST + x;
			for (int y = 1; y < ICON_H && row; y++) row = (m_tiles.tile_r(i + y * 64 + 1) & 0xff80) == ICON_FIRST;
			if (row) at = i;
		}
	const uint32_t code0 = ICON_FIRST & 0x3fff, pal0 = ICON_COLOUR * 16;
	if (m_icon.active && at != m_icon.at) {   // gone (or moved): VF's own back
		for (size_t i = 0; i < m_icon.chars.size(); i++)
			if (m_tiles.char_r(code0 * 16 + i) == m_icon.chars[i]) m_tiles.char_w(code0 * 16 + i, m_icon.keep_chars[i], 0xffff);
		for (int i = 1; i < 16; i++)
			if (m_paletteram16[pal0 + i] == m_icon.pal[i]) model1_paletteram_w(pal0 + i, m_icon.keep_pal[i], 0xffff);
		for (int y = 0; y < ICON_H; y++)
			for (int x = 0; x < ICON_W; x++) {
				const uint32_t o = m_icon.at + y * 64 + x;
				if (m_tiles.tile_r(o) == m_icon.map[y * ICON_W + x]) m_tiles.tile_w(o, m_icon.keep_map[y * ICON_W + x], 0xffff);
			}
		m_icon.active = false;
	}
	if (at == ~0u) return;
	if (!m_icon.active) {   // VF's as it is now
		m_icon.keep_chars.resize(m_icon.chars.size());
		for (size_t i = 0; i < m_icon.chars.size(); i++) m_icon.keep_chars[i] = m_tiles.char_r(code0 * 16 + i);
		m_icon.keep_pal.assign(m_paletteram16 + pal0, m_paletteram16 + pal0 + 16);
		m_icon.keep_map.resize(ICON_W * ICON_H);
		for (int y = 0; y < ICON_H; y++)
			for (int x = 0; x < ICON_W; x++) m_icon.keep_map[y * ICON_W + x] = m_tiles.tile_r(at + y * 64 + x);
		m_icon.at = at;
		m_icon.active = true;
	}
	for (size_t i = 0; i < m_icon.chars.size(); i++) {
		const uint16_t now = m_tiles.char_r(code0 * 16 + i);
		if (now != m_icon.chars[i]) { m_icon.keep_chars[i] = now; m_tiles.char_w(code0 * 16 + i, m_icon.chars[i], 0xffff); }
	}
	for (int i = 1; i < 16; i++) {
		const uint16_t now = m_paletteram16[pal0 + i];
		if (now != m_icon.pal[i]) { m_icon.keep_pal[i] = now; model1_paletteram_w(pal0 + i, m_icon.pal[i], 0xffff); }
	}
	for (int y = 0; y < ICON_H; y++)
		for (int x = 0; x < ICON_W; x++) {
			const uint32_t o = at + y * 64 + x;
			const uint16_t now = m_tiles.tile_r(o), ours = m_icon.map[y * ICON_W + x];
			if (now != ours) { m_icon.keep_map[y * ICON_W + x] = now; m_tiles.tile_w(o, ours, 0xffff); }
		}
}

// No time limit: the round's clock is counted down at fe2190 (dec.h 501285[PC]), and the picture
// is drawn after, so a clock put back each frame still showed a tick off its start (29.96). That
// instruction becomes test.h of the same (the opcode d2 to f2, the same length): the clock stays
// at its start. It's the game's code, so the change needs a recompile (or the interpreter).
bool model1::set_no_time_limit(bool on)
{
	constexpr uint32_t AT = 0xfe2190;
	static const uint8_t REST[5] = { 0xf2, 0xf5, 0xf0, 0x51, 0xff };   // (its operand)
	auto &rom = m_regions.maincpu;
	if (AT + 6 > rom.size() || memcmp(&rom[AT + 1], REST, 5) || (rom[AT] != 0xd2 && rom[AT] != 0xf2)) return false;
	const uint8_t op = on ? 0xf2 : 0xd2;
	if (rom[AT] == op) return false;
	rom[AT] = op;
	return true;
}

// The fighters' energy (ffc4a0: player n's at [50130c] + n * 0x1800, the energy -2e from there,
// what hits take off; the full amount for the round at 501186).
void model1::fight_cheats(int infinite, bool one_hit, bool no_time)
{
	// a 16- or 32-bit value at a main CPU address in either RAM: the fighters' data is in the
	// battery-backed one (seen at 400700), the round's numbers in work RAM
	auto at = [&](uint32_t a, int n) -> uint8_t * {
		if (a >= 0x500000 && a + n - 0x500000 <= m_ram_b.size()) return &m_ram_b[a - 0x500000];
		if (a >= 0x400000 && a + n - 0x400000 <= m_ram_a.size()) return &m_ram_a[a - 0x400000];
		return nullptr;
	};
	auto rd16 = [&](uint32_t a) { const uint8_t *p = at(a, 2); return p ? int16_t(p[0] | p[1] << 8) : int16_t(0); };
	auto rd32 = [&](uint32_t a) { const uint8_t *p = at(a, 4); return p ? uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24) : 0u; };
	auto wr16 = [&](uint32_t a, int16_t v) { if (uint8_t *p = at(a, 2)) { p[0] = uint8_t(v); p[1] = uint8_t(uint16_t(v) >> 8); } };
	// (the fields are below the pointer: player 2's at +0x1800, down to -0x80)
	const uint32_t base = rd32(0x50130c);
	const bool ok = at(base - 0x80, 0x80) && at(base + 0x1800 - 0x80, 0x80);
	static int told = 0;
	if (told < 2 && (ok ? told == 1 || told == 0 : told == 0)) {   // (log.txt: what the cheats found, once each way)
		fprintf(stderr, "cheats: players at %06x%s, energy %d %d of %d, clock %d\n", base, ok ? "" : " (not set yet)",
				ok ? rd16(base - 0x2e) : 0, ok ? rd16(base + 0x1800 - 0x2e) : 0, rd16(0x501186), rd16(0x501285));
		told = ok ? 2 : 1;
	}
	if (!ok) return;
	// The round's clock (501285): 0x384 as a round starts (fe2152), counted down while neither
	// fighter is down (fe2190, which set_no_time_limit stops: their -80's bit 0x1b), time up below
	// 0. Other screens count with it too, so this is only once a round has started it. At the end
	// of a round the game files its replay by the clock (fe2617: a round over while it's still
	// above 0x357, its first moments, goes another way), so as a fighter goes down it's let go at
	// 0x356, a round's time like any other. (Put back each frame too, should the code be VF's own.)
	if (no_time) {
		const int t = rd16(0x501285);
		const bool down = (rd32(base - 0x80) | rd32(base + 0x1800 - 0x80)) >> 0x1b & 1;
		if (t == 0x384 && !down) m_clock_held = true;
		if (m_clock_held && (down || t <= 0 || t > 0x384)) {
			m_clock_held = false;
			if (down && t > 0x356) wr16(0x501285, 0x356);
		}
		if (m_clock_held && t < 0x384) wr16(0x501285, 0x384);
	} else m_clock_held = false;
	if (!infinite && !one_hit) return;
	const int16_t full = rd16(0x501186);
	if (full <= 0 || full > 1000) return;
	for (int p = 0; p < 2; p++) {
		const uint32_t a = base + p * 0x1800 - 0x2e;
		int16_t e = rd16(a);
		if (infinite >> p & 1) e = full;
		else if (one_hit && e > 1) e = 1;
		else continue;
		wr16(a, e);
	}
}


void model1::show_free_play_text()
{
	for (const auto &[addr, bytes] : m_free_play_text) memcpy(&m_regions.maincpu[addr], bytes.data(), bytes.size());
	m_free_play_text.clear();
}

int model1::hide_free_play_text()
{
	if (!m_free_play_text.empty()) return 0;
	auto keep = [&](uint32_t addr, size_t n) {
		auto &rom = m_regions.maincpu;
		if (addr + n <= rom.size()) m_free_play_text.push_back({ addr, std::vector<uint8_t>(&rom[addr], &rom[addr] + n) });
	};
	keep(0x13c01c2, 40);
	keep(0xfd080f, 9);
	int found = 0;
	// The italic FREE PLAY (attract, title): a 14x2 tile image drawn by the routine at ff39a3
	// from a record (base tile, flags, rows, columns, then a byte per tile). Base tile 0x0020,
	// the blank the text layer is cleared with, and every byte 0: blank cells.
	uint8_t blank_image[12 + 28] = { 0x20, 0x00, 0, 0, 2, 0, 0, 0, 14, 0, 0, 0 };
	found += patch_program(0x13c01c2, sizeof blank_image, 0x71d41cda, blank_image);
	// The fight's "FREE PLAY" (top right, between PUSH START BUTTON): a padded string.
	const uint8_t spaces[9] = { ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
	found += patch_program(0xfd080f, sizeof spaces, 0x9dbd988f, spaces);
	return found;
}

// Widescreen is for the fight. VF keeps a pointer to the camera it last sent to the display list
// at 5017f0: the fight's camera (also used by the attract demo, the win and continue screens) is
// at 5020f0; the title and the player select use others and stay 4:3. The pointer is cleared
// every other frame (forcing a resend), so 0 keeps the last choice.
void model1::redraw()
{
	const auto render_done = m_render_done;     // emulation state: drawing again mustn't touch it
	const int quads = m_last_quads;
	if (!m_smooth)
		screen_update(m_bitmap, m_bitmap.cliprect());   // the frame drawn as it ran: nothing has moved since
	else if (!m_shown)
		m_bitmap.fill(0);                               // nothing shown since the start or a reset
	else {
		// Smooth motion: the moment shown, which is a game frame behind the newest list, and when
		// frames ran on without a new picture shown (a held scene change, the hold after a camera
		// cut), older still: then drawn from what was kept as the next frame started.
		const bool old = m_shown_frame != m_frame && m_still.frame == m_shown_frame;
		if (old) swap_still();
		m_redrawing = true;
		render_smooth(m_shown_now);
		m_redrawing = false;
		if (old) swap_still();
	}
	m_unshown = false;
	m_frame_parts = false;
	m_render_done = render_done;
	m_last_quads = quads;
}

bool model1::past_boot() const
{
	const uint8_t *p = &m_ram_b[0x17f0];
	return (p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24) != 0;
}

bool model1::fight_camera() const
{
	const uint8_t *p = &m_ram_b[0x17f0];
	uint32_t cam = p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
	if (cam == 0) return m_fight_cam;
	return cam == 0x5020f0;
}

// A layer at the arcade's resolution (and the widescreen sides), drawn once a game frame, with each
// row's drawn span, so the scaling skips what's empty (the HUD and text layers are mostly empty).
model1::layer_cache &model1::cached_layer(int layer, int flags)
{
	constexpr uint32_t KEY = 0xff000000;
	layer_cache &c = m_layer_cache[layer & 7];
	const int tw = SCREEN_W + 2 * m_ext;
	if (c.frame != m_frame || c.flags != flags || c.ext != m_ext || c.px.width() != tw) {
		if (c.px.width() != tw) c.px.allocate(tw, SCREEN_H);
		c.px.fill(KEY);
		if (!m_layers_together) m_tiles.m_ox = m_ext;     // (else set for them all)
		m_tiles.draw(c.px, c.px.cliprect(), layer, 0, flags);
		if (!m_layers_together) m_tiles.m_ox = 0;
		c.x0.assign(SCREEN_H, 0);
		c.x1.assign(SCREEN_H, 0);
		for (int y = 0; y < SCREEN_H; y++) {
			const uint32_t *r = &c.px.pix(y);
			int a = 0, b = tw;
			while (a < tw && r[a] == KEY) a++;
			if (a == tw) continue;
			while (b > a && r[b - 1] == KEY) b--;
			c.x0[y] = int16_t(a); c.x1[y] = int16_t(b);
		}
		c.frame = m_frame; c.flags = flags; c.ext = m_ext; c.gen++;
	}
	return c;
}

// This frame's layers but the sky, each on a thread of its own (they don't depend on one another).
void model1::prepare_layers()
{
	if (m_scale == 1 && m_ext == 0) return;          // (the arcade's picture draws them straight in)
	static const int LAYERS[] = { 6, 2, 0, 7, 5, 3, 1 };
	int todo[7], n = 0;
	const int tw = SCREEN_W + 2 * m_ext;
	for (int l : LAYERS) {
		const int flags = l == 6 ? segas24_tile::DRAW_OPAQUE : 0;
		const layer_cache &c = m_layer_cache[l];
		if (c.frame != m_frame || c.flags != flags || c.ext != m_ext || c.px.width() != tw) todo[n++] = l;
	}
	if (n < 2) return;
	m_work.layers += n;
	m_tiles.m_ox = m_ext;
	m_layers_together = true;
	parallel_jobs(n, [&](int i) { cached_layer(todo[i], todo[i] == 6 ? segas24_tile::DRAW_OPAQUE : 0); });
	m_layers_together = false;
	m_tiles.m_ox = 0;
}

// The 2D layers are drawn at the arcade's resolution and scaled up to the render's.
void model1::draw_tiles(bitmap_rgb32 &bitmap, const rectangle &cliprect, int layer, int flags)
{
	static const int skip = getenv("VF_TILE_SKIP") ? strtol(getenv("VF_TILE_SKIP"), nullptr, 16) : 0;
	if (skip & (1 << layer)) return;
	if (m_scale == 1 && m_ext == 0) {
		m_tiles.m_ox = 0;
		m_tiles.draw(bitmap, cliprect, layer, 0, flags);
		return;
	}
	constexpr uint32_t KEY = 0xff000000;     // pixels are 0x00rrggbb: never the key
	bitmap_rgb32 *tmp = &m_tile_tmp;
	const int16_t *rx0 = nullptr, *rx1 = nullptr;    // each row's drawn span (cached layers)
	bool sky = false;
	if (layer == 4 && (flags & segas24_tile::DRAW_OPAQUE)) {
		// the sky layer: note where each pixel came from, for a hi-res replacement (drawn once a game
		// frame; which panorama and where, every time)
		if (m_skies.empty()) init_skies();
		sky = !m_skies.empty() && m_wide_frame;
		if (m_l4_frame != m_frame || m_l4_ext != m_ext || m_l4_coords != sky || m_tile_tmp.width() != SCREEN_W + 2 * m_ext) {
			m_tile_tmp.fill(KEY);
			m_tiles.m_ox = m_ext;
			if (sky) {
				m_tile_coord.assign(size_t(m_tile_tmp.width()) * m_tile_tmp.height(), ~0u);
				m_tiles.m_coord = m_tile_coord.data();
			}
			m_tiles.draw(m_tile_tmp, m_tile_tmp.cliprect(), layer, 0, flags);
			m_tiles.m_coord = nullptr;
			m_tiles.m_ox = 0;
			m_l4_frame = m_frame; m_l4_ext = m_ext; m_l4_coords = sky; m_l4_gen++;
		}
		if (sky) find_sky();
	} else {
		layer_cache &c = cached_layer(layer, flags);
		tmp = &c.px; rx0 = c.x0.data(); rx1 = c.x1.data();
	}
	// Widescreen: the opaque layers are VF's sky, which only holds the visible part of its
	// panorama; stretch it across the width. The rest (text, HUD) stays in the middle 4:3.
	const bool stretch = m_ext && m_wide_frame && (flags & segas24_tile::DRAW_OPAQUE);
	const int s = m_scale;
	const int out_w = bitmap.width();
	// smooth motion: the sky between two game frames, moved by the blended scroll at full resolution
	float sdx = 0, sdy = 0;
	if (layer == 4 && (flags & segas24_tile::DRAW_OPAQUE)) smooth_sky_shift(sdx, sdy);
	// a hi-res sky draws over every row of this layer: nothing to draw beneath it
	if (sky && m_sky_now) { draw_sky(bitmap, sdx, sdy); return; }
	// the tile layer itself moves no more than 24 pixels (its padding is 32: more showed its clamped
	// edge); a panorama wraps round, so it takes the whole move
	if (std::fabs(sdx) > 24 || std::fabs(sdy) > 24) sdx = sdy = 0;
	if (m_parts) { parts_layer(layer, flags, *tmp, stretch, sdx, sdy); return; }   // the GPU enlarges it
	if (sdx != 0 || sdy != 0) {
		const int oy = sky_shift_y(sdy, s), ox = sky_shift_x(sdx, s);
		const int x0 = stretch ? 0 : m_ext * s, x1 = stretch ? out_w : (m_ext + SCREEN_W) * s;
		auto fdiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
		m_col_src.resize(out_w);
		for (int X = x0; X < x1; X++) {
			const int lx = stretch ? m_ext + int(std::floor((X + 0.5f) * SCREEN_W / out_w + sdx)) : fdiv(X + ox, s);
			m_col_src[X] = std::clamp(lx, m_ext, m_ext + SCREEN_W - 1);
		}
		parallel_rows(SCREEN_H * s, [&](int Y0, int Y1) {
			for (int Y = Y0; Y < Y1; Y++) {
				const int ly = std::clamp(fdiv(Y + oy, s), 0, SCREEN_H - 1);
				if (rx0 && rx0[ly] >= rx1[ly]) continue;   // nothing on this row
				const uint32_t *src = &tmp->pix(ly);
				uint32_t *d = &bitmap.pix(Y);
				for (int X = x0; X < x1; X++) {
					const uint32_t c = src[m_col_src[X]];
					if (c != KEY) d[X] = c;
				}
			}
		});
		return;
	}
	parallel_rows(SCREEN_H, [&](int ya, int yb) {
	for (int y = ya; y < yb; y++) {
		if (rx0 && rx0[y] >= rx1[y]) continue;     // nothing on this row
		const uint32_t *src = &tmp->pix(y);
		if (stretch) {
			uint32_t *d0 = &bitmap.pix(y * s);
			for (int x = 0; x < out_w; x++) {
				uint32_t c = src[m_ext + int((x + 0.5f) * SCREEN_W / out_w)];
				if (c != KEY) d0[x] = c;
			}
			for (int dy = 1; dy < s; dy++)
				memcpy(&bitmap.pix(y * s + dy), d0, out_w * 4);
			continue;
		}
		const int xa = rx0 ? rx0[y] : 0, xb = rx0 ? rx1[y] : tmp->width();
		for (int x = xa; x < xb; x++) {
			uint32_t c = src[x];
			if (c == KEY) continue;
			for (int dy = 0; dy < s; dy++) {
				uint32_t *d = &bitmap.pix(y * s + dy, x * s);
				for (int dx = 0; dx < s; dx++) d[dx] = c;
			}
		}
		// Widescreen: a row this layer covers from edge to edge is a curtain (the black that hides
		// a stage change): carry each end out across its side.
		const uint32_t el = src[m_ext], er = src[m_ext + SCREEN_W - 1];
		if (m_ext && m_wide_frame && el != KEY && er != KEY) {
			int covered = 0;
			for (int x = m_ext; x < m_ext + SCREEN_W; x++) covered += src[x] != KEY;
			if (covered * 10 >= SCREEN_W * 9)
				for (int dy = 0; dy < s; dy++) {
					uint32_t *d = &bitmap.pix(y * s + dy);
					std::fill(d, d + m_ext * s, el);
					std::fill(d + (m_ext + SCREEN_W) * s, d + bitmap.width(), er);
				}
		}
	}
	});
}

// ---- screen: MAME's screen_update_model1 ----

void model1::screen_update(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	// netplay's names under the energy bars: in the tiles while this picture's drawn, then the game's put back
	std::vector<std::pair<uint32_t, uint16_t>> undo;
	struct put_back {
		model1 &m; std::vector<std::pair<uint32_t, uint16_t>> &undo;
		~put_back()
		{
			for (auto it = undo.rbegin(); it != undo.rend(); ++it)
				if (it->first & CHAR_UNDO) m.m_tiles.char_w(it->first & ~CHAR_UNDO, it->second, 0xffff);
				else m.m_tiles.tile_w(it->first, it->second, 0xffff);
		}
	} hud_back{ *this, undo };
	if (!m_hud_names[0].empty() || !m_hud_names[1].empty()) {
		std::lock_guard<std::mutex> lk(m_hud_mx);
		random_fighter_hud(m_hud_names, &undo);
		if (!undo.empty()) m_tiles_updated = ~0ull;   // (decoded with them)
	}
	view_t *view = m_view.get();
	view->ayyc = cos(view->ayy);
	view->ayys = sin(view->ayy);

	if (m_tiles_updated != m_frame) {                // (once per game frame)
		const uint64_t t0 = trace_ns();
		m_work.tiles_full += m_tiles.all_to_decode();
		m_tiles.update(m_pens);
		m_tiles_updated = m_frame;
		m_work.tiles_ns += trace_ns() - t0;
	}
	{
		const uint64_t t0 = trace_ns();
		prepare_layers();
		m_work.layers_ns += trace_ns() - t0;
	}
	if (m_parts) parts_begin(bitmap);                // the GPU fills it, and the 3D goes over nothing
	else { bitmap.fill(m_pens[0], cliprect); m_bitmap_whole = true; }
	static const bool log_scroll = getenv("VF_LOG_SCROLL") != nullptr;
	if (log_scroll)
		fprintf(stderr, "frame %llu tm2 h %04x v %04x tm3 h %04x v %04x ctrl %04x %04x\n", (unsigned long long)m_frame,
				m_tiles.tile_r(0x5002), m_tiles.tile_r(0x5006), m_tiles.tile_r(0x5003), m_tiles.tile_r(0x5007),
				m_tiles.tile_r(0x5004), m_tiles.tile_r(0x5006));
	m_fight_cam = fight_camera();
	// A screen with painted sides, known by its backdrop: 4:3 from its first frame. (Between scenes the
	// camera reads none, and the last one's kept: a fight's, for a few frames into the select.)
	const screen_art *known = nullptr;
	if (m_ext && !m_screens.empty()) {
		const uint32_t h = backdrop_hash();
		for (const auto &a : m_screens)
			if (std::find(a.hashes.begin(), a.hashes.end(), h) != a.hashes.end()) known = &a;
	}
	const bool no_cam = !(m_ram_b[0x17f0] | m_ram_b[0x17f1] | m_ram_b[0x17f2] | m_ram_b[0x17f3]);
	if (known && no_cam) m_fight_cam = false;
	// a stage's sky in its map with no camera yet (the screen before the attract's fight, its
	// rules over the stage: 4:3 with black sides for 6 frames after the title, then wide): wide
	else if (m_ext && !known && no_cam && !m_fight_cam && sky_identity() >= 0) m_fight_cam = true;
	if (m_ext) {
		bool wide = m_fight_cam;
		static const bool log_wide = getenv("VF_LOG_WIDE") != nullptr;
		if (log_wide && wide != m_wide_frame)
			fprintf(stderr, "frame %llu: %s\n", (unsigned long long)m_frame, wide ? "wide" : "4:3");
		m_wide_frame = wide;
	}

	static const bool log_backdrop = getenv("VF_LOG_BACKDROP") != nullptr;
	if (log_backdrop) {
		static uint32_t last = 0;
		const uint32_t h = backdrop_hash();
		if (h != last)
			fprintf(stderr, "frame %llu backdrop %08x scroll %04x %04x cam %02x%02x%02x%02x\n", (unsigned long long)m_frame, h,
					m_tiles.tile_r(0x5002), m_tiles.tile_r(0x5006), m_ram_b[0x17f3], m_ram_b[0x17f2], m_ram_b[0x17f1], m_ram_b[0x17f0]);
		last = h;
	}
	m_screen_now = m_wide_frame ? nullptr : known;
	draw_tiles(bitmap, cliprect, 6, segas24_tile::DRAW_OPAQUE);
	draw_tiles(bitmap, cliprect, 4, segas24_tile::DRAW_OPAQUE);
	if (m_screen_now && m_parts) {
		// its painted sides, as an image of their own over the backdrop (KEY between them)
		if (m_sides.width() != bitmap.width() || m_sides.height() != bitmap.height()) {
			m_sides.allocate(bitmap.width(), bitmap.height());
			m_sides.fill(0xff000000);
			m_sides_art = nullptr;
		}
		draw_screen_sides(m_sides);
		m_parts->img[TEX_SIDES] = { &m_sides.pix(0), m_sides.width(), m_sides.height(), size_t(m_sides.width()), m_sides_gen };
		gpu_op o; o.tex = TEX_SIDES; o.sw = o.dw = float(m_sides.width()); o.sh = o.dh = float(m_sides.height());
		m_parts->ops.push_back(o);
	} else if (m_screen_now) draw_screen_sides(bitmap);
	if (m_parts) {
		parts_composite(m_mid, m_mid_key, m_mid_gen, { 2, 0 });
		m_parts->img[TEX_MID] = { &m_mid.pix(0), m_mid.width(), m_mid.height(), size_t(m_mid.width()), m_mid_gen };
		gpu_op o; o.tex = TEX_MID; o.sw = float(m_mid.width()); o.sh = SCREEN_H; o.dw = float(m_mid.width() * m_scale); o.dh = float(SCREEN_H * m_scale);
		m_parts->ops.push_back(o);
		o.tex = TEX_3D; o.sw = o.dw = float(bitmap.width()); o.sh = o.dh = float(bitmap.height());
		m_parts->ops.push_back(o);                   // (drawn below, before the host puts it on)
		if (m_screen_now) {                          // the painted sides again, over the 3D (see below)
			gpu_op s; s.tex = TEX_SIDES; s.sw = s.dw = float(m_sides.width()); s.sh = s.dh = float(m_sides.height());
			m_parts->ops.push_back(s);
		}
	} else {
		draw_tiles(bitmap, cliprect, 2, 0);
		draw_tiles(bitmap, cliprect, 0, 0);
	}

	static const bool no_polys = getenv("VF_NO_POLYS") != nullptr;   // debug: the 2D layers alone
	m_above_hud_seen = false;
	m_frame_quads = 0;
	// (Low antialiasing, a picture drawn whole: what's under the 3D marked, to tell the 3D's pixels by)
	if (m_edge_aa && !m_parts && !no_polys)
		parallel_rows(bitmap.height(), [&](int a, int b) {
			for (int y = a; y < b; y++) { uint32_t *p = &bitmap.pix(y); for (int x = 0; x < bitmap.width(); x++) p[x] |= UNDER_3D; }
		});
	const bool g3 = gpu3d_on() && !no_polys;   // the 3D on the GPU (gpu3d_end), else the CPU's
	// A picture's painted sides stay over the 3D: what it draws beyond the arcade's 4:3 was off the
	// cabinet's screen (a boss's empty select portrait, in Cheats' Akira's place, leaves its sky
	// there). Put together on the GPU, the sides go on again above the 3D (above); else kept here.
	std::vector<uint32_t> sides_kept;
	const int side_l = m_ext * m_scale, side_r = std::min((m_ext + SCREEN_W) * m_scale, bitmap.width());
	if (m_screen_now && !m_parts && side_l > 0) {
		const int w = bitmap.width();
		sides_kept.resize(size_t(cliprect.max_y - cliprect.min_y + 1) * (side_l + w - side_r));
		uint32_t *k = sides_kept.data();
		for (int y = cliprect.min_y; y <= cliprect.max_y; y++) {
			const uint32_t *p = &bitmap.pix(y);
			k = std::copy(p, p + side_l, k);
			k = std::copy(p + side_r, p + w, k);
		}
	}
	m_raster_ms = 0;
	if (g3) gpu3d_begin(bitmap);
	if (!no_polys) tgp_render(bitmap, cliprect, RENDER_BELOW_HUD);
	if (g3) gpu3d_end(bitmap);
	else { m_g3.gpu = false; m_g3.ms = m_raster_ms; m_g3.gpu_ms = 0; }
	clear_moire_marks(bitmap, cliprect);
	if (m_edge_aa && !no_polys && m_parts && m_parts->d3_target >= 0) m_parts->d3_edge = true;   // (the host's GPU smooths it)
	else if (m_edge_aa && !no_polys) {
		const uint64_t t0 = trace_ns();
		edge_smooth(bitmap, m_parts != nullptr);
		m_work.edge_ns += trace_ns() - t0;
	}
	if (!sides_kept.empty()) {
		const int w = bitmap.width();
		const uint32_t *k = sides_kept.data();
		for (int y = cliprect.min_y; y <= cliprect.max_y; y++) {
			uint32_t *p = &bitmap.pix(y);
			std::copy(k, k + side_l, p); k += side_l;
			std::copy(k, k + (w - side_r), p + side_r); k += w - side_r;
		}
	}

	if (m_parts) {
		parts_composite(m_top, m_top_key, m_top_gen, { 7, 5, 3, 1 });
		m_parts->img[TEX_TOP] = { &m_top.pix(0), m_top.width(), m_top.height(), size_t(m_top.width()), m_top_gen };
		gpu_op o; o.tex = TEX_TOP; o.sw = float(m_top.width()); o.sh = SCREEN_H; o.dw = float(m_top.width() * m_scale); o.dh = float(SCREEN_H * m_scale);
		m_parts->ops.push_back(o);
		if (m_above_hud_seen) m_parts_whole = true;   // (another Model 1 game's: not VF's)
	} else {
		draw_tiles(bitmap, cliprect, 7, 0);
		draw_tiles(bitmap, cliprect, 5, 0);
		draw_tiles(bitmap, cliprect, 3, 0);
		draw_tiles(bitmap, cliprect, 1, 0);
	}

	// The above-HUD pass and its stencil copy the whole frame twice: only when there is something
	// to draw there (never in VF; it is for the other Model 1 games' radar blips).
	if (m_above_hud_seen && !no_polys) {
		build_overlay_mask(bitmap, cliprect);
		tgp_render(bitmap, cliprect, RENDER_ABOVE_HUD);
		apply_overlay_stencil(bitmap, cliprect);
	}
	clear_moire_marks(bitmap, cliprect);
	m_last_quads = m_frame_quads;
	if (m_parts && !(m_ext && !m_wide_frame && !m_screen_now)) parts_end(bitmap);
	if (m_ext && !m_wide_frame && !m_screen_now) {   // 4:3 screens without a picture's sides: black
		const int l = m_ext * m_scale, r = (m_ext + SCREEN_W) * m_scale;
		if (m_parts) {
			gpu_op o;
			o.dw = float(l); o.dh = float(bitmap.height());
			m_parts->ops.push_back(o);
			o.dx = float(r); o.dw = float(bitmap.width() - r);
			m_parts->ops.push_back(o);
			parts_end(bitmap);
			return;
		}
		for (int y = cliprect.min_y; y <= cliprect.max_y; y++) {
			uint32_t *p = &bitmap.pix(y);
			std::fill(p, p + l, 0);
			std::fill(p + r, p + bitmap.width(), 0);
		}
	}
}


// Above 1x shadows mark the pixels they shade (see draw_hline_moired); drop the marks.
void model1::clear_moire_marks(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	if (!s_moire_blend) return;
	const int y1 = std::max(cliprect.min_y, s_mark_y1), y2 = std::min(cliprect.max_y, s_mark_y2);
	s_mark_y1 = 1 << 30;
	s_mark_y2 = -1;
	for (int y = y1; y <= y2; y++) {
		uint32_t *p = &bitmap.pix(y);
		for (int x = cliprect.min_x; x <= cliprect.max_x; x++) {
			if (p[x] == KEY) continue;              // (KEY: nothing drawn there, in the GPU's parts)
			if ((p[x] >> 24) == SHADOW_KEY >> 24) {   // a shadow over the 2D: blended with it now
				p[x] = ((parts_below(x, y) >> 1) & 0x7f7f7f) + (p[x] & 0x7f7f7f);
				continue;
			}
			p[x] &= 0xffffff | UNDER_3D;              // (Low antialiasing's mark stays until it's read)
		}
	}
}

// A fingerprint of the backdrop map (the tile map layers 4 and 5 draw): which full-screen picture
// is up (title, player select, ranking), whatever its palette is doing.
uint32_t model1::backdrop_hash() const
{
	// (a secret character's select icon in Akira's: its cells counted as VF's own, so the select is
	// known by the same hash and keeps its widescreen sides)
	auto entry = [&](uint32_t o) -> uint16_t {
		if (m_icon.active && o >= m_icon.at && o < m_icon.at + 8 * 64 && (o - m_icon.at) % 64 < 7)
			return m_icon.keep_map[((o - m_icon.at) / 64) * 7 + (o - m_icon.at) % 64];
		return m_tiles.tile_r(o);
	};
	uint32_t h = 2166136261u;
	for (int i = 0; i < 0x1000; i++) h = (h ^ entry(0x2000 + i)) * 16777619u;
	return h;
}
