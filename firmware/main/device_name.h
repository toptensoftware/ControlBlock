#pragma once

// The device name: the mDNS name (<name>.local) and DHCP hostname. Its rules,
// the default (controlblock-xxxxxx from the station MAC), the stored blob, and
// set/get over an injected storage (NVS namespace "cb_name" on the device).
// Platform independent. Spec: docs/protocol.md (NAME_SET).
//
// Not thread safe: the protocol task calls everything after Init.

#include <stdint.h>
#include "blob_storage.h"

#define DEVICE_NAME_MAX           32
#define DEVICE_NAME_BLOB_VERSION  1
#define DEVICE_NAME_BLOB_HEADER   2
#define DEVICE_NAME_BLOB_MAX      (DEVICE_NAME_BLOB_HEADER + DEVICE_NAME_MAX)

// A hostname label: 1-32 bytes of [a-z0-9-] (A-Z are lowercased), not
// starting or ending with '-'. Writes the NUL-terminated name to out
// (DEVICE_NAME_MAX + 1); false (out unspecified) if it breaks the rules.
bool device_name_make(char *out, const uint8_t *name, uint32_t len);

// "controlblock-" and the MAC's last three bytes in hex
void device_name_default(const uint8_t mac[6], char *out);

// The stored blob: u8 version, u8 length, the name. Decode checks the rules
// (a stored name is already lowercase) and that nothing is left over.
uint32_t device_name_blob_encode(const char *name, uint8_t *out);
bool     device_name_blob_decode(const uint8_t *blob, uint32_t len, char *out);

class DeviceName
{
public:
    // Load the stored name; none (or a bad blob, which is erased) means the default
    void Init(const BlobStorage &storage, const uint8_t mac[6]);

    // Told after each real change, from the caller of Set. Must not block.
    void SetChangedCallback(void (*cb)(const char *name, void *ctx), void *ctx)
    {
        m_changed = cb;
        m_changed_ctx = ctx;
    }

    // NAME_SET: a new name, or empty for the default (the stored name is
    // erased, so it follows the MAC again). Skips the write if nothing changes.
    SettingResult Set(const uint8_t *name, uint32_t len);

    const char *Get() const { return m_name; }
    bool IsDefault() const  { return m_default; }

private:
    BlobStorage m_storage = {};
    char        m_name[DEVICE_NAME_MAX + 1] = {};
    char        m_default_name[DEVICE_NAME_MAX + 1] = {};
    bool        m_default = true;
    void      (*m_changed)(const char *, void *) = nullptr;
    void       *m_changed_ctx = nullptr;
};
