#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "waveshare_touch_port.h"

// Horizontal paging: drag the page area left/right, then glide to a page on
// release. Drives screen_set_scroll() once per frame from its own task.
// Call after screen_init().
esp_err_t swipe_init(void);

// Feed touch events in. Safe to call from the touch task.
void swipe_touch(uint8_t id, TOUCH_EVENT_TYPE type, uint16_t x, uint16_t y);

// The page currently shown, or being glided to.
int swipe_current_page(void);
