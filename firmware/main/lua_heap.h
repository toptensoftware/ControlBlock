#pragma once

// The allocator Lua runs on (docs/standalone-ops-plan.md section 2, "Memory"):
// a hard cap on what the state may hold, over a platform realloc (the PSRAM heap
// on the device, the C heap in the host tests). Over the cap an allocation
// fails, so the script errors out and the host carries on. Platform independent.

#include <stddef.h>

struct LuaHeap
{
    void *ctx;
    void *(*realloc)(void *ptr, size_t size, void *ctx);    // size 0 frees; null ptr allocates
    size_t cap;                                             // most bytes Lua may hold at once
    size_t used;
    size_t peak;
    unsigned refused;                                       // allocations refused by the cap (or the platform)
};

// lua_Alloc with a LuaHeap as its `ud`
void *LuaHeapAlloc(void *ud, void *ptr, size_t osize, size_t nsize);
