#pragma once

// Read access to a validated config blob at runtime, by offset (layouts in
// config_layout.h). Pointers in the blob are stored as 32-bit values; `adjust`
// turns one into an address:
//   - on the device the blob is relocated already (the values are addresses): adjust = 0
//   - in the host tests the blob is as baked (the values are file offsets): adjust = the blob's address
// Only for a blob that passed ConfigValidate(): nothing is range checked here.

#include <stdint.h>
#include <string.h>
#include "config_blob.h"        // BPAK_HEADER_SIZE
#include "config_layout.h"

class ConfigView
{
public:
    ConfigView() = default;
    ConfigView(const uint8_t *blob, uintptr_t adjust) : m_blob(blob), m_adjust(adjust) {}

    bool Valid() const { return m_blob != nullptr; }

    const uint8_t *Root() const { return m_blob + BPAK_HEADER_SIZE; }

    uint8_t U8(const uint8_t *at) const { return *at; }

    uint32_t U32(const uint8_t *at) const
    {
        uint32_t v;
        memcpy(&v, at, 4);
        return v;
    }

    // The pointer stored in `slot`, or null
    const uint8_t *Ptr(const uint8_t *slot) const
    {
        uint32_t v = U32(slot);
        return v ? (const uint8_t *)(v + m_adjust) : nullptr;
    }

    const char *Str(const uint8_t *slot) const { return (const char *)Ptr(slot); }

    // A (count, pointer) pair of `slot`, `slot + 4`
    struct Array
    {
        uint32_t       count;
        const uint8_t *items;
    };
    Array GetArray(const uint8_t *slot) const { return Array{ U32(slot), Ptr(slot + 4) }; }

private:
    const uint8_t *m_blob   = nullptr;
    uintptr_t      m_adjust = 0;
};
