#pragma once

// A fake NOR flash for the config store and what sits on it (protocol):
// erase sets bytes to 0xFF in whole sectors, a write may only go to erased
// bytes (a rule the real chip enforces silently, so a bug here would
// corrupt data on the device), and failures can be injected.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "config_store.h"
#include "harness.h"

class FakeFlash
{
public:
    explicit FakeFlash(uint32_t capacity = 6 * CONFIG_SECTOR)
        : m_size(capacity)
    {
        mem = (uint8_t *)malloc(capacity);
        memset(mem, 0xFF, capacity);
    }
    ~FakeFlash() { free(mem); }
    FakeFlash(const FakeFlash &) = delete;
    FakeFlash &operator=(const FakeFlash &) = delete;

    uint8_t *mem;

    // --- What happened ---
    int      erase_calls = 0, write_calls = 0;
    uint32_t erased_bytes = 0, highest_erased = 0;      // highest_erased: end of the furthest erase
    bool     busy = false;
    int      busy_changes = 0;

    // --- Failure injection: the Nth next call fails (1 = the next), 0 = never ---
    int  fail_erase_in = 0, fail_write_in = 0, fail_read_in = 0;
    bool corrupt_next_write = false;        // reports success but stores one flipped bit

    ConfigFlash Host()
    {
        ConfigFlash h = {};
        h.capacity = m_size;
        h.erase = [](uint32_t off, uint32_t len, void *c) { return ((FakeFlash *)c)->Erase(off, len); };
        h.write = [](uint32_t off, const void *p, uint32_t len, void *c) { return ((FakeFlash *)c)->Write(off, p, len); };
        h.read  = [](uint32_t off, void *p, uint32_t len, void *c) { return ((FakeFlash *)c)->Read(off, p, len); };
        h.busy  = [](bool b, void *c) { FakeFlash *f = (FakeFlash *)c; f->busy = b; f->busy_changes++; };
        h.ctx   = this;
        return h;
    }

    uint32_t Capacity() const { return m_size; }

private:
    static bool Fails(int *countdown)
    {
        return *countdown && --*countdown == 0;
    }

    bool Erase(uint32_t off, uint32_t len)
    {
        erase_calls++;
        Check(off % CONFIG_SECTOR == 0 && len % CONFIG_SECTOR == 0 && len > 0);
        Check((uint64_t)off + len <= m_size);
        if (Fails(&fail_erase_in))
            return false;
        memset(mem + off, 0xFF, len);
        erased_bytes += len;
        if (off + len > highest_erased)
            highest_erased = off + len;
        return true;
    }

    bool Write(uint32_t off, const void *p, uint32_t len)
    {
        write_calls++;
        Check((uint64_t)off + len <= m_size);
        if (Fails(&fail_write_in))
            return false;
        for (uint32_t i = 0; i < len; i++)
            Check(mem[off + i] == 0xFF);        // only erased bytes can be written
        memcpy(mem + off, p, len);
        if (corrupt_next_write && len) {
            mem[off] ^= 0x01;
            corrupt_next_write = false;
        }
        return true;
    }

    bool Read(uint32_t off, void *p, uint32_t len)
    {
        Check((uint64_t)off + len <= m_size);
        if (Fails(&fail_read_in))
            return false;
        memcpy(p, mem + off, len);
        return true;
    }

    uint32_t m_size;
};
