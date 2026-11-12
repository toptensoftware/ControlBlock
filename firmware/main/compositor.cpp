// Runs the page compositor core in a FreeRTOS task. All the logic is in
// page_compositor.cpp, which the host tests (firmware/test) exercise.

#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "compositor.h"
#include "fill_gap.h"

static const char *TAG = "comp";

#define COMP_TASK_CORE   1
#define COMP_TASK_PRIO   4       // below touch (5) and swipe (10)
#define COMP_TASK_STACK  4096

// Task notification bits
#define WAKE_WORK        0x01    // frame start, page change, reset
#define WAKE_GAP         0x02    // a bounce fill just finished

static PageCompositor    s_comp;
static TaskHandle_t      s_task;
static SemaphoreHandle_t s_reset_done, s_reset_mutex;

// Fill gaps (fill_gap.h): only given to us while a key is being drawn
static fill_gap_client_t s_gap_client = { NULL, WAKE_GAP, false, 0 };

static int64_t clock_now(void *)
{
    return esp_timer_get_time();
}

static void comp_task(void *arg)
{
    // The last gap given to us. The core draws in it only until its deadline
    // (from the fill's start), so passing it again later is harmless.
    comp_gap_t gap = {};
    bool       have_gap = false;
    uint32_t   bits = 0;
    while (1) {
        int64_t now = esp_timer_get_time();
        if (bits & WAKE_GAP) {
            gap.fill_start_us = now - (int32_t)((uint32_t)now - s_gap_client.fill_start_us);
            gap.period_us     = FILL_GAP_PERIOD_US;
            have_gap          = true;
        }

        PageCompositor::StepResult r = s_comp.Step(now, have_gap ? &gap : NULL);
        s_gap_client.wants = s_comp.WantsGaps();
        if (r == PageCompositor::StepResult::ResetDone)
            xSemaphoreGive(s_reset_done);

        // Wait for the next wake, or (after doing work) collect any that
        // arrived meanwhile without blocking. A wake is never lost.
        bits = 0;
        xTaskNotifyWait(0, ULONG_MAX, &bits, r == PageCompositor::StepResult::Wait ? portMAX_DELAY : 0);
    }
}

esp_err_t comp_init(comp_get_page_t get_page, comp_is_retired_t is_retired, void *ctx)
{
    const size_t size = SCREEN_PAGE_W * SCREEN_PAGE_H * sizeof(uint16_t);
    uint16_t *buffers[COMP_BUFFERS];
    for (int b = 0; b < COMP_BUFFERS; b++) {
        buffers[b] = (uint16_t *)heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
        if (!buffers[b]) {
            ESP_LOGE(TAG, "Out of PSRAM allocating page buffer %d", b);
            return ESP_ERR_NO_MEM;
        }
    }

    // Keys are drawn straight into the page buffers, in fill gaps
    s_comp.Init(buffers, get_page, is_retired, clock_now, ctx);

    s_reset_done  = xSemaphoreCreateBinary();
    s_reset_mutex = xSemaphoreCreateMutex();
    if (!s_reset_done || !s_reset_mutex)
        return ESP_ERR_NO_MEM;

    if (xTaskCreatePinnedToCore(comp_task, "comp", COMP_TASK_STACK, NULL,
                                COMP_TASK_PRIO, &s_task, COMP_TASK_CORE) != pdPASS)
        return ESP_FAIL;
    s_gap_client.task = s_task;
    fill_gap_register(&s_gap_client);

    ESP_LOGI(TAG, "%d page buffers, %u KB each", COMP_BUFFERS, (unsigned)(size / 1024));
    return ESP_OK;
}

// The task is pinned to the other core, which FreeRTOS yields itself, so the
// ISR functions don't need to pass a woken flag back.

IRAM_ATTR int comp_frame(int first, int count, int offset, comp_view_t out[2])
{
    if (!s_task)
        return 0;
    int n = s_comp.Frame(first, count, offset, esp_timer_get_time(), out);
    xTaskNotifyFromISR(s_task, WAKE_WORK, eSetBits, NULL);
    return n;
}

void comp_page_changed(int page, uint32_t flags)
{
    s_comp.PageChanged(page, flags);
    if (s_task)
        xTaskNotify(s_task, WAKE_WORK, eSetBits);
}

void comp_reset(void)
{
    assert(xTaskGetCurrentTaskHandle() != s_task);
    xSemaphoreTake(s_reset_mutex, portMAX_DELAY);
    s_comp.RequestReset();
    xTaskNotify(s_task, WAKE_WORK, eSetBits);
    xSemaphoreTake(s_reset_done, portMAX_DELAY);
    xSemaphoreGive(s_reset_mutex);
}

uint32_t comp_request_sweep(void)
{
    uint32_t gen = s_comp.RequestSweep();
    if (s_task)
        xTaskNotify(s_task, WAKE_WORK, eSetBits);
    return gen;
}

uint32_t comp_swept(void)
{
    return s_comp.Swept();
}

void comp_get_stats(comp_stats_t *stats)
{
    s_comp.GetStats(stats);
}
