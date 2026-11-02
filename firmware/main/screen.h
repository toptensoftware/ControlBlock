#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "waveshare_rgb_lcd_port.h"
#include "screen_layout.h"

// Start the display in bounce-buffer mode with `num_pages` pages. Page
// pixels come from the compositor (compositor.h): at each frame start the
// bounce fill asks it for the pages in view. Call comp_init() first.
esp_err_t screen_init(int num_pages);

int screen_page_count(void);

// Horizontal scroll position of the page area, in pixels: page i is fully
// in view at i * SCREEN_PAGE_W. Values before the first page or past the last
// are allowed (rubber-banding) and show background there. If a page in view
// isn't ready yet, the compositor holds the screen on a page that is.
// Takes effect at the start of the next frame.
void screen_set_scroll(int scroll_px);

// The status bar's pixels (SCREEN_W x SCREEN_STATUS_H, see status_bar.h), or
// NULL for a blank bar. Must be in internal RAM: the bounce fill copies it
// every frame and PSRAM reads would make fills late. Takes effect at the
// next frame start; the buffer must stay valid while shown. status.cpp
// double buffers it.
void screen_set_status_bar(const uint16_t *px);

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
} screen_stats_t;

// Read the stats. The max and overrun count reset on each call.
void screen_get_stats(screen_stats_t *stats);
