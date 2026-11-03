#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "standalone.h"
#include "standalone_task.h"
#include "usb.h"

static const char *TAG = "standalone";

// Core 0, away from the swipe task's core; below the protocol task (4)
#define OP_TASK_CORE    0
#define OP_TASK_PRIO    3
#define OP_TASK_STACK   4096
#define OP_BUSY_POLL_MS 5           // while a delay holds the queue

static Standalone        s_sa;
static SemaphoreHandle_t s_lock;    // a mutex (priority inheritance): every s_sa call is under it
static TaskHandle_t      s_op_task;

// Set by `run` under the lock, logged after it is released (the UART is slow)
static uint32_t s_started_code;

static uint32_t clock_ms(void *)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Called after the executors built into Standalone (hidKey, delay) have run
// the op: it only tells the op task that one started
static void run_op(const ConfigView &, const uint8_t *, uint32_t code, void *)
{
    s_started_code = code;
}

// A HID report for the PC. A failure (not mounted, or the last report never
// finished) is logged after the fact, as it can't be told to the caller.
static void hid_send(const uint8_t *report, void *)
{
    if (!usb_hid_send(report))
        ESP_LOGW(TAG, "HID report dropped (not mounted or busy)");
}

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
        xSemaphoreGive(s_lock);

        if (code)
            ESP_LOGD(TAG, "op %u", (unsigned)code);
        // Busy: an op started (the next may follow at once) or a delay is running
        wait = !busy ? portMAX_DELAY : code ? 0 : pdMS_TO_TICKS(OP_BUSY_POLL_MS);
    }
}

esp_err_t standalone_init(const uint8_t *blob)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;
    StandaloneHost h = {};
    h.now_ms = clock_ms;
    h.run    = run_op;
    h.hid.send = hid_send;
    s_sa.Init(h, blob, 0);          // the device's blob is relocated already
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
    xSemaphoreGive(s_lock);
}
