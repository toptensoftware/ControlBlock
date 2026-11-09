#pragma once

// A ring of recent log lines for the diagnostics endpoints (/log). Pure and
// host tested; log_capture.cpp feeds it on the device (and does the locking).
//
// Lines are stored whole as records [seq u32][len u16][text], each with a
// 32-bit sequence number (wraps). When the buffer is full the oldest whole
// lines are evicted. Text arrives in pieces (Append); a line is committed at
// its '\n' ('\r' is dropped), and is cut at LOG_RING_LINE_MAX characters.

#include <stdint.h>
#include <stddef.h>

#define LOG_RING_LINE_MAX   240

struct LogReadResult
{
    size_t   len;           // bytes written to out (whole lines, each ending '\n')
    uint32_t next;          // pass as `since` next time: the last line returned (or the newest, if none)
    bool     dropped;       // lines after `since` were evicted (or `since` wasn't from this ring)
};

class LogRing
{
public:
    // buf must hold at least one longest record (LOG_RING_LINE_MAX + 6 bytes)
    LogRing(void *buf, size_t size, uint32_t firstSeq = 1);

    void Append(const char *text, size_t len);

    // Lines after `since` (all lines when !hasSince), as many whole lines as fit in cap.
    // A `since` newer than the newest line (e.g. from before a reboot) reads everything, as dropped.
    LogReadResult Read(bool hasSince, uint32_t since, char *out, size_t cap) const;

    uint32_t FirstSeq() const { return m_firstSeq; }    // oldest line held
    uint32_t NextSeq() const  { return m_nextSeq; }     // the next line's number
    uint32_t Count() const    { return m_count; }

private:
    void Commit();
    void EvictOldest();
    size_t RecordSize(size_t off) const;

    uint8_t *m_buf;
    size_t   m_size;
    size_t   m_head = 0;        // oldest record
    size_t   m_tail = 0;        // where the next record goes
    size_t   m_wrapEnd = 0;     // end of the data before offset 0, while wrapped
    bool     m_wrapped = false; // data is [head, wrapEnd) + [0, tail), else [head, tail)
    uint32_t m_count = 0;
    uint32_t m_firstSeq;
    uint32_t m_nextSeq;
    char     m_pending[LOG_RING_LINE_MAX];
    size_t   m_pendingLen = 0;
};
