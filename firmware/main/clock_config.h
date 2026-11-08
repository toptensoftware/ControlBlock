#pragma once

// Clock settings: the time zone (a POSIX TZ string, applied by newlib) and
// the NTP server, their rules, the stored blob format, and set/get over an
// injected storage (NVS namespace "cb_clock" on the device, nvs_blob.cpp).
// Platform independent. Spec: docs/protocol.md (CLOCK_SET).
//
// Not thread safe: the protocol task calls everything after Init.

#include <stddef.h>
#include <stdint.h>
#include "blob_storage.h"

#define CLOCK_TZ_DEFAULT      "UTC0"
#define CLOCK_SERVER_DEFAULT  "pool.ntp.org"
#define CLOCK_FIELD_MAX       63
#define CLOCK_BLOB_VERSION    1
#define CLOCK_BLOB_HEADER     4
#define CLOCK_BLOB_MAX        (CLOCK_BLOB_HEADER + 2 * CLOCK_FIELD_MAX)

// CLOCK_SET's mask: which fields a Set changes
#define CLOCK_SET_TZ          0x01
#define CLOCK_SET_SERVER      0x02

// The effective settings: never empty (an empty value means the default).
// NUL terminated for setenv and SNTP.
struct ClockSettings
{
    uint8_t tz_len, server_len;
    char    tz[CLOCK_FIELD_MAX + 1];
    char    server[CLOCK_FIELD_MAX + 1];
};

// Check the rules and build the settings. An empty value takes its default.
// A time zone is up to 63 bytes of printable ASCII without spaces
// (0x21-0x7E; not checked as POSIX: newlib treats a bad one as UTC); a server
// up to 63 bytes of [A-Za-z0-9.-]. False (`out` unspecified) otherwise.
bool clock_settings_make(ClockSettings *out, const uint8_t *tz, uint32_t tz_len,
                         const uint8_t *server, uint32_t server_len);

bool clock_settings_equal(const ClockSettings &a, const ClockSettings &b);

// The stored blob: u8 version, u8 tz_len, u8 server_len, u8 reserved (0), tz,
// server. Decode checks the version, that neither value is empty, the rules
// and that nothing is left over.
uint32_t clock_blob_encode(const ClockSettings &c, uint8_t *out);
bool     clock_blob_decode(const uint8_t *blob, uint32_t len, ClockSettings *out);

class ClockConfig
{
public:
    ClockConfig();

    // Load the stored settings. A blob that fails to decode counts as none
    // (defaults) and is erased; one that can't be read counts as none and is
    // left. Never a failure.
    void Init(const BlobStorage &storage);

    // Told after each real change, from the caller of Set, with the new
    // settings. Not told at Init: read Get() then. Must not block.
    void SetChangedCallback(void (*cb)(const ClockSettings &now, void *ctx), void *ctx)
    {
        m_changed = cb;
        m_changed_ctx = ctx;
    }

    // CLOCK_SET: change the fields in `mask` (CLOCK_SET_*; an empty value is
    // the default; unmasked fields aren't looked at). All or nothing: BadArg
    // if either masked value breaks the rules. Skips the write if nothing
    // differs. On IoError (or no storage) the old settings stay.
    SettingResult Set(uint8_t mask, const uint8_t *tz, uint32_t tz_len,
                      const uint8_t *server, uint32_t server_len);

    const ClockSettings &Get() const { return m_settings; }

private:
    BlobStorage   m_storage = {};
    ClockSettings m_settings;
    void        (*m_changed)(const ClockSettings &, void *) = nullptr;
    void         *m_changed_ctx = nullptr;
};
