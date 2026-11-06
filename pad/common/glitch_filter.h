// Glitch filter for one digital input, modelled on pigpio's gpioGlitchFilter (what urem
// uses: 5 ms on buttons, 1 ms on the encoder pins). Pure C++, injected time.
//
// Semantics (the exact choice, so the C3 and the tests agree):
//   - A level change is accepted only once the new level has been stable for at least
//     'steady_us' microseconds (>=, so a pulse of exactly steady_us passes). A level that
//     goes back before that is a glitch and is ignored completely (no event at all).
//   - An accepted change is reported with its ORIGINAL edge time (when the raw level
//     first changed), not the time the filter noticed it. pigpio delivers the alert
//     steady_us late; urem timestamps with the delivery time, which shifts every edge of
//     both encoder pins by the same constant, so the intervals between edges are the same.
//   - The filter is edge driven: call Update() on every raw edge and also periodically
//     (any time, with the current raw level) so that a change with no following edge is
//     accepted once its time has passed. One Update() accepts at most one change.
//   - While a change is pending the raw level may be sampled any number of times; the
//     filter assumes the raw level stayed at the pending level between two calls (so call
//     it on every edge).
#pragma once

#include <stdint.h>

class GlitchFilter
{
public:
    // 'level' is the initial (accepted) level
    void Init(uint32_t steady_us, bool level);

    // 'raw' is the raw level at 'now_us'. Returns true if a change was accepted by this
    // call: Level() is the new level and EdgeTime() when it first changed.
    bool Update(uint64_t now_us, bool raw);

    bool Level() const { return m_level; }
    uint64_t EdgeTime() const { return m_edge_time; }
    bool Pending() const { return m_pending; }

private:
    uint32_t m_steady = 0;
    bool     m_level = false;       // accepted level
    uint64_t m_edge_time = 0;       // when the accepted level first appeared
    bool     m_pending = false;     // a different raw level is waiting to become stable
    bool     m_pend_level = false;
    uint64_t m_pend_time = 0;
};
