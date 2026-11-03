#pragma once

// Wi-Fi credentials: the validation rules, the stored blob format, and
// set/get/clear over an injected storage (docs/wifi.md). Platform
// independent: the device supplies the NVS blob through WifiStorage
// (wifi.cpp). Protocol and the device layer share these rules.
//
// Not thread safe: the protocol task calls everything after Init.

#include <stddef.h>
#include <stdint.h>

#define WIFI_SSID_MAX      32
#define WIFI_PASS_MIN      8
#define WIFI_PASS_MAX      63
#define WIFI_BLOB_VERSION  1
#define WIFI_BLOB_HEADER   4
#define WIFI_BLOB_MAX      (WIFI_BLOB_HEADER + WIFI_SSID_MAX + WIFI_PASS_MAX)

// One network. The strings are NUL terminated for the driver (the
// validation keeps NULs out of them); the lengths are what's on the wire.
// Empty ssid = no network (a clear).
struct WifiCredentials
{
    uint8_t ssid_len, pass_len;
    char    ssid[WIFI_SSID_MAX + 1];
    char    pass[WIFI_PASS_MAX + 1];
};

// Check the WIFI_SET rules and build the credentials. False (and `out`
// unspecified) if ssid_len is over 32, pass_len is 1..7 or over 63, a
// password has no ssid, or either contains a NUL. An empty ssid with an
// empty password is valid: it means clear.
bool wifi_credentials_make(WifiCredentials *out, const uint8_t *ssid, uint32_t ssid_len,
                           const uint8_t *pass, uint32_t pass_len);

bool wifi_credentials_equal(const WifiCredentials &a, const WifiCredentials &b);

// The stored blob (little endian): u8 version, u8 ssid_len, u8 pass_len,
// u8 reserved (0), ssid, pass. Encode returns the size (at most WIFI_BLOB_MAX).
// Decode checks the version, the lengths (by the WIFI_SET rules) and that
// nothing is left over; an empty ssid is not a stored network.
uint32_t wifi_blob_encode(const WifiCredentials &c, uint8_t *out);
bool     wifi_blob_decode(const uint8_t *blob, uint32_t len, WifiCredentials *out);

// What the config needs from the device: one blob, written whole (NVS makes
// that atomic: a power loss leaves the old blob or the new, never a mix)
struct WifiStorage
{
    // Read the blob into buf (cap bytes). False if there is none or it can't
    // be read.
    bool (*load)(void *buf, uint32_t cap, uint32_t *len, void *ctx);
    bool (*save)(const void *blob, uint32_t len, void *ctx);
    bool (*erase)(void *ctx);
    void *ctx;
};

enum class WifiResult { Ok, BadArg, IoError };

class WifiConfig
{
public:
    // Load the stored credentials. A blob that fails to decode counts as none
    // and is erased; a blob that can't be read counts as none and is left.
    // Never a failure.
    void Init(const WifiStorage &storage);

    // Told after each real change (a set that differs, a clear of something
    // stored), from the caller of Set, with the new credentials (ssid_len 0
    // after a clear). Not told at Init: read Get() then. Must not block.
    void SetChangedCallback(void (*cb)(const WifiCredentials &now, void *ctx), void *ctx)
    {
        m_changed = cb;
        m_changed_ctx = ctx;
    }

    // WIFI_SET: validate, skip if nothing differs, otherwise store (or erase,
    // for a clear) and notify. On IoError the old credentials stay (and
    // IoError with no storage at all).
    WifiResult Set(const uint8_t *ssid, uint32_t ssid_len, const uint8_t *pass, uint32_t pass_len);

    bool Stored() const { return m_creds.ssid_len != 0; }
    const WifiCredentials &Get() const { return m_creds; }

private:
    WifiStorage     m_storage = {};
    WifiCredentials m_creds = {};
    void          (*m_changed)(const WifiCredentials &, void *) = nullptr;
    void           *m_changed_ctx = nullptr;
};
