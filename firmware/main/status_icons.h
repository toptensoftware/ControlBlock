#pragma once
#include <stdint.h>
#include "wifi_state.h"

// Which status bar icons show (docs/status-bar.md): Wi-Fi state, busy while
// the op queue runs (kept at least STATUS_BUSY_MIN_MS), and an error for
// STATUS_ERROR_SHOW_MS after an external failure. Hosted: Wi-Fi only.
// Pure (host tested), clock injected; not thread safe (status.cpp locks).

// Icon bits (status_bar_content_t.icons); bit n draws STATUS_ICON image n
#define STATUS_ICON_WIFI_CONNECTED  0x01    // the Wi-Fi waves
#define STATUS_ICON_WIFI_OFF        0x02    // a dot: not connected
#define STATUS_ICON_BUSY            0x04
#define STATUS_ICON_ERROR           0x08
#define STATUS_ICON_IMAGES          4

#define STATUS_BUSY_MIN_MS          500     // busy shows at least this long
#define STATUS_ERROR_SHOW_MS        5000    // an error shows this long
#define STATUS_NO_DEADLINE          (-1)    // nothing to wake for

class StatusIcons
{
public:
    void SetWifi(WifiState state) { m_wifi = state; }

    // The op queue started (true) or emptied (false)
    void SetBusy(bool busy, int64_t now_ms);

    // The net worker has requests queued or in flight (true) or none (false).
    // Busy shows while either this or the op queue is busy.
    void SetNetBusy(bool busy, int64_t now_ms);

    // Something outside the device failed: show the error icon
    void ReportError(int64_t now_ms);

    // Hosted clears busy and error and ignores them until standalone again
    void SetHosted(bool hosted);

    // STATUS_ICON_* bits showing at `now_ms`
    uint8_t Visible(int64_t now_ms) const;

    // Ms until Visible() next changes by itself, or STATUS_NO_DEADLINE
    int64_t NextDeadlineMs(int64_t now_ms) const;

private:
    WifiState m_wifi = WifiState::NoCredentials;
    bool      m_hosted = false;
    bool      m_queue_busy = false;     // the op queue is running
    bool      m_net_busy = false;       // requests pending in the net worker
    int64_t   m_busy_until = 0;         // the minimum after the last start

    bool Busy() const { return m_queue_busy || m_net_busy; }
    void SetSource(bool *source, bool busy, int64_t now_ms);
    bool      m_error = false;
    int64_t   m_error_until = 0;
};

// The status task's wait: the nearer of the clock's and the icons' (ms, or
// STATUS_NO_DEADLINE for none)
int64_t status_wait_ms(int64_t clock_ms, int64_t icons_ms);
