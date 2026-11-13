#pragma once

// Our Lua settings, included at the end of lua.h through LUA_USER_H (set by main/CMakeLists.txt
// and test/test.mk.js), so every Lua source and our own code see the same values.

// Most nested C calls (pcall, string.gsub and table.sort callbacks, metamethods, the parser's
// recursion): past it a script gets an error ("C stack overflow"). Lua's default of 200 needs
// far more C stack than a device task has. Measured with -fstack-usage: a pcall level costs ~560 B
// on the device, a string.gsub callback level ~1.25 KB, a parser level ~150 B. The op task's
// stack (standalone_task.cpp SCRIPT_TASK_STACK) is sized for this many gsub levels plus a run's
// base use. Parser nesting counts too (each `..` of a chain nests a level): at 40 a 28-piece
// chain inside a few blocks failed to compile (TestScriptHost), so don't go lower.
#define LUAI_MAXCCALLS  64
