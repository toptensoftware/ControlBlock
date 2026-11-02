#pragma once

// The device's model: the image store (in PSRAM) and the key layout, under
// one lock. It's the page compositor's client (model_get_page,
// model_is_retired). The protocol task changes it on the host's behalf;
// main.cpp's demo and stress run use it directly.

#include "esp_err.h"
#include "image_store.h"
#include "key_layout.h"

// Create the lock, store and layout. Call after comp_init() (the store's
// capacity is what PSRAM has left once the page buffers are allocated).
esp_err_t model_init(void);

// The model lock: hold it around every store and layout call
void model_lock(void);
void model_unlock(void);

ImageStore *model_store(void);
KeyLayout  *model_layout(void);

// For comp_init()
bool model_get_page(int page, comp_key_t keys[COMP_KEYS], void *ctx);
bool model_is_retired(const void *img, void *ctx);

// Touch helpers (take the lock): does the key report presses, and show or
// clear its pressed look
bool model_key_active(int page, int key);
void model_set_key_pressed(int page, int key, bool pressed);

// Upload an image from memory under `id` (the device's own images: the
// demo layout). Takes the lock.
ImageResult model_put_image(uint32_t id, const void *data, uint32_t size);
