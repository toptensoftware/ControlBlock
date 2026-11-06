#include "glitch_filter.h"

void GlitchFilter::Init(uint32_t steady_us, bool level)
{
    m_steady = steady_us;
    m_level = level;
    m_edge_time = 0;
    m_pending = false;
    m_pend_level = level;
    m_pend_time = 0;
}

bool GlitchFilter::Update(uint64_t now_us, bool raw)
{
    bool accepted = false;

    // The pending level has held until now (the raw level hasn't changed since the last
    // call, or this call would have been made on the edge)
    if (m_pending && now_us - m_pend_time >= m_steady) {
        m_level = m_pend_level;
        m_edge_time = m_pend_time;
        m_pending = false;
        accepted = true;
    }

    if (raw == m_level) {
        m_pending = false;                  // back to the accepted level: a glitch
    } else if (!m_pending || raw != m_pend_level) {
        m_pending = true;                   // a new change starts its stable period now
        m_pend_level = raw;
        m_pend_time = now_us;
    }
    return accepted;
}
