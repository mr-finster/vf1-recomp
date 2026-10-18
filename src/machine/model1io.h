// Sega Model 1 I/O board (837-8950-01), run from its own program (EPR-14869B for Virtua Fighter):
// a 4 MHz Z80, the 315-5338A I/O controller that reads the controls and writes the dual-port RAM
// the V60 sees, and a 93C46 serial EEPROM for the settings. Follows MAME's model1io.cpp and
// 315_5338a.cpp (BSD-3-Clause, copyright-holders: Dirk Best) and eepromser.cpp (BSD-3-Clause,
// copyright-holders: Aaron Giles) for the EEPROM. The Z80 is superzazu/z80 (MIT).
#pragma once
#include "mame_lite.h"
extern "C" {
#include "../../third_party/z80/z80.h"
}

class model1io
{
public:
	static constexpr int CLOCK = 4000000;    // 32 MHz / 8

	model1io(const std::vector<uint8_t> &rom, uint8_t *dpram);
	void reset();
	// Runs the Z80 for `usec` microseconds.
	void run(double usec);

	// Active-low inputs wired by the host: system/coin inputs, player 1, player 2.
	uint8_t inputs[3] = { 0xff, 0xff, 0xff };
	uint8_t analog_inputs[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	uint8_t outputs = 0;                      // lamps and coin counters

	// The EEPROM's 64 16-bit words (the board's settings), for saving.
	uint16_t eeprom[64];

	// Z80 bus (public for the core's callbacks)
	uint8_t mem_r(uint16_t a);
	void mem_w(uint16_t a, uint8_t v);

private:
	std::vector<uint8_t> m_rom;
	uint8_t m_ram[0x2000]{};
	uint8_t *m_dpram;
	z80 m_cpu;
	double m_carry = 0;
	uint64_t m_time = 0;                       // Z80 cycles since reset (the EEPROM's clock)

	// 315-5338A
	uint8_t m_port_value[7]{}, m_port_config = 0, m_serial_output = 0, m_cmd = 0;
	uint16_t m_address = 0;
	bool m_secondary = false;
	uint8_t m_adc_shift = 0;
	uint8_t io_r(int offset);
	void io_w(int offset, uint8_t data);
	uint8_t port_in(int port);
	void port_out(int port, uint8_t data);

	// 93C46 (16-bit organisation: 6 address bits, 16 data bits)
	enum ee_state { EE_RESET, EE_START, EE_COMMAND, EE_READ, EE_DATA, EE_DONE };
	ee_state m_ee = EE_RESET;
	int m_cs = 0, m_clk = 0, m_di = 0;
	uint32_t m_accum = 0, m_shift = 0;
	int m_bits = 0, m_cmd_ee = 0, m_ee_addr = 0;
	bool m_locked = true;
	uint64_t m_ready_at = 0, m_cs_rise = 0;
	bool ee_ready() const { return m_time >= m_ready_at; }
	void ee_cs(int s);
	void ee_clk(int s);
	int ee_do() const;
};
