// The recompile done on the player's machine: the V60 program, found with the shipped address map
// (src/rt/analyse.h), is turned into native code (ARM64 or x86-64) with sljit when the game
// starts. Nothing made from the ROM ships; this is the same job tools/recomp does into C++ for
// our own builds, and the lockstep check (model1lock --native) covers it the same way.
#pragma once
#include "analyse.h"
#include "rec_rt.h"

namespace rec {

struct native_stats { size_t functions = 0, instructions = 0, native = 0, code_bytes = 0; double ms = 0; };

// mem: the main CPU's address space (16 MB). Builds every function and makes rec::call use them.
// Call after rec::attach. Returns false (and the game runs on the interpreter) on failure.
bool build_native(const std::vector<uint8_t> &mem, const hints &map, native_stats *stats = nullptr);

} // namespace rec
