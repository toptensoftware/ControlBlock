#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "i2c.h"
#include "ch422g.h"
#include "color.h"
#include "rle565.h"
#include "blobpack.h"
#include "waveshare_rgb_lcd_port.h"
#include "waveshare_touch_port.h"
#include "compositor.h"
#include "layout_boot.h"
#include "model.h"
#include "heap_log.h"
#include "host_link.h"
#include "net_worker.h"
#include "wifi.h"
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

// The once-a-second stats line (fps, fill, compositor, images, net). Off by
// default so it doesn't bury other log messages; build with
//     idf.py --preset debug -DCB_STATS_LOG=ON build
// (-DCB_STATS_LOG=OFF turns it off again). With it off, the net counters are
// still logged, but only when one changes.
#ifndef STATS_LOG
#define STATS_LOG  0
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
#if STATS_LOG || RUN_STRESS
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
#endif

// The network ops' counters, or "" until one has run
static const char *net_counts(void)
{
    static char text[80];
    NetWorkerStats ns;
    net_worker_get_stats(&ns);
    if (!(ns.ok | ns.failed | ns.timeouts | ns.dropped | ns.refused | ns.late))
        return "";
    snprintf(text, sizeof(text), " | net %lu ok, %lu failed, %lu timed out, %lu dropped, %lu late",
             (unsigned long)ns.ok, (unsigned long)ns.failed, (unsigned long)ns.timeouts,
             (unsigned long)(ns.dropped + ns.refused), (unsigned long)ns.late);
    return text;
}

#if RUN_STRESS

#include "test_keys.h"
#include "layout_types.h"
#include "op_exec.h"
#include "script_host.h"
#include "standalone_task.h"
#include "stress_script.h"

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

// What load_demo_images found, logged with the results: anything logged during the load used to be
// lost when the backlight switch-on dropped the UART port
static char     s_load_report[512];
static int      s_load_len;
static uint32_t s_load_store_count, s_load_blobs;

static void report(const char *fmt, ...)
{
    if (s_load_len >= (int)sizeof(s_load_report) - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    s_load_len += vsnprintf(s_load_report + s_load_len, sizeof(s_load_report) - s_load_len, fmt, ap);
    va_end(ap);
}

// The PSRAM copy of the pack against the original embedded in the app. A backlight switch-on once
// corrupted it (bit errors over ~1 KB; CLAUDE.md, Open Items): the early warning if that comes back.
static void report_pack_diffs(const BLOBPACK *pack)
{
    const uint8_t *copy = (const uint8_t *)pack;
    uint32_t size = test_keys_end - test_keys_start;
    int n = 0, first = -1, last = -1;
    for (uint32_t i = 0; i < size; i++) {
        if (copy[i] == test_keys_start[i])
            continue;
        if (n++ == 0)
            first = (int)i;
        last = (int)i;
    }
    if (n == 0)
        return;
    report(" | PSRAM copy CORRUPT: %d bytes differ (%p..%p):", n, copy + first, copy + last);
    for (int i = first; i < first + 8 && i < (int)size; i++)
        report(" %02x/%02x", copy[i], test_keys_start[i]);
}

static esp_err_t load_demo_images(void)
{
    BLOBPACK *pack = load_pack(test_keys_start, test_keys_end);
    if (!pack)
        return ESP_ERR_NO_MEM;
    report_pack_diffs(pack);
    s_icon_count = TEST_KEYS_ICON_COUNT(pack->count);
    uint32_t pack_count = pack->count;

    // Every image is tried, and each refusal noted
    esp_err_t err = ESP_OK;
    for (uint32_t b = 0; b + 1 < pack_count; b++) {
        uint32_t id = b < TEST_KEYS_FIRST_ICON
                    ? DEMO_FACE_ID(b % TEST_KEYS_FACES, b >= TEST_KEYS_FACES)
                    : DEMO_ICON_ID(b - TEST_KEYS_FIRST_ICON);
        ImageResult ir = model_put_image(id, get_blob(pack, b), get_blob_size(pack, b));
        if (ir != ImageResult::Ok) {
            report(" | blob %u (ID 0x%08lx, %lu bytes) refused (%d)", (unsigned)b, (unsigned long)id,
                   (unsigned long)get_blob_size(pack, b), (int)ir);
            err = ESP_ERR_INVALID_STATE;
        }
    }
    heap_caps_free(pack);

    ImageStoreStats is;
    model_lock();
    model_store()->GetStats(&is);
    model_unlock();
    s_load_store_count = is.count;
    s_load_blobs = pack_count - 1;
    return err;
}

// Refusal codes: 1 bad arg, 2 no space, 3 bad image, 4 bad sequence (ImageResult)
static void log_demo_load(void)
{
    ESP_LOGI(TAG, "stress: %lu demo images (%d icons), the store then had %lu images%s",
             (unsigned long)s_load_blobs, s_icon_count, (unsigned long)s_load_store_count,
             s_load_report[0] ? s_load_report : " | all accepted, PSRAM copy intact");
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

// A refused layout change leaves the compositor with nothing to draw, which makes a case's numbers
// meaningless: log the first refusal of each case (stress_run clears s_layout_refused)
static bool s_layout_refused;

static void check_layout(KeyLayoutResult r, const char *what, const KeyDef *defs = nullptr, int count = 0)
{
    if (r == KeyLayoutResult::Ok || s_layout_refused)
        return;
    s_layout_refused = true;

    // Which image the store doesn't have, and how many it does
    uint32_t missing = 0;
    ImageStoreStats is;
    model_lock();
    for (int i = 0; i < count && !missing; i++)
        for (int s = 0; s < KEY_SLOTS && !missing; s++)
            if (defs[i].ids[s] && !model_store()->IsLive(defs[i].ids[s]))
                missing = defs[i].ids[s];
    model_store()->GetStats(&is);
    model_unlock();
    ESP_LOGE(TAG, "stress: %s refused (%d): first missing image 0x%08lx; the store has %lu images, %lu KB of %lu KB",
             what, (int)r, (unsigned long)missing, (unsigned long)is.count, (unsigned long)(is.used / 1024),
             (unsigned long)(is.capacity / 1024));
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
    KeyLayoutResult count = model_layout()->SetPageCount(NUM_PAGES);
    KeyLayoutResult keys  = model_layout()->SetKeys(defs, NUM_PAGES * COMP_KEYS);
    model_unlock();
    check_layout(count, "SetPageCount");
    check_layout(keys, "SetKeys (layout)", defs, NUM_PAGES * COMP_KEYS);
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
    KeyLayoutResult r = model_layout()->SetKeys(defs, n, flags);
    model_unlock();
    check_layout(r, "SetKeys (icon change)", defs, n);
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
    uint32_t       script_runs;     // STRESS_SCRIPT: runs in the case, and the host's totals after it
    uint32_t       script_errors;
    size_t         script_peak;
    unsigned       script_refused;
} stress_result_t;

typedef enum {
    STRESS_KEYS    = 0x01,      // change every key in view each round
    STRESS_STATUS  = 0x02,      // redraw the status bar each round
    STRESS_REPLACE = 0x04,      // replace an icon on screen at ~USB speed
    STRESS_SCRIPT  = 0x08,      // Lua churning its PSRAM heap on core 0, as a busy script op does
} stress_what_t;

// ---------------------------------------------------------------------------
// Lua churn: a script that allocates and frees a lot of its (PSRAM) heap, run back to back
// for STRESS_SCRIPT_BUSY_US, then a tick's rest, on its own core 0 task like the op task
// (app_main's stack is too small for Lua). Real scripts are far shorter; if the fill keeps
// up with this, it keeps up with them.

#define STRESS_SCRIPT_BUSY_US  18000
#define STRESS_SCRIPT_STACK    12288       // as the op task's (standalone_task.cpp)
#define STRESS_SCRIPT_PRIO     3

static ScriptHost        s_stress_script;
static volatile bool     s_script_on;
static volatile uint32_t s_script_runs;
static TaskHandle_t      s_script_task;

static uint32_t stress_clock_ms(void *)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void script_churn_task(void *)
{
    ExecCtx exec = {};
    exec.now_ms = stress_clock_ms;
    exec.script = &s_stress_script;
    layoutdata::scriptOp op = {};
    op.base.op      = OP_SCRIPT;
    op.source       = STRESS_LUA;
    op.source_count = sizeof(STRESS_LUA) - 1;
    while (1) {
        if (!s_script_on) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        while (s_script_on && esp_timer_get_time() - t0 < STRESS_SCRIPT_BUSY_US) {
            OpTask task = {};
            task.param = "";
            s_stress_script.Start(exec, &op, task);     // runs to the end: the script never waits
            s_script_runs = s_script_runs + 1;          // only this task writes it
        }
        vTaskDelay(1);
    }
}

static void stress_script(bool on)
{
    if (on && !s_script_task) {
        s_stress_script.Init(standalone_script_platform(), nullptr);
        xTaskCreatePinnedToCore(script_churn_task, "stress_lua", STRESS_SCRIPT_STACK, nullptr,
                                STRESS_SCRIPT_PRIO, &s_script_task, 0);
    }
    s_script_on = on;
    if (on && s_script_task)
        xTaskNotifyGive(s_script_task);
}

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
    s_layout_refused = false;
    set_demo_layout(opaque);
    if (what & STRESS_REPLACE) {
        // Every key in view shows icon 0
        static KeyDef defs[NUM_PAGES * COMP_KEYS];
        for (int i = 0; i < NUM_PAGES * COMP_KEYS; i++)
            defs[i] = demo_key(i / COMP_KEYS, i % COMP_KEYS, 0);
        model_lock();
        KeyLayoutResult lr = model_layout()->SetKeys(defs, NUM_PAGES * COMP_KEYS);
        model_unlock();
        check_layout(lr, "SetKeys (replace layout)", defs, NUM_PAGES * COMP_KEYS);
    }
    screen_set_scroll(scroll);
    vTaskDelay(pdMS_TO_TICKS(300));         // let both pages build

    screen_get_stats(&r->st);
    comp_get_stats(&r->cs);
    uint32_t frames0 = r->st.frames;
    uint32_t runs0 = s_script_runs;
    if (what & STRESS_SCRIPT)
        stress_script(true);

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
    stress_script(false);

    screen_get_stats(&r->st);
    comp_get_stats(&r->cs);
    r->label        = label;
    r->frames       = r->st.frames - frames0;
    r->busy_percent = r->cs.busy_us * 100.0f / elapsed;
    r->script_runs  = s_script_runs - runs0;
    if (r->script_runs) {
        vTaskDelay(pdMS_TO_TICKS(30));          // the churn task's last run is over: its stats are settled
        r->script_errors  = s_stress_script.Errors();
        r->script_peak    = s_stress_script.HeapPeak();
        r->script_refused = s_stress_script.HeapRefused();
    }
}

static void stress_log(const stress_result_t *r)
{
    ESP_LOGI(TAG, "stress %-28s fill max %4lu us, %3lu over | keys %3lu, max %4lu us, busy %3.0f%%%s",
             r->label, (unsigned long)r->st.max_fill_us, (unsigned long)r->st.overruns,
             (unsigned long)r->cs.keys, (unsigned long)r->cs.max_key_us, r->busy_percent,
             late_fills(&r->st));
    if (r->script_runs)
        ESP_LOGI(TAG, "       lua: %lu runs, %lu errors, heap peak %u KB, %u allocs refused (totals so far)",
                 (unsigned long)r->script_runs, (unsigned long)r->script_errors,
                 (unsigned)(r->script_peak / 1024), r->script_refused);
}

static void run_stress(void)
{
    static stress_result_t results[16];
    int n = 0;
    ESP_LOGI(TAG, "stress run starting (~35 s): touch, USB and Wi-Fi start after it, results at the end");
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

    // Lua writing PSRAM from core 0: alone, then with the compositor's worst case
    stress_run(&results[n++], "lua churn, at rest", true, 0, 0, STRESS_SCRIPT);
    stress_run(&results[n++], "lua churn, mid-swipe", true, SCREEN_PAGE_W / 2, 0, STRESS_SCRIPT);
    stress_run(&results[n++], "lua + keys, mid, flush", true, SCREEN_PAGE_W / 2, COMP_FLUSH,
               STRESS_KEYS | STRESS_SCRIPT);
    s_stress_script.Close();                    // give the Lua heap back

    screen_set_scroll(0);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "---- stress: every key in view changes every 20 ms for 2 s; fill budget %d us ----",
             DISPLAY_BOUNCE_BUDGET_US);
    log_demo_load();
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

// NVS holds the Wi-Fi credentials (and the driver's PHY calibration). If it is
// full or from a newer version, it is erased (the credentials are lost; the
// host can set them again). A failure that remains only costs Wi-Fi: the
// credentials can't be stored then, and the device boots as usual.
static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: %s, erasing it", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "NVS init failed: %s (Wi-Fi settings can't be stored)", esp_err_to_name(err));
}

static void on_touched(uint8_t id, TOUCH_EVENT_TYPE type, uint16_t x, uint16_t y)
{
    if (type != TOUCH_MOVE)
        ESP_LOGD(TAG, "touch id=%d %s x=%3d y=%3d", id, type == TOUCH_DOWN ? "DOWN" : "UP", x, y);
    swipe_touch(id, type, x, y);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ControlBlock starting");
    log_heap(TAG, "at start");

    init_nvs();
    log_heap(TAG, "after NVS");
    i2c_init();
    ESP_ERROR_CHECK(ch422g_init());
    ESP_ERROR_CHECK(init_status_bar());
    log_heap(TAG, "after the status bar");

    ESP_ERROR_CHECK(comp_init(model_get_page, model_is_retired, NULL));
    log_heap(TAG, "after the compositor");
    layout_boot_load();                     // before the store sizes itself and before the display runs
    log_heap(TAG, "after the layout load");
    ESP_ERROR_CHECK(model_init());
    log_heap(TAG, "after the model");
    ESP_ERROR_CHECK(screen_init(1));        // the key layout starts with one empty page
    log_heap(TAG, "after the screen (display driver)");
    layout_boot_apply();                    // the stored layout's Home, if any
    // The backlight is already on (ch422g_init keeps it on: switching it on corrupts PSRAM)
#if NO_DISPLAY
    ESP_ERROR_CHECK(display_enable_backlight(false));
#endif

#if RUN_STRESS
    load_demo_images();                     // refusals are reported with the results (log_demo_load)
    screen_set_page_count(NUM_PAGES);
    run_stress();
#endif

    ESP_ERROR_CHECK(swipe_init());
    log_heap(TAG, "after swipe");
    ESP_ERROR_CHECK(touch_init(on_touched));
    log_heap(TAG, "after touch");
    wifi_init();                            // loads the credentials; no radio yet
    ESP_ERROR_CHECK(host_link_init());
    log_heap(TAG, "after the host link (USB)");
    ESP_ERROR_CHECK(wifi_start());          // last: a Wi-Fi problem can never keep the screen from coming up
    log_heap(TAG, "after init");

#if STATS_LOG
    uint32_t last_frames = 0;
    screen_stats_t st;
    comp_stats_t cs;
#else
    char last_net[80] = "";
#endif
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
#if !STATS_LOG
        const char *net = net_counts();
        if (strcmp(net, last_net) != 0) {
            ESP_LOGI(TAG, "stats%s", net);
            snprintf(last_net, sizeof(last_net), "%s", net);
        }
#else
        ImageStoreStats is;
        model_lock();
        model_store()->GetStats(&is);
        model_unlock();

        screen_get_stats(&st);
        comp_get_stats(&cs);
        ESP_LOGI(TAG, "%lu fps, fill max %3lu us, %lu over %d us, page %d | "
                      "comp: %lu keys (max %lu us, busy %lu us), %lu swaps, %lu held | img %lu (%lu KB)%s%s",
                 (unsigned long)(st.frames - last_frames),
                 (unsigned long)st.max_fill_us, (unsigned long)st.overruns, DISPLAY_BOUNCE_BUDGET_US,
                 swipe_current_page(),
                 (unsigned long)cs.keys, (unsigned long)cs.max_key_us, (unsigned long)cs.busy_us,
                 (unsigned long)cs.swaps, (unsigned long)cs.holds,
                 (unsigned long)is.count, (unsigned long)(is.used / 1024), net_counts(), late_fills(&st));
        last_frames = st.frames;
#endif
    }
}
