// Disassembles V60 code from the Model 1 main CPU image: model1dis build/vf/maincpu.bin <addr> [count]
#include "../src/core/shim_include/emu.h"
#include "../third_party/mame/v60/v60d.h"
#include <sstream>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: model1dis maincpu.bin addr [count]\n"); return 1; }
	FILE *f = fopen(argv[1], "rb");
	std::vector<u8> mem(0x2000000);
	fread(mem.data(), 1, mem.size(), f);
	fclose(f);
	u32 pc = strtoul(argv[2], nullptr, 16);
	int n = argc > 3 ? atoi(argv[3]) : 32;
	v60_disassembler d;
	util::disasm_interface::data_buffer buf;
	buf.rd = [&](offs_t a) { return mem[a & 0xffffff]; };
	for (int i = 0; i < n; i++) {
		std::ostringstream s;
		u32 r = d.disassemble(s, pc, buf, buf);
		u32 len = r & util::disasm_interface::LENGTHMASK;
		printf("%06X: ", pc);
		for (u32 k = 0; k < 8; k++) printf(k < len ? "%02x" : "  ", mem[(pc + k) & 0xffffff]);
		printf("  %s\n", s.str().c_str());
		pc += len ? len : 1;
	}
}
