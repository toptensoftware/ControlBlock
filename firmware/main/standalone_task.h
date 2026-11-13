#pragma once

// Device layer for the standalone owner (standalone.h, host tested): the one
// lock around it, the op task that polls the queue, and the device clock.
// Spec: docs/standalone.md "Plan" step 2.
//
// Executors: hidKey and delay are built into Standalone (HID reports go out
// through usb_hid_send); the rest only log for now.

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "script_host.h"

// The Lua heap (PSRAM) and log for a ScriptHost: the op task's, and the stress run's
ScriptPlatform standalone_script_platform(void);

// A task that runs Lua (the op task, the stress run's churn): on core 0, its stack
// (SCRIPT_TASK_STACK) in PSRAM, sized for Lua's C nesting limit (lua_user.h). Such a
// task must never run with the cache off (no flash writes from it).
BaseType_t standalone_create_script_task(TaskFunction_t fn, const char *name, UBaseType_t prio, TaskHandle_t *out);

// Arm from the loaded layout (layout_blob(): relocated, NULL if none) and
// start the op task. Standalone to begin with.
esp_err_t standalone_init(const uint8_t *blob);

// A router key event (TOUCH_KEY_*) for the key with `id`, on the swipe task.
// Callers decide first whether the host or standalone gets it (host_link.cpp on_key).
void standalone_key(uint16_t id, int action, uint32_t time_ms);

// Once per frame, on the swipe task: `long` presses that are due
void standalone_tick(void);

// The host took control (true) or gave it back (false). Protocol task.
void standalone_set_hosted(bool hosted);

// For the diagnostics (/status): copies of what the op task last saw (any task)
struct StandaloneDiag
{
    bool     busy;              // the op queue is running
    int      activity;          // selected activity, -1 = none
    uint32_t script_runs, script_errors, script_heap_peak;  // since the last re-arm
};
void standalone_get_diag(StandaloneDiag *out);

// An activity's name from the layout ("" for -1 or no layout)
const char *standalone_activity_name(int index);
