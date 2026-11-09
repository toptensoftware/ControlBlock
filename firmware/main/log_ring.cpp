#include "log_ring.h"
#include "clock_text.h"         // utf8_complete_len
#include <string.h>
#include <assert.h>

static const size_t HEADER = 6;     // seq u32, len u16

LogRing::LogRing(void *buf, size_t size, uint32_t firstSeq)
    : m_buf((uint8_t *)buf), m_size(size), m_firstSeq(firstSeq), m_nextSeq(firstSeq)
{
    assert(size >= HEADER + LOG_RING_LINE_MAX);
}

void LogRing::Append(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        char c = text[i];
        if (c == '\n')
            Commit();
        else if (c != '\r' && m_pendingLen < LOG_RING_LINE_MAX)
            m_pending[m_pendingLen++] = c;
    }
}

size_t LogRing::RecordSize(size_t off) const
{
    uint16_t len;
    memcpy(&len, m_buf + off + 4, 2);
    return HEADER + len;
}

void LogRing::EvictOldest()
{
    m_head += RecordSize(m_head);
    m_count--;
    m_firstSeq++;
    if (m_wrapped && m_head == m_wrapEnd)
    {
        m_head = 0;
        m_wrapped = false;
    }
}

void LogRing::Commit()
{
    // A cut (here or in a message cut short) can leave half a character at the end
    m_pendingLen = utf8_complete_len(m_pending, m_pendingLen);
    size_t n = HEADER + m_pendingLen;
    for (;;)
    {
        if (m_count == 0)
        {
            m_head = m_tail = 0;
            m_wrapped = false;
            break;
        }
        if (!m_wrapped)
        {
            if (m_tail + n <= m_size)
                break;
            m_wrapEnd = m_tail;         // continue at the start, evicting from there
            m_tail = 0;
            m_wrapped = true;
        }
        if (m_tail + n <= m_head)
            break;
        EvictOldest();
    }

    uint32_t seq = m_nextSeq++;
    uint16_t len = (uint16_t)m_pendingLen;
    memcpy(m_buf + m_tail, &seq, 4);
    memcpy(m_buf + m_tail + 4, &len, 2);
    memcpy(m_buf + m_tail + HEADER, m_pending, m_pendingLen);
    m_tail += n;
    m_count++;
    m_pendingLen = 0;
}

LogReadResult LogRing::Read(bool hasSince, uint32_t since, char *out, size_t cap) const
{
    LogReadResult res = { 0, m_nextSeq - 1, false };
    if (hasSince)
    {
        if ((int32_t)(since - (m_nextSeq - 1)) > 0)
        {
            hasSince = false;           // from the future: not ours
            res.dropped = true;
        }
        else if (m_count && (int32_t)(m_firstSeq - since) > 1)
            res.dropped = true;
    }

    size_t off = m_head;
    bool wrapped = m_wrapped;
    for (uint32_t i = 0; i < m_count; i++)
    {
        if (wrapped && off == m_wrapEnd)
        {
            off = 0;
            wrapped = false;
        }
        uint32_t seq;
        uint16_t len;
        memcpy(&seq, m_buf + off, 4);
        memcpy(&len, m_buf + off + 4, 2);
        if (!hasSince || (int32_t)(seq - since) > 0)
        {
            if (res.len + len + 1 > cap)
            {
                res.next = seq - 1;
                return res;
            }
            memcpy(out + res.len, m_buf + off + HEADER, len);
            res.len += len;
            out[res.len++] = '\n';
        }
        off += HEADER + len;
    }
    return res;
}
