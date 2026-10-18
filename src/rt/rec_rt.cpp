// Runtime for the recompiled code: cycle counting and interrupts exactly as the interpreter's
// run loop does them (8 cycles an instruction, interrupts taken between instructions), the
// coroutine that hands control back at the end of each slice, and a fallback to the
// interpreter for code the recompiler didn't reach.
#include "rec_rt.h"
#include "coro.h"

#define PC (*rec::pc)
#define TICK() do { if ((rec::cycles -= 8) <= 0 || rec::cpu->irq_line_up()) rec::after_insn_slow(); } while (0)
#include <algorithm>
#include <set>
#include <string>

namespace rec {

v60_device *cpu;
uint32_t *pc;
int cycles;

static coro_ctx host_ctx, game_ctx;
static std::vector<uint8_t> game_stack;
static bool started;
static std::string entries_path;

static void yield() { coro_switch(&game_ctx, &host_ctx); }

void after_insn_slow()
{
	// Interpreter order: after an instruction, take a pending interrupt; the slice ends when the
	// cycles run out; the next slice starts by checking for an interrupt again.
	bool entered = cpu->irq_line_up() && cpu->service_irq();
	while (cycles <= 0) {
		yield();
		if (!entered && cpu->irq_line_up())
			entered = cpu->service_irq();
	}
	if (entered) {
		uint32_t resume = 0;
		(void)resume;
		call(PC);   // the handler runs until its RETIS, which restores PC
	}
}

static bool is_call_op(uint8_t op) { return op == 0x48 || op == 0x49 || op == 0xe8 || op == 0xe9 || op == 0xf8 || op == 0xf9 || op == 0xc9; }
static bool is_ret_op(uint8_t op) { return op == 0xca || op == 0xe2 || op == 0xe3 || op == 0xfa || op == 0xfb || op == 0xea || op == 0xeb; }

static void interpret_until_return(uint32_t addr)
{
	static std::set<uint32_t> logged;
	if (logged.insert(addr).second) {
		// remembered for the next recompile (the Makefile hands v60recomp build/cov/<game>/)
		const char *path = getenv("REC_RUNTIME_ENTRIES");
		if (!path) path = entries_path.c_str();
		if (FILE *f = fopen(path, "a")) {
			fprintf(f, "entry %06x\n", addr);
			fclose(f);
		}
		if (getenv("REC_VERBOSE"))
			fprintf(stderr, "rec: interpreting %06x (not recompiled yet; noted for the next build)\n", addr);
	}
	int depth = 0;
	for (;;) {
		uint8_t op = cpu->read_op8(PC);
		uint32_t n = cpu->exec_handler();
		if (n) PC += n;
		TICK();
		if (!n) {
			if (is_call_op(op)) depth++;
			else if (is_ret_op(op) && depth-- == 0) return;
		}
	}
}

static const rec_entry *g_table = rec_table;
static size_t g_table_size = rec_table_size;
static size_t g_fallback_count;
static uint32_t g_first_fallback = 0xffffffff;

void use_table(const rec_entry *t, size_t n)
{
	g_table = t;
	g_table_size = n;
}

size_t fallback_count()
{
	return g_fallback_count;
}

uint32_t first_fallback_address()
{
	return g_first_fallback;
}

void reset_fallbacks()
{
	g_fallback_count = 0;
	g_first_fallback = 0xffffffff;
}

void call(uint32_t addr)
{
	const rec_entry *e = std::lower_bound(g_table, g_table + g_table_size, addr,
		[](const rec_entry &a, uint32_t v) { return a.addr < v; });
	if (e != g_table + g_table_size && e->addr == addr)
		e->fn(*cpu);
	else {
		if (!g_fallback_count) g_first_fallback = addr;
		g_fallback_count++;
		interpret_until_return(addr);
	}
}

void leave(uint32_t fn, uint32_t to)
{
	static std::set<std::pair<uint32_t, uint32_t>> seen;
	if (getenv("REC_LOG_LEAVE") && seen.insert({fn, to}).second)
		fprintf(stderr, "rec: f_%06x jumps out to %06x\n", fn, to);
}

void bad(uint32_t addr)
{
	fprintf(stderr, "rec: reached undecodable code at %06x\n", addr);
	abort();
}

void len_mismatch(uint32_t addr, uint32_t n, uint32_t want)
{
	fprintf(stderr, "rec: instruction at %06x had length %u, the recompiler expected %u\n", addr, n, want);
	abort();
}

static void game_main(void *)
{
	for (;;)
		call(PC);
}

void attach(v60_device &c, std::string_view game)
{
	entries_path = "build/cov/" + std::string(game) + "/runtime_entries.txt";
	cpu = &c;
	pc = &c.pc_ref();
	cycles = 0;
	c.m_cycles = &cycles;
	reset_fallbacks();
	game_stack.assign(256 << 20, 0);   // deep: tail jumps between functions nest C frames
	coro_init(&game_ctx, game_stack.data(), game_stack.size(), game_main, nullptr);
	started = true;
}

void detach(v60_device &c)
{
	c.own_cycles();
	std::vector<uint8_t>().swap(game_stack);
	started = false;
	cpu = nullptr;
}

void run(int n)
{
	cycles += n;   // the last slice's overrun carries, as on MAME's scheduler
	coro_switch(&host_ctx, &game_ctx);
}

} // namespace rec
