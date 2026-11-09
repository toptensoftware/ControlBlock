#include "status_icons.h"

// A start from either source restarts the minimum
void StatusIcons::SetSource(bool *source, bool busy, int64_t now_ms)
{
    if (m_hosted)
        return;
    if (busy && !*source)
        m_busy_until = now_ms + STATUS_BUSY_MIN_MS;
    *source = busy;
}

void StatusIcons::SetBusy(bool busy, int64_t now_ms)    { SetSource(&m_queue_busy, busy, now_ms); }
void StatusIcons::SetNetBusy(bool busy, int64_t now_ms) { SetSource(&m_net_busy, busy, now_ms); }

void StatusIcons::ReportError(int64_t now_ms)
{
    if (m_hosted)
        return;
    m_error = true;
    m_error_until = now_ms + STATUS_ERROR_SHOW_MS;
}

void StatusIcons::SetHosted(bool hosted)
{
    m_hosted = hosted;
    m_queue_busy = false;
    m_net_busy = false;
    m_busy_until = 0;
    m_error = false;
}

uint8_t StatusIcons::Visible(int64_t now_ms) const
{
    uint8_t bits = m_wifi == WifiState::Connected ? STATUS_ICON_WIFI_CONNECTED : STATUS_ICON_WIFI_OFF;
    if (!m_hosted) {
        // Busy and error share one place: the error wins
        if (m_error && now_ms < m_error_until)
            bits |= STATUS_ICON_ERROR;
        else if (Busy() || now_ms < m_busy_until)
            bits |= STATUS_ICON_BUSY;
    }
    return bits;
}

int64_t StatusIcons::NextDeadlineMs(int64_t now_ms) const
{
    int64_t busy = !m_hosted && !Busy() && now_ms < m_busy_until ? m_busy_until - now_ms : STATUS_NO_DEADLINE;
    int64_t error = !m_hosted && m_error && now_ms < m_error_until ? m_error_until - now_ms : STATUS_NO_DEADLINE;
    return status_wait_ms(busy, error);
}

int64_t status_wait_ms(int64_t clock_ms, int64_t icons_ms)
{
    if (clock_ms == STATUS_NO_DEADLINE)
        return icons_ms;
    if (icons_ms == STATUS_NO_DEADLINE)
        return clock_ms;
    return clock_ms < icons_ms ? clock_ms : icons_ms;
}
