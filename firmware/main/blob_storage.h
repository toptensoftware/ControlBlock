#pragma once

// One stored settings blob, injected into the pure settings classes
// (WifiConfig, ClockConfig). The device supplies NVS (nvs_blob.cpp); the
// host tests a fake (test/blob_env.h).

#include <stdint.h>

// One blob, written whole (NVS makes that atomic: a power loss leaves the old
// blob or the new, never a mix)
struct BlobStorage
{
    // Read the blob into buf (cap bytes). False if there is none or it can't
    // be read.
    bool (*load)(void *buf, uint32_t cap, uint32_t *len, void *ctx);
    bool (*save)(const void *blob, uint32_t len, void *ctx);
    // Erasing a blob that isn't there succeeds
    bool (*erase)(void *ctx);
    void *ctx;
};

// What a settings Set returns (the protocol maps it to a status)
enum class SettingResult { Ok, BadArg, IoError };

// Load the stored blob into buf and decode it into out. A blob that doesn't
// decode is erased; one that can't be read is left. True only if out was
// filled (otherwise the caller keeps its defaults).
template <typename T>
bool blob_storage_load(const BlobStorage &s, uint8_t *buf, uint32_t cap,
                       bool (*decode)(const uint8_t *, uint32_t, T *), T *out)
{
    uint32_t len = 0;
    if (!s.load || !s.load(buf, cap, &len, s.ctx))
        return false;
    T tmp;
    if (!decode(buf, len, &tmp)) {
        if (s.erase)
            s.erase(s.ctx);
        return false;
    }
    *out = tmp;
    return true;
}

// Save (or, with blob null, erase): false if it failed or there's no storage
inline bool blob_storage_write(const BlobStorage &s, const void *blob, uint32_t len)
{
    if (blob)
        return s.save && s.save(blob, len, s.ctx);
    return s.erase && s.erase(s.ctx);
}
