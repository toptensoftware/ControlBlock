// The `layout` flash partition as a LayoutFlash (layout_store.h).

#include "esp_log.h"
#include "esp_partition.h"
#include "layout_flash.h"

static const char *TAG = "layout";

static const esp_partition_t *part(void *ctx)
{
    return (const esp_partition_t *)ctx;
}

static bool cf_erase(uint32_t offset, uint32_t len, void *ctx)
{
    esp_err_t err = esp_partition_erase_range(part(ctx), offset, len);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "erase %u+%u: %s", (unsigned)offset, (unsigned)len, esp_err_to_name(err));
    return err == ESP_OK;
}

static bool cf_write(uint32_t offset, const void *data, uint32_t len, void *ctx)
{
    esp_err_t err = esp_partition_write(part(ctx), offset, data, len);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "write %u+%u: %s", (unsigned)offset, (unsigned)len, esp_err_to_name(err));
    return err == ESP_OK;
}

static bool cf_read(uint32_t offset, void *data, uint32_t len, void *ctx)
{
    esp_err_t err = esp_partition_read(part(ctx), offset, data, len);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "read %u+%u: %s", (unsigned)offset, (unsigned)len, esp_err_to_name(err));
    return err == ESP_OK;
}

void layout_flash_open(LayoutFlash *flash)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        (esp_partition_subtype_t)LAYOUT_PARTITION_SUBTYPE,
                                                        LAYOUT_PARTITION_NAME);
    if (!p) {
        ESP_LOGW(TAG, "no '%s' partition: layout upload is off (flash the partition table)", LAYOUT_PARTITION_NAME);
        flash->capacity = 0;
        return;
    }
    ESP_LOGI(TAG, "partition '%s': %u bytes at 0x%x", LAYOUT_PARTITION_NAME, (unsigned)p->size, (unsigned)p->address);
    flash->capacity = p->size;
    flash->erase    = cf_erase;
    flash->write    = cf_write;
    flash->read     = cf_read;
    flash->ctx      = (void *)p;
}
