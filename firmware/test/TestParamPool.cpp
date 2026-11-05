// The trigger param pool (docs/standalone-ops-plan.md section 1 G): 8 slots of
// 48 bytes, refcounted per trigger by the number of ops pushed for it.

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "param_pool.h"

using namespace SimpleLib;

Fact("ParamPool Empty Or Absent Param Takes No Slot")
{
    ParamPool p;
    p.Init();
    Assert(p.Acquire(nullptr, 3) == PARAM_NONE);
    Assert(p.Acquire("", 3) == PARAM_NONE);
    Assert(p.InUse() == 0);
    Assert(strcmp(p.Get(PARAM_NONE), "") == 0);
    p.Release(PARAM_NONE);                              // harmless
    Assert(p.Dropped() == 0 && p.Truncated() == 0);
}

Fact("ParamPool Zero Refs Takes No Slot")
{
    ParamPool p;
    p.Init();
    Assert(p.Acquire("x", 0) == PARAM_NONE);
    Assert(p.InUse() == 0);
}

Fact("ParamPool Holds The Text Until The Last Ref Is Released")
{
    ParamPool p;
    p.Init();
    int s = p.Acquire("volume=7", 3);
    Assert(s >= 0);
    Assert(p.InUse() == 1);
    Assert(strcmp(p.Get(s), "volume=7") == 0);
    p.Release(s);
    p.Release(s);
    Assert(p.InUse() == 1);                             // one op still to finish
    Assert(strcmp(p.Get(s), "volume=7") == 0);
    p.Release(s);
    Assert(p.InUse() == 0);
}

Fact("ParamPool Fills Up, Drops And Counts, And Reuses Freed Slots")
{
    ParamPool p;
    p.Init();
    int slots[PARAM_SLOTS];
    for (int i = 0; i < PARAM_SLOTS; i++) {
        slots[i] = p.Acquire("a", 1);
        Assert(slots[i] >= 0);
    }
    Assert(p.InUse() == PARAM_SLOTS);
    Assert(p.Acquire("b", 1) == PARAM_FULL);
    Assert(p.Acquire("c", 1) == PARAM_FULL);
    Assert(p.Dropped() == 2);
    // An empty param needs no slot, so it is never dropped
    Assert(p.Acquire("", 1) == PARAM_NONE);
    Assert(p.Dropped() == 2);

    p.Release(slots[3]);
    int s = p.Acquire("d", 1);
    Assert(s == slots[3]);
    Assert(strcmp(p.Get(s), "d") == 0);
}

Fact("ParamPool Truncates A Long Param And Counts It")
{
    ParamPool p;
    p.Init();
    char longp[100];
    memset(longp, 'x', sizeof(longp));
    longp[99] = 0;
    int s = p.Acquire(longp, 1);
    Assert(s >= 0);
    Assert(strlen(p.Get(s)) == PARAM_SLOT_BYTES - 1);
    Assert(p.Truncated() == 1);

    // Exactly the largest that fits is not truncated
    char fit[PARAM_SLOT_BYTES];
    memset(fit, 'y', sizeof(fit));
    fit[PARAM_SLOT_BYTES - 1] = 0;
    int t = p.Acquire(fit, 1);
    Assert(t >= 0 && strlen(p.Get(t)) == PARAM_SLOT_BYTES - 1);
    Assert(p.Truncated() == 1);
}

Fact("ParamPool Init Frees Everything And Clears The Counts")
{
    ParamPool p;
    p.Init();
    for (int i = 0; i < PARAM_SLOTS + 1; i++)
        p.Acquire("a", 2);
    Assert(p.Dropped() == 1);
    p.Init();
    Assert(p.InUse() == 0 && p.Dropped() == 0 && p.Truncated() == 0);
}
