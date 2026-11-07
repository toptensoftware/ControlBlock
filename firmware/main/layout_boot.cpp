// Boot-time layout load. Device layer only: see layout_boot.h.
// NOT YET RUN ON THE HARDWARE.

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "layout_apply.h"
#include "layout_blob.h"
#include "layout_boot.h"
#include "layout_flash.h"
#include "layout_store.h"
#include "model.h"

static const char *TAG = "layout";

static uint8_t      *s_blob;
static uint32_t      s_size;
static LayoutSummary s_summary;

void layout_boot_load(void)
{
    // Reads only: no `busy` callback needed (it's for uploads)
    LayoutFlash flash = {};
    layout_flash_open(&flash);
    LayoutStore store;
    store.Init(flash);

    LayoutInfo info;
    if (!store.Stored(&info)) {
        ESP_LOGI(TAG, "no stored layout: empty Home");
        return;
    }

    // 16-byte aligned so every image in it (4-aligned in the blob) is
    uint8_t *blob = (uint8_t *)heap_caps_aligned_alloc(16, info.size, MALLOC_CAP_SPIRAM);
    if (!blob) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte layout: empty Home", (unsigned)info.size);
        return;
    }
    if (!store.Read(info, blob)) {
        ESP_LOGE(TAG, "layout read or CRC failed: empty Home");
        heap_caps_free(blob);
        return;
    }
    LayoutError err = LayoutValidate(blob, info.size, &s_summary);
    if (err != LayoutError::Ok) {
        ESP_LOGE(TAG, "layout rejected (%s): empty Home", LayoutErrorName(err));
        heap_caps_free(blob);
        return;
    }
    LayoutRelocate(blob, info.size, (uint32_t)(uintptr_t)blob);

    s_blob = blob;
    s_size = info.size;
    ESP_LOGI(TAG, "layout loaded: %u bytes, %u activities, %u devices",
             (unsigned)s_size, (unsigned)s_summary.activity_count, (unsigned)s_summary.device_count);
}

void layout_boot_apply(void)
{
    if (!s_blob)
        return;
    model_lock();
    bool ok = LayoutApplyHome(s_summary, s_blob, model_store(), model_layout());
    model_unlock();
    if (!ok)
        ESP_LOGE(TAG, "Home could not be shown (an image was refused): empty Home");
}

const uint8_t *layout_blob(void)      { return s_blob; }
size_t          layout_blob_size(void) { return s_size; }

const LayoutSummary *layout_summary(void) { return s_blob ? &s_summary : nullptr; }
