#include "model1io.h"

namespace {
uint8_t z_read(void *u, uint16_t a) { return static_cast<model1io *>(u)->mem_r(a); }
void z_write(void *u, uint16_t a, uint8_t v) { static_cast<model1io *>(u)->mem_w(a, v); }
uint8_t z_in(z80 *, uint8_t) { return 0xff; }
void z_out(z80 *, uint8_t, uint8_t) {}

// MAME's 93C46 operation times, in Z80 cycles
constexpr uint64_t us(double u) { return uint64_t(u * model1io::CLOCK / 1e6); }
constexpr uint64_t WRITE_TIME = us(1750), WRITE_ALL_TIME = us(8000), ERASE_TIME = us(1000), ERASE_ALL_TIME = us(8000);
}

model1io::model1io(const std::vector<uint8_t> &rom, uint8_t *dpram) : m_rom(rom), m_dpram(dpram)
{
	m_rom.resize(0x10000, 0xff);
	std::fill(std::begin(eeprom), std::end(eeprom), 0xffff);   // MAME's default: all 1s
}

void model1io::reset()
{
	z80_init(&m_cpu);
	m_cpu.read_byte = z_read;
	m_cpu.write_byte = z_write;
	m_cpu.port_in = z_in;
	m_cpu.port_out = z_out;
	m_cpu.userdata = this;
	m_carry = 0;
	std::fill(std::begin(m_port_value), std::end(m_port_value), 0);
	m_port_config = m_serial_output = m_cmd = 0;
	m_address = 0;
	m_secondary = false;
	m_ee = EE_RESET;
	m_cs = m_clk = m_di = 0;
	m_adc_shift = 0;
	m_locked = true;
	m_time = m_ready_at = m_cs_rise = 0;   // the Z80's cycle count starts again from 0
}

void model1io::run(double usec)
{
	double target = usec * CLOCK / 1e6 + m_carry;
	unsigned long start = m_cpu.cyc;
	while (double(m_cpu.cyc - start) < target) {
		m_time = m_cpu.cyc;
		z80_step(&m_cpu);
	}
	m_carry = target - double(m_cpu.cyc - start);
	m_time = m_cpu.cyc;
}

// ---- Z80 map (MAME: model1io_device::mem_map) ----

uint8_t model1io::mem_r(uint16_t a)
{
	m_time = m_cpu.cyc;
	if (a < 0x4000) return m_rom[a];
	if (a < 0x6000) return m_ram[a - 0x4000];
	if (a >= 0x8000 && a < 0x8010) return io_r(a & 0xf);
	if (a >= 0xc000 && a < 0xc004) {
		// The MSM6253 latches on address_w and shifts its sample out MSB-first on D0.
		const uint8_t bit = m_adc_shift >> 7;
		m_adc_shift <<= 1;
		return uint8_t(0xfe | bit);
	}
	return 0;
}

void model1io::mem_w(uint16_t a, uint8_t v)
{
	m_time = m_cpu.cyc;
	if (a >= 0x4000 && a < 0x6000) m_ram[a - 0x4000] = v;
	else if (a >= 0x8000 && a < 0x8010) io_w(a & 0xf, v);
	else if (a >= 0xc000 && a < 0xc004)
		m_adc_shift = analog_inputs[(a & 3) + (m_secondary ? 4 : 0)];
}

// ---- 315-5338A (MAME: sega_315_5338a_device::read/write) ----

uint8_t model1io::io_r(int offset)
{
	switch (offset) {
	case 0: case 1: case 2: case 3: case 4: case 5: case 6:
		return BIT(m_port_config, offset) ? port_in(offset) : m_port_value[offset];
	case 0x08: return m_port_config;
	case 0x0a: return m_serial_output;
	case 0x0b: return m_cmd;
	case 0x0c: return m_dpram[m_address & 0x7ff];
	case 0x0d: return 0x08;
	}
	return 0xff;
}

void model1io::io_w(int offset, uint8_t data)
{
	switch (offset) {
	case 0: case 1: case 2: case 3: case 4: case 5: case 6:
		m_port_value[offset] = data;
		port_out(offset, data);
		break;
	case 0x08: {
		uint8_t changed = data ^ m_port_config;
		for (int i = 0; i < 7; i++)
			if (BIT(changed, i) && BIT(data, i) == 0)
				port_out(i, m_port_value[i]);
		m_port_config = data;
		break;
	}
	case 0x09:
		m_cmd = data;
		switch (data) {
		case 0x00: m_address = (m_address & 0xff00) | m_serial_output; break;
		case 0x01: m_address = (m_address & 0x00ff) | (m_serial_output << 8); break;
		case 0x07: m_dpram[m_address & 0x7ff] = m_serial_output; break;
		case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
			m_dpram[data & 7] = m_serial_output;
			break;
		}
		break;
	case 0x0a: m_serial_output = data; break;
	}
}

// The board's ports (MAME: model1io_device::io_p*_r/w); the DIP switches are all off (0xff).
uint8_t model1io::port_in(int port)
{
	switch (port) {
	case 1: return m_secondary ? 0xff : inputs[0];
	case 2: return m_secondary ? 0xff : inputs[1];
	case 3: return m_secondary ? 0xff : inputs[2];
	case 4: return 0xff;                                       // drive board
	case 6: return uint8_t((ee_do() << 7) | 0x70 | 0x0f);      // EEPROM DO, board buttons up
	}
	return 0xff;
}

void model1io::port_out(int port, uint8_t data)
{
	if (port == 0) {
		// 7 eeprom clk, 6 eeprom cs, 5 eeprom di, 0 control switch
		m_di = BIT(data, 5);
		ee_cs(BIT(data, 6));
		ee_clk(BIT(data, 7));
		m_secondary = BIT(data, 0);
	} else if (port == 5)
		outputs = data;
}

// ---- 93C46 (MAME: eeprom_serial_93cxx_device) ----

void model1io::ee_cs(int s)
{
	if (s == m_cs) return;
	m_cs = s;
	if (s) {
		m_cs_rise = m_time;
		if (m_ee == EE_RESET) m_ee = EE_START;
	} else
		m_ee = EE_RESET;
}

void model1io::ee_clk(int s)
{
	if (s == m_clk) return;
	m_clk = s;
	if (!s) return;     // everything happens on rising edges
	switch (m_ee) {
	case EE_START:
		if (m_di && ee_ready() && m_time > m_cs_rise) {
			m_accum = 0;
			m_bits = 0;
			m_ee = EE_COMMAND;
		}
		break;
	case EE_COMMAND:
		m_accum = (m_accum << 1) | m_di;
		if (++m_bits == 2 + 6) {
			int addr = m_accum & 0x3f;
			int op = m_accum >> 6;
			m_bits = 0;
			if (op == 0) {
				switch (addr >> 4) {
				case 0: m_locked = true; m_ee = EE_RESET; break;                    // EWDS
				case 1: m_cmd_ee = 1; m_shift = 0; m_ee = EE_DATA; break;           // WRAL
				case 2:                                                             // ERAL
					if (m_locked) { m_ee = EE_RESET; break; }
					std::fill(std::begin(eeprom), std::end(eeprom), 0xffff);
					m_ready_at = m_time + ERASE_ALL_TIME;
					m_ee = EE_DONE;
					break;
				case 3: m_locked = false; m_ee = EE_RESET; break;                   // EWEN
				}
			} else if (op == 1) {                                                    // WRITE
				m_cmd_ee = 0; m_ee_addr = addr; m_shift = 0; m_ee = EE_DATA;
			} else if (op == 2) {                                                    // READ
				m_ee_addr = addr; m_shift = 0; m_ee = EE_READ;
			} else {                                                                 // ERASE
				if (m_locked) { m_ee = EE_RESET; break; }
				eeprom[addr] = 0xffff;
				m_ready_at = m_time + ERASE_TIME;
				m_ee = EE_DONE;
			}
		}
		break;
	case EE_READ: {
		int bit = m_bits++;
		if (bit % 16 == 0 && bit == 0)
			m_shift = uint32_t(eeprom[(m_ee_addr + m_bits / 16) & 0x3f]) << 16;
		else
			m_shift = (m_shift << 1) | 1;
		break;
	}
	case EE_DATA:
		m_shift = (m_shift << 1) | m_di;
		if (++m_bits == 16) {
			if (m_locked) { m_ee = EE_RESET; break; }
			if (m_cmd_ee == 0) {
				eeprom[m_ee_addr] = uint16_t(m_shift);
				m_ready_at = m_time + WRITE_TIME;
			} else {
				for (auto &w : eeprom) w &= uint16_t(m_shift);
				m_ready_at = m_time + WRITE_ALL_TIME;
			}
			m_ee = EE_DONE;
		}
		break;
	default:
		break;
	}
}

int model1io::ee_do() const
{
	if (m_ee == EE_START) return ee_ready() ? 1 : 0;   // READY/BUSY
	if (m_ee == EE_READ) return (m_shift & 0x80000000) ? 1 : 0;
	return 1;                                        // tri-stated, pulled up
}
