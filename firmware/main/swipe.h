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
