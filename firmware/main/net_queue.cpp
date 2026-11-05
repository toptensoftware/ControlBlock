#include "net_queue.h"
#include "op_exec.h"        // OpReached

#include <string.h>

void NetQueue::Init()
{
    memset(this, 0, sizeof(*this));
}

void NetQueue::Remove(uint32_t index)
{
    for (uint32_t i = index; i + 1 < m_count; i++)
        m_q[i] = m_q[i + 1];
    m_count--;
}

bool NetQueue::Submit(const NetRequest &r)
{
    if (m_count == NET_QUEUE_SIZE) {
        uint32_t victim = 0;
        while (victim < m_count && m_q[victim].wait)
            victim++;
        if (victim == m_count) {
            m_refused++;
            return false;
        }
        Remove(victim);
        m_dropped++;
    }
    m_q[m_count++] = r;
    return true;
}

bool NetQueue::Next(uint32_t now, NetRequest *out)
{
    if (m_busy || m_count == 0)
        return false;
    m_flight       = m_q[0];
    m_flight_start = now;
    m_flight_dead  = false;
    m_busy         = true;
    Remove(0);
    *out = m_flight;
    return true;
}

void NetQueue::Complete(uint32_t generation, NetResult result, uint16_t status, uint32_t body_len, const uint8_t *body)
{
    if (!m_busy || m_flight.generation != generation)
        return;                                         // not the one in flight: nothing to finish
    bool wanted = m_flight.wait && !m_flight_dead;
    if (m_flight.wait && m_flight_dead)
        m_late++;
    m_busy = false;
    if (!wanted)
        return;

    NetReply rep = {};
    rep.result    = result;
    rep.status    = status;
    rep.truncated = NetClipResponse(&body_len);
    rep.body_len  = body_len;
    rep.body      = body_len ? body : nullptr;
    m_reply       = rep;
    m_reply_gen   = generation;
    m_has_reply   = true;
}

bool NetQueue::TakeReply(uint32_t generation, NetReply *out)
{
    if (!m_has_reply || m_reply_gen != generation)
        return false;
    *out        = m_reply;
    m_has_reply = false;
    return true;
}

void NetQueue::Abandon(uint32_t generation)
{
    for (uint32_t i = 0; i < m_count;) {
        if (m_q[i].wait && m_q[i].generation == generation)
            Remove(i);
        else
            i++;
    }
    if (m_busy && m_flight.wait && m_flight.generation == generation)
        m_flight_dead = true;
    if (m_has_reply && m_reply_gen == generation)
        m_has_reply = false;
}

void NetQueue::Expire(uint32_t now)
{
    if (!m_busy || !m_flight.wait || m_flight_dead)
        return;
    if (!OpReached(now, m_flight_start + NET_HTTP_TIMEOUT_MS))
        return;
    m_flight_dead = true;
    NetReply rep  = {};
    rep.result    = NetResult::Timeout;
    m_reply       = rep;
    m_reply_gen   = m_flight.generation;
    m_has_reply   = true;
}
