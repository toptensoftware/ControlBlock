#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "i2c.h"
#include "waveshare_rgb_lcd_port.h"
#include "waveshare_touch_port.h"

static const char *TAG = "controlblock";

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct {
    uint8_t          id;
    TOUCH_EVENT_TYPE type;
    uint16_t         x, y;
} touch_event_t;

static QueueHandle_t s_touch_queue;

// Runs on the touch task. Hand the event to the main loop.
static void on_touched(uint8_t id, TOUCH_EVENT_TYPE type, uint16_t x, uint16_t y)
{
    touch_event_t ev = { .id = id, .type = type, .x = x, .y = y };
    xQueueSend(s_touch_queue, &ev, 0);
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    // Clip to the screen
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_H_RES) w = DISPLAY_H_RES - x;
    if (y + h > DISPLAY_V_RES) h = DISPLAY_V_RES - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = y; row < y + h; row++) {
        uint16_t *p = fb + row * DISPLAY_H_RES + x;
        for (int i = 0; i < w; i++)
            p[i] = color;
    }
}

// Test pattern:
//  - Top 2/3: eight vertical colour bars.
//  - Bottom 1/3: red, green and blue ramps from dark to full, left to right.
//  - 1 px white border on the screen edge, to check nothing is offset or cropped.
static void draw_test_pattern(uint16_t *fb)
{
    static const uint16_t bars[8] = {
        RGB565(255, 255, 255), RGB565(255, 255, 0), RGB565(0, 255, 255), RGB565(0, 255, 0),
        RGB565(255, 0, 255),   RGB565(255, 0, 0),   RGB565(0, 0, 255),   RGB565(0, 0, 0),
    };

    const int bar_h  = DISPLAY_V_RES * 2 / 3;
    const int bar_w  = DISPLAY_H_RES / 8;
    for (int i = 0; i < 8; i++)
        fill_rect(fb, i * bar_w, 0, bar_w, bar_h, bars[i]);

    const int ramp_h = (DISPLAY_V_RES - bar_h) / 3;
    for (int x = 0; x < DISPLAY_H_RES; x++) {
        int v = x * 255 / (DISPLAY_H_RES - 1);
        fill_rect(fb, x, bar_h,              1, ramp_h, RGB565(v, 0, 0));
        fill_rect(fb, x, bar_h + ramp_h,     1, ramp_h, RGB565(0, v, 0));
        fill_rect(fb, x, bar_h + ramp_h * 2, 1, DISPLAY_V_RES - bar_h - ramp_h * 2, RGB565(0, 0, v));
    }

    const uint16_t white = RGB565(255, 255, 255);
    fill_rect(fb, 0, 0, DISPLAY_H_RES, 1, white);
    fill_rect(fb, 0, DISPLAY_V_RES - 1, DISPLAY_H_RES, 1, white);
    fill_rect(fb, 0, 0, 1, DISPLAY_V_RES, white);
    fill_rect(fb, DISPLAY_H_RES - 1, 0, 1, DISPLAY_V_RES, white);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ControlBlock starting");

    s_touch_queue = xQueueCreate(32, sizeof(touch_event_t));

    i2c_init();
    ESP_ERROR_CHECK(display_init(NULL));
    ESP_ERROR_CHECK(touch_init(on_touched));
    ESP_ERROR_CHECK(display_enable_backlight(true));

    // The driver scans out fb0 until display_set_framebuffer() is called, which
    // this test never does. Draw the same pattern into both buffers anyway.
    uint16_t *fb0, *fb1;
    display_get_frame_buffers(&fb0, &fb1);
    draw_test_pattern(fb0);
    draw_test_pattern(fb1);
    ESP_LOGI(TAG, "Test pattern drawn");

    // Each finger leaves a trail of dots in its own colour, to check that
    // touch coordinates line up with the display.
    static const uint16_t finger_colors[5] = {
        RGB565(0, 0, 0), RGB565(255, 0, 0), RGB565(0, 160, 0), RGB565(0, 0, 255), RGB565(255, 128, 0),
    };
    static const char *type_names[] = { "DOWN", "MOVE", "UP" };

    touch_event_t ev;
    while (xQueueReceive(s_touch_queue, &ev, portMAX_DELAY)) {
        ESP_LOGI(TAG, "touch id=%d %-4s x=%3d y=%3d",
                 ev.id, type_names[ev.type], ev.x, ev.y);

        if (ev.type != TOUCH_UP)
            fill_rect(fb0, ev.x - 4, ev.y - 4, 9, 9, finger_colors[ev.id % 5]);
    }
}
