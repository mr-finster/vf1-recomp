// The TGP run from its own program (315-5724, VF's internal ROM) on MAME's MB86233 core,
// with the Model 1 coprocessor glue from MAME's current src/mame/sega/model1_m.cpp
// (BSD-3-Clause, copyright-holders: Olivier Galibert).
//
// MAME runs the TGP alongside the V60 on its scheduler. Here it runs on demand: whenever the
// V60 touches the coprocessor, the TGP runs until it waits for input. The results are the same,
// and runs repeat exactly.
#include "model1.h"
#include "../core/mb86233.h"

struct model1::tgp_lle : mb86233_bus
{
	model1 &m;
	std::vector<u32> prog;               // 0x800 words
	const std::vector<uint8_t> &data_rom;
	u32 ram0[0x100]{}, ram1[0x200]{};    // internal RAM banks at 0x000 and 0x200
	std::vector<u32> copro_ram;          // shared with the V60, 0x2000 words
	u32 ram_adr[4]{};
	u32 sincos_base = 0, inv_base = 0, isqrt_base = 0, atan_base[4]{}, data_base = 0;
	std::deque<u32> fifo_in, fifo_out;
	mb86233_device cpu{*this};

	tgp_lle(model1 &mm, const std::vector<uint8_t> &rom, const std::vector<uint8_t> &data)
		: m(mm), prog(0x800), data_rom(data), copro_ram(0x2000)
	{
		memcpy(prog.data(), rom.data(), std::min(rom.size(), size_t(0x2000)));
	}

	void reset()
	{
		cpu.reset();
		fifo_in.clear();
		fifo_out.clear();
		std::fill(std::begin(ram_adr), std::end(ram_adr), 0);
		std::fill(copro_ram.begin(), copro_ram.end(), 0);
		sincos_base = inv_base = isqrt_base = data_base = 0;
		std::fill(std::begin(atan_base), std::end(atan_base), 0);
	}

	// Runs the TGP until it waits on an empty input FIFO.
	void sync()
	{
		for (int guard = 0; guard < 64; guard++) {
			m.m_tgp_cycles += cpu.run(100000);
			if (cpu.stalled())
				return;
		}
		fprintf(stderr, "tgp: still running after 6.4M instructions (pc %03x)\n", cpu.pc());
	}

	u32 prog_r(u32 a) override { return prog[a & 0x7ff]; }

	u32 data_r(u32 a) override
	{
		a &= 0xffff;
		if (a < 0x100) return ram0[a];
		if (a == 0x100) {
			if (fifo_in.empty()) { cpu.stall(); return 0; }
			u32 v = fifo_in.front();
			fifo_in.pop_front();
			return v;
		}
		if (a >= 0x200 && a < 0x400) return ram1[a - 0x200];
		return 0;
	}

	void data_w(u32 a, u32 v) override
	{
		a &= 0xffff;
		if (a < 0x100) ram0[a] = v;
		else if (a >= 0x200 && a < 0x400) ram1[a - 0x200] = v;
		else if (a == 0x400) fifo_out.push_back(v);
	}

	u32 io_r(u32 a) override
	{
		a &= 0xffff;
		if (a < 0x20) {
			int bank = (a >> 3) & 3;
			if ((a & 7) == 0) return ram_adr[bank];
			if ((a & 7) == 1) {
				u32 v = copro_ram[ram_adr[bank] & 0x1fff];
				ram_adr[bank] += (ram_adr[bank] & 0x40000) ? 4 : 1;
				return v;
			}
			return 0;
		}
		if (a >= 0x20 && a < 0x24) return sincos_r(a - 0x20);
		if (a >= 0x24 && a < 0x28) return atan_r();
		if (a >= 0x28 && a < 0x2a) return inv_r(a - 0x28);
		if (a >= 0x2a && a < 0x2c) return isqrt_r(a - 0x2a);
		if (a >= 0x8000) {
			if (data_rom.size() < 4) return 0;
			const size_t words = data_rom.size() / 4;
			const size_t index = ((data_base & ~0x7fffu) | (a & 0x7fffu)) & (words - 1);
			const uint8_t *p = data_rom.data() + index * 4;
			return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
		}
		return 0;
	}

	void io_w(u32 a, u32 v) override
	{
		a &= 0xffff;
		if (a < 0x20) {
			int bank = (a >> 3) & 3;
			if ((a & 7) == 0) ram_adr[bank] = v;
			else if ((a & 7) == 1) {
				copro_ram[ram_adr[bank] & 0x1fff] = v;
				ram_adr[bank] += (ram_adr[bank] & 0x40000) ? 4 : 1;
			}
			return;
		}
		if (a >= 0x20 && a < 0x24) sincos_base = v;
		else if (a >= 0x24 && a < 0x28) atan_base[a - 0x24] = v;
		else if (a >= 0x28 && a < 0x2a) inv_base = v;
		else if (a >= 0x2a && a < 0x2c) isqrt_base = v;
		else if (a == 0x2e) data_base = v;
	}

	const u32 *tables() const { return m.m_copro_tables; }

	u32 sincos_r(u32 offset)
	{
		offs_t ang = sincos_base + offset * 0x4000;
		offs_t index = ang & 0x3fff;
		if (ang & 0x4000)
			index = std::min(0x4000 - (int)index, 0x3fff);
		u32 result = tables()[index];
		if (ang & 0x8000)
			result ^= 0x80000000;
		return result;
	}

	u32 inv_r(u32 offset)
	{
		offs_t index = ((inv_base >> 9) & 0x3ffe) | (offset & 1);
		u32 result = tables()[index | 0x8000];
		u8 bexp = (inv_base >> 23) & 0xff;
		u8 exp = (result >> 23) + (0x7f - bexp);
		result = (result & 0x807fffff) | (exp << 23);
		if (inv_base & 0x80000000)
			result ^= 0x80000000;
		return result;
	}

	u32 isqrt_r(u32 offset)
	{
		offs_t index = 0x2000 ^ (((isqrt_base >> 10) & 0x3ffe) | (offset & 1));
		u32 result = tables()[index | 0xc000];
		u8 bexp = (isqrt_base >> 24) & 0x7f;
		u8 exp = (result >> 23) + (0x3f - bexp);
		result = (result & 0x807fffff) | (exp << 23);
		if (!(offset & 1))
			result &= 0x7fffffff;
		return result;
	}

	u32 atan_r()
	{
		u32 idx = atan_base[3] & 0xffff;
		if (idx & 0xc000)
			idx = 0x3fff;
		u32 result = tables()[idx | 0x4000];
		// MAME: corrects a bug in the table the hardware seems to work around
		u16 dt = (result >> 16) + result;
		if (dt & 0x001) { if ((result & 0x00f) == 0x00e) result -= 0x00000001; else result -= 0x00010000; }
		if (dt & 0x010) { if ((result & 0x0f0) == 0x0e0) result -= 0x00000010; else result -= 0x00100000; }
		if (dt & 0x100) { if ((result & 0xf00) == 0xe00) result -= 0x00000100; else result -= 0x01000000; }
		bool s0 = atan_base[0] & 0x80000000;
		bool s1 = atan_base[1] & 0x80000000;
		bool s2 = atan_base[2] & 0x80000000;
		if (s0 ^ s1 ^ s2) result >>= 16;
		if (s2) result += 0x4000;
		if ((s0 && !s2) || (s1 && s2)) result += 0x8000;
		return result & 0xffff;
	}
};

void model1::tgp_lle_deleter::operator()(tgp_lle *p) const { delete p; }

bool model1::use_tgp_program(const std::vector<uint8_t> &rom)
{
	if (rom.size() < 0x2000)
		return false;
	m_lle.reset(new tgp_lle(*this, rom, m_regions.copro_data));
	m_lle->reset();
	return true;
}

void model1::lle_reset() { if (m_lle) m_lle->reset(); }

uint64_t model1::tgp_cycles_executed() const { return m_tgp_cycles; }

// V60 side, as MAME's model1_m.cpp with the TGP brought up to date first.
u16 model1::lle_v60_r(uint32_t a)
{
	tgp_lle &t = *m_lle;
	t.sync();
	if (a >= 0xd00000 && a < 0xd20000) return m_v60_copro_ram_adr;
	if (a >= 0xd20000 && a < 0xd40000) {
		int offset = (a >> 1) & 1;
		u32 w = t.copro_ram[m_v60_copro_ram_adr & 0x1fff];
		if (!offset) return u16(w);
		if (m_v60_copro_ram_adr & 0x8000) m_v60_copro_ram_adr++;
		return u16(w >> 16);
	}
	if (a >= 0xd80000 && a < 0xda0000) {
		if (!((a >> 1) & 1)) {
			if (t.fifo_out.empty()) {
				m_tgp_underflow[m_maincpu.pc()]++;
				m_maincpu.stall();     // an IN instruction retries, as on MAME
				return 0;
			}
			m_v60_copro_fifo_r = t.fifo_out.front();
			t.fifo_out.pop_front();
			return u16(m_v60_copro_fifo_r);
		}
		return u16(m_v60_copro_fifo_r >> 16);
	}
	return 0xffff;   // dc0000: FIFO status
}

void model1::lle_v60_w(uint32_t a, uint16_t data, uint16_t mem_mask)
{
	tgp_lle &t = *m_lle;
	if (a >= 0xd00000 && a < 0xd20000) { COMBINE_DATA(&m_v60_copro_ram_adr); return; }
	if (a >= 0xd20000 && a < 0xd40000) {
		int offset = (a >> 1) & 1;
		COMBINE_DATA(m_v60_copro_ram_latch + offset);
		if (offset) {
			t.sync();
			t.copro_ram[m_v60_copro_ram_adr & 0x1fff] = m_v60_copro_ram_latch[0] | (m_v60_copro_ram_latch[1] << 16);
			if (m_v60_copro_ram_adr & 0x8000) m_v60_copro_ram_adr++;
		}
		return;
	}
	if (a >= 0xd80000 && a < 0xda0000) {
		if ((a >> 1) & 1) {
			m_v60_copro_fifo_w = (m_v60_copro_fifo_w & 0x0000ffff) | (u32(data) << 16);
			t.fifo_in.push_back(m_v60_copro_fifo_w);
			t.sync();
		} else
			m_v60_copro_fifo_w = (m_v60_copro_fifo_w & 0xffff0000) | data;
	}
}
