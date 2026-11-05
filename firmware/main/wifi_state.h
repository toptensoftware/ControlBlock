#pragma once

// The Wi-Fi connection state machine and its retry backoff (docs/wifi.md).
// Pure: driver events in, "what to do now" out. The device layer (wifi.cpp)
// turns driver callbacks into calls here, runs the actions it returns, and
// owns the retry timer.
//
// Not thread safe: one task (the Wi-Fi task) drives it.

#include <stdint.h>

// The connection state, as WIFI_STATUS / WIFI_STATE report it
enum class WifiState : uint8_t
{
    NoCredentials = 0,      // nothing stored; the driver isn't running
    Connecting    = 1,      // the first attempt: associating or waiting for an IP
    Connected     = 2,      // has an IP
    Retrying      = 3,      // an attempt failed or the link dropped: waiting to retry, or retrying
};

// WIFI_STATUS response fields. The device layer fills it (rssi and mac come
// from the driver).
struct WifiStatus
{
    uint8_t state;          // WifiState
    uint8_t reason;         // last disconnect reason (esp wifi_err_reason_t), 0 if none
    int8_t  rssi;           // dBm while connected, else 0
    uint8_t ip[4];          // a.b.c.d while Connected, else 0
    uint8_t mac[6];         // station MAC; zeros with no credentials
};

// Retry delays: double from the minimum to the cap, reset by success
#define WIFI_RETRY_MIN_MS   1000
#define WIFI_RETRY_MAX_MS   30000

// Disconnect reasons the machine treats specially. These are esp_wifi's
// wifi_err_reason_t values (wifi.cpp static_asserts them); they are plain
// numbers here so this file builds on the host.
//   LEAVE: "disassociated: the station is leaving", which is what our own
//   esp_wifi_disconnect() / stop produces.
//   Authentication failures (wrong password: MIC failure, 802.1X, auth fail):
//   retrying won't help and some routers lock out, so the machine stops (state stays Retrying, reason set,
//   no timer) until new credentials arrive or the device reboots.
#define CB_WIFI_REASON_LEAVE         8
#define CB_WIFI_REASON_MIC_FAILURE   14
#define CB_WIFI_REASON_8021X_FAILED  23
#define CB_WIFI_REASON_AUTH_FAIL     202

// What the device layer must do after an event. Several can be set; run in
// this order: stop, disconnect, start, connect.
struct WifiAction
{
    bool    start = false;          // bring the driver up (init, mode, config, start)
    bool    stop = false;           // take it down (credentials cleared)
    bool    disconnect = false;     // drop the current attempt or link first
    bool    connect = false;        // (re)load the credentials and connect now
    bool    cancel_retry = false;   // stop the retry timer
    int32_t retry_ms = -1;          // >= 0: (re)start the retry timer, replacing any
    bool    changed = false;        // the state or the address changed: post WIFI_STATE
};

class WifiMachine
{
public:
    // The stored credentials appeared or changed (present), or were cleared.
    // Resets the backoff and connects at once.
    WifiAction OnCredentials(bool present);
    WifiAction OnGotIp(const uint8_t ip[4]);
    WifiAction OnLostIp();
    // STA_DISCONNECTED: an attempt failed or the link dropped
    WifiAction OnDisconnected(uint8_t reason);
    // The retry timer fired
    WifiAction OnRetryTimer();

    WifiState GetState() const { return m_state; }
    uint8_t Reason() const { return m_reason; }
    const uint8_t *Ip() const { return m_ip; }

private:
    WifiState m_state = WifiState::NoCredentials;
    uint8_t   m_reason = 0;
    uint8_t   m_ip[4] = {};
    int       m_failures = 0;           // failed attempts since the last success
    bool      m_halted = false;         // authentication failed: no retries until new credentials
    bool      m_in_flight = false;     // an attempt or a link exists (connect issued, no disconnect yet)
    bool      m_leave_expected = false; // our own disconnect's event is still to come
};
