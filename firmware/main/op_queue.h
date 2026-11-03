#pragma once

// The global op queue (docs/standalone.md "Actions"): a ring of pointers to
// ops in the config blob, one started per Poll(). Platform independent and
// single threaded: the device layer serialises Push (from triggers) and Poll
// (from the op task).
//
// `delay` is handled here: it holds the queue until its time has passed (as
// Hold() lets any op do).
// Every op, delay included, is also passed to the host's `run` when it
// starts; that is where the executors for the other ops plug in (they are
// fire and forget for now; timed ones such as a waiting script will need a
// poll hook).

#include <stdint.h>
#include "config_view.h"

#define OP_QUEUE_SIZE 32

struct OpHost
{
    void *ctx;
    uint32_t (*now_ms)(void *ctx);                                                  // wrapping millisecond clock
    void (*run)(const ConfigView &cfg, const uint8_t *op, uint32_t code, void *ctx);   // an op starts
};

class OpQueue
{
public:
    void Init(const ConfigView *cfg, const OpHost &host);

    // Add `count` ops (an array of op pointers in the blob) all or nothing.
    // A list that doesn't fit is dropped and counted.
    // `release`: the list was triggered by a `release` event (see Releasing).
    bool PushList(const ConfigView::Array &ops, bool release = false);

    // For the `run` callback of the op being started: it came from a `release`
    // binding (a hidKey `hold` is up there, down elsewhere).
    bool Releasing() const { return m_releasing; }

    // For the `run` callback: hold the queue for `ms` after this op (delay
    // does the same for itself).
    void Hold(uint32_t ms);

    // Start the next op if not held by a delay. True while there is work
    // (an op was started, or a delay is still running); false when idle.
    bool Poll();

    int      Count() const   { return m_count; }
    uint32_t Dropped() const { return m_dropped; }

private:
    const ConfigView *m_cfg = nullptr;
    OpHost            m_host = {};
    const uint8_t    *m_ring[OP_QUEUE_SIZE];
    bool              m_release[OP_QUEUE_SIZE];     // parallel to m_ring
    bool              m_releasing = false;
    int               m_head = 0, m_count = 0;
    bool              m_waiting = false;
    uint32_t          m_until = 0;
    uint32_t          m_dropped = 0;
};
