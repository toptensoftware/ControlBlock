#pragma once

// Device layer for the standalone owner (standalone.h, host tested): the one
// lock around it, the op task that polls the queue, and the device clock.
// Spec: docs/standalone.md "Plan" step 2.
//
// Executors: hidKey and delay are built into Standalone (HID reports go out
// through usb_hid_send); the rest only log for now.

#include <stdint.h>
#include "esp_err.h"

// Arm from the loaded config (config_blob(): relocated, NULL if none) and
// start the op task. Standalone to begin with.
esp_err_t standalone_init(const uint8_t *blob);

// A router key event (TOUCH_KEY_*), on the swipe task. Callers decide first
// whether the host or standalone gets it (host_link.cpp on_key).
void standalone_key(int page, int key, int action, uint32_t time_ms);

// Once per frame, on the swipe task: `long` presses that are due
void standalone_tick(void);

// The host took control (true) or gave it back (false). Protocol task.
void standalone_set_hosted(bool hosted);
