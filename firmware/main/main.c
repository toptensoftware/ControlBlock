#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "i2c.h"
#include "waveshare_rgb_lcd_port.h"
#include "waveshare_touch_port.h"

static const char *TAG = "controlblock";

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Test layout: fixed bands top and bottom, scrolling page area between.
#define STATUS_H      40
#define PAGER_H       40
#define PAGE_TOP      STATUS_H
#define PAGE_BOTTOM   (DISPLAY_V_RES - PAGER_H)
#define PAGE_MID      ((PAGE_TOP + PAGE_BOTTOM) / 2)
#define SCROLL_PX     2       // pixels per frame

#define COLOR_STATUS  RGB565(48, 48, 48)
#define COLOR_PAGER   RGB565(24, 32, 64)
#define COLOR_EDGE    RGB565(160, 160, 160)
#define COLOR_CROSS   RGB565(255, 255, 255)

// One row of eight colour bars, stored twice side by side so a scrolled
// row is a single memcpy from an offset. Internal RAM, for the ISR.
static DRAM_ATTR uint16_t s_bars[DISPLAY_H_RES * 2];

// Shared between the fill ISR and the rest of the app.
static volatile uint32_t s_frame_count;
static volatile uint32_t s_max_fill_cycles;
static volatile int      s_touch_x = -1;     // finger 0, -1 = not touching
static volatile int      s_touch_y = -1;

// Latched at the start of each frame so every line in a frame agrees.
static int s_frame_scroll;
static int s_frame_touch_x;
static int s_frame_touch_y;

static void init_bars(void)
{
    static const uint16_t bars[8] = {
        RGB565(255, 255, 255), RGB565(255, 255, 0), RGB565(0, 255, 255), RGB565(0, 255, 0),
        RGB565(255, 0, 255),   RGB565(255, 0, 0),   RGB565(0, 0, 255),   RGB565(0, 0, 0),
    };
    for (int x = 0; x < DISPLAY_H_RES; x++)
        s_bars[x] = s_bars[x + DISPLAY_H_RES] = bars[x * 8 / DISPLAY_H_RES];
}

IRAM_ATTR static void fill_line(uint16_t *dest, uint16_t color)
{
    uint32_t c2 = color | ((uint32_t)color << 16);
    uint32_t *d = (uint32_t *)dest;
    for (int i = 0; i < DISPLAY_H_RES / 2; i++)
        d[i] = c2;
}

// Bounce buffer fill: runs in the LCD DMA ISR.
IRAM_ATTR static bool fill_lines(uint16_t *dest, int y, int count, void *ctx)
{
    uint32_t start = esp_cpu_get_cycle_count();

    if (y == 0) {
        s_frame_count++;
        s_frame_scroll  = (s_frame_count * SCROLL_PX) % DISPLAY_H_RES;
        s_frame_touch_x = s_touch_x;
        s_frame_touch_y = s_touch_y;
    }

    for (int i = 0; i < count; i++, y++, dest += DISPLAY_H_RES) {
        if (y < PAGE_TOP) {
            fill_line(dest, y == PAGE_TOP - 1 ? COLOR_EDGE : COLOR_STATUS);
        } else if (y >= PAGE_BOTTOM) {
            fill_line(dest, y == PAGE_BOTTOM ? COLOR_EDGE : COLOR_PAGER);
        } else {
            // Top half scrolls right, bottom half scrolls left
            int offset = y < PAGE_MID ? DISPLAY_H_RES - s_frame_scroll : s_frame_scroll;
            memcpy(dest, s_bars + offset, DISPLAY_H_RES * sizeof(uint16_t));
        }

        // Crosshair under finger 0
        if (s_frame_touch_x >= 0) {
            if (y == s_frame_touch_y)
                fill_line(dest, COLOR_CROSS);
            else
                dest[s_frame_touch_x] = COLOR_CROSS;
        }
    }

    uint32_t cycles = esp_cpu_get_cycle_count() - start;
    if (cycles > s_max_fill_cycles)
        s_max_fill_cycles = cycles;
    return false;
}

typedef struct {
    uint8_t          id;
    TOUCH_EVENT_TYPE type;
    uint16_t         x, y;
} touch_event_t;

static QueueHandle_t s_touch_queue;

// Runs on the touch task.
static void on_touched(uint8_t id, TOUCH_EVENT_TYPE type, uint16_t x, uint16_t y)
{
    if (id == 0) {
        bool up = type == TOUCH_UP;
        s_touch_x = up || x >= DISPLAY_H_RES ? -1 : x;
        s_touch_y = up || y >= DISPLAY_V_RES ? -1 : y;
    }

    touch_event_t ev = { .id = id, .type = type, .x = x, .y = y };
    xQueueSend(s_touch_queue, &ev, 0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ControlBlock starting");

    s_touch_queue = xQueueCreate(32, sizeof(touch_event_t));
    init_bars();

    i2c_init();
    ESP_ERROR_CHECK(display_init_bounce(fill_lines, NULL));
    ESP_ERROR_CHECK(touch_init(on_touched));
    ESP_ERROR_CHECK(display_enable_backlight(true));

    // Time available to fill one bounce buffer: 15 lines, each 800 visible
    // pixels plus 20 pixels of horizontal sync and porches (see the driver).
    const int budget_us = DISPLAY_BOUNCE_BUFFER_LINES * (DISPLAY_H_RES + 20)
                          / (DISPLAY_PIXEL_CLOCK_HZ / 1000000);

    static const char *type_names[] = { "DOWN", "MOVE", "UP" };
    TickType_t last_stats = xTaskGetTickCount();
    uint32_t   last_frames = s_frame_count;

    while (1) {
        touch_event_t ev;
        if (xQueueReceive(s_touch_queue, &ev, pdMS_TO_TICKS(100))) {
            ESP_LOGI(TAG, "touch id=%d %-4s x=%3d y=%3d",
                     ev.id, type_names[ev.type], ev.x, ev.y);
        }

        TickType_t now = xTaskGetTickCount();
        if (now - last_stats >= pdMS_TO_TICKS(1000)) {
            uint32_t frames     = s_frame_count;
            uint32_t max_cycles = s_max_fill_cycles;
            s_max_fill_cycles   = 0;

            ESP_LOGI(TAG, "%lu fps, slowest fill %lu us (budget %d us)",
                     (unsigned long)(frames - last_frames),
                     (unsigned long)(max_cycles / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
                     budget_us);

            last_frames = frames;
            last_stats  = now;
        }
    }
}
