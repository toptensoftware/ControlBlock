#pragma once

// A fake NVS blob for WifiConfig (and what sits on it, the protocol): one
// stored blob, with call counts and injected failures.

#include <stdint.h>
#include <string.h>
#include "wifi_credentials.h"

class FakeWifiStorage
{
public:
    uint8_t  blob[256];
    uint32_t blob_len = 0;
    bool     present = false;

    int  loads = 0, saves = 0, erases = 0;
    bool fail_save = false, fail_erase = false, fail_load = false;

    WifiStorage Host()
    {
        WifiStorage s = {};
        s.load = [](void *buf, uint32_t cap, uint32_t *len, void *c) {
            FakeWifiStorage *f = (FakeWifiStorage *)c;
            f->loads++;
            if (f->fail_load || !f->present || f->blob_len > cap)
                return false;
            memcpy(buf, f->blob, f->blob_len);
            *len = f->blob_len;
            return true;
        };
        s.save = [](const void *b, uint32_t len, void *c) {
            FakeWifiStorage *f = (FakeWifiStorage *)c;
            f->saves++;
            if (f->fail_save || len > sizeof(f->blob))
                return false;
            memcpy(f->blob, b, len);
            f->blob_len = len;
            f->present = true;
            return true;
        };
        s.erase = [](void *c) {
            FakeWifiStorage *f = (FakeWifiStorage *)c;
            f->erases++;
            if (f->fail_erase)
                return false;
            f->present = false;
            f->blob_len = 0;
            return true;
        };
        s.ctx = this;
        return s;
    }

    // Put a blob in storage as if an earlier run had written it
    void Preload(const void *b, uint32_t len)
    {
        memcpy(blob, b, len);
        blob_len = len;
        present = true;
    }
};
