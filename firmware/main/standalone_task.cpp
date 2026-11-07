#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
// Locks: every call into s_sa is under s_lock (a mutex). Applying an activity (the
// ActivityApplier, called by s_sa inside Key()/Poll()) then takes the model lock
// for the layout swap, so the order is always: s_lock, then the model lock.
// Nothing takes them the other way round: the swipe task takes the model lock only
// inside model_key_active / model_set_key_pressed (taken and released within the
// call, never held across the router's key callback, which is where standalone_key
// is entered), and the protocol task's ResetModel releases the model lock before
// it calls standalone_set_hosted. The compositor takes only the model lock.
// The swipe/status calls the applier makes are queue sends and a spinlock-guarded
// setter, made outside the model lock.

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "activity_host.h"
#include "layout_blob.h"
#include "model.h"
#include "net_worker.h"
#include "standalone.h"
#include "standalone_task.h"
#include "status.h"
#include "swipe.h"
#include "usb.h"

static const char *TAG = "standalone";

// Core 0, away from the swipe task's core; below the protocol task (4)
#define OP_TASK_CORE    0
#define OP_TASK_PRIO    3
#define OP_TASK_STACK   12288       // internal RAM: Lua's C recursion (script_host.h) and the executors
#define OP_BUSY_POLL_MS 5           // while a delay holds the queue
// At least one tick: at 100 Hz, pdMS_TO_TICKS(5) is 0, and a zero wait spins the op
// task through a whole delay (starving IDLE0 until the task watchdog fires)
#define OP_BUSY_POLL_TICKS (pdMS_TO_TICKS(OP_BUSY_POLL_MS) > 0 ? pdMS_TO_TICKS(OP_BUSY_POLL_MS) : 1)

static Standalone        s_sa;
static SemaphoreHandle_t s_lock;    // a mutex (priority inheritance): every s_sa call is under it
static TaskHandle_t      s_op_task;

// Set by `run` under the lock, logged after it is released (the UART is slow)
static uint32_t s_started_code;
static uint32_t s_logged_errors;    // ops that ended in an error, as of the last log (op task only)

// For the busy/idle and activity log lines (op task only)
static const layoutdata::layoutRoot *s_root;
static int      s_logged_activity = -1;
static bool     s_was_busy;
static uint32_t s_busy_since_ms;

static const char *activity_name(int index)
{
    return index < 0 || !s_root ? "none" : s_root->activities[index].name;
}

static uint32_t clock_ms(void *)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Called after the executors built into Standalone (hidKey, delay) have run
// the op: it only tells the op task that one started
static void run_op(const layoutdata::op *, uint32_t code, void *)
{
    s_started_code = code;
}

// The Lua heap is PSRAM (capped by the ScriptHost); a script's output goes to the log.
// Both are called under the lock, from the op task.
static void *script_realloc(void *ptr, size_t size, void *)
{
    if (size == 0) {
        heap_caps_free(ptr);
        return nullptr;
    }
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void script_log(const char *line, void *)
{
    ESP_LOGI("lua", "%s", line);
}

ScriptPlatform standalone_script_platform(void)
{
    ScriptPlatform p = {};
    p.realloc = script_realloc;
    p.log     = script_log;
    return p;
}

// Script stats for the "op queue empty" line (op task only)
static uint32_t s_logged_runs;

// A HID report for the PC. A failure (not mounted, or the last report never
// finished) is logged after the fact, as it can't be told to the caller.
static void hid_send(const uint8_t *report, void *)
{
    if (!usb_hid_send(report))
        ESP_LOGW(TAG, "HID report dropped (not mounted or busy)");
}

// The page swap of a switchActivity (activity_host.cpp does the sequence)
static ActivityApplier s_activity;
static void as_lock(void *)                              { model_lock(); }
static void as_unlock(void *)                            { model_unlock(); }
static void as_set_page_count(int count, void *)         { swipe_set_page_count(count); }
static void as_go_to_page(int page, bool animate, void *) { swipe_go_to_page(page, animate); }
static void as_set_status_left(const char *text, void *) { status_set_left(text); }

static void wake_op_task(void)
{
    if (s_op_task)
        xTaskNotifyGive(s_op_task);
}

static void op_task(void *)
{
    TickType_t wait = portMAX_DELAY;
    while (1) {
        ulTaskNotifyTake(pdTRUE, wait);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_started_code = 0;
        bool busy = s_sa.Poll();
        uint32_t code = s_started_code;
        uint32_t errors = s_sa.Errors();
        const char *why = s_sa.LastError();     // a string literal in the executors: safe after the unlock
        int activity = s_sa.Activity();
        const ScriptHost &scripts = s_sa.Scripts();
        uint32_t runs = scripts.Runs(), script_errors = scripts.Errors();
        size_t heap_peak = scripts.HeapPeak();
        unsigned heap_refused = scripts.HeapRefused();
        xSemaphoreGive(s_lock);

        uint32_t now = clock_ms(nullptr);
        if (busy && !s_was_busy)
            s_busy_since_ms = now;
        if (activity != s_logged_activity) {
            // The switch has been committed and its hook and device ops queued (or a takeover dropped it)
            ESP_LOGI(TAG, "activity: %s -> %s", activity_name(s_logged_activity), activity_name(activity));
            s_logged_activity = activity;
        }
        if (code)
            ESP_LOGD(TAG, "op %u", (unsigned)code);
        if (errors > s_logged_errors) {
            // A network op while Wi-Fi is down, a full net queue, ...: the op was dropped
            ESP_LOGE(TAG, "op failed: %s (%lu so far)", why ? why : "?", (unsigned long)errors);
        }
        s_logged_errors = errors;               // (takeover and release start the count again from 0)
        if (!busy && s_was_busy) {
            ESP_LOGI(TAG, "op queue empty (busy %lu ms)", (unsigned long)(now - s_busy_since_ms));
            if (runs != s_logged_runs) {
                // The Lua checks: heap use and the op task's stack headroom (the high-water mark is
                // in bytes on ESP-IDF, the least ever free)
                ESP_LOGI(TAG, "scripts: %lu runs, %lu errors, heap peak %u KB, %u allocs refused, op stack %u bytes free",
                         (unsigned long)runs, (unsigned long)script_errors, (unsigned)(heap_peak / 1024),
                         heap_refused, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
            }
        }
        s_logged_runs = runs;                   // (a re-arm closes the host and starts it again from 0)
        s_was_busy = busy;
        // Busy: an op started (the next may follow at once) or a delay is running
        wait = !busy ? portMAX_DELAY : code ? 0 : OP_BUSY_POLL_TICKS;
    }
}

esp_err_t standalone_init(const uint8_t *blob)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;
    if (net_worker_init() != ESP_OK)            // the network ops' worker, before anything can fire
        return ESP_FAIL;
    StandaloneHost h = {};
    h.now_ms = clock_ms;
    h.run    = run_op;
    h.net    = net_worker_host();
    h.hid.send = hid_send;
    h.script = standalone_script_platform();
    const layoutdata::layoutRoot *root = blob ? LayoutRoot(blob) : nullptr;    // the device's blob is relocated already
    s_root = root;
    if (root) {
        ActivityScreen screen = {};
        screen.lock            = as_lock;
        screen.unlock          = as_unlock;
        screen.set_page_count  = as_set_page_count;
        screen.go_to_page      = as_go_to_page;
        screen.set_status_left = as_set_status_left;
        s_activity.Init(root, blob, model_store(), model_layout(), screen);
        h.activity = s_activity.Host();
    }
    s_sa.Init(h, root);
    if (xTaskCreatePinnedToCore(op_task, "ops", OP_TASK_STACK, nullptr, OP_TASK_PRIO, &s_op_task, OP_TASK_CORE) != pdPASS)
        return ESP_FAIL;
    return ESP_OK;
}

void standalone_key(int page, int key, int action, uint32_t time_ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sa.Key(page, key, action, time_ms);
    bool work = s_sa.Queued() > 0;
    xSemaphoreGive(s_lock);
    if (work)
        wake_op_task();
}

void standalone_tick(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sa.Tick();
    bool work = s_sa.Queued() > 0;
    xSemaphoreGive(s_lock);
    if (work)
        wake_op_task();
}

void standalone_set_hosted(bool hosted)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sa.SetHosted(hosted);
    s_activity.Reset();             // the protocol reset the store before this: no activity's pages are on the layout
    xSemaphoreGive(s_lock);
    wake_op_task();                 // so it logs the dropped selection now, not at the next key

}
