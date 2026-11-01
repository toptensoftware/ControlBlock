#include <string.h>
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp32s3/rom/cache.h"
#include "screen.h"

static const char *TAG = "screen";

// Start loading the next chunk's page data into cache at the end of each
// fill, so it arrives while the display is sending the current chunk.
// Measured: a page chunk takes ~285 us copied cold from PSRAM, ~68 us preloaded.
#define SCREEN_CACHE_PRELOAD 1

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Placeholder colours until the status bar and pager have content.
#define COLOR_STATUS      RGB565(32, 32, 32)
#define COLOR_PAGER       RGB565(32, 32, 32)
#define COLOR_EDGE        RGB565(96, 96, 96)
#define COLOR_BACKGROUND  RGB565(0, 0, 0)       // beyond the first/last page

static uint16_t *s_pages[SCREEN_MAX_PAGES];
static int       s_num_pages;

// Written by the app, latched by the ISR at the start of each frame.
static volatile int          s_scroll;
static volatile TaskHandle_t s_frame_task;

// ISR-only state.
static int s_frame_scroll;

static volatile uint32_t s_frame_count;
static volatile uint32_t s_max_fill_cycles;

// Fill with a solid colour, two pixels per 32-bit store. A plain per-pixel
// loop was ~350 us for a 15-line chunk, more than copying a page from PSRAM.
IRAM_ATTR static void fill_px(uint16_t *dest, int n, uint16_t color)
{
    if (n > 0 && ((uintptr_t)dest & 2)) {
        *dest++ = color;
        n--;
    }

    uint32_t  c2 = color | ((uint32_t)color << 16);
    uint32_t *d  = (uint32_t *)dest;
    int pairs = n / 2;
    while (pairs >= 8) {
        d[0] = c2; d[1] = c2; d[2] = c2; d[3] = c2;
        d[4] = c2; d[5] = c2; d[6] = c2; d[7] = c2;
        d += 8;
        pairs -= 8;
    }
    while (pairs--)
        *d++ = c2;

    if (n & 1)
        *(uint16_t *)d = color;
}

// Copy `n` pixels from src, or fill with background if src is NULL.
IRAM_ATTR static void copy_px(uint16_t *dest, const uint16_t *src, int n)
{
    if (src)
        memcpy(dest, src, n * sizeof(uint16_t));
    else
        fill_px(dest, n, COLOR_BACKGROUND);
}

IRAM_ATTR static const uint16_t *page_row(int page, int row)
{
    if (page < 0 || page >= s_num_pages)
        return NULL;
    return s_pages[page] + row * SCREEN_PAGE_W;
}

// Preload the page data the chunk starting at line `y` will read. The cache
// can only preload one contiguous range at a time, so when two pages are in
// view this picks whichever covers more of the screen.
IRAM_ATTR static void preload_chunk(int y, int left, int offset)
{
    int r0 = y - SCREEN_PAGE_TOP;
    int r1 = r0 + DISPLAY_BOUNCE_BUFFER_LINES;
    if (r0 < 0)
        r0 = 0;
    if (r1 > SCREEN_PAGE_H)
        r1 = SCREEN_PAGE_H;
    if (r0 >= r1)
        return;

    const uint16_t *start, *end;
    if (offset <= SCREEN_PAGE_W / 2) {
        // Mostly the left page: columns offset..W of each row
        const uint16_t *p = page_row(left, r0);
        if (!p)
            return;
        start = p + offset;
        end   = page_row(left, r1 - 1) + SCREEN_PAGE_W;
    } else {
        // Mostly the right page: columns 0..offset of each row
        const uint16_t *p = page_row(left + 1, r0);
        if (!p)
            return;
        start = p;
        end   = page_row(left + 1, r1 - 1) + offset;
    }

    Cache_Start_DCache_Preload((uint32_t)start, (uint32_t)((end - start) * sizeof(uint16_t)), 0);
}

// Bounce buffer fill: runs in the LCD DMA ISR.
IRAM_ATTR static bool fill_lines(uint16_t *dest, int y, int count, void *ctx)
{
    uint32_t start = esp_cpu_get_cycle_count();
    BaseType_t woken = pdFALSE;

    if (y == 0) {
        s_frame_count++;
        s_frame_scroll = s_scroll;
        TaskHandle_t task = s_frame_task;
        if (task)
            vTaskNotifyGiveFromISR(task, &woken);
    }

    // Split the scroll position into the page at the left edge of the
    // screen and how far into it we are.
    int scroll = s_frame_scroll;
    int left   = scroll >= 0 ? scroll / SCREEN_PAGE_W
                             : -((-scroll + SCREEN_PAGE_W - 1) / SCREEN_PAGE_W);
    int offset = scroll - left * SCREEN_PAGE_W;

    for (int i = 0; i < count; i++, y++, dest += DISPLAY_H_RES) {
        if (y < SCREEN_PAGE_TOP) {
            fill_px(dest, DISPLAY_H_RES, y == SCREEN_PAGE_TOP - 1 ? COLOR_EDGE : COLOR_STATUS);
        } else if (y >= SCREEN_PAGE_TOP + SCREEN_PAGE_H) {
            fill_px(dest, DISPLAY_H_RES, y == SCREEN_PAGE_TOP + SCREEN_PAGE_H ? COLOR_EDGE : COLOR_PAGER);
        } else {
            int row = y - SCREEN_PAGE_TOP;
            const uint16_t *lp = page_row(left, row);
            copy_px(dest, lp ? lp + offset : NULL, SCREEN_PAGE_W - offset);
            if (offset)
                copy_px(dest + SCREEN_PAGE_W - offset, page_row(left + 1, row), offset);
        }
    }

#if SCREEN_CACHE_PRELOAD
    // y is now the first line of the next chunk. The scroll position only
    // changes at frame start, so the preload is right unless the next chunk
    // starts a new frame (y == DISPLAY_V_RES), which is the status bar anyway.
    if (y < DISPLAY_V_RES)
        preload_chunk(y, left, offset);
#endif

    uint32_t cycles = esp_cpu_get_cycle_count() - start;
    if (cycles > s_max_fill_cycles)
        s_max_fill_cycles = cycles;
    return woken == pdTRUE;
}

esp_err_t screen_init(int num_pages)
{
    if (num_pages < 1 || num_pages > SCREEN_MAX_PAGES)
        return ESP_ERR_INVALID_ARG;

    size_t size = SCREEN_PAGE_W * SCREEN_PAGE_H * sizeof(uint16_t);
    for (int i = 0; i < num_pages; i++) {
        s_pages[i] = heap_caps_aligned_calloc(64, 1, size, MALLOC_CAP_SPIRAM);
        if (!s_pages[i]) {
            ESP_LOGE(TAG, "Out of PSRAM allocating page %d", i);
            return ESP_ERR_NO_MEM;
        }
    }
    s_num_pages = num_pages;
    ESP_LOGI(TAG, "%d page buffers, %u KB each", num_pages, (unsigned)(size / 1024));

    return display_init_bounce(fill_lines, NULL);
}

int screen_page_count(void)
{
    return s_num_pages;
}

uint16_t *screen_page_buffer(int page)
{
    return page >= 0 && page < s_num_pages ? s_pages[page] : NULL;
}

void screen_set_scroll(int scroll_px)
{
    s_scroll = scroll_px;
}

void screen_set_frame_task(TaskHandle_t task)
{
    s_frame_task = task;
}

void screen_get_stats(screen_stats_t *stats)
{
    stats->frames      = s_frame_count;
    stats->max_fill_us = s_max_fill_cycles / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    s_max_fill_cycles  = 0;
}
