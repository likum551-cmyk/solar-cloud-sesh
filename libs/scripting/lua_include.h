// The Lua C headers with C linkage. Lua is compiled as C (third_party/lua), the rest of this
// library as C++17 with -fno-exceptions, so Lua errors are setjmp/longjmp: no C++ object with a
// destructor may be alive in a frame that a Lua error can unwind through (see the "Lua error
// safety" notes in internal.h).
#pragma once

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
