#pragma once

// The link to the host app (mode 1): USB (usb.h) plus the protocol task,
// which runs Protocol (protocol.h) against the model (model.h), the
// compositor and the screen. Page changes from the swipe task go to the host
// as events.

#include "esp_err.h"
#include "link_stats.h"

// Start USB and the protocol task (core 0). Call after model_init(),
// screen_init() and swipe_init().
esp_err_t host_link_init(void);

// A host app has sent a valid message since boot
bool host_link_seen(void);

// The receive path's counters since the last call (link_stats.h), reset.
// One caller: the stats line (main.cpp, CB_STATS_LOG).
LinkSample host_link_stats(void);
