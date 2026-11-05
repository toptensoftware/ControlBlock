#pragma once

// The trigger param pool (docs/standalone-ops-plan.md section 1 G). A trigger's
// `param` string is copied into a slot and shared by every op pushed for that
// trigger: the slot is refcounted by the op count and freed when the last op
// has finished. Platform independent and single threaded (the queue's owner
// serialises).
//
// An empty or absent param takes no slot (Get() of PARAM_NONE is ""), so the
// common case costs nothing and can't be dropped. A param longer than a slot is
// truncated and counted; a full pool refuses the trigger (PARAM_FULL) and counts
// it.

#include <stdint.h>
#include <string.h>

#define PARAM_SLOTS       8
#define PARAM_SLOT_BYTES  48        // including the NUL
#define PARAM_NONE        (-1)      // no slot needed
#define PARAM_FULL        (-2)      // no slot free

class ParamPool
{
public:
    void Init()
    {
        memset(m_refs, 0, sizeof(m_refs));
        m_dropped = m_truncated = 0;
    }

    // Take a slot for `param`, shared by `refs` ops. Returns the slot,
    // PARAM_NONE (nothing to hold), or PARAM_FULL.
    int Acquire(const char *param, uint32_t refs)
    {
        if (!param || !*param || refs == 0)
            return PARAM_NONE;
        for (int i = 0; i < PARAM_SLOTS; i++) {
            if (m_refs[i])
                continue;
            size_t n = strlen(param);
            if (n > PARAM_SLOT_BYTES - 1) {
                n = PARAM_SLOT_BYTES - 1;
                m_truncated++;
            }
            memcpy(m_text[i], param, n);
            m_text[i][n] = 0;
            m_refs[i]    = refs;
            return i;
        }
        m_dropped++;
        return PARAM_FULL;
    }

    // One op of the trigger has finished
    void Release(int slot)
    {
        if (slot >= 0 && slot < PARAM_SLOTS && m_refs[slot])
            m_refs[slot]--;
    }

    // Valid while the slot is held; "" for PARAM_NONE
    const char *Get(int slot) const { return slot >= 0 && slot < PARAM_SLOTS ? m_text[slot] : ""; }

    int      InUse() const     { int n = 0; for (int i = 0; i < PARAM_SLOTS; i++) n += m_refs[i] != 0; return n; }
    uint32_t Dropped() const   { return m_dropped; }
    uint32_t Truncated() const { return m_truncated; }

private:
    char     m_text[PARAM_SLOTS][PARAM_SLOT_BYTES] = {};
    uint32_t m_refs[PARAM_SLOTS] = {};
    uint32_t m_dropped = 0, m_truncated = 0;
};
