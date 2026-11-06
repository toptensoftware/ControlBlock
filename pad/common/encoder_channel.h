// One rotary encoder: a glitch filter on each pin feeding a QuadDecoder, which together
// reproduce urem's encoder path (pigpio glitchFilter on A and B, then updateEncoder on
// every accepted level change). Pure C++, injected time in microseconds.
//
// Deliberate difference to urem: urem's updateEncoder re-reads both pins with
// digitalRead() when the alert for ONE pin arrives, so it sees the raw level of the
// other pin, which may have changed but not yet passed its filter. Here each accepted
// change calls the decoder with the FILTERED levels of both pins as of that change, so
// every call differs from the previous state by exactly one bit. The two agree on clean
// and on bouncing streams; they can differ when the two pins change within the filter
// time of each other (a rare overlap that urem itself decodes by chance).
#pragma once

#include <stdint.h>
#include "glitch_filter.h"
#include "quad_decoder.h"

constexpr uint32_t PAD_ENCODER_FILTER_US = 1000;       // urem: 1 ms on the encoder pins

struct QuadTick
{
    uint64_t time_us;       // the edge time (original, before the filter delay)
    int      dir;           // +1 or -1
    uint32_t interval_us;   // the velocity interval handed over with this tick
};

class EncoderChannel
{
public:
    void Init(uint32_t filter_us, bool a, bool b, uint32_t detents = 0);

    // Raw pin levels at 'now_us'. Call on every pin edge and periodically (so a pending
    // change is accepted in time). Writes up to 2 ticks to 'out'; returns how many.
    int Update(uint64_t now_us, bool raw_a, bool raw_b, QuadTick out[2]);

    uint32_t Detents() const { return m_dec.Detents(); }
    int32_t EdgePosition() const { return m_dec.EdgePosition(); }
    // The interval handed over with the latest tick (1 s until the first tick)
    uint32_t LastTickIntervalUs() const { return m_last_interval; }

private:
    GlitchFilter m_fa, m_fb;
    QuadDecoder  m_dec;
    bool     m_la = false, m_lb = false;    // filtered levels
    uint32_t m_last_interval = QUAD_INTERVAL_CAP_US;
};
