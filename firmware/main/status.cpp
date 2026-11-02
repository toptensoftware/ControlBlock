#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "rle565.h"
#include "screen.h"
#include "status_bar.h"
#include "ui_assets.h"
#include "status.h"

static const char *TAG = "status";

// Below the compositor (4) and swipe (10): drawing only writes internal RAM,
// so it doesn't disturb the bounce fill
#define STATUS_TASK_PRIORITY  3
#define STATUS_TASK_STACK     3072

static StatusBar          s_bar;
static status_bar_style_t s_style;
static TaskHandle_t       s_task;

static void status_task(void *arg)
{
    status_bar_content_t content;
    while (1) {
        uint16_t *back = s_bar.BeginDraw(screen_status_bar_shown(), &content);
        if (back) {
            screen_note_status_drawing(true);
            status_bar_render(back, &s_style, &content);
            screen_note_status_drawing(false);
            s_bar.EndDraw();
            screen_set_status_bar(s_bar.Front());
            continue;
        }

        // Wait for a change, or, if one is waiting on the last swap, for
        // the next frame start (a tick is at least one frame)
        ulTaskNotifyTake(pdTRUE, s_bar.Pending() ? 1 : portMAX_DELAY);
    }
}

static bool ui_ok(const BLOBPACK *ui)
{
    if (ui->count < UI_ASSET_COUNT ||
        !check_font(get_blob(ui, UI_ASSET_STATUS_FONT), get_blob_size(ui, UI_ASSET_STATUS_FONT)))
        return false;
    for (int i = UI_ASSET_PAGER_CURRENT; i <= UI_ASSET_PAGER_OTHER; i++)
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

    uint16_t *buf[2];
    for (int i = 0; i < 2; i++) {
        buf[i] = (uint16_t *)heap_caps_malloc(STATUS_BAR_PIXELS * sizeof(uint16_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf[i])
            return ESP_ERR_NO_MEM;
    }
    s_bar.Init(buf[0], buf[1]);

    if (xTaskCreatePinnedToCore(status_task, "status", STATUS_TASK_STACK, NULL,
                                STATUS_TASK_PRIORITY, &s_task, 1) != pdPASS)
        return ESP_FAIL;
    return ESP_OK;
}

static void changed(bool c)
{
    if (c && s_task)
        xTaskNotifyGive(s_task);
}

void status_set_left(const char *text)    { changed(s_bar.SetLeft(text)); }
void status_set_right(const char *text)   { changed(s_bar.SetRight(text)); }
void status_set_pill_count(int count)     { changed(s_bar.SetPillCount(count)); }
void status_set_selected_pill(int index)  { changed(s_bar.SetSelectedPill(index)); }
