// Boot-time config load. Device layer only: see config_boot.h.
// NOT YET RUN ON THE HARDWARE.

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "config_apply.h"
#include "config_blob.h"
#include "config_boot.h"
#include "config_flash.h"
#include "config_store.h"
#include "model.h"

static const char *TAG = "config";

static uint8_t      *s_blob;
static uint32_t      s_size;
static ConfigSummary s_summary;

void config_boot_load(void)
{
    // Reads only: no `busy` callback needed (it's for uploads)
    ConfigFlash flash = {};
    config_flash_open(&flash);
    ConfigStore store;
    store.Init(flash);

    ConfigInfo info;
    if (!store.Stored(&info)) {
        ESP_LOGI(TAG, "no stored config: empty Home");
        return;
    }

    // 16-byte aligned so every image in it (4-aligned in the blob) is
    uint8_t *blob = (uint8_t *)heap_caps_aligned_alloc(16, info.size, MALLOC_CAP_SPIRAM);
    if (!blob) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte config: empty Home", (unsigned)info.size);
        return;
    }
    if (!store.Read(info, blob)) {
        ESP_LOGE(TAG, "config read or CRC failed: empty Home");
        heap_caps_free(blob);
        return;
    }
    ConfigError err = ConfigValidate(blob, info.size, &s_summary);
    if (err != ConfigError::Ok) {
        ESP_LOGE(TAG, "config rejected (%s): empty Home", ConfigErrorName(err));
        heap_caps_free(blob);
        return;
    }
    ConfigRelocate(blob, info.size, (uint32_t)(uintptr_t)blob);

    s_blob = blob;
    s_size = info.size;
    ESP_LOGI(TAG, "config loaded: %u bytes, %u activities, %u devices",
             (unsigned)s_size, (unsigned)s_summary.activity_count, (unsigned)s_summary.device_count);
}

void config_boot_apply(void)
{
    if (!s_blob)
        return;
    model_lock();
    bool ok = ConfigApplyHome(s_summary, s_blob, model_store(), model_layout());
    model_unlock();
    if (!ok)
        ESP_LOGE(TAG, "Home could not be shown (an image was refused): empty Home");
}

const uint8_t *config_blob(void)      { return s_blob; }
size_t          config_blob_size(void) { return s_size; }
