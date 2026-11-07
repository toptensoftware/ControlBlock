#pragma once

// The RUN_STRESS Lua churn script (main.cpp): allocates and frees a lot of its (PSRAM) heap, as a
// busy script op does. Run back to back by the stress run for STRESS_SCRIPT_BUSY_US at a time, so one
// run must be short (well under the SCRIPT_SLICE_MS slice) and small (well under SCRIPT_HEAP_CAP),
// or the stress run measures the script's limits instead of its PSRAM traffic. The host tests
// (TestScriptHost.cpp) check its heap; its time is only known on the device. 800 iterations took
// ~90 ms a run on the device (over 100 ms mid-swipe) and peaked at the cap; 160 aims at ~18 ms.

static const char STRESS_LUA[] =
    "local t = {}\n"
    "for i = 1, 160 do t[i] = string.rep('x', 40) .. i end\n"
    "local s = table.concat(t, ',')\n"
    "local u = {}\n"
    "for w in s:gmatch('[^,]+') do u[#u + 1] = #w end\n"
    "total = (total or 0) + #s + #u\n";
