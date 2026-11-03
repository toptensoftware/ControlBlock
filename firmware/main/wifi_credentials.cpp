// Wi-Fi credentials. Spec: docs/wifi.md.

#include <string.h>
#include "wifi_credentials.h"

namespace
{
    bool has_nul(const uint8_t *p, uint32_t n)
    {
        return n && memchr(p, 0, n) != nullptr;
    }

    bool lengths_ok(uint32_t ssid_len, uint32_t pass_len)
    {
        if (ssid_len > WIFI_SSID_MAX)
            return false;
        if (pass_len && (pass_len < WIFI_PASS_MIN || pass_len > WIFI_PASS_MAX))
            return false;
        return ssid_len || !pass_len;           // a password needs an ssid
    }
}

bool wifi_credentials_make(WifiCredentials *out, const uint8_t *ssid, uint32_t ssid_len,
                           const uint8_t *pass, uint32_t pass_len)
{
    if (!lengths_ok(ssid_len, pass_len) || has_nul(ssid, ssid_len) || has_nul(pass, pass_len))
        return false;
    memset(out, 0, sizeof(*out));
    out->ssid_len = (uint8_t)ssid_len;
    out->pass_len = (uint8_t)pass_len;
    if (ssid_len)
        memcpy(out->ssid, ssid, ssid_len);
    if (pass_len)
        memcpy(out->pass, pass, pass_len);
    return true;
}

bool wifi_credentials_equal(const WifiCredentials &a, const WifiCredentials &b)
{
    return a.ssid_len == b.ssid_len && a.pass_len == b.pass_len
        && !memcmp(a.ssid, b.ssid, a.ssid_len) && !memcmp(a.pass, b.pass, a.pass_len);
}

uint32_t wifi_blob_encode(const WifiCredentials &c, uint8_t *out)
{
    out[0] = WIFI_BLOB_VERSION;
    out[1] = c.ssid_len;
    out[2] = c.pass_len;
    out[3] = 0;
    memcpy(out + WIFI_BLOB_HEADER, c.ssid, c.ssid_len);
    memcpy(out + WIFI_BLOB_HEADER + c.ssid_len, c.pass, c.pass_len);
    return WIFI_BLOB_HEADER + c.ssid_len + c.pass_len;
}

bool wifi_blob_decode(const uint8_t *blob, uint32_t len, WifiCredentials *out)
{
    if (len < WIFI_BLOB_HEADER || blob[0] != WIFI_BLOB_VERSION || blob[3] != 0)
        return false;
    uint32_t ssid_len = blob[1], pass_len = blob[2];
    if (!ssid_len || len != WIFI_BLOB_HEADER + ssid_len + pass_len)
        return false;
    return wifi_credentials_make(out, blob + WIFI_BLOB_HEADER, ssid_len, blob + WIFI_BLOB_HEADER + ssid_len, pass_len);
}

void WifiConfig::Init(const WifiStorage &storage)
{
    m_storage = storage;
    memset(&m_creds, 0, sizeof(m_creds));
    if (!m_storage.load)
        return;

    uint8_t  blob[WIFI_BLOB_MAX];
    uint32_t len = 0;
    if (!m_storage.load(blob, sizeof(blob), &len, m_storage.ctx))
        return;
    if (!wifi_blob_decode(blob, len, &m_creds)) {
        memset(&m_creds, 0, sizeof(m_creds));
        if (m_storage.erase)
            m_storage.erase(m_storage.ctx);
    }
}

WifiResult WifiConfig::Set(const uint8_t *ssid, uint32_t ssid_len, const uint8_t *pass, uint32_t pass_len)
{
    WifiCredentials next;
    if (!wifi_credentials_make(&next, ssid, ssid_len, pass, pass_len))
        return WifiResult::BadArg;
    if (wifi_credentials_equal(next, m_creds))
        return WifiResult::Ok;                  // no flash write, no reconnect

    if (next.ssid_len) {
        uint8_t blob[WIFI_BLOB_MAX];
        uint32_t n = wifi_blob_encode(next, blob);
        if (!m_storage.save || !m_storage.save(blob, n, m_storage.ctx))
            return WifiResult::IoError;
    } else {
        if (!m_storage.erase || !m_storage.erase(m_storage.ctx))
            return WifiResult::IoError;
    }

    m_creds = next;
    if (m_changed)
        m_changed(m_creds, m_changed_ctx);
    return WifiResult::Ok;
}
