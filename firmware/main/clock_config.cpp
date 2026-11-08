// Clock settings (time zone, NTP server). Spec: docs/protocol.md (CLOCK_SET).

#include <string.h>
#include "clock_config.h"

namespace
{
    bool tz_char(uint8_t c)     { return c >= 0x21 && c <= 0x7e; }

    bool server_char(uint8_t c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '.' || c == '-';
    }

    // Copy one field (empty = the default) into dst (CLOCK_FIELD_MAX + 1)
    bool make_field(char *dst, uint8_t *dst_len, const uint8_t *src, uint32_t len,
                    bool (*ok)(uint8_t), const char *def)
    {
        if (!len) {
            src = (const uint8_t *)def;
            len = (uint32_t)strlen(def);
        }
        if (len > CLOCK_FIELD_MAX)
            return false;
        for (uint32_t i = 0; i < len; i++)
            if (!ok(src[i]))
                return false;
        memcpy(dst, src, len);
        dst[len] = 0;
        *dst_len = (uint8_t)len;
        return true;
    }
}

bool clock_settings_make(ClockSettings *out, const uint8_t *tz, uint32_t tz_len,
                         const uint8_t *server, uint32_t server_len)
{
    memset(out, 0, sizeof(*out));
    return make_field(out->tz, &out->tz_len, tz, tz_len, tz_char, CLOCK_TZ_DEFAULT)
        && make_field(out->server, &out->server_len, server, server_len, server_char, CLOCK_SERVER_DEFAULT);
}

bool clock_settings_equal(const ClockSettings &a, const ClockSettings &b)
{
    return a.tz_len == b.tz_len && a.server_len == b.server_len
        && !memcmp(a.tz, b.tz, a.tz_len) && !memcmp(a.server, b.server, a.server_len);
}

uint32_t clock_blob_encode(const ClockSettings &c, uint8_t *out)
{
    out[0] = CLOCK_BLOB_VERSION;
    out[1] = c.tz_len;
    out[2] = c.server_len;
    out[3] = 0;
    memcpy(out + CLOCK_BLOB_HEADER, c.tz, c.tz_len);
    memcpy(out + CLOCK_BLOB_HEADER + c.tz_len, c.server, c.server_len);
    return CLOCK_BLOB_HEADER + c.tz_len + c.server_len;
}

bool clock_blob_decode(const uint8_t *blob, uint32_t len, ClockSettings *out)
{
    if (len < CLOCK_BLOB_HEADER || blob[0] != CLOCK_BLOB_VERSION || blob[3] != 0)
        return false;
    uint32_t tz_len = blob[1], server_len = blob[2];
    if (!tz_len || !server_len || len != CLOCK_BLOB_HEADER + tz_len + server_len)
        return false;
    return clock_settings_make(out, blob + CLOCK_BLOB_HEADER, tz_len, blob + CLOCK_BLOB_HEADER + tz_len, server_len);
}

ClockConfig::ClockConfig()
{
    clock_settings_make(&m_settings, nullptr, 0, nullptr, 0);
}

void ClockConfig::Init(const BlobStorage &storage)
{
    m_storage = storage;
    clock_settings_make(&m_settings, nullptr, 0, nullptr, 0);
    uint8_t blob[CLOCK_BLOB_MAX];
    blob_storage_load(m_storage, blob, sizeof(blob), clock_blob_decode, &m_settings);
}

SettingResult ClockConfig::Set(uint8_t mask, const uint8_t *tz, uint32_t tz_len,
                               const uint8_t *server, uint32_t server_len)
{
    // Unmasked fields keep their current values
    if (!(mask & CLOCK_SET_TZ)) {
        tz = (const uint8_t *)m_settings.tz;
        tz_len = m_settings.tz_len;
    }
    if (!(mask & CLOCK_SET_SERVER)) {
        server = (const uint8_t *)m_settings.server;
        server_len = m_settings.server_len;
    }

    ClockSettings next;
    if (!clock_settings_make(&next, tz, tz_len, server, server_len))
        return SettingResult::BadArg;
    if (clock_settings_equal(next, m_settings))
        return SettingResult::Ok;               // no flash write, no resync

    uint8_t blob[CLOCK_BLOB_MAX];
    if (!blob_storage_write(m_storage, blob, clock_blob_encode(next, blob)))
        return SettingResult::IoError;

    m_settings = next;
    if (m_changed)
        m_changed(m_settings, m_changed_ctx);
    return SettingResult::Ok;
}
