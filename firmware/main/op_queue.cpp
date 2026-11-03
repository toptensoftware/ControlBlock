#include "op_queue.h"

void OpQueue::Init(const ConfigView *cfg, const OpHost &host)
{
    m_cfg     = cfg;
    m_host    = host;
    m_head    = m_count = 0;
    m_waiting = false;
    m_dropped = 0;
}

bool OpQueue::PushList(const ConfigView::Array &ops, bool release)
{
    if (ops.count > (uint32_t)(OP_QUEUE_SIZE - m_count)) {
        m_dropped++;
        return false;
    }
    for (uint32_t i = 0; i < ops.count; i++) {
        int slot = (m_head + m_count++) % OP_QUEUE_SIZE;
        m_ring[slot]    = m_cfg->Ptr(ops.items + i * 4);
        m_release[slot] = release;
    }
    return true;
}

void OpQueue::Hold(uint32_t ms)
{
    m_until   = m_host.now_ms(m_host.ctx) + ms;
    m_waiting = true;
}

bool OpQueue::Poll()
{
    if (m_waiting) {
        // Signed difference: right across the clock wrapping
        if ((int32_t)(m_host.now_ms(m_host.ctx) - m_until) < 0)
            return true;
        m_waiting = false;
    }
    if (m_count == 0)
        return false;

    const uint8_t *op = m_ring[m_head];
    m_releasing = m_release[m_head];
    m_head = (m_head + 1) % OP_QUEUE_SIZE;
    m_count--;

    uint32_t code = m_cfg->U32(op + config_layout::OP_CODE);
    if (code == config_layout::OP_DELAY)
        Hold(m_cfg->U32(op + config_layout::OP_DELAY_MS));
    m_host.run(*m_cfg, op, code, m_host.ctx);
    return true;
}
