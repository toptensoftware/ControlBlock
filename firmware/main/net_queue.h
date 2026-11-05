#pragma once

// The request queue in front of the one net worker (docs/standalone-ops-plan.md
// section 3, "HTTP worker rules"). Platform independent, no locking (the device
// layer serialises); the clock is passed in.
//
//   - Bounded (NET_QUEUE_SIZE), strictly FIFO: a GET waits behind queued posts.
//   - Full: the oldest fire-and-forget request is dropped to make room. A request
//     someone waits on (`wait`) is never dropped; if nothing else can go, the new
//     request is refused.
//   - One request in flight at a time (Next hands out the next only after Complete).
//   - A waited request's reply is kept for TakeReply. If the waiter gave up
//     (Abandon) or the request timed out (Expire), the worker's late Complete is
//     counted and ignored.

#include "net_host.h"

#define NET_QUEUE_SIZE  8

class NetQueue
{
public:
    void Init();

    // Producer (the op task). False: refused (every queued request is waited on).
    bool Submit(const NetRequest &r);

    // Worker: take the oldest request. False when the queue is empty or a request is in flight.
    bool Next(uint32_t now, NetRequest *out);
    // Worker: the in-flight request (identified by its generation) is finished. `body_len` is the
    // full length; the reply keeps it clipped.
    // `body` (may be null) is kept in the reply for the waiter.
    void Complete(uint32_t generation, NetResult result, uint16_t status, uint32_t body_len, const uint8_t *body = nullptr);

    // Waiter: the reply for `generation`, once. False if none yet (or it was dropped).
    bool TakeReply(uint32_t generation, NetReply *out);
    // Waiter gives up (cancel on takeover): its queued waited request goes, an in-flight one's
    // reply will be dropped. Fire-and-forget requests are untouched.
    void Abandon(uint32_t generation);
    // Give up on an in-flight waited request that has run NET_HTTP_TIMEOUT_MS.
    void Expire(uint32_t now);

    uint32_t Count() const   { return m_count; }
    bool     Busy() const    { return m_busy; }
    uint32_t Dropped() const { return m_dropped; }      // fire-and-forget requests dropped to make room
    uint32_t Refused() const { return m_refused; }
    uint32_t Late() const    { return m_late; }         // replies that came after the waiter had gone

private:
    NetRequest  m_q[NET_QUEUE_SIZE];                    // oldest first, packed at the front
    uint32_t    m_count;

    bool        m_busy;                                 // a request is in flight
    NetRequest  m_flight;
    uint32_t    m_flight_start;
    bool        m_flight_dead;                          // its waiter is gone or it timed out: drop the answer

    bool        m_has_reply;
    uint32_t    m_reply_gen;
    NetReply    m_reply;

    uint32_t    m_dropped, m_refused, m_late;

    void Remove(uint32_t index);
};
