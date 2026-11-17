// The stored display timing. Device layer only (hardware): see display_settings.h.

#include "esp_log.h"
#include "nvs_blob.h"
#include "screen.h"
#include "display_settings.h"

static const char *TAG = "display";

static const NvsBlob  s_nvs = { "cb_display", "timing", "the display timing" };
static DisplayConfig  s_config;

static void apply(const DisplayTimingSetting &t, const char *why)
{
    uint32_t hz = (uint32_t)t.mhz * 1000000u;
    esp_err_t err = screen_set_timing(hz, t.lines);
    uint32_t fps_x10 = DisplayTimingFpsX10(hz, DISPLAY_H_TOTAL, t.lines);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "%s: %u MHz, %u lines (~%lu.%lu fps)", why, (unsigned)t.mhz, (unsigned)t.lines,
                 (unsigned long)(fps_x10 / 10), (unsigned long)(fps_x10 % 10));
    else
        ESP_LOGE(TAG, "%s: %u MHz, %u lines refused: %s", why, (unsigned)t.mhz, (unsigned)t.lines, esp_err_to_name(err));
}

static void on_changed(const DisplayTimingSetting &now, void *)
{
    apply(now, "timing set");
}

void display_settings_init(void)
{
    s_config.Init(nvs_blob_storage(&s_nvs), DisplayTimingLimitsFor(DISPLAY_PIXEL_CLOCK_HZ / 1000000));
    s_config.SetChangedCallback(on_changed, nullptr);
    if (!s_config.IsDefault())
        apply(s_config.Get(), "stored timing");
}

DisplayConfig *display_config(void)
{
    return &s_config;
}
