#include "quad_decoder.h"

namespace
{
    // Indexed by (previous << 2) | current. CW: 00>01>11>10>00, CCW the reverse.
    const int8_t TRANSITIONS[16] = {
        /* 00>00 */ 0,  /* 00>01 */ +1, /* 00>10 */ -1, /* 00>11 */ 0,
        /* 01>00 */ -1, /* 01>01 */ 0,  /* 01>10 */ 0,  /* 01>11 */ +1,
        /* 10>00 */ +1, /* 10>01 */ 0,  /* 10>10 */ 0,  /* 10>11 */ -1,
        /* 11>00 */ 0,  /* 11>01 */ -1, /* 11>10 */ +1, /* 11>11 */ 0,
    };
}

void QuadDecoder::Init(bool a, bool b, uint32_t detents)
{
    for (int i = 0; i < 4; i++)
        m_ring[i] = QUAD_INTERVAL_CAP_US;
    m_sum = 4 * QUAD_INTERVAL_CAP_US;
    m_index = 0;
    m_last_time = 0;
    m_have_last = false;
    m_prev = (uint8_t)((a ? 2 : 0) | (b ? 1 : 0));
    m_edge_pos = 0;
    m_detents = detents;
}

int QuadDecoder::Edge(uint64_t time_us, bool a, bool b)
{
    // How long since the last call (every call counts, valid or not)
    uint64_t elapsed = QUAD_INTERVAL_CAP_US;
    if (m_have_last) {
        elapsed = time_us >= m_last_time ? time_us - m_last_time : 0;
        if (elapsed > QUAD_INTERVAL_CAP_US)
            elapsed = QUAD_INTERVAL_CAP_US;
    }
    m_last_time = time_us;
    m_have_last = true;

    m_sum = m_sum - m_ring[m_index] + (uint32_t)elapsed;
    m_ring[m_index] = (uint32_t)elapsed;
    m_index = (m_index + 1) & 3;

    uint8_t cur = (uint8_t)((a ? 2 : 0) | (b ? 1 : 0));
    int dir = TRANSITIONS[(m_prev << 2) | cur];
    m_prev = cur;

    if (dir != 0) {
        m_edge_pos += dir;
        if ((m_edge_pos % 4) == 0) {
            m_detents += (uint32_t)dir;     // -1 wraps
            return dir;
        }
    }
    return 0;
}
