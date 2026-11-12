#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "compositor.h"
#include "model.h"

static const char *TAG = "model";

// PSRAM kept back from the image store for everything else
#define IMAGE_RESERVE  (512 * 1024)

static SemaphoreHandle_t s_lock;
static ImageStore        s_store;
static KeyLayout         s_layout;
static volatile bool     s_ready;

static void *psram_alloc(size_t size, void *)  { return heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM); }
static void  psram_free(void *p, void *)        { heap_caps_free(p); }
static size_t psram_largest(void *)             { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }
static uint32_t request_sweep(void *)           { return comp_request_sweep(); }
static uint32_t swept(void *)                   { return comp_swept(); }

// Called with the model lock held (by the layout)
static void page_changed(int page, uint32_t flags, void *)
{
    comp_page_changed(page, flags);
}

esp_err_t model_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t capacity = free_psram > IMAGE_RESERVE ? (uint32_t)(free_psram - IMAGE_RESERVE) : 0;

    ImageStoreHost host = {};
    host.alloc         = psram_alloc;
    host.free          = psram_free;
    host.largest_free  = psram_largest;
    host.request_sweep = request_sweep;
    host.swept         = swept;
    // Opaque key images, any size up to the grid (a stored layout blob of version 1, with its
    // transparent 128 images, is refused before this: Home boots empty until it is re-uploaded)
    s_store.Init(host, capacity, KEY_GRID_W, KEY_GRID_H, true);
    s_store.SetListener(&s_layout);
    s_layout.Init(&s_store, page_changed, nullptr);
    s_ready = true;

    ESP_LOGI(TAG, "image store: %u KB", (unsigned)(capacity / 1024));
    return ESP_OK;
}

void model_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
void model_unlock(void) { xSemaphoreGive(s_lock); }

ImageStore *model_store(void)  { return &s_store; }
KeyLayout  *model_layout(void) { return &s_layout; }

bool model_get_page(int page, comp_page_t *out, void *)
{
    if (!s_ready)
        return false;
    model_lock();
    bool ok = s_layout.GetPage(page, out);
    model_unlock();
    return ok;
}

bool model_is_retired(const void *img, void *)
{
    return s_store.PixelsRetired(img);
}

int model_page_keys(int page, TouchKey *out)
{
    model_lock();
    int count;
    const KeyState *keys = s_layout.PageKeys(page, &count);
    for (int i = 0; i < count; i++)
        out[i] = TouchKey{ keys[i].id, KeyRectPixels(keys[i].rect) };
    model_unlock();
    return count;
}

bool model_key_active(int page, uint16_t id)
{
    model_lock();
    int key_page = -1;
    bool active = s_layout.IsActive(id) && s_layout.Find(id, &key_page) && key_page == page;
    model_unlock();
    return active;
}

void model_set_key_pressed(uint16_t id, bool pressed)
{
    model_lock();
    s_layout.SetPressed(id, pressed);
    model_unlock();
}

ImageResult model_put_image(uint32_t id, const void *data, uint32_t size)
{
    model_lock();
    ImageResult r = s_store.Put(id, data, size);
    model_unlock();
    return r;
}
