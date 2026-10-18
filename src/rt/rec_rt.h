// Runtime for the recompiled V60 code (build/gen/<game>_rec.cpp).
#pragma once
#include "../core/v60_device.h"
#include <cstddef>
#include <string_view>

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
size_t fallback_count();
uint32_t first_fallback_address();
void reset_fallbacks();
[[noreturn]] void bad(uint32_t addr);
void leave(uint32_t fn, uint32_t to);     // a function jumping somewhere outside its body
void len_mismatch(uint32_t addr, uint32_t n, uint32_t want);

// Host side
// game: whose code it is (the entries it falls back to the interpreter for are noted in
// build/cov/<game>/runtime_entries.txt when that folder is there; REC_RUNTIME_ENTRIES: elsewhere)
void attach(v60_device &c, std::string_view game);
// Back to the interpreter: the CPU counts its own cycles again and the game stack is freed.
void detach(v60_device &c);
void run(int cycles);                   // runs the game for a scanline's worth of cycles
}
