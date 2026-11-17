// Render lab, device layer (render_lab_run.h). Device only: no host test.
// The cases, the runner and the result line are render_lab_cases.cpp's
// (host tested); this file only finds buffers, times, puts and logs.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "compositor.h"
#include "heap_log.h"
#include "model.h"
#include "render_lab_cases.h"
#include "render_lab_run.h"
#include "screen.h"
#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "render_lab";

// Lab images' IDs: a reserved high range, clear of blob offsets (< 4 MB), host CRCs aside
#define LAB_IMAGE_ID(i)         (0xFF000000u + (uint32_t)(i))
#define LAB_KEY_ID(i)           (0xF000 + (i))

#define LAB_BAND_ROWS           16          // the banded render's band (the status bar's way)
#define LAB_TARGET_NS           300000000ull// time each case for about this long
#define LAB_MAX_REPS            50
#define LAB_INTERNAL_MAX        (48 * 1024) // a buffer bigger than this always goes to PSRAM
#define LAB_INTERNAL_RESERVE    (24 * 1024) // and internal RAM keeps at least this in its largest block
#define LAB_TASK_STACK          8192
#define LAB_TASK_PRIO           3           // as the op task
#define LAB_SWAP_TIMEOUT_US     1000000

static RenderLabAssets s_assets;

static uint64_t clock_ns(void)
{
    return (uint64_t)esp_timer_get_time() * 1000;
}

// ---------------------------------------------------------------------------
// Buffers: internal RAM where allowed and it fits, else PSRAM

enum { BUF_PX, BUF_SCRATCH, BUF_WS, BUF_OUT, BUF_COUNT };

struct LabBuffers
{
    RenderLabBuffers b;
    char where[BUF_COUNT];          // 'I' internal, 'P' PSRAM, '-' none
};

static void *lab_alloc(size_t size, bool allow_internal, char *where)
{
    *where = '-';
    if (!size)
        return nullptr;
    if (allow_internal && size <= LAB_INTERNAL_MAX &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= size + LAB_INTERNAL_RESERVE) {
        void *p = heap_caps_aligned_alloc(4, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) {
            *where = 'I';
            return p;
        }
    }
    void *p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
    if (p)
        *where = 'P';
    return p;
}

static void lab_free(LabBuffers *lb)
{
    heap_caps_free(lb->b.px);
    heap_caps_free(lb->b.scratch);
    heap_caps_free(lb->b.ws);
    heap_caps_free(lb->b.out);
    memset(lb, 0, sizeof(*lb));
}

static void where_text(const LabBuffers *lb, char *out, size_t size)
{
    snprintf(out, size, "%c/%c/%c/%c", lb->where[BUF_PX], lb->where[BUF_SCRATCH], lb->where[BUF_WS],
             lb->where[BUF_OUT]);
}

// Buffers for case `c` (`source`: a recolour's source image): the picture and path scratch, a
// probe run with only the colour count bitmap for the encoder's needs, then the workspace and
// output. `px_internal`: the picture may go to internal RAM. False if memory ran out.
static bool lab_buffers(const RenderLabCase *c, const void *source, bool px_internal, LabBuffers *lb)
{
    memset(lb, 0, sizeof(*lb));
    RenderLabBuffers &b = lb->b;
    b.band_rows = LAB_BAND_ROWS;

    if (c->kind == RENDER_CASE_RECOLOR) {
        b.out_size = source ? get_image_packed_size(source) : 0;
        b.out = lab_alloc(b.out_size, true, &lb->where[BUF_OUT]);
        lb->where[BUF_PX] = lb->where[BUF_SCRATCH] = lb->where[BUF_WS] = '-';
        return b.out != nullptr;
    }

    RenderLabResult sizes;
    RenderLabCaseSizes(c, LAB_BAND_ROWS, &sizes);
    b.px_bytes = sizes.px_whole;
    b.px = (uint16_t *)lab_alloc(b.px_bytes, px_internal, &lb->where[BUF_PX]);
    b.scratch_size = sizes.scratch_whole;
    b.scratch = lab_alloc(b.scratch_size, true, &lb->where[BUF_SCRATCH]);
    b.ws_size = RENDER_LAB_COUNT_BYTES;
    b.ws = lab_alloc(b.ws_size, true, &lb->where[BUF_WS]);
    if (!b.px || (b.scratch_size && !b.scratch) || !b.ws)
        return false;

    // The probe: refused for want of an output, but tells the colours, workspace and bound
    RenderLabResult probe;
    RenderLabCaseRun(c, &b, nullptr, nullptr, 1, &probe);
    if (!probe.colors)
        return false;
    if (probe.workspace > b.ws_size) {
        heap_caps_free(b.ws);
        b.ws_size = probe.workspace;
        b.ws = lab_alloc(b.ws_size, true, &lb->where[BUF_WS]);
    }
    b.out_size = probe.bound;
    b.out = lab_alloc(b.out_size, true, &lb->where[BUF_OUT]);
    return b.ws && b.out;
}

// ---------------------------------------------------------------------------
// Putting results into the model

// Put an encoded image under `id` (replacing it if there): the time spent waiting for the model
// lock, and the time it was held (allocate, copy into PSRAM, check_image)
static ImageResult lab_put(uint32_t id, const void *data, size_t size, int64_t *wait_us, int64_t *held_us)
{
    int64_t t0 = esp_timer_get_time();
    model_lock();
    int64_t t1 = esp_timer_get_time();
    ImageResult r = model_store()->Put(id, data, (uint32_t)size);
    int64_t t2 = esp_timer_get_time();
    model_unlock();
    *wait_us = t1 - t0;
    *held_us = t2 - t1;
    return r;
}

static void lab_collect(void)
{
    model_lock();
    model_store()->Collect();
    model_unlock();
}

// Forget swaps so far (comp_get_stats resets the counters)
static void swaps_clear(void)
{
    comp_stats_t s;
    comp_get_stats(&s);
}

// Microseconds from `t0` until the bounce fill takes a buffer (the change is on screen), -1 if not
// within LAB_SWAP_TIMEOUT_US. Polls every 100 us (a tick, 10 ms, is too coarse for a ~3 ms draw).
static int64_t wait_swap(int64_t t0)
{
    for (;;) {
        comp_stats_t s;
        comp_get_stats(&s);
        int64_t us = esp_timer_get_time() - t0;
        if (s.swaps)
            return us;
        if (us > LAB_SWAP_TIMEOUT_US)
            return -1;
        esp_rom_delay_us(100);
    }
}

// ---------------------------------------------------------------------------
// Boot run

#define LAB_KEYS_MAX    KEY_GRID_SLOTS

static KeyDef        s_defs[LAB_KEYS_MAX];
static int           s_def_count;
static int           s_cursor;
static void         *s_outs[RENDER_LAB_CASE_COUNT];     // each case's encoded image (recolour sources)

// The keys so far on page 0, flushed; microseconds until on screen (-1: refused or timed out)
static int64_t show_keys(void)
{
    vTaskDelay(pdMS_TO_TICKS(50));          // let earlier changes settle
    swaps_clear();
    int64_t t0 = esp_timer_get_time();
    model_lock();
    KeyLayoutResult r = model_layout()->SetKeys(0, 1, s_defs, s_def_count, COMP_FLUSH);
    model_unlock();
    if (r != KeyLayoutResult::Ok) {
        ESP_LOGE(TAG, "  page 0 keys refused (%d)", (int)r);
        return -1;
    }
    return wait_swap(t0);
}

// Case `i` as a key on page 0 (if it fits the grid), then a state change timed end to end: a
// fresh render + encode, the put (a replace), until on screen
static void show_case(int i, const RenderLabCase *c, LabBuffers *lb, const void *source)
{
    KeyRect rect;
    if (s_def_count == LAB_KEYS_MAX || !RenderLabPlaceKey(c->w, c->h, &s_cursor, &rect)) {
        ESP_LOGI(TAG, "  not shown (no room on page 0, or not whole cells)");
        return;
    }
    KeyDef d = {};
    d.id = (uint16_t)LAB_KEY_ID(i);
    d.page = 0;
    d.rect = rect;
    d.normal = LAB_IMAGE_ID(i);
    s_defs[s_def_count++] = d;
    int64_t build_us = show_keys();

    vTaskDelay(pdMS_TO_TICKS(50));
    swaps_clear();
    RenderLabBuffers whole = lb->b;
    whole.band_rows = 0;            // a state change renders once
    int64_t t0 = esp_timer_get_time();
    RenderLabResult r;
    bool ok = RenderLabCaseRun(c, &whole, source, nullptr, 1, &r);
    int64_t t1 = esp_timer_get_time();
    int64_t wait_us = 0, held_us = 0;
    ImageResult pr = ok ? lab_put(LAB_IMAGE_ID(i), lb->b.out, r.size, &wait_us, &held_us) : ImageResult::BadArg;
    int64_t t2 = esp_timer_get_time();
    int64_t shown = pr == ImageResult::Ok ? wait_swap(t0) : -1;
    lab_collect();
    if (shown < 0) {
        ESP_LOGE(TAG, "  state change failed (run %d, put %d, or not on screen within 1 s)", ok, (int)pr);
        return;
    }
    ESP_LOGI(TAG, "  key %u at (%u,%u)q: page 0 with %d keys on screen in %.1f ms | state change to screen "
                  "%.2f ms = render+encode %.2f + put %.2f + draw until shown %.2f ms",
             (unsigned)d.id, rect.x, rect.y, s_def_count, build_us / 1000.0, shown / 1000.0,
             (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (shown - (t2 - t0)) / 1000.0);
}

static void run_case(int i)
{
    static RenderLabCase c;         // ~1.2 KB: off the task stack
    if (!RenderLabCaseGet(i, &s_assets, &c)) {
        ESP_LOGE(TAG, "case %d: missing assets", i);
        return;
    }
    const void *source = c.kind == RENDER_CASE_RECOLOR ? s_outs[c.source] : nullptr;
    LabBuffers lb;
    char where[16];
    if (!lab_buffers(&c, source, true, &lb)) {
        where_text(&lb, where, sizeof(where));
        ESP_LOGE(TAG, "%s: no memory for its buffers (px/scr/ws/out %s)", c.name, where);
        lab_free(&lb);
        return;
    }
    where_text(&lb, where, sizeof(where));

    // A cold run (its times kept), then enough repeats for ~LAB_TARGET_NS
    RenderLabResult cold, r;
    uint64_t t0 = clock_ns();
    bool ok = RenderLabCaseRun(&c, &lb.b, source, clock_ns, 1, &cold);
    int reps = RenderLabRepsFor(clock_ns() - t0, LAB_TARGET_NS, LAB_MAX_REPS);
    ok = ok && RenderLabCaseRun(&c, &lb.b, source, clock_ns, reps, &r);
    if (!ok) {
        ESP_LOGE(TAG, "%s: run failed (colours %u, workspace %u, bound %u)", c.name, (unsigned)cold.colors,
                 (unsigned)cold.workspace, (unsigned)cold.bound);
        lab_free(&lb);
        return;
    }

    // The first put of the result (a new image) under the lab's ID
    int64_t wait_us, held_us;
    ImageResult pr = lab_put(LAB_IMAGE_ID(i), lb.b.out, r.size, &wait_us, &held_us);

    char line[256];
    RenderLabFormatResult(&c, &r, line, sizeof(line));
    ESP_LOGI(TAG, "%s | %d reps, cold %.1f us | put %lld us (lock wait %lld us)%s | px/scr/ws/out %s",
             line, reps, (cold.render_ns + cold.encode_ns) / 1000.0, (long long)held_us, (long long)wait_us,
             pr == ImageResult::Ok ? "" : " REFUSED", where);

    s_outs[i] = heap_caps_malloc(r.size, MALLOC_CAP_SPIRAM);
    if (s_outs[i])
        memcpy(s_outs[i], lb.b.out, r.size);

    if (pr == ImageResult::Ok)
        show_case(i, &c, &lb, source);
    lab_free(&lb);
}

static SemaphoreHandle_t s_done;

static void lab_task(void *)
{
    screen_stats_t st;
    screen_get_stats(&st);          // resets the maxima: the numbers below are the lab's
    s_def_count = 0;
    s_cursor = 0;
    for (int i = 0; i < RenderLabCaseCount(); i++) {
        run_case(i);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    screen_get_stats(&st);
    ESP_LOGI(TAG, "fills during the lab: fill max %lu us, %lu over | start late max %lu us, end max %lu us "
                  "(budget %d us), %lu missed, %lu seq breaks, %lu buffer mismatches",
             (unsigned long)st.max_fill_us, (unsigned long)st.overruns, (unsigned long)st.max_late_us,
             (unsigned long)st.max_end_us, DISPLAY_BOUNCE_BUDGET_US, (unsigned long)st.missed,
             (unsigned long)st.seq_breaks, (unsigned long)st.buffer_mismatches);
    for (void *&p : s_outs) {
        heap_caps_free(p);
        p = nullptr;
    }
    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "lab task stack: %u of %d bytes never used", (unsigned)stack_free, LAB_TASK_STACK);
    xSemaphoreGive(s_done);
    vTaskDelete(nullptr);
}

void render_lab_init(const BLOBPACK *lab, const void *vectors, size_t vectors_size)
{
    s_assets.lab = lab;
    s_assets.vectors = vectors;
    s_assets.vectors_size = vectors_size;
}

void render_lab_run(void)
{
#if CONFIG_COMPILER_OPTIMIZATION_PERF
    const char *build = "release (-O2)";
#elif CONFIG_COMPILER_OPTIMIZATION_DEBUG
    const char *build = "debug (-Og)";
#else
    const char *build = "other optimisation";
#endif
    ESP_LOGI(TAG, "==== render lab: %s build, fw %s, CPU %d MHz, pixel clock %d MHz (fill budget %d us); "
                  "%d cases on a core 0 task, ~%d ms each, banded = %d-row bands; times are means, "
                  "cold = the first run's render + encode ====",
             build, esp_app_get_description()->version, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             DISPLAY_PIXEL_CLOCK_HZ / 1000000, DISPLAY_BOUNCE_BUDGET_US, RenderLabCaseCount(),
             (int)(LAB_TARGET_NS / 1000000), LAB_BAND_ROWS);
    ESP_LOGI(TAG, "assets: render_lab.bin %s, path_vectors.bin %u B; buffers px/scr/ws/out: I internal "
                  "(<= %d KB, keeping %d KB in the largest block), P PSRAM, - none",
             s_assets.lab ? "loaded" : "MISSING", (unsigned)s_assets.vectors_size, LAB_INTERNAL_MAX / 1024,
             LAB_INTERNAL_RESERVE / 1024);
    log_heap(TAG, "before the lab");
    ImageStoreStats is0, is1;
    model_lock();
    model_store()->GetStats(&is0);
    model_unlock();

    s_done = xSemaphoreCreateBinary();
    if (!s_done || xTaskCreatePinnedToCore(lab_task, "render_lab", LAB_TASK_STACK, nullptr, LAB_TASK_PRIO,
                                           nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the lab task");
        return;
    }
    xSemaphoreTake(s_done, portMAX_DELAY);
    vSemaphoreDelete(s_done);
    s_done = nullptr;

    log_heap(TAG, "after the lab");
    model_lock();
    model_store()->GetStats(&is1);
    model_unlock();
    ESP_LOGI(TAG, "image store: %lu images, %lu KB used (was %lu, %lu KB); page 0 shows %d lab keys "
                  "(stored layout's Home replaced until a host takes control or the next boot) ====",
             (unsigned long)is1.count, (unsigned long)(is1.used / 1024), (unsigned long)is0.count,
             (unsigned long)(is0.used / 1024), s_def_count);
}

// ---------------------------------------------------------------------------
// Stress run's render load

#define CHURN_BUSY_US   18000           // as the stress run's Lua churn
#define CHURN_STACK     8192

static RenderLabCase      s_churn_case;
static LabBuffers         s_churn_buf;
static uint32_t           s_churn_id;
static volatile bool      s_churn_on, s_churn_idle = true;
static volatile uint32_t  s_churn_runs, s_churn_failed;
static volatile uint64_t  s_churn_busy_us;
static TaskHandle_t       s_churn_task;

static void churn_task(void *)
{
    for (;;) {
        s_churn_idle = true;
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_churn_idle = false;
        while (s_churn_on) {
            int64_t t0 = esp_timer_get_time();
            while (s_churn_on && esp_timer_get_time() - t0 < CHURN_BUSY_US) {
                int64_t r0 = esp_timer_get_time();
                RenderLabResult r;
                int64_t wait_us, held_us;
                bool ok = RenderLabCaseRun(&s_churn_case, &s_churn_buf.b, nullptr, nullptr, 1, &r) &&
                          lab_put(s_churn_id, s_churn_buf.b.out, r.size, &wait_us, &held_us) == ImageResult::Ok;
                lab_collect();
                s_churn_runs = s_churn_runs + 1;        // only this task writes them
                if (!ok)
                    s_churn_failed = s_churn_failed + 1;
                s_churn_busy_us = s_churn_busy_us + (uint64_t)(esp_timer_get_time() - r0);
            }
            vTaskDelay(1);
        }
    }
}

bool render_lab_churn_start(int index, uint32_t id, bool psram)
{
    if (!s_churn_idle || !RenderLabCaseGet(index, &s_assets, &s_churn_case) ||
        s_churn_case.kind != RENDER_CASE_RENDER)
        return false;
    if (!lab_buffers(&s_churn_case, nullptr, !psram, &s_churn_buf)) {
        lab_free(&s_churn_buf);
        return false;
    }
    s_churn_buf.b.band_rows = 0;        // one render a run
    if (!s_churn_task &&
        xTaskCreatePinnedToCore(churn_task, "render_churn", CHURN_STACK, nullptr, LAB_TASK_PRIO,
                                &s_churn_task, 0) != pdPASS) {
        lab_free(&s_churn_buf);
        return false;
    }
    s_churn_id = id;
    s_churn_runs = 0;
    s_churn_failed = 0;
    s_churn_busy_us = 0;
    s_churn_on = true;
    xTaskNotifyGive(s_churn_task);
    return true;
}

void render_lab_churn_stop(RenderLabChurnStats *out)
{
    s_churn_on = false;
    if (s_churn_task)
        while (!s_churn_idle || ulTaskNotifyValueClear(s_churn_task, 0))     // parked, nothing pending
            vTaskDelay(1);
    memset(out, 0, sizeof(*out));
    out->name = s_churn_case.name;
    out->runs = s_churn_runs;
    out->failed = s_churn_failed;
    out->busy_us = s_churn_busy_us;
    where_text(&s_churn_buf, out->where, sizeof(out->where));
    lab_free(&s_churn_buf);
}
