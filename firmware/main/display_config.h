#pragma once

// The display timing setting: the pixel clock and the total lines a frame
// (the vertical front porch takes the extra), within the build's limits
// (display_timing.h DisplayTimingLimitsFor), the stored blob, and set/get
// over an injected storage (NVS namespace "cb_display" on the device,
// display_settings.cpp). Experimental: it lets the user live with a lower
// frame rate. Platform independent. Spec: docs/protocol.md (DISPLAY_SET).
//
// Not thread safe: the protocol task calls everything after Init.

#include <stdint.h>
#include "blob_storage.h"
#include "display_timing.h"

#define DISPLAY_CONFIG_BLOB_VERSION  1
#define DISPLAY_CONFIG_BLOB_SIZE     4

struct DisplayTimingSetting
{
    uint8_t  mhz;       // pixel clock, whole MHz
    uint16_t lines;     // total lines a frame
};

// The stored blob: u8 version, u8 mhz, u16 lines (little endian). Decode checks
// the version and the length only: the range is the build's (Init checks it).
uint32_t display_config_blob_encode(const DisplayTimingSetting &t, uint8_t *out);
bool     display_config_blob_decode(const uint8_t *blob, uint32_t len, DisplayTimingSetting *out);

class DisplayConfig
{
public:
    // Load the stored timing. None, a bad blob or one outside `limits` (stored
    // by a build with another clock; erased) means the defaults.
    void Init(const BlobStorage &storage, const DisplayTimingLimits &limits);

    // Told after each real change, from the caller of Set. Not told at Init:
    // read Get() then. Must not block.
    void SetChangedCallback(void (*cb)(const DisplayTimingSetting &now, void *ctx), void *ctx)
    {
        m_changed = cb;
        m_changed_ctx = ctx;
    }

    // DISPLAY_SET: 0 in a field is that field's default. BadArg (nothing
    // changes) outside the limits. The defaults aren't stored (the blob is
    // erased). Skips the write if nothing changes. On IoError the old timing stays.
    SettingResult Set(uint32_t mhz, uint32_t lines);

    const DisplayTimingSetting &Get() const { return m_timing; }
    bool IsDefault() const;
    const DisplayTimingLimits &Limits() const { return m_limits; }

private:
    BlobStorage          m_storage = {};
    DisplayTimingLimits  m_limits = {};
    DisplayTimingSetting m_timing = {};
    void               (*m_changed)(const DisplayTimingSetting &, void *) = nullptr;
    void                *m_changed_ctx = nullptr;
};
