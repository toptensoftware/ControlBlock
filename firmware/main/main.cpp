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
#include "clock.h"
#include "display_settings.h"
#include "protocol.h"
#include "log_capture.h"
#include "diag_collect.h"
#include "pclk_cycle.h"

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

// Render lab (wip/wip-on-device-render-feasibility.md): times on-device key renders at boot and
// leaves the results on page 0 (render_lab_run.cpp); with RUN_STRESS too, the stress run adds
// render cases. Off by default; -DCB_RENDER_LAB=ON (see main/CMakeLists.txt).
#ifndef RENDER_LAB
#define RENDER_LAB  0
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

// The once-a-second stats line (fps, fill, compositor, images, usb rx, net). Off by
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

#if RENDER_LAB
#include "render_lab_cases.h"       // RENDER_LAB_CASE_* (the stress cases)
#include "render_lab_run.h"

extern const uint8_t render_lab_start[]   asm("_binary_render_lab_bin_start");
extern const uint8_t render_lab_end[]     asm("_binary_render_lab_bin_end");
extern const uint8_t path_vectors_start[] asm("_binary_path_vectors_bin_start");
extern const uint8_t path_vectors_end[]   asm("_binary_path_vectors_bin_end");

// The lab's assets, as aligned PSRAM copies (the paths need 2-byte, rle565 4-byte alignment)
static void init_render_lab(void)
{
    render_lab_init(load_pack(render_lab_start, render_lab_end), load_pack(path_vectors_start, path_vectors_end),
                    (size_t)(path_vectors_end - path_vectors_start));
}
#endif

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

#if RUN_STRESS
// Fill start timing and bounce buffer order (screen_stats_t), then late_fills()
static const char *fill_timing(const screen_stats_t *st)
{
    static char text[200];
    int n = snprintf(text, sizeof(text), " | start late max %3lu us, end max %3lu us, %lu missed",
                     (unsigned long)st->max_late_us, (unsigned long)st->max_end_us, (unsigned long)st->missed);
    if ((st->seq_breaks || st->buffer_mismatches) && n < (int)sizeof(text))
        n += snprintf(text + n, sizeof(text) - n, ", %lu seq breaks, %lu BUFFER MISMATCHES",
                      (unsigned long)st->seq_breaks, (unsigned long)st->buffer_mismatches);
    if (n < (int)sizeof(text))
        snprintf(text + n, sizeof(text) - n, "%s", late_fills(st));
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
#include "fill_gap.h"
#include "layout_types.h"
#include "op_exec.h"
#include "script_host.h"
#include "standalone_task.h"
#include "stress_script.h"

#define NUM_PAGES 3

// The stress run's own image IDs: test_keys.bin's opaque 1U and 2U keys
#define DEMO_1U_ID(i)           (0xDE000200u + (uint32_t)(i))
#define DEMO_2U_ID(i)           (0xDE000300u + (uint32_t)(i))

extern const uint8_t test_keys_start[] asm("_binary_test_keys_bin_start");
extern const uint8_t test_keys_end[]   asm("_binary_test_keys_bin_end");

// ---------------------------------------------------------------------------
// Stress layouts: test_keys.bin's opaque keys (fully baked, 1U and 2U), uploaded into the image
// store as the host would. Two page layouts, in quarter units (KeyRect):
//  - plain: 15 x 1U, the plain grid
//  - mixed: 2U spans, 1U keys at fractional positions (x 0.5U, 1.5U) and 0.5U keys with no image
//    (drawn as background), so a page differs from the plain grid and the black fill runs
// Pages 0 and 2 are plain, page 1 mixed: mid-swipe the two pages in view have different layouts.

enum { LAYOUT_PLAIN, LAYOUT_MIXED };

static const KeyRect MIXED_RECTS[] = {
    {  0, 0, 8, 4 }, {  8, 0, 4, 4 }, { 12, 0, 4, 4 }, { 16, 0, 4, 4 },                   // 2U, 1U, 1U, 1U
    {  0, 4, 2, 4 }, {  2, 4, 4, 4 }, {  6, 4, 4, 4 }, { 10, 4, 8, 4 }, { 18, 4, 2, 4 },  // 0.5U, 1U, 1U, 2U, 0.5U
    {  0, 8, 4, 4 }, {  4, 8, 8, 4 }, { 12, 8, 8, 4 },                                    // 1U, 2U, 2U
};
#define MIXED_COUNT  (int)(sizeof(MIXED_RECTS) / sizeof(MIXED_RECTS[0]))

static int s_page_layout[NUM_PAGES];

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

static void put_demo_image(uint32_t id, const BLOBPACK *pack, uint32_t b, esp_err_t *err)
{
    ImageResult ir = model_put_image(id, get_blob(pack, b), get_blob_size(pack, b));
    if (ir == ImageResult::Ok)
        return;
    report(" | blob %u (ID 0x%08lx, %lu bytes) refused (%d)", (unsigned)b, (unsigned long)id,
           (unsigned long)get_blob_size(pack, b), (int)ir);
    *err = ESP_ERR_INVALID_STATE;
}

// The opaque keys only: the 128 faces and icons are the host tests' (test_keys.h)
static esp_err_t load_demo_images(void)
{
    BLOBPACK *pack = load_pack(test_keys_start, test_keys_end);
    if (!pack)
        return ESP_ERR_NO_MEM;
    report_pack_diffs(pack);

    // Every image is tried, and each refusal noted
    esp_err_t err = ESP_OK;
    for (int i = 0; i < TEST_KEYS_KEYS_1U; i++)
        put_demo_image(DEMO_1U_ID(i), pack, (uint32_t)TEST_KEYS_KEY_1U(pack->count, i), &err);
    for (int i = 0; i < TEST_KEYS_KEYS_2U; i++)
        put_demo_image(DEMO_2U_ID(i), pack, (uint32_t)TEST_KEYS_KEY_2U(pack->count, i), &err);
    heap_caps_free(pack);

    ImageStoreStats is;
    model_lock();
    model_store()->GetStats(&is);
    model_unlock();
    s_load_store_count = is.count;
    s_load_blobs = TEST_KEYS_KEYS;
    return err;
}

// Refusal codes: 1 bad arg, 2 no space, 3 bad image, 4 bad sequence (ImageResult)
static void log_demo_load(void)
{
    ESP_LOGI(TAG, "stress: %lu demo images (%d 1U, %d 2U), the store then had %lu images%s",
             (unsigned long)s_load_blobs, TEST_KEYS_KEYS_1U, TEST_KEYS_KEYS_2U, (unsigned long)s_load_store_count,
             s_load_report[0] ? s_load_report : " | all accepted, PSRAM copy intact");
}

// The demo image for a key of `r`'s size (1U or 2U), number `i`; none for other sizes (background)
static uint32_t demo_image(KeyRect r, int i)
{
    if (r.h != 4)
        return IMAGE_ID_NONE;
    if (r.w == 4)
        return DEMO_1U_ID(i % TEST_KEYS_KEYS_1U);
    if (r.w == 8)
        return DEMO_2U_ID(i % TEST_KEYS_KEYS_2U);
    return IMAGE_ID_NONE;
}

// Page `page`'s keys in its current layout into `out`; returns the count. Each key's images are
// shifted by `shift` (pressed = the next image); `same`: every key shows image 0 (pressed 1).
static int page_defs(int page, int shift, bool same, KeyDef *out)
{
    bool mixed = s_page_layout[page] == LAYOUT_MIXED;
    int count = mixed ? MIXED_COUNT : KEY_GRID_SLOTS;
    for (int i = 0; i < count; i++) {
        KeyDef d = {};
        d.id      = (uint16_t)(page * COMP_MAX_KEYS + i + 1);     // unique across the pages
        d.page    = (uint8_t)page;
        d.rect    = mixed ? MIXED_RECTS[i] : KeySlotRect(i);
        int n     = same ? 0 : page * 5 + i + shift;
        d.normal  = demo_image(d.rect, n);
        d.pressed = demo_image(d.rect, n + 1);
        out[i] = d;
    }
    return count;
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
        for (uint32_t id : { defs[i].normal, defs[i].pressed })
            if (id && !missing && !model_store()->IsLive(id))
                missing = id;
    model_store()->GetStats(&is);
    model_unlock();
    ESP_LOGE(TAG, "stress: %s refused (%d): first missing image 0x%08lx; the store has %lu images, %lu KB of %lu KB",
             what, (int)r, (unsigned long)missing, (unsigned long)is.count, (unsigned long)(is.used / 1024),
             (unsigned long)(is.capacity / 1024));
}

// Pages first..last get their keys again in their current layouts, images shifted by `shift`
static void set_keys(int first, int last, int shift, bool same, uint32_t flags, const char *what)
{
    static KeyDef defs[NUM_PAGES * COMP_MAX_KEYS];
    int n = 0;
    if (last >= NUM_PAGES)
        last = NUM_PAGES - 1;
    for (int p = first; p <= last; p++)
        n += page_defs(p, shift, same, defs + n);
    model_lock();
    KeyLayoutResult r = model_layout()->SetKeys(first, last - first + 1, defs, n, flags);
    model_unlock();
    check_layout(r, what, defs, n);
}

// The starting layouts: pages 0 and 2 plain, page 1 mixed
static void set_demo_layout(bool same)
{
    for (int p = 0; p < NUM_PAGES; p++)
        s_page_layout[p] = p == 1 ? LAYOUT_MIXED : LAYOUT_PLAIN;
    model_lock();
    KeyLayoutResult count = model_layout()->SetPageCount(NUM_PAGES);
    model_unlock();
    check_layout(count, "SetPageCount");
    set_keys(0, NUM_PAGES - 1, 0, same, 0, "SetKeys (layout)");
}

// ---------------------------------------------------------------------------
// Stress run

// Fill impact: keep every key on the pages in view changing (or the case's other load), so the
// compositor draws flat out for 2 s. Results are logged afterwards, so the log isn't competing
// with the run.
typedef struct {
    const char    *label;
    uint32_t       frames, frames_expected;
    float          busy_percent;
    screen_stats_t st;
    comp_stats_t   cs;              // summed over the case
    uint32_t       builds;          // STRESS_BUILD: whole-page builds timed, their average and max
    int64_t        build_us_sum, build_us_max;
    uint32_t       script_runs;     // STRESS_SCRIPT: runs in the case, and the host's totals after it
    uint32_t       script_errors;
    size_t         script_peak;
    unsigned       script_refused;
#if RENDER_LAB
    bool           render_asked;    // STRESS_RENDER: asked for, started, and the churn's numbers
    bool           render_started;
    RenderLabChurnStats render;
#endif
} stress_result_t;

typedef enum {
    STRESS_KEYS    = 0x01,      // change every key in view each round
    STRESS_STATUS  = 0x02,      // redraw the status bar each round
    STRESS_REPLACE = 0x04,      // replace an image on screen at ~USB speed
    STRESS_SCRIPT  = 0x08,      // Lua churning its PSRAM heap on core 0, as a busy script op does
    STRESS_LAYOUT  = 0x10,      // switch the pages in view between the plain and mixed layouts each round
    STRESS_BUILD   = 0x20,      // change every key in view (flushed) and time it until it's on screen
    STRESS_RENDER  = 0x40,      // render lab: render + encode + put on core 0, back to back (stress_render_t)
} stress_what_t;

// STRESS_RENDER: which render lab case, onto which image (one the keys in view show), and whether
// its picture buffer is in PSRAM
typedef struct {
    int         index;          // RENDER_LAB_CASE_*
    uint32_t    id;
    bool        psram;
} stress_render_t;

#define STRESS_CASE_US      2000000
#define STRESS_SETTLE_MS    600     // let the pages in view build before measuring (a full page ~100-300 ms)
#define STRESS_BUILD_MAX_US 2000000 // a build that hasn't shown by then is given up

// The compositor's counters since the last call, added to `acc` (comp_get_stats resets them)
static void comp_add_stats(comp_stats_t *acc)
{
    comp_stats_t s;
    comp_get_stats(&s);
    acc->keys    += s.keys;
    acc->swaps   += s.swaps;
    acc->holds   += s.holds;
    acc->busy_us += s.busy_us;
    acc->erases  += s.erases;
    acc->chunks  += s.chunks;
    acc->gaps    += s.gaps;
    acc->bytes   += s.bytes;
    acc->late_gaps += s.late_gaps;
    if (s.max_key_us > acc->max_key_us)
        acc->max_key_us = s.max_key_us;
    if (s.max_chunk_us > acc->max_chunk_us)
        acc->max_chunk_us = s.max_chunk_us;
    if (s.max_gap_end_us > acc->max_gap_end_us)
        acc->max_gap_end_us = s.max_gap_end_us;
}

// ---------------------------------------------------------------------------
// Lua churn: a script that allocates and frees a lot of its (PSRAM) heap, run back to back
// for STRESS_SCRIPT_BUSY_US, then a tick's rest, on its own core 0 task like the op task
// (app_main's stack is too small for Lua). Real scripts are far shorter; if the fill keeps
// up with this, it keeps up with them.

#define STRESS_SCRIPT_BUSY_US  18000
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
        standalone_create_script_task(script_churn_task, "stress_lua", STRESS_SCRIPT_PRIO, &s_script_task);   // as the op task
    }
    s_script_on = on;
    if (on && s_script_task)
        xTaskNotifyGive(s_script_task);
}

// Replace image 0 (shown on every 1U key in view) with another 1U key's pixels: writes the image
// to PSRAM, checks it, and redraws the keys using it
static void replace_image(int round)
{
    static BLOBPACK *pack;
    if (!pack)
        pack = load_pack(test_keys_start, test_keys_end);
    int b = TEST_KEYS_KEY_1U(pack->count, round % TEST_KEYS_KEYS_1U);
    model_put_image(DEMO_1U_ID(0), get_blob(pack, b), get_blob_size(pack, b));
    model_lock();
    model_store()->Collect();
    model_unlock();
}

// Change every key on pages first..last (flushed) and wait until the compositor has swapped it
// onto the screen: one whole-page build, timed (to the tick: ~10 ms)
static void timed_build(stress_result_t *r, int first, int last, int round)
{
    uint32_t swaps0 = r->cs.swaps;
    int64_t t0 = esp_timer_get_time();
    set_keys(first, last, round, false, COMP_FLUSH, "SetKeys (build)");
    int64_t us;
    do {
        vTaskDelay(1);
        comp_add_stats(&r->cs);
        us = esp_timer_get_time() - t0;
    } while (r->cs.swaps == swaps0 && us < STRESS_BUILD_MAX_US);
    r->builds++;
    r->build_us_sum += us;
    if (us > r->build_us_max)
        r->build_us_max = us;
}

static void stress_run(stress_result_t *r, const char *label, int scroll, uint32_t flags,
                       uint32_t what = STRESS_KEYS, const stress_render_t *render = nullptr)
{
    // Live, so flicker on the screen can be matched to a case
    ESP_LOGI(TAG, "stress case: %s (%d s)", label, STRESS_CASE_US / 1000000);
    memset(r, 0, sizeof(*r));
    s_layout_refused = false;
    // Replaces and renders: every 1U key in view shows image 0 (and every 2U key 2U image 0)
    set_demo_layout((what & (STRESS_REPLACE | STRESS_RENDER)) != 0);
    screen_set_scroll(scroll);
    vTaskDelay(pdMS_TO_TICKS(STRESS_SETTLE_MS));

    comp_stats_t discard;
    screen_get_stats(&r->st);       // resets the maxima and late counts
    comp_get_stats(&discard);
    uint32_t frames0 = r->st.frames;
    uint32_t runs0 = s_script_runs;
    if (what & STRESS_SCRIPT)
        stress_script(true);
#if RENDER_LAB
    if ((what & STRESS_RENDER) && render)
        r->render_started = render_lab_churn_start(render->index, render->id, render->psram);
#endif

    int first = scroll / SCREEN_PAGE_W;
    int last  = (scroll + SCREEN_PAGE_W - 1) / SCREEN_PAGE_W;
    int64_t t0 = esp_timer_get_time();
    for (int round = 1; esp_timer_get_time() - t0 < STRESS_CASE_US; round++) {
        if (what & STRESS_STATUS) {
            char text[16];
            snprintf(text, sizeof(text), "%d", round);
            status_set_right(text);
        }
        if (what & STRESS_LAYOUT)
            for (int p = first; p <= last && p < NUM_PAGES; p++)
                s_page_layout[p] = s_page_layout[p] == LAYOUT_PLAIN ? LAYOUT_MIXED : LAYOUT_PLAIN;
        if (what & STRESS_BUILD) {
            timed_build(r, first, last, round);
            continue;
        }
        if (what & (STRESS_KEYS | STRESS_LAYOUT))
            set_keys(first, last, round, false, flags, "SetKeys (change)");
        if (what & STRESS_REPLACE) {
            // ~1 MB/s, about what USB full speed can deliver: two images a tick
            replace_image(round * 2);
            replace_image(round * 2 + 1);
            vTaskDelay(1);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        comp_add_stats(&r->cs);
    }
    int64_t elapsed = esp_timer_get_time() - t0;
    stress_script(false);
#if RENDER_LAB
    if (r->render_started)
        render_lab_churn_stop(&r->render);
    r->render_asked = (what & STRESS_RENDER) != 0;
#endif

    screen_get_stats(&r->st);
    comp_add_stats(&r->cs);
    r->label           = label;
    r->frames          = r->st.frames - frames0;
    r->frames_expected = (uint32_t)(elapsed * (DISPLAY_PIXEL_CLOCK_HZ / 1000000) /
                                    (DISPLAY_H_TOTAL * DISPLAY_V_TOTAL));
    r->busy_percent    = r->cs.busy_us * 100.0f / elapsed;
    r->script_runs     = s_script_runs - runs0;
    if (r->script_runs) {
        vTaskDelay(pdMS_TO_TICKS(30));          // the churn task's last run is over: its stats are settled
        r->script_errors  = s_stress_script.Errors();
        r->script_peak    = s_stress_script.HeapPeak();
        r->script_refused = s_stress_script.HeapRefused();
    }
}

// The deadline a gap's work must end by, from its fill's start
#define STRESS_GAP_DEADLINE_US  (FILL_GAP_PERIOD_US - COMP_GAP_MARGIN_US)

static void stress_log(const stress_result_t *r)
{
    const comp_stats_t &c = r->cs;
    char build[64] = "";
    if (r->builds)
        snprintf(build, sizeof(build), " | build %.0f ms (max %.0f), %lu builds",
                 r->build_us_sum / 1000.0f / r->builds, r->build_us_max / 1000.0f, (unsigned long)r->builds);
    ESP_LOGI(TAG, "stress %-26s fill max %3lu us, %lu over | keys %3lu (max %5lu us), %3lu erases, busy %3.0f%% | "
                  "%4lu B/gap, chunk max %2lu/%d us, gap end max %3lu/%d us, %lu late | frames %lu/%lu%s%s",
             r->label, (unsigned long)r->st.max_fill_us, (unsigned long)r->st.overruns,
             (unsigned long)c.keys, (unsigned long)c.max_key_us, (unsigned long)c.erases, r->busy_percent,
             (unsigned long)(c.gaps ? c.bytes / c.gaps : 0), (unsigned long)c.max_chunk_us, COMP_CHUNK_WORST_US,
             (unsigned long)c.max_gap_end_us, STRESS_GAP_DEADLINE_US, (unsigned long)c.late_gaps,
             (unsigned long)r->frames, (unsigned long)r->frames_expected, build, fill_timing(&r->st));
    if (r->script_runs)
        ESP_LOGI(TAG, "       lua: %lu runs, %lu errors, heap peak %u KB, %u allocs refused (totals so far)",
                 (unsigned long)r->script_runs, (unsigned long)r->script_errors,
                 (unsigned)(r->script_peak / 1024), r->script_refused);
#if RENDER_LAB
    const RenderLabChurnStats &rs = r->render;
    if (r->render_asked && !r->render_started)
        ESP_LOGE(TAG, "       render: NOT STARTED (no memory for the case's buffers?)");
    else if (r->render_asked)
        ESP_LOGI(TAG, "       render: %s, %lu runs, %lu failed, %.2f ms a run (render + encode + put), "
                      "px/scr/ws/out %s",
                 rs.name, (unsigned long)rs.runs, (unsigned long)rs.failed,
                 rs.runs ? rs.busy_us / 1000.0 / rs.runs : 0.0, rs.where);
#endif
}

static void run_stress(void)
{
    static stress_result_t results[32];
    int n = 0;
    const int MID = SCREEN_PAGE_W / 2;      // pages 0 (plain) and 1 (mixed) in view
    ESP_LOGI(TAG, "stress run starting (~50 s, ~70 s with the render lab): touch, USB and Wi-Fi start "
                  "after it, results at the end");

    // Every key in view changing every 20 ms
    stress_run(&results[n++], "at rest, incremental",   0, 0);
    stress_run(&results[n++], "at rest, flush",         0, COMP_FLUSH);
    stress_run(&results[n++], "spans at rest, incr",    SCREEN_PAGE_W, 0);
    stress_run(&results[n++], "mid-swipe, incremental", MID, 0);
    stress_run(&results[n++], "mid-swipe, flush",       MID, COMP_FLUSH);

    // Whole-page builds, timed until on screen
    stress_run(&results[n++], "page build, 1U",         0, COMP_FLUSH, STRESS_BUILD);
    stress_run(&results[n++], "page build, spans",      SCREEN_PAGE_W, COMP_FLUSH, STRESS_BUILD);

    // Layout changes: the black fill of uncovered areas (erases) each round
    stress_run(&results[n++], "layout changes, at rest", 0, COMP_FLUSH, STRESS_LAYOUT);
    stress_run(&results[n++], "layout changes, mid",    MID, 0, STRESS_LAYOUT);

    // Status bar redraws: on their own, then on top of the worst case above
    stress_run(&results[n++], "status bar only, mid",   MID, 0, STRESS_STATUS);
    stress_run(&results[n++], "mid, flush + status",    MID, COMP_FLUSH, STRESS_KEYS | STRESS_STATUS);
    status_set_right("");

    // Image uploads (replaces) writing PSRAM while the keys using them redraw
    stress_run(&results[n++], "image replaces, at rest", 0, 0, STRESS_REPLACE);
    stress_run(&results[n++], "image replaces, mid",    MID, 0, STRESS_REPLACE);

    // Lua writing PSRAM from core 0: alone, then with the compositor's worst case
    stress_run(&results[n++], "lua churn, at rest",     0, 0, STRESS_SCRIPT);
    stress_run(&results[n++], "lua churn, mid-swipe",   MID, 0, STRESS_SCRIPT);
    stress_run(&results[n++], "lua + keys, mid, flush", MID, COMP_FLUSH, STRESS_KEYS | STRESS_SCRIPT);
    s_stress_script.Close();                    // give the Lua heap back

#if RENDER_LAB
    // Render lab: render + encode + put on core 0 onto the image the keys in view show (1U: internal
    // buffers; 2U and 720x432: picture in PSRAM, a new heavy PSRAM writer; >256 colours: the
    // compositor's large-palette decode)
    static const stress_render_t R_1U    = { RENDER_LAB_CASE_SLIDER, DEMO_1U_ID(0), false };
    static const stress_render_t R_2U    = { RENDER_LAB_CASE_2U, DEMO_2U_ID(0), true };
    static const stress_render_t R_LARGE = { RENDER_LAB_CASE_LARGE_PALETTE, DEMO_1U_ID(0), false };
    static const stress_render_t R_FULL  = { RENDER_LAB_CASE_FULL, 0xFF0000FFu, true };     // not shown
    stress_run(&results[n++], "render 1U, at rest",     0, 0, STRESS_RENDER, &R_1U);
    stress_run(&results[n++], "render 1U, mid-swipe",   MID, 0, STRESS_RENDER, &R_1U);
    stress_run(&results[n++], "render 1U + keys, mid, fl", MID, COMP_FLUSH, STRESS_KEYS | STRESS_RENDER, &R_1U);
    stress_run(&results[n++], "render 2U PSRAM, mid",   MID, 0, STRESS_RENDER, &R_2U);
    stress_run(&results[n++], "render 2U PSRAM + keys, fl", MID, COMP_FLUSH, STRESS_KEYS | STRESS_RENDER, &R_2U);
    stress_run(&results[n++], "render >256 colours, mid", MID, 0, STRESS_RENDER, &R_LARGE);
    stress_run(&results[n++], "render 720x432 PSRAM, mid", MID, 0, STRESS_RENDER, &R_FULL);
#endif

    screen_set_scroll(0);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "---- stress: fill budget %d us; compositor: ~%d B chunks, chunk worst case "
                  "COMP_CHUNK_WORST_US %d us, margin COMP_GAP_MARGIN_US %d us (gap deadline %d us after the "
                  "fill start), guard estimate COMP_GAP_BYTES_EST %d B/fill ----",
             DISPLAY_BOUNCE_BUDGET_US, COMP_CHUNK_BYTES, COMP_CHUNK_WORST_US, COMP_GAP_MARGIN_US,
             STRESS_GAP_DEADLINE_US, COMP_GAP_BYTES_EST);
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

// Load ui.bin and start the status bar, empty: the boot sets the standalone
// texts (the host its own), and the swipe task sets the pills from the page count
static esp_err_t init_status_bar(void)
{
    s_ui = load_pack(ui_start, ui_end);
    if (!s_ui)
        return ESP_ERR_NO_MEM;
    return status_init(s_ui);
}

// NVS holds the Wi-Fi credentials, the clock settings (and the driver's PHY calibration). If it is
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
    log_capture_init();                     // first: capture every log line for /log
    ESP_LOGI(TAG, "ControlBlock starting");
    log_heap(TAG, "at start");

    init_nvs();
    clock_init();                           // the time zone, before the status bar reads the clock
    display_settings_init();                // the stored display timing: the panel starts with it
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

#if RENDER_LAB
    init_render_lab();
#endif
#if RUN_STRESS
    load_demo_images();                     // refusals are reported with the results (log_demo_load)
    screen_set_page_count(NUM_PAGES);
    run_stress();
#endif
#if RENDER_LAB
    render_lab_run();                       // after the stress run: its keys stay on page 0
#endif
    status_set_right(STATUS_TEXT_STANDALONE_RIGHT);     // standalone at boot: the clock (blank until set)

    ESP_ERROR_CHECK(swipe_init());
    log_heap(TAG, "after swipe");
    ESP_ERROR_CHECK(touch_init(on_touched));
    log_heap(TAG, "after touch");
    wifi_init();                            // loads the credentials; no radio yet
    ESP_ERROR_CHECK(host_link_init());
    log_heap(TAG, "after the host link (USB)");
    ESP_ERROR_CHECK(wifi_start());          // last: a Wi-Fi problem can never keep the screen from coming up
    log_heap(TAG, "after init");

    screen_stats_t st;
    comp_stats_t cs;
#if STATS_LOG
    uint32_t last_frames = 0, last_checks = 0, last_check_cycles = 0;
    char link_text[160];
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
        diag_sample_second(&st, &cs);       // the one reader of the screen and compositor stats (/status)
#if PCLK_CYCLE
        pclk_cycle_second(&st);             // frame rate experiment: per-clock stats, may switch the clock
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
        FormatLinkSample(host_link_stats(), is.checks - last_checks, is.check_cycles - last_check_cycles,
                         port::CyclesPerUs(), link_text, sizeof(link_text));
        last_checks = is.checks;
        last_check_cycles = is.check_cycles;

        ESP_LOGI(TAG, "%lu fps, fill max %3lu us, %lu over %d us, page %d | "
                      "comp: %lu keys (max %lu us, busy %lu us), %lu swaps, %lu held | img %lu (%lu KB)%s%s%s",
                 (unsigned long)(st.frames - last_frames),
                 (unsigned long)st.max_fill_us, (unsigned long)st.overruns, DISPLAY_BOUNCE_BUDGET_US,
                 swipe_current_page(),
                 (unsigned long)cs.keys, (unsigned long)cs.max_key_us, (unsigned long)cs.busy_us,
                 (unsigned long)cs.swaps, (unsigned long)cs.holds,
                 (unsigned long)is.count, (unsigned long)(is.used / 1024), link_text, net_counts(), late_fills(&st));
        last_frames = st.frames;
#endif
    }
}
