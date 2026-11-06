#include "encoder_channel.h"

void EncoderChannel::Init(uint32_t filter_us, bool a, bool b, uint32_t detents)
{
    m_fa.Init(filter_us, a);
    m_fb.Init(filter_us, b);
    m_dec.Init(a, b, detents);
    m_la = a;
    m_lb = b;
    m_last_interval = QUAD_INTERVAL_CAP_US;
}

int EncoderChannel::Update(uint64_t now_us, bool raw_a, bool raw_b, QuadTick out[2])
{
    // Both filters are always updated together, so changes come out in edge order
    // (equal filter times: the earlier edge expires first or in the same call)
    bool ca = m_fa.Update(now_us, raw_a);
    bool cb = m_fb.Update(now_us, raw_b);

    int n = 0;
    for (int i = 0; i < 2; i++) {
        // Next change in time order (A first on a tie)
        bool take_b;
        if (ca && cb)
            take_b = m_fb.EdgeTime() < m_fa.EdgeTime();
        else if (ca)
            take_b = false;
        else if (cb)
            take_b = true;
        else
            break;

        uint64_t t;
        if (take_b) {
            m_lb = m_fb.Level();
            t = m_fb.EdgeTime();
            cb = false;
        } else {
            m_la = m_fa.Level();
            t = m_fa.EdgeTime();
            ca = false;
        }

        int dir = m_dec.Edge(t, m_la, m_lb);
        if (dir != 0) {
            m_last_interval = m_dec.IntervalUs();
            out[n].time_us = t;
            out[n].dir = dir;
            out[n].interval_us = m_last_interval;
            n++;
        }
    }
    return n;
}
