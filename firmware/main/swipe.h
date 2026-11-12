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

// Change the number of pages (1..SCREEN_MAX_PAGES): the swipe bounds, the
// screen and the pager. If the current page goes, moves to the last one.
// Any task; done on the swipe task at the next frame.
void swipe_set_page_count(int count);

// Go to `page`, gliding there as if swiped (`animate`) or at once. Any task.
void swipe_go_to_page(int page, bool animate);

// Told once the page settles on a new page (not while a swipe is in
// progress), on the swipe task. `cause` is SWIPE_CAUSE_*.
#define SWIPE_CAUSE_USER  0
#define SWIPE_CAUSE_HOST  1
void swipe_set_page_changed_cb(void (*cb)(int page, int cause));

// Told of every key action (TOUCH_KEY_PRESS / RELEASE / CANCEL, touch_router.h)
// with the device time in ms, on the swipe task. The key's pressed look is
// already handled. Touches are routed by touch_router (hit test on the page's
// key rects); see docs/interactions.md. The key is its id; `page` is the page
// the touch went down on.
void swipe_set_key_cb(void (*cb)(int page, uint16_t id, int action, uint32_t time_ms));

// The backlight went on or off (docs/backlight.md). While off, touches only
// wake: nothing reaches keys or swipes. Any task; applied on the swipe task,
// in order with the touches.
void swipe_set_enabled(bool enabled);

// Told on the swipe task of each touch-down while disabled, with the device
// time in ms.
void swipe_set_wake_cb(void (*cb)(uint32_t time_ms));

// Called on the swipe task at the start of every frame, after the frame's
// touches were handled (the standalone gestures' `long` timer).
void swipe_set_tick_cb(void (*cb)(void));
