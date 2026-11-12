#pragma once

// Counters for the host link's receive path, for the once-a-second stats line
// (main.cpp, CB_STATS_LOG; wip/wip-upload-throughput.md). Writers on any task
// (the TinyUSB task counts transfers, the protocol task the rest); one reader
// takes and resets them each second. Lock free: each update is an atomic op or
// two, so the counting stays on in every build.

#include <stddef.h>
#include <stdint.h>
#include "port.h"

// One second's worth (from LinkCounters::Take)
struct LinkSample
{
    uint32_t transfers;         // bulk OUT transfers completed (TinyUSB's rx callback)
    uint32_t bytes;             // and their bytes
    uint32_t wakes;             // protocol task wake-ups
    uint32_t reads;             // reads of the RX FIFO that found bytes waiting
    uint32_t fifo_min;          // bytes waiting in the RX FIFO at those reads (0s if none)
    uint32_t fifo_avg;
    uint32_t fifo_max;
    uint32_t receive_cycles;    // CPU cycles in Protocol::Receive
};

class LinkCounters
{
public:
    void Transfer(uint32_t bytes)       { Add(&m_transfers, 1); Add(&m_bytes, bytes); }
    void Wake()                         { Add(&m_wakes, 1); }
    void ReceiveCycles(uint32_t cycles) { Add(&m_receive_cycles, cycles); }

    // A read that found `waiting` bytes in the FIFO
    void Read(uint32_t waiting)
    {
        Add(&m_reads, 1);
        Add(&m_fifo_sum, waiting);
        // Stored inverted, so 0 means "none yet" for both
        Max(&m_fifo_min_inv, ~waiting);
        Max(&m_fifo_max, waiting);
    }

    // The counts since the last Take; resets them
    LinkSample Take()
    {
        LinkSample s = {};
        s.transfers      = port::AtomicExchange(&m_transfers, 0);
        s.bytes          = port::AtomicExchange(&m_bytes, 0);
        s.wakes          = port::AtomicExchange(&m_wakes, 0);
        s.reads          = port::AtomicExchange(&m_reads, 0);
        uint32_t sum     = port::AtomicExchange(&m_fifo_sum, 0);
        uint32_t min_inv = port::AtomicExchange(&m_fifo_min_inv, 0);
        s.fifo_max       = port::AtomicExchange(&m_fifo_max, 0);
        s.receive_cycles = port::AtomicExchange(&m_receive_cycles, 0);
        s.fifo_min       = min_inv ? ~min_inv : 0;
        s.fifo_avg       = s.reads ? sum / s.reads : 0;
        return s;
    }

private:
    static void Add(uint32_t *p, uint32_t v)
    {
        uint32_t old = port::AtomicLoad(p);
        while (!port::AtomicCompareExchange(p, old, old + v))
            old = port::AtomicLoad(p);
    }
    static void Max(uint32_t *p, uint32_t v)
    {
        uint32_t old = port::AtomicLoad(p);
        while (v > old && !port::AtomicCompareExchange(p, old, v))
            old = port::AtomicLoad(p);
    }

    uint32_t m_transfers = 0, m_bytes = 0, m_wakes = 0, m_reads = 0;
    uint32_t m_fifo_sum = 0, m_fifo_min_inv = 0, m_fifo_max = 0;
    uint32_t m_receive_cycles = 0;
};

// " | usb rx: ..." for the stats line, or "" for a second with no transfers,
// reads or checks. `checks` and `check_cycles` are the image store's opacity
// checks that second (ImageStoreStats deltas). Truncated to fit `size`.
void FormatLinkSample(const LinkSample &s, uint32_t checks, uint32_t check_cycles, uint32_t cycles_per_us,
                      char *out, size_t size);
