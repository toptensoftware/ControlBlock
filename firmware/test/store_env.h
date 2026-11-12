#pragma once

// Test environment for the image store and what sits on it (key layout,
// protocol): an allocator that poisons freed blocks and reuses their
// addresses straight away, a fake compositor sweep, and test images.

#include <stdint.h>
#include "Core.h"
#include "image_store.h"
#include "harness.h"

class StoreEnv
{
public:
    StoreEnv();
    ~StoreEnv();

    // --- Allocator ---
    bool fail_alloc = false;                // next allocations return NULL
    int  LiveBlocks() const { return m_live.GetCount(); }
    bool IsAllocated(const void *p) const;  // p inside a live block
    bool IsPoisoned(const void *p, size_t bytes) const;

    // --- Fake compositor ---
    uint32_t sweeps_requested = 0;
    uint32_t swept = 0;
    void Sweep() { swept = sweeps_requested; }     // the compositor catches up

    ImageStoreHost Host();

    // --- Images (rle565 from test_keys.bin: 128x128 faces and icons, opaque 1U/2U keys) ---
    const void *Face(int i = 0, bool opaque = false) const { return TestImages::Get().Face(i, opaque); }
    const void *Icon(int i = 0) const { return TestImages::Get().Icon(i); }
    const void *Key1U(int i = 0) const { return TestImages::Get().Key1U(i); }
    const void *Key2U(int i = 0) const { return TestImages::Get().Key2U(i); }
    uint32_t    Size(const void *img) const { return (uint32_t)TestImages::Get().Size(img); }

    // Upload `img` under `id` in one chunk (ImageStore::Put)
    ImageResult Upload(ImageStore &store, uint32_t id, const void *img);

private:
    struct Block { uint8_t *p; size_t size; };
    static void    *Alloc(size_t size, void *ctx);
    static void     Free(void *p, void *ctx);
    static size_t   LargestFree(void *ctx);
    static uint32_t RequestSweep(void *ctx);
    static uint32_t Swept(void *ctx);

    SimpleLib::List<Block> m_live;
    SimpleLib::List<Block> m_freed;         // poisoned, reused newest first
};
