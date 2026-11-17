// The display timing setting. Spec: docs/protocol.md (DISPLAY_SET).

#include "display_config.h"

uint32_t display_config_blob_encode(const DisplayTimingSetting &t, uint8_t *out)
{
    out[0] = DISPLAY_CONFIG_BLOB_VERSION;
    out[1] = t.mhz;
    out[2] = (uint8_t)t.lines;
    out[3] = (uint8_t)(t.lines >> 8);
    return DISPLAY_CONFIG_BLOB_SIZE;
}

bool display_config_blob_decode(const uint8_t *blob, uint32_t len, DisplayTimingSetting *out)
{
    if (len != DISPLAY_CONFIG_BLOB_SIZE || blob[0] != DISPLAY_CONFIG_BLOB_VERSION)
        return false;
    out->mhz   = blob[1];
    out->lines = (uint16_t)(blob[2] | blob[3] << 8);
    return true;
}

void DisplayConfig::Init(const BlobStorage &storage, const DisplayTimingLimits &limits)
{
    m_storage = storage;
    m_limits  = limits;
    m_timing  = { limits.default_mhz, limits.default_lines };
    uint8_t blob[DISPLAY_CONFIG_BLOB_SIZE];
    DisplayTimingSetting t;
    if (!blob_storage_load(m_storage, blob, sizeof(blob), display_config_blob_decode, &t))
        return;
    if (DisplayTimingValid(m_limits, t.mhz, t.lines))
        m_timing = t;
    else
        blob_storage_write(m_storage, nullptr, 0);      // another build's: not this one's to use
}

bool DisplayConfig::IsDefault() const
{
    return m_timing.mhz == m_limits.default_mhz && m_timing.lines == m_limits.default_lines;
}

SettingResult DisplayConfig::Set(uint32_t mhz, uint32_t lines)
{
    if (!mhz)
        mhz = m_limits.default_mhz;
    if (!lines)
        lines = m_limits.default_lines;
    if (!DisplayTimingValid(m_limits, mhz, lines))
        return SettingResult::BadArg;
    if (mhz == m_timing.mhz && lines == m_timing.lines)
        return SettingResult::Ok;               // no flash write, no switch

    DisplayTimingSetting next = { (uint8_t)mhz, (uint16_t)lines };
    bool to_default = mhz == m_limits.default_mhz && lines == m_limits.default_lines;
    uint8_t blob[DISPLAY_CONFIG_BLOB_SIZE];
    bool ok = to_default ? blob_storage_write(m_storage, nullptr, 0)
                         : blob_storage_write(m_storage, blob, display_config_blob_encode(next, blob));
    if (!ok)
        return SettingResult::IoError;

    m_timing = next;
    if (m_changed)
        m_changed(m_timing, m_changed_ctx);
    return SettingResult::Ok;
}
