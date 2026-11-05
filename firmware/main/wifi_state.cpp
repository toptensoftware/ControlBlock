// Wi-Fi connection state machine. Spec: docs/wifi.md.

#include <string.h>
#include "wifi_state.h"

namespace
{
    bool is_auth_failure(uint8_t reason)
    {
        switch (reason) {
        // Timeouts (4-way handshake, handshake) are not here: a weak signal
        // causes them too, so they keep retrying
        case CB_WIFI_REASON_MIC_FAILURE:
        case CB_WIFI_REASON_8021X_FAILED:
        case CB_WIFI_REASON_AUTH_FAIL:
            return true;
        }
        return false;
    }

    int32_t backoff_ms(int failures)
    {
        int32_t ms = WIFI_RETRY_MIN_MS;
        for (int i = 0; i < failures && ms < WIFI_RETRY_MAX_MS; i++)
            ms *= 2;
        return ms < WIFI_RETRY_MAX_MS ? ms : WIFI_RETRY_MAX_MS;
    }
}

WifiAction WifiMachine::OnCredentials(bool present)
{
    WifiAction a;
    WifiState prev = m_state;

    // Whatever was running is dropped. Its disconnect event is still to come
    // when something was in flight (it may arrive after the next connect).
    if (m_in_flight) {
        m_leave_expected = true;
        a.disconnect = !!present;
    }
    m_in_flight = false;
    m_failures  = 0;
    m_halted    = false;
    a.cancel_retry = true;
    memset(m_ip, 0, sizeof(m_ip));

    if (!present) {
        if (prev == WifiState::NoCredentials)
            return WifiAction();            // nothing to stop
        a.stop = true;
        a.disconnect = false;
        m_state  = WifiState::NoCredentials;
        m_reason = 0;
        a.changed = true;
        return a;
    }

    a.start   = prev == WifiState::NoCredentials;
    a.connect = true;
    m_state   = WifiState::Connecting;
    m_in_flight = true;
    a.changed = prev != WifiState::Connecting;
    return a;
}

WifiAction WifiMachine::OnGotIp(const uint8_t ip[4])
{
    WifiAction a;
    if (m_state == WifiState::NoCredentials)
        return a;                           // stale: cleared since
    a.changed = m_state != WifiState::Connected || memcmp(m_ip, ip, 4) != 0;
    a.cancel_retry = true;
    m_state     = WifiState::Connected;
    m_in_flight = true;
    m_failures  = 0;
    memcpy(m_ip, ip, 4);
    return a;
}

WifiAction WifiMachine::OnLostIp()
{
    WifiAction a;
    if (m_state != WifiState::Connected)
        return a;
    m_state = WifiState::Connecting;        // the driver asks DHCP again by itself
    memset(m_ip, 0, sizeof(m_ip));
    a.changed = true;
    return a;
}

WifiAction WifiMachine::OnDisconnected(uint8_t reason)
{
    WifiAction a;
    if (m_state == WifiState::NoCredentials)
        return a;                           // stale: cleared since
    if (reason == CB_WIFI_REASON_LEAVE && m_leave_expected) {
        m_leave_expected = false;           // ours, not a failure
        return a;
    }
    m_leave_expected = false;
    if (!m_in_flight)
        return a;                           // nothing was attempted: a stray event

    WifiState prev = m_state;
    m_in_flight = false;
    m_reason    = reason;
    memset(m_ip, 0, sizeof(m_ip));
    m_state     = WifiState::Retrying;
    a.changed   = prev != WifiState::Retrying;

    if (is_auth_failure(reason)) {
        // Wrong password: stop until new credentials or a reboot
        m_halted = true;
        a.cancel_retry = true;
        return a;
    }
    a.retry_ms = backoff_ms(m_failures);    // 1 s, doubling
    m_failures++;
    return a;
}

WifiAction WifiMachine::OnRetryTimer()
{
    WifiAction a;
    if (m_state != WifiState::Retrying || m_in_flight || m_halted)
        return a;                           // stale timer
    m_in_flight = true;
    a.connect = true;                       // still Retrying: no event per attempt
    return a;
}
