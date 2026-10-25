// Runtime for the recompiled V60 code (build/gen/vf_rec.cpp).
#pragma once
#include "../core/v60_device.h"

struct rec_entry { uint32_t addr; void (*fn)(v60_device &); };
extern const rec_entry rec_table[];
extern const size_t rec_table_size;

namespace rec {
extern v60_device *cpu;
extern uint32_t *pc;
extern int cycles;

void after_insn_slow();
void call(uint32_t addr);               // runs the code at addr until it returns
void use_table(const rec_entry *t, size_t n);   // the recompiled functions (the C++ build's by default)
[[noreturn]] void bad(uint32_t addr);
void leave(uint32_t fn, uint32_t to);     // a function jumping somewhere outside its body
void len_mismatch(uint32_t addr, uint32_t n, uint32_t want);

// Host side
void attach(v60_device &c);
// Back to the interpreter: the CPU counts its own cycles again and the game stack is freed.
void detach(v60_device &c);
void run(int cycles);                   // runs the game for a scanline's worth of cycles
// The save state's part for the recompiled code (netplay's rollback): the game's stack as it's parked at a
// frame's end, its host C frames being the guest's call chain (a frame ends deep inside the game's code). Only
// for the same process and code: its bytes hold the host's addresses (the code, the board), so they're never
// compared between two machines; nothing it points to moves while a match runs (no recompile, the same board).
// Appended to out; load_stack false for a stack from no coroutine.
void save_stack(std::vector<uint8_t> &out);
bool load_stack(const uint8_t *p, size_t n);
size_t stack_used();                    // bytes, as parked now
}

