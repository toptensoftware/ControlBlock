// BlobStorage over NVS, device layer. Hardware only.

#include "esp_log.h"
#include "nvs.h"
#include "nvs_blob.h"

static const char *TAG = "nvs_blob";

static bool nvs_load(void *buf, uint32_t cap, uint32_t *len, void *ctx)
{
    const NvsBlob *b = (const NvsBlob *)ctx;
    nvs_handle_t h;
    if (nvs_open(b->ns, NVS_READONLY, &h) != ESP_OK)
        return false;                       // no namespace yet: nothing stored
    size_t n = cap;
    esp_err_t err = nvs_get_blob(h, b->key, buf, &n);
    nvs_close(h);
    if (err != ESP_OK)
        return false;
    *len = (uint32_t)n;
    return true;
}

static bool nvs_save(const void *blob, uint32_t len, void *ctx)
{
    const NvsBlob *b = (const NvsBlob *)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(b->ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, b->key, blob, len);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "saving %s failed: %s", b->what, esp_err_to_name(err));
    return err == ESP_OK;
}

static bool nvs_erase(void *ctx)
{
    const NvsBlob *b = (const NvsBlob *)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(b->ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, b->key);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "erasing %s failed: %s", b->what, esp_err_to_name(err));
    return err == ESP_OK;
}

BlobStorage nvs_blob_storage(const NvsBlob *blob)
{
    BlobStorage s = {};
    s.load  = nvs_load;
    s.save  = nvs_save;
    s.erase = nvs_erase;
    s.ctx   = (void *)blob;
    return s;
}
