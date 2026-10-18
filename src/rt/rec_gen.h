// Included by the generated code (build/gen/<game>_rec.cpp): its functions are members of a class
// derived from v60_device, so MAME's handler bodies and macros work in them unchanged.
#pragma once
#include "rec_rt.h"
#include "../core/v60_macros.h"

#define EXEC() exec_handler()
#define TICK() do { if ((rec::cycles -= 8) <= 0 || irq_line_up()) rec::after_insn_slow(); } while (0)
#define CALL(a) rec::call(a)
#define JUMP(a) rec::call(a)
#define BAD(a) rec::bad(a)
#define LEAVE(fn) rec::leave(fn, PC)
