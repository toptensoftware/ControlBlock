#include "op_queue.h"

void OpQueue::Init(const OpHost &host)
{
    m_host        = host;
    m_head        = m_count = 0;
    m_cur         = {};
    m_cur_next    = 0;
    m_waiting     = 0;
    m_next_inserted = 0;
    m_active      = false;
    m_run         = {};
    m_parked      = false;
    m_parked_run  = {};
    m_park_requested = false;
    m_generation  = 0;
    m_dropped     = m_errors = 0;
    m_last_error  = nullptr;
    m_params.Init();
}

bool OpQueue::PushList(uint32_t count, layoutdata::op *const *ops, bool release, const char *param)
{
    OpListRef ref;
    ref.ops = ops; ref.count = count; ref.release = release; ref.param = param;
    return PushLists(&ref, 1);
}

bool OpQueue::Reserve(const OpListRef *lists, int n, int extra, int8_t *slots, int &k)
{
    int needed = 0;
    for (int i = 0; i < n; i++)
        needed += lists[i].count != 0;
    if (needed > 0 && m_count + needed + extra > OP_QUEUE_SIZE) {
        m_dropped += needed;
        return false;
    }

    // Take every param slot first so a failure leaves nothing behind
    k = 0;
    for (int i = 0; i < n; i++) {
        if (lists[i].count == 0)
            continue;
        int slot = m_params.Acquire(lists[i].param, lists[i].count);
        if (slot == PARAM_FULL) {       // counted by the pool: give back what this call took
            for (int j = 0, q = 0; q < k; j++) {
                if (lists[j].count == 0)
                    continue;
                for (uint32_t r = 0; r < lists[j].count; r++)
                    m_params.Release(slots[q]);
                q++;
            }
            return false;
        }
        slots[k++] = (int8_t)slot;
    }
    return true;
}

void OpQueue::Insert(int at, const Entry &e)
{
    for (int i = m_count - 1; i >= at; i--)
        m_ring[(m_head + i + 1) % OP_QUEUE_SIZE] = m_ring[(m_head + i) % OP_QUEUE_SIZE];
    m_ring[(m_head + at) % OP_QUEUE_SIZE] = e;
    m_count++;
    m_waiting += (int)e.count;
}

void OpQueue::Insert(int at, const OpListRef *lists, int n, const int8_t *slots, int k)
{
    for (int i = 0, q = 0; q < k; i++) {
        if (lists[i].count == 0)
            continue;
        Entry e;
        e.ops     = lists[i].ops;
        e.count   = lists[i].count;
        e.slot    = slots[q];
        e.release = lists[i].release;
        Insert(at + q++, e);
    }
}

bool OpQueue::PushLists(const OpListRef *lists, int n)
{
    int8_t slots[OP_QUEUE_SIZE];
    int    k;
    if (!Reserve(lists, n, 0, slots, k))
        return false;
    Insert(m_count, lists, n, slots, k);
    return true;
}

bool OpQueue::PushListsNext(const OpListRef *lists, int n)
{
    bool   split = m_cur_next < m_cur.count;   // ops left in the current list
    int8_t slots[OP_QUEUE_SIZE];
    int    k;
    if (!Reserve(lists, n, split + 1, slots, k))    // + 1: room for a resume marker (ParkActive)
        return false;
    if (k == 0)
        return true;

    if (split) {                                // its rest becomes the front entry, param refs and all
        Entry rest   = m_cur;
        rest.ops    += m_cur_next;
        rest.count  -= m_cur_next;
        m_cur.count  = m_cur_next;
        m_waiting   -= (int)rest.count;         // Insert counts them back
        Insert(0, rest);
    }
    Insert(m_next_inserted, lists, n, slots, k);
    m_next_inserted += k;
    return true;
}

bool OpQueue::ParkActive()
{
    // PushListsNext kept room for the marker; the full check is belt and braces
    if (!m_active || m_next_inserted == 0 || m_parked || m_park_requested || m_count >= OP_QUEUE_SIZE)
        return false;
    Entry marker = { nullptr, 0, PARAM_NONE, false };
    Insert(m_next_inserted, marker);
    m_park_requested = true;
    return true;
}

// The active op is over, in whatever way: count an error and free its param
void OpQueue::Finish(OpStatus st)
{
    if (st == OpStatus::Error) {
        m_errors++;
        m_last_error = m_run.task.error;
    }
    m_active = false;
    m_params.Release(m_run.slot);
    m_run.slot = PARAM_NONE;
}

void OpQueue::Settle(OpStatus st)
{
    bool park = m_park_requested;
    m_park_requested = false;
    if (st != OpStatus::Pending)
        Finish(st);                             // a park request lapses: its marker will be skipped
    else if (park) {                            // step aside, task and param ref kept
        m_parked_run = m_run;
        m_parked     = true;
        m_active     = false;
        m_run        = {};
    }
}

bool OpQueue::Resume()
{
    m_run    = m_parked_run;
    m_parked = false;
    m_parked_run = {};
    m_active = true;
    m_next_inserted = 0;                        // its next-pushes go straight after it again
    Settle(m_host.poll ? m_host.poll(m_run.op, m_run.task, m_host.ctx) : OpStatus::Done);
    return true;
}

bool OpQueue::StartNext()
{
    while (m_cur_next >= m_cur.count) {         // the current list is spent: take the next
        if (m_count == 0)
            return m_parked ? Resume() : false; // (a parked op always has a marker: defensive)
        m_cur      = m_ring[m_head];
        m_cur_next = 0;
        m_head     = (m_head + 1) % OP_QUEUE_SIZE;
        m_count--;
        if (!m_cur.ops && m_parked)             // a resume marker (one with nothing parked is skipped)
            return Resume();
    }
    const layoutdata::op *op = m_cur.ops[m_cur_next++];
    m_waiting--;
    m_next_inserted = 0;                        // a new op: its next-pushes go straight after it

    m_run.op   = op;
    m_run.slot = m_cur.slot;
    m_run.task = {};
    m_run.task.param      = m_params.Get(m_cur.slot);
    m_run.task.releasing  = m_cur.release;
    m_run.task.generation = ++m_generation;

    m_active = true;                            // before start: the op may ParkActive from it
    Settle(m_host.start ? m_host.start(op, m_run.task, m_host.ctx) : OpStatus::Done);
    return true;
}

bool OpQueue::Poll()
{
    if (m_active) {
        Settle(m_host.poll ? m_host.poll(m_run.op, m_run.task, m_host.ctx) : OpStatus::Done);
        if (m_active)
            return true;                        // still Pending
    }
    return StartNext();
}

void OpQueue::Cancel()
{
    if (m_active) {
        if (m_host.cancel)
            m_host.cancel(m_run.op, m_run.task, m_host.ctx);
        Finish(OpStatus::Done);         // abandoned: not an error
    }
    if (m_parked) {
        if (m_host.cancel)
            m_host.cancel(m_parked_run.op, m_parked_run.task, m_host.ctx);
        m_params.Release(m_parked_run.slot);
        m_parked = false;
        m_parked_run = {};
    }
    m_park_requested = false;
    // Each op still waiting holds one reference on its list's param slot (markers hold none)
    for (; m_cur_next < m_cur.count; m_cur_next++)
        m_params.Release(m_cur.slot);
    while (m_count > 0) {
        for (uint32_t i = 0; i < m_ring[m_head].count; i++)
            m_params.Release(m_ring[m_head].slot);
        m_head = (m_head + 1) % OP_QUEUE_SIZE;
        m_count--;
    }
    m_head = 0;
    m_waiting = 0;
    m_next_inserted = 0;
}
