#include "../src/machine/model1io.h"
#include <cassert>

static uint8_t read_adc(model1io &io, int channel)
{
	io.mem_w(uint16_t(0xc000 + channel), 0);
	uint8_t sample = 0;
	for (int bit = 0; bit < 8; bit++)
		sample = uint8_t((sample << 1) | (io.mem_r(0xc000) & 1));
	return sample;
}

int main()
{
	uint8_t dpram[0x800]{};
	model1io io({}, dpram);
	io.analog_inputs[0] = 0x96;
	io.analog_inputs[1] = 0x31;
	io.analog_inputs[4] = 0xa5;

	assert(read_adc(io, 0) == 0x96);
	assert(io.mem_r(0xc000) == 0xfe);

	io.analog_inputs[2] = 0x00;
	io.mem_w(0xc002, 0);
	io.analog_inputs[2] = 0xff;
	uint8_t latched = 0;
	for (int bit = 0; bit < 8; bit++)
		latched = uint8_t((latched << 1) | (io.mem_r(0xc003) & 1));
	assert(latched == 0x00);

	io.mem_w(0x8000, 0x01);
	assert(read_adc(io, 0) == 0xa5);
	assert(read_adc(io, 1) == 0xff);
}
