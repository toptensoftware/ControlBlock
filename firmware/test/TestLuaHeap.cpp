// The Lua allocator (lua_heap.cpp). On the device the platform realloc is the PSRAM heap,
// whose realloc copies a moved block inside the heap's critical section (interrupts off
// on that core): a big copy there held off the bounce fill's interrupts and shifted the
// picture for good (a Lua stack grown by deep recursion). So a block that grows is
// allocated anew, copied and freed through separate calls; only shrinking may resize.

#include <string.h>
#include <stdlib.h>
#include "UnitTesting.h"
#include "Core.h"
#include "lua_heap.h"

using namespace SimpleLib;

namespace
{
    struct Platform
    {
        int resizes_up;         // realloc(non-null, bigger)
        int resizes_down;
        size_t sizes[64];       // the size of each live block, by slot
        void *blocks[64];
        bool fail;
    };

    int Slot(Platform *p, void *ptr)
    {
        for (int i = 0; i < 64; i++)
            if (p->blocks[i] == ptr)
                return i;
        return -1;
    }

    void *Realloc(void *ptr, size_t size, void *ctx)
    {
        Platform *p = (Platform *)ctx;
        if (size == 0) {
            p->blocks[Slot(p, ptr)] = nullptr;
            free(ptr);
            return nullptr;
        }
        if (p->fail)
            return nullptr;
        if (ptr) {
            int s = Slot(p, ptr);
            if (size > p->sizes[s])
                p->resizes_up++;
            else
                p->resizes_down++;
            void *q = realloc(ptr, size);
            p->blocks[s] = q;
            p->sizes[s] = size;
            return q;
        }
        int s = Slot(p, nullptr);
        p->blocks[s] = malloc(size);
        p->sizes[s] = size;
        return p->blocks[s];
    }

    LuaHeap MakeHeap(Platform *p, size_t cap = 1 << 20)
    {
        memset(p, 0, sizeof(*p));
        LuaHeap h = {};
        h.ctx = p;
        h.realloc = Realloc;
        h.cap = cap;
        return h;
    }

    int Live(Platform *p)
    {
        int n = 0;
        for (int i = 0; i < 64; i++)
            n += p->blocks[i] != nullptr;
        return n;
    }
}

Fact("Lua Heap Grows A Block Without A Platform Resize")
{
    Platform p;
    LuaHeap h = MakeHeap(&p);
    char *a = (char *)LuaHeapAlloc(&h, nullptr, 0, 100);
    Assert(a != nullptr);
    for (int i = 0; i < 100; i++)
        a[i] = (char)i;

    char *b = (char *)LuaHeapAlloc(&h, a, 100, 5000);
    Assert(b != nullptr);
    Assert(p.resizes_up == 0);
    for (int i = 0; i < 100; i++)
        Assert(b[i] == (char)i);
    Assert(Live(&p) == 1);                                  // the old block was freed
    Assert(h.used == 5000 && h.peak == 5000);
    LuaHeapAlloc(&h, b, 5000, 0);
    Assert(Live(&p) == 0 && h.used == 0);
}

Fact("Lua Heap Shrinks A Block In Place")
{
    Platform p;
    LuaHeap h = MakeHeap(&p);
    char *a = (char *)LuaHeapAlloc(&h, nullptr, 0, 400);
    memset(a, 'x', 400);
    char *b = (char *)LuaHeapAlloc(&h, a, 400, 100);
    Assert(b != nullptr && p.resizes_down == 1);
    Assert(b[0] == 'x' && b[99] == 'x');
    Assert(h.used == 100);
    LuaHeapAlloc(&h, b, 100, 0);
}

Fact("Lua Heap Failed Growth Keeps The Old Block")
{
    Platform p;
    LuaHeap h = MakeHeap(&p);
    char *a = (char *)LuaHeapAlloc(&h, nullptr, 0, 100);
    a[0] = 'k';
    p.fail = true;
    Assert(LuaHeapAlloc(&h, a, 100, 200) == nullptr);
    Assert(h.refused == 1 && h.used == 100);
    Assert(Live(&p) == 1 && a[0] == 'k');                   // Lua still owns it
    p.fail = false;
    LuaHeapAlloc(&h, a, 100, 0);
}

Fact("Lua Heap Cap Refuses Growth")
{
    Platform p;
    LuaHeap h = MakeHeap(&p, 1000);
    void *a = LuaHeapAlloc(&h, nullptr, 0, 600);
    Assert(LuaHeapAlloc(&h, a, 600, 1200) == nullptr);
    Assert(h.refused == 1 && h.used == 600 && Live(&p) == 1);
    LuaHeapAlloc(&h, a, 600, 0);
}
