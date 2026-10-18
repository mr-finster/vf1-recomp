#include "analyse.h"
#include "../core/shim_include/emu.h"
#include "../../third_party/mame/v60/v60d.h"
#include <algorithm>
#include <deque>
#include <regex>
#include <sstream>

namespace rec {

namespace {

struct analyser
{
	const std::vector<uint8_t> &mem;
	bool keep_text;
	program p;
	std::set<uint32_t> entries;
	std::map<uint32_t, std::set<uint32_t>> dyn;

	uint8_t rd8(uint32_t a) const { return mem[a & 0xffffff]; }
	uint16_t rd16(uint32_t a) const { return rd8(a) | (rd8(a + 1) << 8); }

	static bool is_code_addr(uint32_t a) { return (a >= 0x200000 && a < 0x300000) || (a >= 0xf80000 && a <= 0xffffff); }

	insn decode(uint32_t a) const
	{
		insn in;
		in.addr = a;
		in.op = rd8(a);
		v60_disassembler d;
		util::disasm_interface::data_buffer buf;
		buf.rd = [this](offs_t x) { return rd8(x); };
		std::ostringstream s;
		const uint32_t r = d.disassemble(s, a, buf, buf);
		in.len = r & util::disasm_interface::LENGTHMASK;
		const std::string text = s.str();
		if (keep_text) in.text = text;
		if (!in.len || text.find('$') == 0 || text.find("ERR") != std::string::npos) {
			in.kind = insn_kind::bad;
			return in;
		}
		const uint8_t op = in.op;
		auto jmp_target = [&](const std::string &t) -> bool {
			// "jmp     FE39F8[PC]" or "jsr     FE0000": a static target
			static const std::regex re(R"(^\w+\s+([0-9A-F]+)(\[PC\])?$)");
			std::smatch m;
			if (std::regex_match(t, m, re)) {
				in.targets.push_back(strtoul(m[1].str().c_str(), nullptr, 16) & 0xffffff);
				return true;
			}
			return false;
		};
		auto s8 = [](uint8_t v) { return int32_t(int8_t(v)); };
		auto s16 = [](uint16_t v) { return int32_t(int16_t(v)); };
		if (op >= 0x60 && op <= 0x7f) {
			const bool longd = op >= 0x70;
			const int32_t disp = longd ? s16(rd16(a + 1)) : s8(rd8(a + 1));
			in.targets.push_back((a + disp) & 0xffffff);
			in.kind = (op == 0x6a || op == 0x7a) ? insn_kind::jump : insn_kind::branch;
		} else if (op == 0xc6 || op == 0xc7) {
			in.targets.push_back((a + s16(rd16(a + 2))) & 0xffffff);
			in.kind = insn_kind::branch;
		} else if (op == 0x48) {
			in.targets.push_back((a + s16(rd16(a + 1))) & 0xffffff);
			in.kind = insn_kind::call;
		} else if (op == 0xe8 || op == 0xe9) {
			in.kind = insn_kind::call;
			in.indirect = !jmp_target(text);
		} else if (op == 0xd6 || op == 0xd7) {
			in.kind = insn_kind::jump;
			in.indirect = !jmp_target(text);
		} else if (op == 0x49) {
			in.kind = insn_kind::call;      // CALL addr, args: target known at run time unless PC-relative
			static const std::regex re(R"(^call\s+([0-9A-F]+)(\[PC\])?,.*$)");
			std::smatch m;
			if (std::regex_match(text, m, re))
				in.targets.push_back(strtoul(m[1].str().c_str(), nullptr, 16) & 0xffffff);
			else
				in.indirect = true;
		} else if (op == 0xf8 || op == 0xf9 || op == 0xc9) {
			in.kind = insn_kind::call;      // TRAP / BRKV: through a vector, back through RETIS/RETIU
			in.indirect = true;
		} else if (op == 0xca || op == 0xe2 || op == 0xe3 || op == 0xfa || op == 0xfb || op == 0xea || op == 0xeb) {
			in.kind = insn_kind::ret;
		}
		return in;
	}

	// All code reachable from the entries and the hints.
	void discover(const std::set<uint32_t> &covered)
	{
		std::deque<uint32_t> work(entries.begin(), entries.end());
		for (uint32_t a : covered) work.push_back(a);
		while (!work.empty()) {
			uint32_t a = work.front();
			work.pop_front();
			while (is_code_addr(a) && !p.insns.count(a)) {
				insn in = decode(a);
				p.insns[a] = in;
				if (in.kind == insn_kind::bad)
					break;
				for (uint32_t t : in.targets) {
					if (in.kind == insn_kind::call) entries.insert(t);
					work.push_back(t);
				}
				if (auto it = dyn.find(a); it != dyn.end())
					for (uint32_t t : it->second) {
						if (in.kind == insn_kind::call) entries.insert(t);
						work.push_back(t);
					}
				if (in.kind == insn_kind::jump || in.kind == insn_kind::ret)
					break;
				a += in.len;
			}
		}
	}

	// Everything reachable from the entry without following calls.
	std::set<uint32_t> body_of(uint32_t entry) const
	{
		std::set<uint32_t> body;
		std::deque<uint32_t> work{entry};
		while (!work.empty()) {
			uint32_t a = work.front();
			work.pop_front();
			while (!body.count(a)) {
				auto it = p.insns.find(a);
				if (it == p.insns.end() || it->second.kind == insn_kind::bad)
					break;
				const insn &in = it->second;
				body.insert(a);
				if (in.kind == insn_kind::branch || in.kind == insn_kind::jump) {
					for (uint32_t t : in.targets) work.push_back(t);
					if (auto d = dyn.find(a); d != dyn.end())
						for (uint32_t t : d->second) work.push_back(t);
				}
				if (in.kind == insn_kind::jump || in.kind == insn_kind::ret)
					break;
				a += in.len;
			}
		}
		return body;
	}
};

} // namespace

bool hints::read(const std::string &path)
{
	FILE *f = fopen(path.c_str(), "r");
	if (!f) return false;
	char line[256];
	unsigned a, b;
	unsigned long long n;
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "pc %x %llu", &a, &n) == 2) covered.insert(a);
		else if (sscanf(line, "edge %x %x", &a, &b) == 2) edges[a].insert(b);
		else if (sscanf(line, "irq %x %x", &a, &b) == 2) entries.insert(b);
		else if (sscanf(line, "entry %x", &a) == 1) entries.insert(a);
	}
	fclose(f);
	return true;
}

program analyse(const std::vector<uint8_t> &mem, const hints &h, bool keep_text)
{
	analyser an{ mem, keep_text };
	an.entries = h.entries;
	an.entries.insert(0xfffff0);       // reset
	an.dyn = h.edges;
	an.discover(h.covered);
	// keep the hinted edges that are real changes of flow (not the next instruction, nor from a
	// plain instruction), then look again with only those: their call targets become entries
	for (auto it = an.dyn.begin(); it != an.dyn.end();) {
		auto in = an.p.insns.find(it->first);
		if (in == an.p.insns.end()) { it = an.dyn.erase(it); continue; }
		it->second.erase(it->first + in->second.len);
		if (in->second.kind == insn_kind::normal) it->second.clear();
		if (it->second.empty()) it = an.dyn.erase(it); else ++it;
	}
	an.p.insns.clear();
	an.discover(h.covered);
	for (uint32_t e : an.entries) {
		auto it = an.p.insns.find(e);
		if (it == an.p.insns.end() || it->second.kind == insn_kind::bad) continue;
		function fn;
		fn.entry = e;
		fn.body.push_back(e);
		for (uint32_t a : an.body_of(e))
			if (a != e) fn.body.push_back(a);
		an.p.functions.push_back(std::move(fn));
	}
	an.p.edges = an.dyn;
	// the indirect jumps' hinted targets, for the code generators
	for (auto &[a, ts] : an.dyn) {
		insn &in = an.p.insns[a];
		if (in.kind == insn_kind::jump || in.kind == insn_kind::branch)
			in.hinted.assign(ts.begin(), ts.end());
	}
	return std::move(an.p);
}

void write_map(const program &p, const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "# Model 1 code map for the recompiler: where the functions start\n"
			   "# and where the indirect jumps and calls lead. Addresses only, nothing from the ROM.\n");
	for (const function &fn : p.functions) fprintf(f, "entry %06x\n", fn.entry);
	for (auto &[a, ts] : p.edges)
		for (uint32_t t : ts) fprintf(f, "edge %06x %06x\n", a, t);
	fclose(f);
}

void write_listing(const program &p, const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) return;
	static const char *kinds[] = { "normal", "branch", "jump", "call", "ret", "bad" };
	for (const function &fn : p.functions) {
		fprintf(f, "F %06x\n", fn.entry);
		for (uint32_t a : fn.body) {
			const insn &in = p.insns.at(a);
			fprintf(f, "I %06x %u %s", a, in.len, kinds[int(in.kind)]);
			for (uint32_t t : in.targets) fprintf(f, " %06x", t);
			fprintf(f, " ; %s\n", in.text.c_str());
		}
	}
	fclose(f);
}

} // namespace rec
