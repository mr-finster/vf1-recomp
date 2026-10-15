// Sega Model 1 glue: memory map, IRQ controller, timers, ROM banking and the per-frame loop.
// Follows MAME's src/mame/sega/model1.cpp (BSD-3-Clause, copyright-holders: Olivier Galibert).
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
}

void model1::reset()
{
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
	// the picture on screen was drawn from the state this frame is about to change: keep that
	if (m_smooth && m_shown && m_shown_frame == m_frame) keep_still();
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
		if (m_audio) m_audio->rx_byte(uint8_t(m_uart_shift));
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
		for (int i = 0; i < N; i++) memcpy(akira[i], &rom[AT[i].addr], AT[i].size);
		ribbon = akira[RIBBON1][0] == 0x16 && akira[RIBBON2][0] == 0x0b &&   // (VF's lists as expected)
				 akira[RIBBON3][0] == 0x16 && akira[RIBBON4][0] == 0x0b;
		kept = true;
	}
	which = std::clamp(which, 0, int(std::size(UNUSED_CHARACTERS)) - 1);
	for (int i = 0; i < N; i++) {
		uint8_t *p = &rom[AT[i].addr];
		if (TO[which][i] < 0 || (i >= RIBBON1 && !ribbon)) memcpy(p, akira[i], AT[i].size);
		else for (int b = 0; b < AT[i].size; b++) p[b] = uint8_t(TO[which][i] >> (8 * b));   // (little-endian)
	}
	akira_slot_heads(TO[which][MODEL1] < 0 ? 0 : 0xfd0000 | uint32_t(TO[which][MODEL1]));
	// The select's camera for Akira's spot (fd278b: x, y, z, then 3 angles, a camera per place in
	// the row): each head sits differently in its own frame (its origin, how it's turned), so each
	// has its own: the eyes at the cast's height (a little below the middle), the face across the
	// middle, the head the cast's size, the neck running off the bottom. Dural's head (the fight's)
	// is turned and tilted down besides: hers turns it to face as the rest. (Found by rendering
	// each against the cast's.)
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
	static uint8_t akira_cam[18];
	static bool cam_kept = false;
	if (!cam_kept) { memcpy(akira_cam, &rom[CAMERA], sizeof akira_cam); cam_kept = true; }
	memcpy(&rom[CAMERA], akira_cam, sizeof akira_cam);
	if (which) {
		const cam &c = CAMS[which];
		const float f[3] = { c.x, c.y, c.z };
		const int16_t h[3] = { c.a, c.b, c.c };
		memcpy(&rom[CAMERA], f, sizeof f);       // (little-endian, as VF's)
		memcpy(&rom[CAMERA + 12], h, sizeof h);
	}
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
		for (size_t i = 0; i < std::size(NO_PORTRAIT); i++) ours[i] = rd(NO_PORTRAIT[i] + 0x72) == 0;   // (VF's: none)
		kept = true;
	}
	const uint32_t blink = rd(rd(BLINK_OF + 0x72) + 0x3c);
	bool made = false;
	for (size_t i = 0; i < std::size(NO_PORTRAIT); i++) {
		if (!ours[i]) continue;
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

static std::vector<std::pair<uint32_t, std::vector<uint8_t>>> s_free_play_text;   // the bytes hide_free_play_text replaced

void model1::show_free_play_text()
{
	for (const auto &[addr, bytes] : s_free_play_text) memcpy(&m_regions.maincpu[addr], bytes.data(), bytes.size());
	s_free_play_text.clear();
}

int model1::hide_free_play_text()
{
	if (!s_free_play_text.empty()) return 0;
	auto keep = [&](uint32_t addr, size_t n) {
		auto &rom = m_regions.maincpu;
		if (addr + n <= rom.size()) s_free_play_text.push_back({ addr, std::vector<uint8_t>(&rom[addr], &rom[addr] + n) });
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
	m_screen_now = nullptr;
	if (m_ext && !m_wide_frame && !m_screens.empty()) {
		const uint32_t h = backdrop_hash();
		for (const auto &a : m_screens)
			if (std::find(a.hashes.begin(), a.hashes.end(), h) != a.hashes.end()) m_screen_now = &a;
	}
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
	if (m_edge_aa && !no_polys) {
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
	uint32_t h = 2166136261u;
	for (int i = 0; i < 0x1000; i++) h = (h ^ m_tiles.tile_r(0x2000 + i)) * 16777619u;
	return h;
}
