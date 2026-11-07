#include "op_queue.h"

void OpQueue::Init(const OpHost &host)
{
    m_host        = host;
    m_head        = m_count = 0;
    m_cur         = {};
    m_cur_next    = 0;
    m_waiting     = 0;
    m_active      = false;
    m_active_op   = nullptr;
    m_active_slot = PARAM_NONE;
    m_task        = {};
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

bool OpQueue::PushLists(const OpListRef *lists, int n)
{
    int needed = 0;
    for (int i = 0; i < n; i++)
        needed += lists[i].count != 0;
    if (m_count + needed > OP_QUEUE_SIZE) {
        m_dropped += needed;
        return false;
    }

    // Take every param slot first so a failure leaves nothing behind
    int8_t slots[OP_QUEUE_SIZE];
    int    k = 0;
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

    k = 0;
    for (int i = 0; i < n; i++) {
        if (lists[i].count == 0)
            continue;
        Entry &e  = m_ring[(m_head + m_count++) % OP_QUEUE_SIZE];
        e.ops     = lists[i].ops;
        e.count   = lists[i].count;
        e.slot    = slots[k++];
        e.release = lists[i].release;
        m_waiting += (int)lists[i].count;
    }
    return true;
}

// The active op is over, in whatever way: count an error and free its param
void OpQueue::Finish(OpStatus st)
{
    if (st == OpStatus::Error) {
        m_errors++;
        m_last_error = m_task.error;
    }
    m_active = false;
    m_params.Release(m_active_slot);
    m_active_slot = PARAM_NONE;
}

void OpQueue::StartNext()
{
    if (m_cur_next >= m_cur.count) {            // the current list is spent: take the next
        m_cur      = m_ring[m_head];
        m_cur_next = 0;
        m_head     = (m_head + 1) % OP_QUEUE_SIZE;
        m_count--;
    }
    const layoutdata::op *op = m_cur.ops[m_cur_next++];
    m_waiting--;

    m_active_op   = op;
    m_active_slot = m_cur.slot;
    m_task        = {};
    m_task.param      = m_params.Get(m_cur.slot);
    m_task.releasing  = m_cur.release;
    m_task.generation = ++m_generation;

    OpStatus st = m_host.start ? m_host.start(op, m_task, m_host.ctx) : OpStatus::Done;
    m_active = true;
    if (st != OpStatus::Pending)
        Finish(st);
}

bool OpQueue::Poll()
{
    if (m_active) {
        OpStatus st = m_host.poll ? m_host.poll(m_active_op, m_task, m_host.ctx) : OpStatus::Done;
        if (st == OpStatus::Pending)
            return true;
        Finish(st);
    }
    if (m_waiting == 0)
        return false;
    StartNext();
    return true;
}

void OpQueue::Cancel()
{
    if (m_active) {
        if (m_host.cancel)
            m_host.cancel(m_active_op, m_task, m_host.ctx);
        Finish(OpStatus::Done);         // abandoned: not an error
    }
    // Each op still waiting holds one reference on its list's param slot
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
}
