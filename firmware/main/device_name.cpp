// The device name. Spec: docs/protocol.md (NAME_SET).

#include <stdio.h>
#include <string.h>
#include "device_name.h"

bool device_name_make(char *out, const uint8_t *name, uint32_t len)
{
    if (!len || len > DEVICE_NAME_MAX || name[0] == '-' || name[len - 1] == '-')
        return false;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = name[i];
        if (c >= 'A' && c <= 'Z')
            c = (uint8_t)(c - 'A' + 'a');
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
        out[i] = (char)c;
    }
    out[len] = 0;
    return true;
}

void device_name_default(const uint8_t mac[6], char *out)
{
    snprintf(out, DEVICE_NAME_MAX + 1, "controlblock-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

uint32_t device_name_blob_encode(const char *name, uint8_t *out)
{
    uint32_t len = (uint32_t)strlen(name);
    out[0] = DEVICE_NAME_BLOB_VERSION;
    out[1] = (uint8_t)len;
    memcpy(out + DEVICE_NAME_BLOB_HEADER, name, len);
    return DEVICE_NAME_BLOB_HEADER + len;
}

bool device_name_blob_decode(const uint8_t *blob, uint32_t len, char *out)
{
    if (len < DEVICE_NAME_BLOB_HEADER || blob[0] != DEVICE_NAME_BLOB_VERSION
        || len != DEVICE_NAME_BLOB_HEADER + blob[1])
        return false;
    if (!device_name_make(out, blob + DEVICE_NAME_BLOB_HEADER, blob[1]))
        return false;
    return memcmp(out, blob + DEVICE_NAME_BLOB_HEADER, blob[1]) == 0;    // stored lowercase
}

namespace
{
    // blob_storage_load copies a T: the name as a value
    struct NameText { char s[DEVICE_NAME_MAX + 1]; };

    bool decode_text(const uint8_t *blob, uint32_t len, NameText *out)
    {
        return device_name_blob_decode(blob, len, out->s);
    }
}

void DeviceName::Init(const BlobStorage &storage, const uint8_t mac[6])
{
    m_storage = storage;
    device_name_default(mac, m_default_name);
    uint8_t blob[DEVICE_NAME_BLOB_MAX];
    NameText t;
    m_default = !blob_storage_load(m_storage, blob, sizeof(blob), decode_text, &t);
    memcpy(m_name, m_default ? m_default_name : t.s, sizeof(m_name));
}

SettingResult DeviceName::Set(const uint8_t *name, uint32_t len)
{
    char next[DEVICE_NAME_MAX + 1];
    bool to_default = !len;
    if (to_default)
        memcpy(next, m_default_name, sizeof(next));
    else if (!device_name_make(next, name, len))
        return SettingResult::BadArg;
    if (to_default == m_default && !strcmp(next, m_name))
        return SettingResult::Ok;               // no flash write, no restart of mDNS

    uint8_t blob[DEVICE_NAME_BLOB_MAX];
    bool ok = to_default ? blob_storage_write(m_storage, nullptr, 0)
                         : blob_storage_write(m_storage, blob, device_name_blob_encode(next, blob));
    if (!ok)
        return SettingResult::IoError;

    bool changed = strcmp(next, m_name) != 0;
    memcpy(m_name, next, sizeof(m_name));
    m_default = to_default;
    if (changed && m_changed)
        m_changed(m_name, m_changed_ctx);
    return SettingResult::Ok;
}
