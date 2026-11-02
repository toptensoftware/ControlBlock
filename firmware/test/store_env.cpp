#include <stdlib.h>
#include <string.h>
#include "store_env.h"

using namespace SimpleLib;

#define POISON 0xDD

StoreEnv::StoreEnv()
{
}

StoreEnv::~StoreEnv()
{
    for (int i = 0; i < m_live.GetCount(); i++)
        free(m_live[i].p);
    for (int i = 0; i < m_freed.GetCount(); i++)
        free(m_freed[i].p);
}

ImageStoreHost StoreEnv::Host()
{
    ImageStoreHost h = {};
    h.alloc         = Alloc;
    h.free          = Free;
    h.largest_free  = LargestFree;
    h.request_sweep = RequestSweep;
    h.swept         = Swept;
    h.ctx           = this;
    return h;
}

void *StoreEnv::Alloc(size_t size, void *ctx)
{
    StoreEnv *e = (StoreEnv *)ctx;
    if (e->fail_alloc)
        return nullptr;

    // Reuse a freed block if one is big enough, so stale pointers would
    // point at new data
    for (int i = e->m_freed.GetCount() - 1; i >= 0; i--) {
        Block b = e->m_freed[i];
        if (b.size >= size) {
            e->m_freed.RemoveAt(i);
            e->m_live.Add(b);
            return b.p;
        }
    }
    Block b = { (uint8_t *)malloc(size), size };
    e->m_live.Add(b);
    return b.p;
}

void StoreEnv::Free(void *p, void *ctx)
{
    StoreEnv *e = (StoreEnv *)ctx;
    for (int i = 0; i < e->m_live.GetCount(); i++) {
        Block b = e->m_live[i];
        if (b.p == p) {
            memset(b.p, POISON, b.size);
            e->m_live.RemoveAt(i);
            e->m_freed.Add(b);
            return;
        }
    }
    Check(!"freed a block that isn't allocated");
}

size_t StoreEnv::LargestFree(void *ctx)
{
    return ((StoreEnv *)ctx)->fail_alloc ? 0 : 1 << 20;
}

uint32_t StoreEnv::RequestSweep(void *ctx)
{
    return ++((StoreEnv *)ctx)->sweeps_requested;
}

uint32_t StoreEnv::Swept(void *ctx)
{
    return ((StoreEnv *)ctx)->swept;
}

bool StoreEnv::IsAllocated(const void *p) const
{
    for (int i = 0; i < m_live.GetCount(); i++)
        if ((const uint8_t *)p >= m_live[i].p && (const uint8_t *)p < m_live[i].p + m_live[i].size)
            return true;
    return false;
}

bool StoreEnv::IsPoisoned(const void *p, size_t bytes) const
{
    for (size_t i = 0; i < bytes; i++)
        if (((const uint8_t *)p)[i] != POISON)
            return false;
    return true;
}

ImageResult StoreEnv::Upload(ImageStore &store, uint32_t id, const void *img)
{
    return store.Put(id, img, Size(img));
}
