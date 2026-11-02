#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "i2c.h"
#include "ch422g.h"
#include "color.h"
#include "rle565.h"
#include "blobpack.h"
#include "waveshare_rgb_lcd_port.h"
#include "waveshare_touch_port.h"
#include "compositor.h"
#include "model.h"
#include "host_link.h"
#include "screen.h"
#include "swipe.h"
#include "status.h"

static const char *TAG = "controlblock";

// The device boots with no pages, keys or status text: the host supplies
// them all (docs/protocol.md). The only built-in content is the UI pack for
// the status bar.

// Boot-time stress run: the compositor drawing flat out while the display
// runs, logging fills over budget. A quick device check after changing the
// compositor, the bounce fill or anything that writes PSRAM. Off by default;
// it embeds assets/test_keys.bin as its test images and builds with
//     idf.py --preset debug -DCB_RUN_STRESS=ON build
// (see main/CMakeLists.txt; use -DCB_RUN_STRESS=OFF to turn it off again).
#ifndef RUN_STRESS
#define RUN_STRESS  0
#endif

// Backlight cycle: toggle the backlight every this many seconds, to test the
// hardware (USB behaviour, power) without the host. Goes straight to the
// display, bypassing the touch gate and the host's backlight state, so don't
// use it together with a host app. Off (0) by default; build with
//     idf.py --preset debug -DCB_BACKLIGHT_CYCLE_S=5 build
// (-DCB_BACKLIGHT_CYCLE_S=0 turns it off again).
#ifndef BACKLIGHT_CYCLE_S
#define BACKLIGHT_CYCLE_S  0
#endif

extern const uint8_t ui_start[] asm("_binary_ui_bin_start");
extern const uint8_t ui_end[]   asm("_binary_ui_bin_end");

static const BLOBPACK *s_ui;            // PSRAM copy (rle565 needs 4-byte alignment)

// Copy an embedded blobpack to PSRAM (rle565 needs 4-byte alignment)
static BLOBPACK *load_pack(const uint8_t *start, const uint8_t *end)
{
    size_t size = end - start;
    void *p = heap_caps_aligned_alloc(4, size, MALLOC_CAP_SPIRAM);
    if (p)
        memcpy(p, start, size);
    return (BLOBPACK *)p;
}

// Where late fills were (see screen_stats_t), or "" if none
static const char *late_fills(const screen_stats_t *st)
{
    static char text[96];
    if (!st->overruns)
        return "";
    snprintf(text, sizeof(text), " [late: status %lu, 1 page %lu, 2 pages %lu; status drawing %lu]",
             (unsigned long)st->late_status, (unsigned long)st->late_one_page,
             (unsigned long)st->late_two_pages, (unsigned long)st->late_status_drawing);
    return text;
}

#if RUN_STRESS

#include "test_keys.h"

#define NUM_PAGES 3

// The demo faces are opaque (no clear before each key) or have transparent padding
#define USE_OPAQUE_FACES 1

// The stress run's own image IDs
#define DEMO_FACE_ID(i, opaque) (0xDE000100u + ((opaque) ? 0x10u : 0u) + (uint32_t)(i))
#define DEMO_ICON_ID(i)         (0xDE000200u + (uint32_t)(i))

extern const uint8_t test_keys_start[] asm("_binary_test_keys_bin_start");
extern const uint8_t test_keys_end[]   asm("_binary_test_keys_bin_end");

static int  s_icon_count;
static bool s_opaque;                   // the layout uses the opaque faces

// ---------------------------------------------------------------------------
// Stress layout: test_keys.bin's faces and icons, uploaded into the image
// store as the host would, on 3 pages

static uint32_t face_id(int i) { return DEMO_FACE_ID(i, s_opaque); }
static uint32_t icon_id(int i) { return DEMO_ICON_ID(i % s_icon_count); }
static int      page_face(int page) { return page % TEST_KEYS_FACE_PRESSED; }

static esp_err_t load_demo_images(void)
{
    BLOBPACK *pack = load_pack(test_keys_start, test_keys_end);
    if (!pack)
        return ESP_ERR_NO_MEM;
    s_icon_count = TEST_KEYS_ICON_COUNT(pack->count);

    esp_err_t err = ESP_OK;
    for (uint32_t b = 0; b + 1 < pack->count && err == ESP_OK; b++) {
        uint32_t id = b < TEST_KEYS_FIRST_ICON
                    ? DEMO_FACE_ID(b % TEST_KEYS_FACES, b >= TEST_KEYS_FACES)
                    : DEMO_ICON_ID(b - TEST_KEYS_FIRST_ICON);
        if (model_put_image(id, get_blob(pack, b), get_blob_size(pack, b)) != ImageResult::Ok) {
            ESP_LOGE(TAG, "Demo image %u was rejected", (unsigned)b);
            err = ESP_ERR_INVALID_STATE;
        }
    }
    heap_caps_free(pack);
    return err;
}

static KeyDef demo_key(int page, int k, int icon)
{
    KeyDef d = {};
    d.page = (uint8_t)page;
    d.key  = (uint8_t)k;
    d.ids[KEY_SLOT_BG]         = face_id(page_face(page));
    d.ids[KEY_SLOT_FG]         = icon_id(icon);
    d.ids[KEY_SLOT_PRESSED_BG] = face_id(TEST_KEYS_FACE_PRESSED);
    d.ids[KEY_SLOT_PRESSED_FG] = icon_id(icon);
    return d;
}

// Every page: its face colour, a different run of icons
static void set_demo_layout(bool opaque)
{
    static KeyDef defs[NUM_PAGES * COMP_KEYS];
    s_opaque = opaque;
    for (int p = 0; p < NUM_PAGES; p++)
        for (int k = 0; k < COMP_KEYS; k++)
            defs[p * COMP_KEYS + k] = demo_key(p, k, p * 5 + k);

    model_lock();
    model_layout()->SetPageCount(NUM_PAGES);
    model_layout()->SetKeys(defs, NUM_PAGES * COMP_KEYS);
    model_unlock();
}

// Every key on pages first..last gets icons shifted by `shift`
static void change_icons(int first, int last, int shift, uint32_t flags)
{
    static KeyDef defs[NUM_PAGES * COMP_KEYS];
    int n = 0;
    for (int p = first; p <= last && p < NUM_PAGES; p++)
        for (int k = 0; k < COMP_KEYS; k++)
            defs[n++] = demo_key(p, k, p * 5 + k + shift);
    model_lock();
    model_layout()->SetKeys(defs, n, flags);
    model_unlock();
}

// ---------------------------------------------------------------------------
// Stress run

// Fill impact: keep every key on the pages in view changing, so the
// compositor draws flat out for 2 s. Results are logged afterwards, so the
// log isn't competing with the run.
typedef struct {
    const char    *label;
    uint32_t       frames;
    float          busy_percent;
    screen_stats_t st;
    comp_stats_t   cs;
} stress_result_t;

typedef enum {
    STRESS_KEYS    = 0x01,      // change every key in view each round
    STRESS_STATUS  = 0x02,      // redraw the status bar each round
    STRESS_REPLACE = 0x04,      // replace an icon on screen at ~USB speed
} stress_what_t;

// Replace the icon shown on every key in view with another icon's pixels:
// writes the image to PSRAM, checks it, and redraws the keys using it
static void replace_icon(int round)
{
    static BLOBPACK *pack;
    if (!pack)
        pack = load_pack(test_keys_start, test_keys_end);
    int b = TEST_KEYS_FIRST_ICON + round % s_icon_count;
    model_put_image(icon_id(0), get_blob(pack, b), get_blob_size(pack, b));
    model_lock();
    model_store()->Collect();
    model_unlock();
}

static void stress_run(stress_result_t *r, const char *label, bool opaque, int scroll, uint32_t flags,
                       uint32_t what = STRESS_KEYS)
{
    set_demo_layout(opaque);
    if (what & STRESS_REPLACE) {
        // Every key in view shows icon 0
        static KeyDef defs[NUM_PAGES * COMP_KEYS];
        for (int i = 0; i < NUM_PAGES * COMP_KEYS; i++)
            defs[i] = demo_key(i / COMP_KEYS, i % COMP_KEYS, 0);
        model_lock();
        model_layout()->SetKeys(defs, NUM_PAGES * COMP_KEYS);
        model_unlock();
    }
    screen_set_scroll(scroll);
    vTaskDelay(pdMS_TO_TICKS(300));         // let both pages build

    screen_get_stats(&r->st);
    comp_get_stats(&r->cs);
    uint32_t frames0 = r->st.frames;

    int first = scroll / SCREEN_PAGE_W;
    int last  = (scroll + SCREEN_PAGE_W - 1) / SCREEN_PAGE_W;
    int64_t t0 = esp_timer_get_time();
    for (int round = 1; esp_timer_get_time() - t0 < 2000000; round++) {
        if (what & STRESS_STATUS) {
            char text[16];
            snprintf(text, sizeof(text), "%d", round);
            status_set_right(text);
        }
        if (what & STRESS_KEYS)
            change_icons(first, last, round, flags);
        if (what & STRESS_REPLACE) {
            // ~1 MB/s, about what USB full speed can deliver: two icons a tick
            replace_icon(round * 2);
            replace_icon(round * 2 + 1);
            vTaskDelay(1);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    int64_t elapsed = esp_timer_get_time() - t0;

    screen_get_stats(&r->st);
    comp_get_stats(&r->cs);
    r->label        = label;
    r->frames       = r->st.frames - frames0;
    r->busy_percent = r->cs.busy_us * 100.0f / elapsed;
}

static void stress_log(const stress_result_t *r)
{
    ESP_LOGI(TAG, "stress %-28s fill max %4lu us, %3lu over | keys %3lu, max %4lu us, busy %3.0f%%%s",
             r->label, (unsigned long)r->st.max_fill_us, (unsigned long)r->st.overruns,
             (unsigned long)r->cs.keys, (unsigned long)r->cs.max_key_us, r->busy_percent,
             late_fills(&r->st));
}

static void run_stress(void)
{
    static stress_result_t results[12];
    int n = 0;
    for (int opaque = 0; opaque < 2; opaque++) {
        const char *rest  = opaque ? "opaque, at rest, incremental" : "at rest, incremental";
        const char *restf = opaque ? "opaque, at rest, flush"       : "at rest, flush";
        const char *mid   = opaque ? "opaque, mid-swipe, incr"      : "mid-swipe, incremental";
        const char *midf  = opaque ? "opaque, mid-swipe, flush"     : "mid-swipe, flush";
        stress_run(&results[n++], rest,  opaque, 0, 0);
        stress_run(&results[n++], restf, opaque, 0, COMP_FLUSH);
        stress_run(&results[n++], mid,   opaque, SCREEN_PAGE_W / 2, 0);
        stress_run(&results[n++], midf,  opaque, SCREEN_PAGE_W / 2, COMP_FLUSH);
    }

    // Status bar redraws: on their own, then on top of the worst case above
    stress_run(&results[n++], "status bar only, mid-swipe", true, SCREEN_PAGE_W / 2, 0, STRESS_STATUS);
    stress_run(&results[n++], "opaque, mid, flush + status", true, SCREEN_PAGE_W / 2, COMP_FLUSH,
               STRESS_KEYS | STRESS_STATUS);
    status_set_right("");

    // Image uploads (replaces) writing PSRAM while the keys using them redraw
    stress_run(&results[n++], "image replaces, at rest", true, 0, 0, STRESS_REPLACE);
    stress_run(&results[n++], "image replaces, mid-swipe", true, SCREEN_PAGE_W / 2, 0, STRESS_REPLACE);

    screen_set_scroll(0);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "---- stress: every key in view changes every 20 ms for 2 s; fill budget %d us ----",
             DISPLAY_BOUNCE_BUDGET_US);
    for (int i = 0; i < n; i++) {
        stress_log(&results[i]);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Back to the empty device a host finds (the same steps as RESET)
    screen_set_page_count(1);
    model_lock();
    model_layout()->Clear();
    model_unlock();
    comp_reset();
    model_lock();
    model_store()->Reset();
    model_unlock();
}

#endif  // RUN_STRESS

// ---------------------------------------------------------------------------

// Load ui.bin and start the status bar, empty: the host sets the texts, and
// the swipe task sets the pills from the page count
static esp_err_t init_status_bar(void)
{
    s_ui = load_pack(ui_start, ui_end);
    if (!s_ui)
        return ESP_ERR_NO_MEM;
    return status_init(s_ui);
}

// Internal RAM is the scarce one (USB, Wi-Fi and the display all want it)
static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: internal %u free (largest %u, min ever %u), PSRAM %u free",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void on_touched(uint8_t id, TOUCH_EVENT_TYPE type, uint16_t x, uint16_t y)
{
    if (type != TOUCH_MOVE)
        ESP_LOGI(TAG, "touch id=%d %s x=%3d y=%3d", id, type == TOUCH_DOWN ? "DOWN" : "UP", x, y);
    swipe_touch(id, type, x, y);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ControlBlock starting");

    i2c_init();
    ESP_ERROR_CHECK(ch422g_init());
    ESP_ERROR_CHECK(init_status_bar());

    ESP_ERROR_CHECK(comp_init(model_get_page, model_is_retired, NULL));
    ESP_ERROR_CHECK(model_init());
    ESP_ERROR_CHECK(screen_init(1));        // the layout starts with one empty page
    ESP_ERROR_CHECK(display_enable_backlight(true));

#if RUN_STRESS
    ESP_ERROR_CHECK(load_demo_images());
    screen_set_page_count(NUM_PAGES);
    run_stress();
#endif

    ESP_ERROR_CHECK(swipe_init());
    ESP_ERROR_CHECK(touch_init(on_touched));
    ESP_ERROR_CHECK(host_link_init());
    log_heap("after init");

    uint32_t last_frames = 0;
    screen_stats_t st;
    comp_stats_t cs;
#if BACKLIGHT_CYCLE_S
    uint32_t seconds = 0;
    bool backlight = true;
#endif
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
#if BACKLIGHT_CYCLE_S
        if (++seconds % BACKLIGHT_CYCLE_S == 0) {
            backlight = !backlight;
            ESP_LOGI(TAG, "backlight cycle: %s", backlight ? "on" : "off");
            display_enable_backlight(backlight);
        }
#endif
        ImageStoreStats is;
        model_lock();
        model_store()->GetStats(&is);
        model_unlock();

        screen_get_stats(&st);
        comp_get_stats(&cs);
        ESP_LOGI(TAG, "%lu fps, fill max %3lu us, %lu over %d us, page %d | "
                      "comp: %lu keys (max %lu us, busy %lu us), %lu swaps, %lu held | img %lu (%lu KB)%s",
                 (unsigned long)(st.frames - last_frames),
                 (unsigned long)st.max_fill_us, (unsigned long)st.overruns, DISPLAY_BOUNCE_BUDGET_US,
                 swipe_current_page(),
                 (unsigned long)cs.keys, (unsigned long)cs.max_key_us, (unsigned long)cs.busy_us,
                 (unsigned long)cs.swaps, (unsigned long)cs.holds,
                 (unsigned long)is.count, (unsigned long)(is.used / 1024), late_fills(&st));
        last_frames = st.frames;
    }
}
