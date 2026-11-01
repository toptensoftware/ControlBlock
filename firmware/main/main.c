#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "i2c.h"
#include "waveshare_rgb_lcd_port.h"
#include "waveshare_touch_port.h"
#include "screen.h"
#include "swipe.h"

static const char *TAG = "controlblock";

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define NUM_PAGES 3

static void fill_rect(uint16_t *page, int x, int y, int w, int h, uint16_t color)
{
    for (int row = y; row < y + h; row++) {
        uint16_t *p = page + row * SCREEN_PAGE_W + x;
        for (int i = 0; i < w; i++)
            p[i] = color;
    }
}

// Test page: a coloured background with a 5x3 grid of placeholder keys.
// Each key shows (page + 1) white squares so the pages are easy to tell apart.
static void draw_test_page(int index)
{
    static const uint16_t backs[] = { RGB565(96, 0, 0),    RGB565(0, 72, 0),    RGB565(0, 0, 112) };
    static const uint16_t keys[]  = { RGB565(200, 64, 64), RGB565(64, 176, 64), RGB565(64, 96, 224) };
    const uint16_t white = RGB565(255, 255, 255);

    uint16_t *page = screen_page_buffer(index);
    fill_rect(page, 0, 0, SCREEN_PAGE_W, SCREEN_PAGE_H, backs[index % 3]);

    const int cols = 5, rows = 3;
    const int cell_w = SCREEN_PAGE_W / cols, cell_h = SCREEN_PAGE_H / rows;
    const int key_w = 120, key_h = 112;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int kx = c * cell_w + (cell_w - key_w) / 2;
            int ky = r * cell_h + (cell_h - key_h) / 2;
            fill_rect(page, kx, ky, key_w, key_h, keys[index % 3]);

            for (int n = 0; n <= index; n++)
                fill_rect(page, kx + 12 + n * 20, ky + 12, 12, 12, white);
        }
    }
}

static void on_touched(uint8_t id, TOUCH_EVENT_TYPE type, uint16_t x, uint16_t y)
{
    if (type != TOUCH_MOVE)
        ESP_LOGI(TAG, "touch id=%d %s x=%3d y=%3d", id, type == TOUCH_DOWN ? "DOWN" : "UP", x, y);
    swipe_touch(id, type, x, y);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ControlBlock starting");

    i2c_init();
    ESP_ERROR_CHECK(screen_init(NUM_PAGES));
    for (int i = 0; i < NUM_PAGES; i++)
        draw_test_page(i);

    ESP_ERROR_CHECK(swipe_init());
    ESP_ERROR_CHECK(touch_init(on_touched));
    ESP_ERROR_CHECK(display_enable_backlight(true));

    // Time available to fill one bounce buffer: 15 lines, each 800 visible
    // pixels plus 20 pixels of horizontal sync and porches (see the driver).
    const int budget_us = DISPLAY_BOUNCE_BUFFER_LINES * (DISPLAY_H_RES + 20)
                          / (DISPLAY_PIXEL_CLOCK_HZ / 1000000);

    uint32_t last_frames = 0;
    screen_stats_t st;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        screen_get_stats(&st);
        ESP_LOGI(TAG, "%lu fps, slowest fill %3lu us (budget %d us), page %d",
                 (unsigned long)(st.frames - last_frames),
                 (unsigned long)st.max_fill_us, budget_us, swipe_current_page());
        last_frames = st.frames;
    }
}
