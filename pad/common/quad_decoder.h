// Quadrature decoder that reproduces urem's updateEncoder (urem/frontend/gpio.js) exactly.
// Pure C++, injected time in microseconds (64-bit, so no wrap).
//
// urem's algorithm, which "works great" and which ControlBlock must match:
//   - Edge() is called once per (filtered) level change of A or B, with both levels.
//   - Every call, valid or not, measures the time since the previous call, caps it at
//     1 s, and stores it in a ring of the last 4; the ring starts as 4 x 1 s.
//     The velocity interval is the average of the ring (sum / 4, integer microseconds).
//     urem's value is in ms and may be fractional; on a millisecond grid this is the
//     same number x 1000, otherwise it is floored.
//     The very first call has no previous time and counts as the 1 s cap (urem starts
//     its clock at 0, so its first elapsed is always over 1 s).
//   - The 2-bit state (A << 1 | B) transition goes through the 8-entry Gray table
//     (CW 00>01>11>10>00, CCW the reverse). Anything else (no change, or both bits changed)
//     is ignored, but the new state is still remembered as the previous one.
//   - A valid transition moves a running edge position by +/-1 and, when that position
//     is a multiple of 4 (urem's `encoderPos % 4 == 0`, so also negative multiples and
//     also when reached going the other way), a tick of that direction fires. So a
//     reversal part way through a detent fires a tick of the opposite sign when the
//     edge position comes back to a multiple of 4, even if no tick of the first
//     direction fired: ticks are not "full detents travelled", they are crossings of
//     multiples of 4 in the running edge position, exactly as in urem.
//   - The detent position is the running sum of ticks, wrapping as a uint32.
#pragma once

#include <stdint.h>

constexpr uint32_t QUAD_INTERVAL_CAP_US = 1000000;     // urem caps each interval at 1000 ms

// The STATE packet carries the interval as a u16 of microseconds: saturate to 65535
inline uint16_t QuadIntervalToU16(uint32_t us)
{
    return us > 0xFFFF ? (uint16_t)0xFFFF : (uint16_t)us;
}

class QuadDecoder
{
public:
    // a, b: the initial levels; 'detents': the starting detent position
    void Init(bool a, bool b, uint32_t detents = 0);

    // One call per level change (urem's updateEncoder); a and b are the levels now.
    // Returns the tick (+1 or -1), or 0. Time going backwards counts as 0 elapsed.
    int Edge(uint64_t time_us, bool a, bool b);

    // The average of the last 4 inter-call intervals, as of the latest call
    uint32_t IntervalUs() const { return m_sum / 4; }

    uint32_t Detents() const { return m_detents; }
    int32_t EdgePosition() const { return m_edge_pos; }

private:
    uint32_t m_ring[4];
    uint32_t m_sum = 0;
    uint32_t m_index = 0;
    uint64_t m_last_time = 0;
    bool     m_have_last = false;
    uint8_t  m_prev = 0;            // A << 1 | B
    int32_t  m_edge_pos = 0;
    uint32_t m_detents = 0;
};
