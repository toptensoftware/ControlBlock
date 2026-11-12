#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "waveshare_rgb_lcd_port.h"
#include "screen_layout.h"

// Diagnostic: screen_init() leaves the display off (no panel, no fill ISR) and
// main leaves the backlight off. Build with -DCB_NO_DISPLAY=ON (main/CMakeLists.txt).
#ifndef NO_DISPLAY
#define NO_DISPLAY  0
#endif

// Diagnostic: the panel runs as normal (same signals and clock) but the bounce
// fill returns at once, so the screen shows stale lines. Tells radio
// interference from the panel apart from the fill's CPU and PSRAM load. Build
// with -DCB_IDLE_FILL=ON (main/CMakeLists.txt).
#ifndef IDLE_FILL
#define IDLE_FILL  0
#endif

// Start the display in bounce-buffer mode with `num_pages` pages. Page
// pixels come from the compositor (compositor.h): at each frame start the
// bounce fill asks it for the pages in view. Call comp_init() first.
esp_err_t screen_init(int num_pages);

int screen_page_count(void);

// Change the number of pages (1..SCREEN_MAX_PAGES); takes effect at the next
// frame start. swipe_set_page_count() calls it along with the swipe bounds.
void screen_set_page_count(int num_pages);

// Horizontal scroll position of the page area, in pixels: page i is fully
// in view at i * SCREEN_PAGE_W. Values before the first page or past the last
// are allowed (rubber-banding) and show background there. If a page in view
// isn't ready yet, the compositor holds the screen on a page that is.
// Takes effect at the start of the next frame.
void screen_set_scroll(int scroll_px);

// The status bar's pixels (SCREEN_W x SCREEN_STATUS_H, see status_bar.h), or
// NULL for a blank bar. Normally in PSRAM, 64-byte aligned and written back
// (the fill preloads its chunks); status.cpp writes it only in fill gaps.
// Takes effect at the next frame start; the buffer must stay valid while
// shown. status.cpp double buffers it.
void screen_set_status_bar(const uint16_t *px);

// Two pages are in view (mid-swipe) at the last frame start
bool screen_two_pages_in_view(void);

// The status bar buffer latched at the last frame start. Once this is the
// buffer last set, any earlier one is no longer read.
const uint16_t *screen_status_bar_shown(void);

// The status task is drawing the bar (for the late-fill stats)
void screen_note_status_drawing(bool drawing);

// The task to notify (xTaskNotifyGive) at the start of every frame, or NULL.
void screen_set_frame_task(TaskHandle_t task);

typedef struct {
    uint32_t frames;           // frames shown so far
    uint32_t max_fill_us;      // slowest bounce fill since the last call
    uint32_t overruns;         // fills over DISPLAY_BOUNCE_BUDGET_US since the last call
    // ...of which (status + one page + two pages = overruns):
    uint32_t late_status;           // status bar lines only
    uint32_t late_one_page;         // page lines, one page in view
    uint32_t late_two_pages;        // page lines, mid-swipe
    uint32_t late_status_drawing;   // (any of the above) while the status bar was being drawn
    // Fill start timing: max_fill_us and overruns only time the fill itself; a fill that starts late
    // (the ISR held off) can still miss the DMA with a short fill
    uint32_t max_late_us;      // latest fill start, against the evenly spaced fills of the frame
    uint32_t max_end_us;       // latest fill end, from when it should have started (budget: DISPLAY_BOUNCE_BUDGET_US)
    uint32_t missed;           // fills that ended past the budget that way (includes the overruns)
    // Bounce buffer order (a lasting picture shift would show here)
    uint32_t seq_breaks;       // fills whose line didn't follow the last fill's (the driver resynced)
    uint32_t buffer_mismatches; // fills written into the buffer the DMA is sending (lasting desync)
} screen_stats_t;

// Read the stats. The max and overrun count reset on each call.
void screen_get_stats(screen_stats_t *stats);
