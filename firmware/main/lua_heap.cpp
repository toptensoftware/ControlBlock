#include "lua_heap.h"

#include <string.h>

void *LuaHeapAlloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    LuaHeap *h = (LuaHeap *)ud;
    if (!ptr)
        osize = 0;                      // otherwise a type tag, not a size

    if (nsize == 0) {
        if (ptr) {
            h->used -= osize;
            h->realloc(ptr, 0, h->ctx);
        }
        return nullptr;
    }
    if (nsize > osize && h->used - osize + nsize > h->cap) {
        h->refused++;
        return nullptr;
    }
    // A growing block is never resized by the platform: the device's heap copies a moved
    // block inside its critical section, which holds off the display's interrupts (a big
    // copy shifted the picture for good). Allocate, copy and free here instead.
    void *p;
    if (ptr && nsize > osize) {
        p = h->realloc(nullptr, nsize, h->ctx);
        if (p) {
            memcpy(p, ptr, osize);
            h->realloc(ptr, 0, h->ctx);
        }
    } else {
        p = h->realloc(ptr, nsize, h->ctx);
    }
    if (!p) {
        h->refused++;
        return nullptr;
    }
    h->used = h->used - osize + nsize;
    if (h->used > h->peak)
        h->peak = h->used;
    return p;
}
