#include <string.h>
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp32s3/rom/cache.h"
#include "color.h"
#include "compositor.h"
#include "fill_gap.h"
#include "screen_view.h"
#include "status_bar.h"
#include "screen.h"

static const char *TAG = "screen";

static_assert(SCREEN_W == DISPLAY_H_RES && SCREEN_H == DISPLAY_V_RES &&
              SCREEN_BOUNCE_LINES == DISPLAY_BOUNCE_BUFFER_LINES,
              "screen_layout.h doesn't match the panel");

// The compositor frees buffers at frame start, and they must not be read
// again: the frame's first fill has to be all status bar
static_assert(SCREEN_STATUS_H >= SCREEN_BOUNCE_LINES, "first bounce fill must not read the page area");

// Start loading the next chunk's page data into cache at the end of each
// fill, so it arrives while the display is sending the current chunk.
// Measured: a page chunk takes ~285 us copied cold from PSRAM, ~68 us preloaded.
#define SCREEN_CACHE_PRELOAD 1

// The status bar, page area and pager bar share one background, so a
// missing status bar or page is filled the same way (copy_px)
static_assert(STATUS_BAR_BACKGROUND == COMP_BACKGROUND, "status bar and page area backgrounds differ");

#define BUDGET_CYCLES     (DISPLAY_BOUNCE_BUDGET_US * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)

static volatile int s_num_pages;    // latched by the ISR at frame start

// Written by the app, latched by the ISR at the start of each frame.
static volatile int             s_scroll;
static volatile TaskHandle_t    s_frame_task;
static const uint16_t *volatile s_status;

// ISR-only: what this frame shows, latched at frame start
static FrameView s_view;

// Latched by the ISR at frame start; read by the status task to know when
// the other buffer is free
static const uint16_t *volatile s_status_frame;

static volatile uint32_t s_frame_count;
static volatile uint32_t s_max_fill_cycles;
static volatile uint32_t s_overruns;
static volatile uint32_t s_late_status;
static volatile uint32_t s_late_one_page;
static volatile uint32_t s_late_two_pages;
static volatile uint32_t s_late_status_drawing;
static volatile bool     s_status_drawing;

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
        fill_px(dest, n, COMP_BACKGROUND);
}

// Row `row` of the left (slot 0) or right (slot 1) page in view, or NULL.
IRAM_ATTR static const uint16_t *page_row(int slot, int row)
{
    const uint16_t *px = s_view.px[slot];
    return px ? px + row * SCREEN_PAGE_W : NULL;
}

// Latch the scroll position and ask the compositor for the pages it shows.
IRAM_ATTR static void latch_frame(void)
{
    PagesInView pages = screen_pages_in_view(s_scroll, s_num_pages);
    comp_view_t v[2];
    int n = comp_frame(pages.first, pages.count, pages.count == 2 ? pages.offset : 0, v);
    s_view = screen_resolve_view(pages, v, n);
    s_status_frame = s_status;
}

// Preload the page data the chunk starting at line `y` will read. The cache
// can only preload one contiguous range at a time, so when two pages are in
// view this picks whichever covers more of the screen.
IRAM_ATTR static void preload_chunk(int y)
{
    // The status bar is in PSRAM too. Chunk 0 of the next frame (y past the
    // end) reads s_status, which is what the frame will latch.
    int s0 = y >= DISPLAY_V_RES ? 0 : y;
    int s1 = s0 + DISPLAY_BOUNCE_BUFFER_LINES;
    if (s1 > SCREEN_STATUS_H)
        s1 = SCREEN_STATUS_H;

    int r0 = y - SCREEN_PAGE_TOP;
    int r1 = r0 + DISPLAY_BOUNCE_BUFFER_LINES;
    if (r0 < 0)
        r0 = 0;
    if (r1 > SCREEN_PAGE_H)
        r1 = SCREEN_PAGE_H;
    if (y >= DISPLAY_V_RES)
        r0 = r1 = 0;

    // Only one range can be preloaded: the status bar's rows if the chunk
    // has at least as many of them as page rows
    if (s0 < s1 && s1 - s0 >= r1 - r0) {
        const uint16_t *bar = y >= DISPLAY_V_RES ? s_status : s_status_frame;
        if (bar)
            Cache_Start_DCache_Preload((uint32_t)(bar + s0 * SCREEN_W), (uint32_t)((s1 - s0) * SCREEN_W * sizeof(uint16_t)), 0);
        return;
    }
    if (r0 >= r1)
        return;

    int offset = s_view.offset;
    const uint16_t *start, *end;
    if (offset <= SCREEN_PAGE_W / 2) {
        // Mostly the left page: columns offset..W of each row
        const uint16_t *p = page_row(0, r0);
        if (!p)
            return;
        start = p + offset;
        end   = page_row(0, r1 - 1) + SCREEN_PAGE_W;
    } else {
        // Mostly the right page: columns 0..offset of each row
        const uint16_t *p = page_row(1, r0);
        if (!p)
            return;
        start = p;
        end   = page_row(1, r1 - 1) + offset;
    }

    Cache_Start_DCache_Preload((uint32_t)start, (uint32_t)((end - start) * sizeof(uint16_t)), 0);
}

// Bounce buffer fill: runs in the LCD DMA ISR.
IRAM_ATTR static bool fill_lines(uint16_t *dest, int y, int count, void *ctx)
{
#if IDLE_FILL
    // Diagnostic (screen.h): the panel keeps running but the bounce buffers are
    // never refilled, so no PSRAM reads and almost no ISR time
    if (y == 0)
        s_frame_count = s_frame_count + 1;
    return false;
#endif
    uint32_t start = esp_cpu_get_cycle_count();
    BaseType_t woken = pdFALSE;
    const int y0 = y;

    if (y == 0) {
        // The first chunk is the status bar, so the buffers the compositor
        // frees here aren't read again.
        s_frame_count = s_frame_count + 1;
        latch_frame();
        TaskHandle_t task = s_frame_task;
        if (task)
            vTaskNotifyGiveFromISR(task, &woken);
    }

    int offset = s_view.offset;
    for (int i = 0; i < count; i++, y++, dest += DISPLAY_H_RES) {
        if (y < SCREEN_PAGE_TOP) {
            const uint16_t *status = s_status_frame;
            copy_px(dest, status ? status + y * SCREEN_W : NULL, DISPLAY_H_RES);
        } else if (y >= SCREEN_PAGE_TOP + SCREEN_PAGE_H) {
            fill_px(dest, DISPLAY_H_RES, COMP_BACKGROUND);      // pager bar, blank for now
        } else {
            int row = y - SCREEN_PAGE_TOP;
            const uint16_t *lp = page_row(0, row);
            copy_px(dest, lp ? lp + offset : NULL, SCREEN_PAGE_W - offset);
            if (offset)
                copy_px(dest + SCREEN_PAGE_W - offset, page_row(1, row), offset);
        }
    }

#if SCREEN_CACHE_PRELOAD
    // y is now the first line of the next chunk (DISPLAY_V_RES: the next
    // frame's first, all status bar). The view only changes at frame start,
    // so the page preload is right.
    preload_chunk(y);
#endif

    // The gap before the next fill is when the compositor and the status bar
    // write PSRAM
    fill_gap_done();

    uint32_t cycles = esp_cpu_get_cycle_count() - start;
    if (cycles > s_max_fill_cycles)
        s_max_fill_cycles = cycles;
    if (cycles > BUDGET_CYCLES) {
        // Where late fills happen, to find what's slowing them
        s_overruns = s_overruns + 1;
        if (y0 + count <= SCREEN_PAGE_TOP)
            s_late_status = s_late_status + 1;
        else if (offset)
            s_late_two_pages = s_late_two_pages + 1;
        else
            s_late_one_page = s_late_one_page + 1;
        if (s_status_drawing)
            s_late_status_drawing = s_late_status_drawing + 1;
    }
    return woken == pdTRUE;
}

esp_err_t screen_init(int num_pages)
{
    if (num_pages < 1 || num_pages > SCREEN_MAX_PAGES)
        return ESP_ERR_INVALID_ARG;
    s_num_pages = num_pages;
    ESP_LOGI(TAG, "%d pages", num_pages);

#if NO_DISPLAY
    // Diagnostic (main/CMakeLists.txt): no panel, so no fill ISR and no PSRAM reads
    ESP_LOGW(TAG, "NO_DISPLAY build: the display is not started");
    return ESP_OK;
#else
    return display_init_bounce(fill_lines, NULL);
#endif
}

int screen_page_count(void)
{
    return s_num_pages;
}

void screen_set_page_count(int num_pages)
{
    if (num_pages >= 1 && num_pages <= SCREEN_MAX_PAGES)
        s_num_pages = num_pages;
}

void screen_set_scroll(int scroll_px)
{
    s_scroll = scroll_px;
}

void screen_set_status_bar(const uint16_t *px)
{
    s_status = px;
}

bool screen_two_pages_in_view(void)
{
    return s_view.offset != 0;
}

void screen_note_status_drawing(bool drawing)
{
    s_status_drawing = drawing;
}

const uint16_t *screen_status_bar_shown(void)
{
    return s_status_frame;
}

void screen_set_frame_task(TaskHandle_t task)
{
    s_frame_task = task;
}

void screen_get_stats(screen_stats_t *stats)
{
    stats->frames      = s_frame_count;
    stats->max_fill_us = s_max_fill_cycles / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    stats->overruns    = s_overruns;
    stats->late_status         = s_late_status;
    stats->late_one_page       = s_late_one_page;
    stats->late_two_pages      = s_late_two_pages;
    stats->late_status_drawing = s_late_status_drawing;
    s_max_fill_cycles     = 0;
    s_overruns            = 0;
    s_late_status         = 0;
    s_late_one_page       = 0;
    s_late_two_pages      = 0;
    s_late_status_drawing = 0;
}
