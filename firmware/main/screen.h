#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "waveshare_rgb_lcd_port.h"

// Screen layout. The status bar and pager are fixed; the page area between
// them scrolls horizontally across a row of page buffers.
#define SCREEN_STATUS_H   40
#define SCREEN_PAGER_H    40
#define SCREEN_PAGE_W     DISPLAY_H_RES
#define SCREEN_PAGE_H     (DISPLAY_V_RES - SCREEN_STATUS_H - SCREEN_PAGER_H)
#define SCREEN_PAGE_TOP   SCREEN_STATUS_H

#define SCREEN_MAX_PAGES  8

// Allocate `num_pages` page buffers (SCREEN_PAGE_W x SCREEN_PAGE_H, RGB565,
// in PSRAM) and start the display in bounce-buffer mode.
esp_err_t screen_init(int num_pages);

int screen_page_count(void);

// Page buffer to draw into. Rows are SCREEN_PAGE_W pixels apart.
uint16_t *screen_page_buffer(int page);

// Horizontal scroll position of the page area, in pixels: page i is fully
// in view at i * SCREEN_PAGE_W. Values before the first page or past the last
// are allowed (rubber-banding) and show background there.
// Takes effect at the start of the next frame.
void screen_set_scroll(int scroll_px);

// The task to notify (xTaskNotifyGive) at the start of every frame, or NULL.
void screen_set_frame_task(TaskHandle_t task);

typedef struct {
    uint32_t frames;           // frames shown so far
    uint32_t max_fill_us;      // slowest bounce fill since the last call (resets it)
} screen_stats_t;

void screen_get_stats(screen_stats_t *stats);
