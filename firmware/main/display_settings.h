#pragma once

// The stored display timing (DisplayConfig, display_config.h), device layer
// (hardware only): loads it from NVS, starts the panel with it and applies a
// change at the next VSYNC. Experimental (docs/protocol.md DISPLAY_SET).

#include "display_config.h"

// Load the timing from NVS (namespace cb_display; nvs_flash_init() first) and
// hand it to the screen as the timing the panel starts with. Before screen_init:
// the panel is created with it, so the boot shows no switch.
void display_settings_init(void);

// The setting (shared with Protocol): a change applies at the next VSYNC
DisplayConfig *display_config(void);
