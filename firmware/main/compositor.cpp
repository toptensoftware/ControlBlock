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

// Fill gaps: the ISR only reports them while a key is being copied
static volatile bool     s_wants_gaps;
static volatile uint32_t s_fill_end_us;     // low 32 bits of esp_timer_get_time()

static void comp_task(void *arg)
{
    uint32_t bits = 0;
    while (1) {
        // A gap is only worth using if the fill finished just now: the copy
        // must be done before the next fill needs the bus
        uint32_t now = (uint32_t)esp_timer_get_time();
        bool gap = (bits & WAKE_GAP) && now - s_fill_end_us < COMP_GAP_FRESH_US;

        PageCompositor::StepResult r = s_comp.Step(esp_timer_get_time(), gap);
        s_wants_gaps = s_comp.WantsGaps();
        if (r == PageCompositor::StepResult::ResetDone)
            xSemaphoreGive(s_reset_done);

        // Wait for the next wake, or (after doing work) collect any that
        // arrived meanwhile without blocking. A wake is never lost.
        bits = 0;
        xTaskNotifyWait(0, ULONG_MAX, &bits, r == PageCompositor::StepResult::Wait ? portMAX_DELAY : 0);
    }
}

esp_err_t comp_init(comp_get_page_t get_page, void *ctx)
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

    // Keys are rendered here, off the PSRAM bus
    const size_t cell_size = COMP_KEY_SIZE * COMP_KEY_SIZE * sizeof(uint16_t);
    uint16_t *cell = (uint16_t *)heap_caps_aligned_alloc(4, cell_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!cell) {
        ESP_LOGE(TAG, "Out of internal RAM allocating the render cell");
        return ESP_ERR_NO_MEM;
    }
    s_comp.Init(buffers, cell, get_page, ctx);

    s_reset_done  = xSemaphoreCreateBinary();
    s_reset_mutex = xSemaphoreCreateMutex();
    if (!s_reset_done || !s_reset_mutex)
        return ESP_ERR_NO_MEM;

    if (xTaskCreatePinnedToCore(comp_task, "comp", COMP_TASK_STACK, NULL,
                                COMP_TASK_PRIO, &s_task, COMP_TASK_CORE) != pdPASS)
        return ESP_FAIL;

    ESP_LOGI(TAG, "%d page buffers, %u KB each; %u KB render cell in internal RAM",
             COMP_BUFFERS, (unsigned)(size / 1024), (unsigned)(cell_size / 1024));
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

IRAM_ATTR void comp_fill_done(void)
{
    if (!s_wants_gaps || !s_task)
        return;
    s_fill_end_us = (uint32_t)esp_timer_get_time();
    xTaskNotifyFromISR(s_task, WAKE_GAP, eSetBits, NULL);
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

void comp_get_stats(comp_stats_t *stats)
{
    s_comp.GetStats(stats);
}
