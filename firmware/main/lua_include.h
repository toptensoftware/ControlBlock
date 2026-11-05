#pragma once

// Lua's C headers for C++ files. The ESP-IDF toolchain's <limits.h> leaves LLONG_MAX
// undefined in C++ mode, which luaconf.h takes for "no long long" (Lua is compiled as C,
// where it is defined, so both sides must agree on 64-bit integers): define them here.

#include <limits.h>

#if !defined(LLONG_MAX) && defined(__LONG_LONG_MAX__)
#define LLONG_MAX   __LONG_LONG_MAX__
#define LLONG_MIN   (-LLONG_MAX - 1LL)
#define ULLONG_MAX  (LLONG_MAX * 2ULL + 1ULL)
#endif

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
