#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <limits.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "rle565.h"
#include "fill_gap.h"
#include "page_compositor.h"
#include "screen.h"
#include "status_bar.h"
#include "ui_assets.h"
#include "status.h"
#include "clock.h"
#include "status_icons.h"
#include "esp_timer.h"

static const char *TAG = "status";

// Below the compositor (4) and swipe (10). The bar is in PSRAM: rows are
// rendered into a small internal band, then copied across in fill gaps only
// (nothing else may write PSRAM heavily while fills run).
#define STATUS_TASK_PRIORITY  3
#define STATUS_TASK_STACK     3072

// Rows copied to PSRAM per fill gap (at most STATUS_BAR_BAND_ROWS_MAX). The
// compositor copies 8 KB (one page in view) or 3 KB (two) per gap; these are
// 6.4 KB and 3.2 KB. A redraw takes 10 or 20 gaps (0.17 or 0.33 s).
#define STATUS_BAND_ROWS_ONE_PAGE   4
#define STATUS_BAND_ROWS_TWO_PAGES  2
static_assert(STATUS_BAND_ROWS_ONE_PAGE <= STATUS_BAR_BAND_ROWS_MAX &&
              STATUS_BAND_ROWS_TWO_PAGES <= STATUS_BAR_BAND_ROWS_MAX, "band bigger than the scratch");

// Notification bits
#define WAKE_CHANGE  0x01       // a property changed
#define WAKE_GAP     0x02       // given a fill gap (fill_gap.h)

static StatusBar          s_bar;
static status_bar_style_t s_style;
static TaskHandle_t       s_task;
static fill_gap_client_t  s_gap_client = { NULL, WAKE_GAP, false, 0 };
static uint16_t          *s_scratch;        // SCREEN_W x STATUS_BAR_BAND_ROWS_MAX, internal RAM

static StatusIcons        s_icons;          // under s_icons_lock
static port::SpinLock     s_icons_lock;

static int64_t uptime_ms(void) { return esp_timer_get_time() / 1000; }

// Bring the bar's icons up to date; returns ms until they next change by
// themselves (or STATUS_NO_DEADLINE)
static int64_t refresh_icons(void)
{
    int64_t now = uptime_ms();
    s_icons_lock.Enter();
    int icons = s_icons.Visible(now);
    int64_t next = s_icons.NextDeadlineMs(now);
    s_icons_lock.Exit();
    s_bar.SetIcons(icons);
    return next;
}

// Ticks to the next wake: just after the next minute boundary while a text
// shows the clock, or an icon's expiry. Rounded up, so never early and never
// 0 (the tick is 10 ms).
static TickType_t wait_ticks(int64_t icons_ms)
{
    int64_t clock = s_bar.HasClock() ? clock_wait_ms(clock_now_ms()) : STATUS_NO_DEADLINE;
    int64_t ms = status_wait_ms(clock, icons_ms);
    if (ms == STATUS_NO_DEADLINE)
        return portMAX_DELAY;
    TickType_t t = (TickType_t)((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
    return t ? t : 1;
}

// Bring the clock's text up to date (a redraw only if a text shows it and it
// changed). Logs the task's stack headroom once, after the first time the
// local time was worked out (newlib's TZ code runs on this stack).
static void refresh_clock(void)
{
    char text[CLOCK_TEXT_MAX];
    clock_local_text(text);
    s_bar.SetClock(text);
#ifndef NDEBUG
    static bool logged;
    if (text[0] && !logged) {
        logged = true;
        ESP_LOGI(TAG, "stack: %u bytes free", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
#endif
}

static void status_task(void *arg)
{
    status_bar_content_t content;
    uint16_t *back = NULL;      // the buffer being drawn
    int row = 0;                // next row of the bar to copy to it
    int rendered = 0;           // rows waiting in the scratch band
    uint32_t bits = 0;
    int64_t icons_ms = STATUS_NO_DEADLINE;

    while (1) {
        if (!back) {
            refresh_clock();                    // between draws: each wake, the minute one included
            icons_ms = refresh_icons();
            back = s_bar.BeginDraw(screen_status_bar_shown(), &content);
            row = rendered = 0;
        }

        if (back) {
            if (!rendered) {
                // Render the next band now (reads only); it is copied in the next gap
                rendered = screen_two_pages_in_view() ? STATUS_BAND_ROWS_TWO_PAGES : STATUS_BAND_ROWS_ONE_PAGE;
                if (rendered > SCREEN_STATUS_H - row)
                    rendered = SCREEN_STATUS_H - row;
                status_bar_render_rows(s_scratch, row, rendered, &s_style, &content);
                s_gap_client.wants = true;
            }
            if ((bits & WAKE_GAP) && fill_gap_fresh(COMP_GAP_FRESH_US)) {
                screen_note_status_drawing(true);
                uint16_t *dst = back + row * SCREEN_W;
                size_t bytes = rendered * SCREEN_W * sizeof(uint16_t);
                memcpy(dst, s_scratch, bytes);
                port::CacheWriteback(dst, bytes);       // so the fill reads it, and the bus traffic is here
                screen_note_status_drawing(false);

                row += rendered;
                rendered = 0;
                bits = 0;
                if (row >= SCREEN_STATUS_H) {
                    s_gap_client.wants = false;
                    s_bar.EndDraw();
                    screen_set_status_bar(s_bar.Front());
                    back = NULL;
                }
                continue;
            }
        }

        // Wait for a change or a gap, or, if a change waits on the last swap,
        // for the next frame start (a tick is at least one frame); while a
        // text shows the clock, at most until the minute turns over (and an
        // icon expiry)
        TickType_t wait;
        if (!back && s_bar.Pending())
            wait = 1;
        else
            wait = wait_ticks(icons_ms);
        bits = 0;
        xTaskNotifyWait(0, ULONG_MAX, &bits, wait);
    }
}

static bool ui_ok(const BLOBPACK *ui)
{
    if (ui->count < UI_ASSET_COUNT ||
        !check_font(get_blob(ui, UI_ASSET_STATUS_FONT), get_blob_size(ui, UI_ASSET_STATUS_FONT)))
        return false;
    for (int i = UI_ASSET_PAGER_CURRENT; i < UI_ASSET_COUNT; i++)      // pills and icons
        if (!check_image_header(get_blob(ui, i), get_blob_size(ui, i)))
            return false;
    return true;
}

esp_err_t status_init(const BLOBPACK *ui)
{
    if (!ui_ok(ui)) {
        ESP_LOGE(TAG, "Bad ui.bin");
        return ESP_ERR_INVALID_STATE;
    }
    s_style = status_bar_style(ui);

    // The bar is read by the fill every frame, so 64-byte (cache line) aligned
    uint16_t *buf[2];
    for (int i = 0; i < 2; i++) {
        buf[i] = (uint16_t *)heap_caps_aligned_alloc(64, STATUS_BAR_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!buf[i])
            return ESP_ERR_NO_MEM;
    }
    s_bar.Init(buf[0], buf[1]);

    s_scratch = (uint16_t *)heap_caps_aligned_alloc(4, SCREEN_W * STATUS_BAR_BAND_ROWS_MAX * sizeof(uint16_t),
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_scratch)
        return ESP_ERR_NO_MEM;

    if (xTaskCreatePinnedToCore(status_task, "status", STATUS_TASK_STACK, NULL,
                                STATUS_TASK_PRIORITY, &s_task, 1) != pdPASS)
        return ESP_FAIL;
    s_gap_client.task = s_task;
    fill_gap_register(&s_gap_client);
    return ESP_OK;
}

static void changed(bool c)
{
    if (c && s_task)
        xTaskNotify(s_task, WAKE_CHANGE, eSetBits);
}

void status_set_left(const char *text)    { changed(s_bar.SetLeft(text)); }
void status_set_right(const char *text)   { changed(s_bar.SetRight(text)); }
void status_set_pill_count(int count)     { changed(s_bar.SetPillCount(count)); }
void status_set_selected_pill(int index)  { changed(s_bar.SetSelectedPill(index)); }
void status_clock_changed(void)           { changed(true); }

// The icons' state changes; the task works out what shows (it may not change)
template <typename F>
static void icons_update(F f)
{
    s_icons_lock.Enter();
    f(uptime_ms());
    s_icons_lock.Exit();
    changed(true);
}

void status_set_wifi(WifiState state)     { icons_update([&](int64_t) { s_icons.SetWifi(state); }); }
void status_busy(bool busy)               { icons_update([&](int64_t now) { s_icons.SetBusy(busy, now); }); }
void status_net_busy(bool busy)           { icons_update([&](int64_t now) { s_icons.SetNetBusy(busy, now); }); }
void status_error(void)                   { icons_update([&](int64_t now) { s_icons.ReportError(now); }); }
void status_set_hosted(bool hosted)       { icons_update([&](int64_t) { s_icons.SetHosted(hosted); }); }
