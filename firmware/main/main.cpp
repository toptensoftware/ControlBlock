#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
#include "screen.h"
#include "swipe.h"
#include "test_keys.h"

static const char *TAG = "controlblock";

#define NUM_PAGES 3

// Boot-time stress run: the compositor drawing flat out while the display
// runs, logging fills over budget. A quick device check after changing the
// compositor or the bounce fill. 0 = boot straight in.
#define RUN_STRESS  0

// After boot, use the opaque faces (no clear before each key) or the ones
// with transparent padding.
#define USE_OPAQUE_FACES 1

extern const uint8_t test_keys_start[] asm("_binary_test_keys_bin_start");
extern const uint8_t test_keys_end[]   asm("_binary_test_keys_bin_end");

static const BLOBPACK *s_keys;          // PSRAM copy (rle565 needs 4-byte alignment)
static int             s_icon_count;
static bool            s_opaque_ok;     // the opaque faces passed comp_image_is_opaque()

// The page model, the compositor's client. Changed under s_model_lock, and
// comp_page_changed() is called before the lock is released.
static SemaphoreHandle_t s_model_lock;
static comp_key_t        s_model[NUM_PAGES][COMP_KEYS];
static bool              s_opaque;      // faces in the model are the opaque set

static bool get_page(int page, comp_key_t keys[COMP_KEYS], void *ctx)
{
    if (page < 0 || page >= NUM_PAGES)
        return false;
    xSemaphoreTake(s_model_lock, portMAX_DELAY);
    memcpy(keys, s_model[page], sizeof(s_model[page]));
    xSemaphoreGive(s_model_lock);
    return true;
}

static const void *face(int i)  { return get_blob(s_keys, TEST_KEYS_FACE(i, s_opaque)); }
static const void *icon(int i)  { return get_blob(s_keys, TEST_KEYS_FIRST_ICON + i % s_icon_count); }
static uint32_t    face_flags() { return s_opaque ? COMP_KEY_BG_OPAQUE : 0; }

// The page's own face colour (the pressed face is the last one)
static int page_face(int page)  { return page % TEST_KEYS_FACE_PRESSED; }

static esp_err_t load_test_keys(void)
{
    size_t size = test_keys_end - test_keys_start;
    void *p = heap_caps_aligned_alloc(4, size, MALLOC_CAP_SPIRAM);
    if (!p)
        return ESP_ERR_NO_MEM;
    memcpy(p, test_keys_start, size);
    s_keys = (const BLOBPACK *)p;
    s_icon_count = TEST_KEYS_ICON_COUNT(s_keys->count);

    for (uint32_t i = 0; i + 1 < s_keys->count; i++) {
        const void *img = get_blob(s_keys, i);
        if (!check_image_header(img, get_blob_size(s_keys, i)) ||
            get_image_width(img) != COMP_KEY_SIZE || get_image_height(img) != COMP_KEY_SIZE) {
            ESP_LOGE(TAG, "Bad test key image %u", (unsigned)i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Check the opaque faces really are, as an icon cache would at load
    s_opaque_ok = true;
    for (int i = 0; i < TEST_KEYS_FACES; i++) {
        int b = TEST_KEYS_FACE(i, true);
        if (!comp_image_is_opaque(get_blob(s_keys, b), get_blob_size(s_keys, b))) {
            ESP_LOGE(TAG, "Opaque face %d isn't opaque: using transparent faces", i);
            s_opaque_ok = false;
        }
    }
    return ESP_OK;
}

// Every page: its face colour (opaque or not), a different run of icons
static void reset_model(bool opaque)
{
    xSemaphoreTake(s_model_lock, portMAX_DELAY);
    s_opaque = opaque && s_opaque_ok;
    for (int p = 0; p < NUM_PAGES; p++) {
        for (int k = 0; k < COMP_KEYS; k++)
            s_model[p][k] = comp_key_t{ face(page_face(p)), icon(p * 5 + k), face_flags() };
        comp_page_changed(p, COMP_FLUSH);
    }
    xSemaphoreGive(s_model_lock);
}

// Test updates, once a second:
//  - one key on the current page toggles pressed and changes icon (incremental)
//  - every 10 s, every key on every page changes icon, flushed on alternate
//    bursts (incremental shows over a couple of frames, flush all at once)
static void test_update(uint32_t tick)
{
    int  page  = swipe_current_page();
    bool burst = tick % 10 == 0;
    bool flush = tick % 20 == 0;

    xSemaphoreTake(s_model_lock, portMAX_DELAY);
    if (burst) {
        for (int p = 0; p < NUM_PAGES; p++) {
            for (int k = 0; k < COMP_KEYS; k++)
                s_model[p][k].fg = icon(p * 5 + k + tick / 10);
            comp_page_changed(p, flush ? COMP_FLUSH : 0);
        }
    } else {
        comp_key_t *key = &s_model[page][tick % COMP_KEYS];
        const void *pressed = face(TEST_KEYS_FACE_PRESSED);
        key->bg = key->bg == pressed ? face(page_face(page)) : pressed;
        key->fg = icon(tick);
        comp_page_changed(page, 0);
    }
    xSemaphoreGive(s_model_lock);

    if (burst)
        ESP_LOGI(TAG, "test: all keys changed (%s)", flush ? "flush" : "incremental");
}

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

static void stress_run(stress_result_t *r, const char *label, bool opaque, int scroll, uint32_t flags)
{
    reset_model(opaque);
    screen_set_scroll(scroll);
    vTaskDelay(pdMS_TO_TICKS(300));         // let both pages build

    screen_get_stats(&r->st);
    comp_get_stats(&r->cs);
    uint32_t frames0 = r->st.frames;

    int first = scroll / SCREEN_PAGE_W;
    int last  = (scroll + SCREEN_PAGE_W - 1) / SCREEN_PAGE_W;
    int64_t t0 = esp_timer_get_time();
    for (int round = 1; esp_timer_get_time() - t0 < 2000000; round++) {
        xSemaphoreTake(s_model_lock, portMAX_DELAY);
        for (int p = first; p <= last && p < NUM_PAGES; p++) {
            for (int k = 0; k < COMP_KEYS; k++)
                s_model[p][k].fg = icon(p * 5 + k + round);
            comp_page_changed(p, flags);
        }
        xSemaphoreGive(s_model_lock);
        vTaskDelay(pdMS_TO_TICKS(20));
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
    ESP_LOGI(TAG, "stress %-28s fill max %4lu us, %3lu over | keys %3lu, max %4lu us, busy %3.0f%%",
             r->label, (unsigned long)r->st.max_fill_us, (unsigned long)r->st.overruns,
             (unsigned long)r->cs.keys, (unsigned long)r->cs.max_key_us, r->busy_percent);
}

static void run_stress(void)
{
    static stress_result_t results[8];
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
    screen_set_scroll(0);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "---- stress: every key in view changes every 20 ms for 2 s; fill budget %d us ----",
             DISPLAY_BOUNCE_BUDGET_US);
    for (int i = 0; i < n; i++) {
        stress_log(&results[i]);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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

    s_model_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_model_lock ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(load_test_keys());

    ESP_ERROR_CHECK(comp_init(get_page, NULL));
    reset_model(USE_OPAQUE_FACES);
    ESP_ERROR_CHECK(screen_init(NUM_PAGES));
    ESP_ERROR_CHECK(display_enable_backlight(true));

    if (RUN_STRESS) {
        run_stress();
        reset_model(USE_OPAQUE_FACES);
    }
    ESP_LOGI(TAG, "faces: %s", s_opaque ? "opaque (no clear)" : "transparent padding");

    ESP_ERROR_CHECK(swipe_init());
    ESP_ERROR_CHECK(touch_init(on_touched));

    uint32_t last_frames = 0, tick = 0;
    screen_stats_t st;
    comp_stats_t cs;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        test_update(++tick);

        screen_get_stats(&st);
        comp_get_stats(&cs);
        ESP_LOGI(TAG, "%lu fps, fill max %3lu us, %lu over %d us, page %d | "
                      "comp: %lu keys (max %lu us, busy %lu us), %lu swaps, %lu held",
                 (unsigned long)(st.frames - last_frames),
                 (unsigned long)st.max_fill_us, (unsigned long)st.overruns, DISPLAY_BOUNCE_BUDGET_US,
                 swipe_current_page(),
                 (unsigned long)cs.keys, (unsigned long)cs.max_key_us, (unsigned long)cs.busy_us,
                 (unsigned long)cs.swaps, (unsigned long)cs.holds);
        last_frames = st.frames;
    }
}
