#include "native.h"
#include "../../third_party/sljit/sljitLir.h"
#include <algorithm>
#include <chrono>
#include <regex>
#include <unordered_map>

#define PC (*rec::pc)

namespace rec {

namespace {

struct native_fn
{
	uint32_t entry = 0;
	void *code = nullptr;
	std::unordered_map<uint32_t, sljit_uw> labels;   // V60 address -> native address in the function
};

std::vector<native_fn> g_fns;
std::vector<rec_entry> g_table;

// ---- what the native code calls ----

// One instruction at PC, as the generated C++ does with EXEC(): MAME's handler, then the
// interpreter's cycle count and interrupt check. Returns the length, or 0 when PC was set.
sljit_u32 SLJIT_FUNC rt_step()
{
	const uint32_t n = cpu->exec_handler();
	if (n) PC += n;
	if ((cycles -= 8) <= 0 || cpu->irq_line_up()) after_insn_slow();
	return n;
}

sljit_u32 SLJIT_FUNC rt_rd8(sljit_uw a) { return cpu->data_read8(uint32_t(a)); }
void SLJIT_FUNC rt_slow() { after_insn_slow(); }

void SLJIT_FUNC rt_call() { call(PC); }
void SLJIT_FUNC rt_jump(sljit_uw to) { call(uint32_t(to)); }
void SLJIT_FUNC rt_bad(sljit_uw at) { bad(uint32_t(at)); }

// Where in function `fn` to carry on for the current PC (0: not in it).
sljit_uw SLJIT_FUNC rt_dispatch(sljit_uw fn)
{
	const auto &m = g_fns[fn].labels;
	auto it = m.find(PC);
	return it == m.end() ? 0 : it->second;
}

void SLJIT_FUNC rt_leave(sljit_uw fn)
{
	leave(g_fns[fn].entry, PC);
	call(PC);
}

// ---- code generation ----

struct gen
{
	sljit_compiler *c = nullptr;
	const program &p;
	const function &fn;
	size_t index;
	std::unordered_map<uint32_t, sljit_label *> labels;
	std::vector<std::pair<sljit_jump *, uint32_t>> to_label;   // jumps to V60 addresses in the body
	std::vector<sljit_jump *> to_dispatch;

	void load_pc() { sljit_emit_op1(c, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM0(), sljit_sw(rec::pc)); }
	void call0(void *f, sljit_s32 args) { sljit_emit_icall(c, SLJIT_CALL, args, SLJIT_IMM, sljit_sw(f)); }
	void call1(void *f, sljit_s32 args, sljit_sw a)
	{
		sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, a);
		sljit_emit_icall(c, SLJIT_CALL, args, SLJIT_IMM, sljit_sw(f));
	}
	void jump_if_pc(uint32_t addr)        // PC (in R0) == addr: go to its label
	{
		to_label.push_back({ sljit_emit_cmp(c, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, addr), addr });
	}

	const bool native_ok = getenv("M1_NO_NATIVE_OPS") == nullptr;   // testing: every instruction via MAME

	// ---- instructions done natively (the rest run MAME's handler through rt_step) ----

	sljit_sw reg_addr(int r) const { return sljit_sw(cpu->reg_ptr() + r); }
	sljit_sw flag_addr(int f) const { return sljit_sw(cpu->flags_ptr() + f); }   // 0 CY, 1 OV, 2 S, 3 Z

	void set_pc(uint32_t v) { sljit_emit_op1(c, SLJIT_MOV32, SLJIT_MEM0(), sljit_sw(rec::pc), SLJIT_IMM, v); }

	// The run loop's bookkeeping after an instruction: 8 cycles, and the slow path (end of the
	// slice, or an interrupt to take) when they run out or the interrupt line is up.
	void tick()
	{
		sljit_emit_op2(c, SLJIT_SUB32, SLJIT_MEM0(), sljit_sw(&cycles), SLJIT_MEM0(), sljit_sw(&cycles), SLJIT_IMM, 8);
		sljit_jump *out_of_cycles = sljit_emit_cmp(c, SLJIT_SIG_LESS_EQUAL | SLJIT_32, SLJIT_MEM0(), sljit_sw(&cycles), SLJIT_IMM, 0);
		sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM0(), sljit_sw(cpu->irq_line_ptr()));
		sljit_jump *irq = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		sljit_jump *done = sljit_emit_jump(c, SLJIT_JUMP);
		sljit_label *slow = sljit_emit_label(c);
		sljit_set_label(out_of_cycles, slow);
		sljit_set_label(irq, slow);
		call0((void *)rt_slow, SLJIT_ARGS0V());
		sljit_set_label(done, sljit_emit_label(c));
	}

	// After a native instruction: carry on at PC (normally the next one; an interrupt taken in
	// tick() returns to it as well).
	void continue_at(const insn &in, const std::set<uint32_t> &body, bool may_branch)
	{
		load_pc();
		if (may_branch)
			for (uint32_t t : in.targets) if (body.count(t)) jump_if_pc(t);
		const uint32_t next = in.addr + in.len;
		sljit_jump *on = sljit_emit_cmp(c, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, next);
		to_dispatch.push_back(sljit_emit_jump(c, SLJIT_JUMP));
		sljit_set_label(on, sljit_emit_label(c));
	}

	bool emit_native(const insn &in, const std::set<uint32_t> &body)
	{
		static const std::regex mov_b_abs(R"(^mov\.b\s+([0-9A-F]+), R(\d+)$)");
		static const std::regex test1_imm(R"(^test1\s+#([0-9A-F]+), R(\d+)$)");
		std::smatch m;
		const uint32_t next = in.addr + in.len;
		if (std::regex_match(in.text, m, mov_b_abs)) {
			// Rn's low byte = the byte at the address (through the bus: it may be I/O)
			const int r = std::stoi(m[2]);
			if (r > 31) return false;
			call1((void *)rt_rd8, SLJIT_ARGS1(32, W), sljit_sw(std::stoul(m[1], nullptr, 16)));
			sljit_emit_op1(c, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_MEM0(), reg_addr(r));
			sljit_emit_op2(c, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, ~0xff);
			sljit_emit_op2(c, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xff);
			sljit_emit_op2(c, SLJIT_OR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
			sljit_emit_op1(c, SLJIT_MOV32, SLJIT_MEM0(), reg_addr(r), SLJIT_R1, 0);
			set_pc(next);
			tick();
			continue_at(in, body, false);
			return true;
		}
		if (std::regex_match(in.text, m, test1_imm)) {
			// CY = bit k of Rn, Z = !CY
			const int r = std::stoi(m[2]);
			const unsigned k = std::stoul(m[1], nullptr, 16);
			if (r > 31 || k > 31) return false;
			sljit_emit_op1(c, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_MEM0(), reg_addr(r));
			if (k) sljit_emit_op2(c, SLJIT_LSHR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, k);
			sljit_emit_op2(c, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 1);
			sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_MEM0(), flag_addr(0), SLJIT_R1, 0);
			sljit_emit_op2(c, SLJIT_XOR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 1);
			sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_MEM0(), flag_addr(3), SLJIT_R1, 0);
			set_pc(next);
			tick();
			continue_at(in, body, false);
			return true;
		}
		const uint8_t op = in.op;
		const bool be = op == 0x64 || op == 0x74, bne = op == 0x65 || op == 0x75, br = op == 0x6a || op == 0x7a;
		if ((be || bne || br) && in.targets.size() == 1) {
			const uint32_t target = in.targets[0];
			if (!br) {
				// the conditional branches first make every flag 0 or 1 (MAME's NORMALIZEFLAGS)
				for (int f = 0; f < 4; f++) {
					sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM0(), flag_addr(f));
					sljit_emit_op2u(c, SLJIT_SUB32 | SLJIT_SET_Z, SLJIT_R1, 0, SLJIT_IMM, 0);
					sljit_emit_op_flags(c, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_NOT_ZERO);
					sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_MEM0(), flag_addr(f), SLJIT_R1, 0);
				}
				// R1 holds Z now
				sljit_jump *taken = sljit_emit_cmp(c, be ? SLJIT_NOT_EQUAL : SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
				set_pc(next);
				sljit_jump *joined = sljit_emit_jump(c, SLJIT_JUMP);
				sljit_set_label(taken, sljit_emit_label(c));
				set_pc(target);
				sljit_set_label(joined, sljit_emit_label(c));
			} else {
				set_pc(target);
			}
			tick();
			if (br) {
				load_pc();
				if (body.count(target)) jump_if_pc(target);
				to_dispatch.push_back(sljit_emit_jump(c, SLJIT_JUMP));
			} else {
				continue_at(in, body, true);
			}
			return true;
		}
		return false;
	}

	bool build(native_fn &out, native_stats &st)
	{
		c = sljit_create_compiler(nullptr);
		if (!c) return false;
		sljit_emit_enter(c, 0, SLJIT_ARGS1V(P), 2, 1, 0);
		const std::set<uint32_t> body(fn.body.begin(), fn.body.end());

		// entered somewhere other than the top (a jump into the middle): dispatch
		load_pc();
		to_dispatch.push_back(sljit_emit_cmp(c, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, fn.entry));

		for (size_t i = 0; i < fn.body.size(); i++) {
			const uint32_t a = fn.body[i];
			const insn &in = p.insns.at(a);
			const uint32_t next = a + in.len;
			labels[a] = sljit_emit_label(c);
			st.instructions++;
			if (in.kind == insn_kind::bad) {
				call1((void *)rt_bad, SLJIT_ARGS1V(W), a);
				sljit_emit_return_void(c);
				continue;
			}
			if (native_ok && emit_native(in, body)) {
				st.native++;
				if (in.kind == insn_kind::jump) continue;     // (br: always went to its target)
				goto after;
			}
			call0((void *)rt_step, SLJIT_ARGS0(32));
			switch (in.kind) {
			case insn_kind::normal:
				to_dispatch.push_back(sljit_emit_cmp(c, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0));
				break;
			case insn_kind::branch:
			case insn_kind::jump: {
				sljit_jump *went_on = sljit_emit_cmp(c, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0);
				load_pc();
				for (uint32_t t : in.targets) if (body.count(t)) jump_if_pc(t);
				for (uint32_t t : in.hinted) if (body.count(t)) jump_if_pc(t);
				to_dispatch.push_back(sljit_emit_jump(c, SLJIT_JUMP));
				if (in.kind == insn_kind::branch) sljit_set_label(went_on, sljit_emit_label(c));
				else to_dispatch.push_back(went_on);      // (a jump always sets PC itself)
				break;
			}
			case insn_kind::call: {
				sljit_jump *went_on = sljit_emit_cmp(c, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0);
				call0((void *)rt_call, SLJIT_ARGS0V());
				load_pc();
				to_dispatch.push_back(sljit_emit_cmp(c, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, next));
				sljit_set_label(went_on, sljit_emit_label(c));
				break;
			}
			case insn_kind::ret:
				sljit_emit_return_void(c);
				break;
			default:
				break;
			}
		after:
			const bool falls = in.kind != insn_kind::jump && in.kind != insn_kind::ret;
			if (falls) {
				const uint32_t written_next = i + 1 < fn.body.size() ? fn.body[i + 1] : ~0u;
				if (written_next != next) {
					if (body.count(next)) {
						to_label.push_back({ sljit_emit_jump(c, SLJIT_JUMP), next });
					} else {
						call1((void *)rt_jump, SLJIT_ARGS1V(W), next);
						sljit_emit_return_void(c);
					}
				}
			}
		}

		// dispatch: carry on at PC inside this function if it's here, else leave for it
		sljit_label *dispatch = sljit_emit_label(c);
		call1((void *)rt_dispatch, SLJIT_ARGS1(W, W), sljit_sw(index));
		sljit_jump *not_here = sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
		sljit_emit_ijump(c, SLJIT_JUMP, SLJIT_R0, 0);
		sljit_set_label(not_here, sljit_emit_label(c));
		call1((void *)rt_leave, SLJIT_ARGS1V(W), sljit_sw(index));
		sljit_emit_return_void(c);

		for (sljit_jump *j : to_dispatch) if (j) sljit_set_label(j, dispatch);
		for (auto &[j, t] : to_label) sljit_set_label(j, labels.at(t));

		out.entry = fn.entry;
		out.code = sljit_generate_code(c, 0, nullptr);
		if (!out.code) { sljit_free_compiler(c); return false; }
		st.code_bytes += sljit_get_generated_code_size(c);
		for (auto &[a, l] : labels) out.labels[a] = sljit_get_label_addr(l);
		sljit_free_compiler(c);
		return true;
	}
};

} // namespace

bool build_native(const std::vector<uint8_t> &mem, const hints &map, native_stats *stats)
{
	const auto t0 = std::chrono::steady_clock::now();
	native_stats st;
	program p = analyse(mem, map, true);
	std::vector<native_fn> fns(p.functions.size());
	for (size_t i = 0; i < p.functions.size(); i++) {
		gen g{ nullptr, p, p.functions[i], i };
		if (!g.build(fns[i], st)) {
			fprintf(stderr, "rec: couldn't make native code for %06x\n", p.functions[i].entry);
			for (auto &f : fns) if (f.code) sljit_free_code(f.code, nullptr);
			return false;
		}
		st.functions++;
	}
	for (auto &f : g_fns) if (f.code) sljit_free_code(f.code, nullptr);
	g_fns = std::move(fns);
	g_table.clear();
	for (const native_fn &f : g_fns)
		g_table.push_back({ f.entry, reinterpret_cast<void (*)(v60_device &)>(f.code) });
	std::sort(g_table.begin(), g_table.end(), [](const rec_entry &a, const rec_entry &b) { return a.addr < b.addr; });
	use_table(g_table.data(), g_table.size());
	st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (stats) *stats = st;
	return true;
}

} // namespace rec
