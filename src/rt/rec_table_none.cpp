// For builds without the generated C++ (build/gen/<game>_rec.cpp): the game's code is recompiled to
// native code when it starts (src/rt/native.cpp), which fills the table instead.
#include "rec_rt.h"

const rec_entry rec_table[1] = { { 0, nullptr } };
const size_t rec_table_size = 0;
