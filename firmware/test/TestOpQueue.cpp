// The op queue and the executor protocol (docs/standalone-ops-plan.md section 1
// E): one active slot, start / poll / cancel, param delivery. The executors are
// fakes that record what the queue does, so these test the queue alone.

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "op_queue.h"

using namespace SimpleLib;

namespace
{
    const int MAX_LOG = 64;

    // Ops are identified by their code; the fake executor decides what each does
    struct Rig
    {
        layoutdata::op ops[8];
        layoutdata::op *list[8];
        OpQueue      queue;

        // What the fake executor is told to do
        OpStatus     start_result = OpStatus::Done;          // for every op
        int          pending_polls = 0;                      // a Pending op stays so for this many polls
        bool         poll_error = false;
        const char  *error_text = "boom";

        // What it saw
        uint32_t     started[MAX_LOG];  int n_started = 0;
        char         params[MAX_LOG][64];
        bool         releasing[MAX_LOG];
        uint32_t     generations[MAX_LOG];
        int          polls = 0;
        uint32_t     cancelled[MAX_LOG]; int n_cancelled = 0;
        int          polls_left = 0;

        Rig()
        {
            for (int i = 0; i < 8; i++) {
                ops[i].op = 100 + i;
                list[i]   = &ops[i];
            }
            OpHost h = {};
            h.ctx    = this;
            h.start  = [](const layoutdata::op *op, OpTask &t, void *c) {
                Rig *r = (Rig *)c;
                if (r->n_started < MAX_LOG) {
                    r->started[r->n_started] = op->op;
                    strncpy(r->params[r->n_started], t.param, 63);
                    r->params[r->n_started][63] = 0;
                    r->releasing[r->n_started]  = t.releasing;
                    r->generations[r->n_started] = t.generation;
                    r->n_started++;
                }
                r->polls_left = r->pending_polls;
                if (r->start_result == OpStatus::Error)
                    t.error = r->error_text;
                return r->start_result;
            };
            h.poll = [](const layoutdata::op *, OpTask &t, void *c) {
                Rig *r = (Rig *)c;
                r->polls++;
                if (r->poll_error) { t.error = r->error_text; return OpStatus::Error; }
                return r->polls_left-- > 0 ? OpStatus::Pending : OpStatus::Done;
            };
            h.cancel = [](const layoutdata::op *op, OpTask &, void *c) {
                Rig *r = (Rig *)c;
                if (r->n_cancelled < MAX_LOG)
                    r->cancelled[r->n_cancelled++] = op->op;
            };
            queue.Init(h);
        }

        bool Push(int n, bool release = false, const char *param = nullptr)
        {
            return queue.PushList(n, list, release, param);
        }
        void Drain() { for (int i = 0; i < 1000 && queue.Poll(); i++) {} }
    };
}

Fact("OpQueue Starts One Op Per Poll In Order")
{
    Rig r;
    Assert(r.Push(3));
    Assert(r.queue.Count() == 3);
    Assert(r.queue.Poll() && r.n_started == 1 && r.started[0] == 100);
    Assert(r.queue.Poll() && r.n_started == 2 && r.started[1] == 101);
    Assert(r.queue.Poll() && r.n_started == 3 && r.started[2] == 102);
    Assert(!r.queue.Poll());
    Assert(r.polls == 0);                       // Done at start: never polled
}

Fact("OpQueue Pending Op Holds The Queue Until Its Poll Says Done")
{
    Rig r;
    r.start_result  = OpStatus::Pending;
    r.pending_polls = 2;
    r.Push(2);
    Assert(r.queue.Poll());                     // starts op 100
    Assert(r.n_started == 1);
    Assert(r.queue.Active());
    Assert(r.queue.Poll() && r.polls == 1);     // pending
    Assert(r.queue.Poll() && r.polls == 2);     // pending
    Assert(r.n_started == 1);                   // the second is held back
    Assert(r.queue.Poll());                     // poll: Done, and the next starts in the same call
    Assert(r.polls == 3 && r.n_started == 2 && r.started[1] == 101);
    Assert(r.queue.Active());
}

Fact("OpQueue Goes Idle When The Last Pending Op Finishes")
{
    Rig r;
    r.start_result  = OpStatus::Pending;
    r.pending_polls = 0;
    r.Push(1);
    Assert(r.queue.Poll());
    Assert(r.queue.Active());
    Assert(!r.queue.Poll());                    // poll says Done, nothing left
    Assert(!r.queue.Active());
}

Fact("OpQueue Error Is Counted And The Queue Carries On")
{
    Rig r;
    r.start_result = OpStatus::Error;
    r.Push(2);
    r.Drain();
    Assert(r.n_started == 2);
    Assert(r.queue.Errors() == 2);
    Assert(strcmp(r.queue.LastError(), "boom") == 0);
    Assert(!r.queue.Active());

    // An error from a poll counts too
    Rig p;
    p.start_result  = OpStatus::Pending;
    p.pending_polls = 5;
    p.poll_error    = true;
    p.error_text    = "timeout";
    p.Push(2);
    p.Drain();
    Assert(p.queue.Errors() == 2 && p.n_started == 2);
    Assert(strcmp(p.queue.LastError(), "timeout") == 0);
}

Fact("OpQueue Gives Each Started Op A New Generation")
{
    Rig r;
    r.Push(3);
    r.Drain();
    Assert(r.generations[0] != r.generations[1]);
    Assert(r.generations[1] != r.generations[2]);
    Assert(r.generations[0] != r.generations[2]);
}

Fact("OpQueue Tells The Executor Where The List Came From")
{
    Rig r;
    r.Push(1, false);
    r.Push(1, true);
    r.Drain();
    Assert(!r.releasing[0] && r.releasing[1]);
}

Fact("OpQueue Delivers The Param, Empty When There Is None")
{
    Rig r;
    r.Push(1);                                  // no param
    r.Push(1, false, "");                       // empty
    r.Push(2, false, "left");                   // shared by both ops of the list
    r.Drain();
    Assert(strcmp(r.params[0], "") == 0);
    Assert(strcmp(r.params[1], "") == 0);
    Assert(strcmp(r.params[2], "left") == 0);
    Assert(strcmp(r.params[3], "left") == 0);
}

Fact("OpQueue Param Stays Valid While Its Op Is Pending And Is Freed After")
{
    Rig r;
    r.start_result  = OpStatus::Pending;
    r.pending_polls = 1;
    r.Push(1, false, "abc");
    r.queue.Poll();
    Assert(r.queue.ParamsInUse() == 1);         // still needed by the pending op
    r.queue.Poll();                             // pending
    Assert(r.queue.ParamsInUse() == 1);
    r.queue.Poll();                             // done
    Assert(r.queue.ParamsInUse() == 0);
}

Fact("OpQueue Param Slot Is Freed After Every Op Of The List Finished")
{
    Rig r;
    r.Push(3, false, "abc");
    Assert(r.queue.ParamsInUse() == 1);
    r.queue.Poll();
    r.queue.Poll();
    Assert(r.queue.ParamsInUse() == 1);         // the third has yet to run
    r.queue.Poll();
    Assert(r.queue.ParamsInUse() == 0);
}

Fact("OpQueue Drops A Trigger When The Param Pool Is Full")
{
    Rig r;
    for (int i = 0; i < PARAM_SLOTS; i++)
        Assert(r.Push(1, false, "p"));
    Assert(r.queue.ParamsInUse() == PARAM_SLOTS);
    Assert(!r.Push(2, false, "q"));             // all or nothing
    Assert(r.queue.Count() == PARAM_SLOTS);
    Assert(r.queue.ParamDropped() == 1);
    Assert(r.queue.Dropped() == 0);             // the ring had room
    Assert(r.Push(1));                          // a trigger with no param is not affected
    r.Drain();
    Assert(r.queue.ParamsInUse() == 0);
    Assert(r.Push(1, false, "q"));              // and the pool is free again
}

Fact("OpQueue Counts A Truncated Param")
{
    Rig r;
    char big[100];
    memset(big, 'z', sizeof(big));
    big[99] = 0;
    Assert(r.Push(1, false, big));
    r.Drain();
    Assert(r.queue.ParamTruncated() == 1);
    Assert(strlen(r.params[0]) == PARAM_SLOT_BYTES - 1);
}

Fact("OpQueue Full Ring Drops Whole Lists Without Taking A Param Slot")
{
    Rig r;
    for (int i = 0; i < OP_QUEUE_SIZE; i++)
        Assert(r.Push(1));
    Assert(!r.Push(1, false, "x"));
    Assert(r.queue.Dropped() == 1);
    Assert(r.queue.ParamsInUse() == 0);
    Assert(r.queue.ParamDropped() == 0);
}

// The ring holds list references, not ops: its capacity is in lists
Fact("OpQueue Ring Is Forty List References")
{
    // 40 covers the worst-case activity switch (every device changing: up to ~10 lists)
    // with room for queued key presses; see docs/standalone.md "Actions"
    static_assert(OP_QUEUE_SIZE == 40, "the ring holds 40 list references");
}

Fact("OpQueue Long Lists Take One Ring Entry Each")
{
    // 64 ops (LAYOUT_MAX_OPS) per list, far more than the ring has entries
    static layoutdata::op big[64];
    static layoutdata::op *biglist[64];
    for (int i = 0; i < 64; i++) { big[i].op = 200 + i; biglist[i] = &big[i]; }

    Rig r;
    for (int i = 0; i < OP_QUEUE_SIZE; i++)
        Assert(r.queue.PushList(64, biglist));
    Assert(r.queue.Lists() == OP_QUEUE_SIZE);
    Assert(r.queue.Count() == 64 * OP_QUEUE_SIZE);      // ops waiting
    Assert(!r.queue.PushList(64, biglist));             // ring full of references
    Assert(r.queue.Dropped() == 1);
    Assert(r.queue.Lists() == OP_QUEUE_SIZE);
}

Fact("OpQueue Walks The Current List Op By Op Then Takes The Next List")
{
    Rig r;
    r.Push(2);
    r.Push(3);
    Assert(r.queue.Lists() == 2 && r.queue.Count() == 5);
    r.queue.Poll();
    Assert(r.queue.Lists() == 1);                       // the first list is now the current one
    Assert(r.queue.Count() == 4);                       // one op remains of it, three in the next
    r.Drain();
    Assert(r.n_started == 5);
    const uint32_t want[5] = { 100, 101, 100, 101, 102 };
    for (int i = 0; i < 5; i++)
        Assert(r.started[i] == want[i]);
    Assert(r.queue.Lists() == 0 && r.queue.Count() == 0);
}

Fact("OpQueue Pending Op Mid List Holds The Rest Of The List And The Next Lists")
{
    Rig r;
    r.start_result  = OpStatus::Pending;
    r.pending_polls = 1;
    r.Push(3, false, "abc");
    r.Push(1);
    r.queue.Poll();                                     // op 100 pending
    Assert(r.queue.Poll() && r.n_started == 1);         // still waiting
    Assert(r.queue.Poll() && r.n_started == 2);         // 100 done, 101 starts, in the same call
    Assert(r.started[1] == 101);
    Assert(r.queue.ParamsInUse() == 1);                 // two ops of the list still to finish
    r.Drain();
    Assert(r.n_started == 4 && r.started[2] == 102 && r.started[3] == 100);
    Assert(r.queue.ParamsInUse() == 0);
}

Fact("OpQueue Releasing Tag Follows The List Op By Op")
{
    Rig r;
    r.Push(2, true);
    r.Push(2, false);
    r.Push(2, true);
    r.Drain();
    const bool want[6] = { true, true, false, false, true, true };
    for (int i = 0; i < 6; i++)
        Assert(r.releasing[i] == want[i]);
}

Fact("OpQueue Cancel Mid List Drops The Rest Of The Current List And Frees Its Param")
{
    Rig r;
    r.start_result  = OpStatus::Pending;
    r.pending_polls = 100;
    r.Push(3, false, "abc");
    r.queue.Poll();                                     // op 100 active, 101 and 102 behind it
    Assert(r.queue.Count() == 2 && r.queue.ParamsInUse() == 1);
    r.queue.Cancel();
    Assert(r.n_cancelled == 1 && r.cancelled[0] == 100);
    Assert(r.queue.Count() == 0 && r.queue.Lists() == 0 && r.queue.ParamsInUse() == 0);
    Assert(!r.queue.Poll() && r.n_started == 1);

    // Usable afterwards, and the freed list does not come back
    r.start_result = OpStatus::Done;
    r.Push(1);
    r.Drain();
    Assert(r.n_started == 2 && r.started[1] == 100);
}

Fact("OpQueue Empty List Succeeds And Holds Nothing")
{
    Rig r;
    Assert(r.Push(0, false, "x"));
    Assert(r.queue.Count() == 0 && r.queue.Lists() == 0 && r.queue.ParamsInUse() == 0);
}

Fact("OpQueue Cancel Stops The Active Op, Clears The Rest And Frees Params")
{
    Rig r;
    r.start_result  = OpStatus::Pending;
    r.pending_polls = 100;
    r.Push(3, false, "abc");
    r.Push(1, false, "def");
    r.queue.Poll();                             // op 100 active, 3 queued behind
    Assert(r.queue.Active() && r.queue.Count() == 3);
    Assert(r.queue.ParamsInUse() == 2);

    r.queue.Cancel();
    Assert(r.n_cancelled == 1 && r.cancelled[0] == 100);    // only the active op is told
    Assert(!r.queue.Active());
    Assert(r.queue.Count() == 0);
    Assert(r.queue.ParamsInUse() == 0);
    Assert(!r.queue.Poll());                    // nothing left to start
    Assert(r.n_started == 1);
}

Fact("OpQueue Cancel When Idle Is Harmless And The Queue Works Afterwards")
{
    Rig r;
    r.queue.Cancel();
    Assert(r.n_cancelled == 0);
    r.Push(1);
    r.Drain();
    Assert(r.n_started == 1);
}

// Multi-list push (an activity switch is up to six lists, all or nothing)
Fact("OpQueue PushLists Takes All Lists In Order")
{
    Rig r;
    OpListRef refs[3] = { { r.list, 1 }, { r.list + 1, 2 }, { r.list + 3, 1 } };
    Assert(r.queue.PushLists(refs, 3));
    Assert(r.queue.Lists() == 3 && r.queue.Count() == 4);
    r.Drain();
    const uint32_t want[4] = { 100, 101, 102, 103 };
    Assert(r.n_started == 4);
    for (int i = 0; i < 4; i++)
        Assert(r.started[i] == want[i]);
}

Fact("OpQueue PushLists Skips Empty Lists Without Taking An Entry")
{
    Rig r;
    OpListRef refs[4] = { { r.list, 0 }, { r.list, 1 }, { nullptr, 0, false, "x" }, { r.list + 1, 1 } };
    Assert(r.queue.PushLists(refs, 4));
    Assert(r.queue.Lists() == 2 && r.queue.Count() == 2 && r.queue.ParamsInUse() == 0);
    Assert(r.queue.PushLists(refs, 0));                 // nothing at all succeeds
    Assert(r.queue.Lists() == 2);
}

Fact("OpQueue PushLists Fits Exactly And Refuses One Too Many Leaving The Queue Unchanged")
{
    // OP_QUEUE_SIZE - 6 used: six more fit exactly
    {
        Rig r;
        for (int i = 0; i < OP_QUEUE_SIZE - 6; i++)
            Assert(r.Push(1));
        OpListRef refs[6];
        for (int i = 0; i < 6; i++) refs[i] = { r.list + i, 1 };
        Assert(r.queue.PushLists(refs, 6));
        Assert(r.queue.Lists() == OP_QUEUE_SIZE && r.queue.Dropped() == 0);
    }
    // OP_QUEUE_SIZE - 5 used: refused, nothing added (not even the first lists), params untouched
    {
        Rig r;
        for (int i = 0; i < OP_QUEUE_SIZE - 5; i++)
            Assert(r.Push(1));
        OpListRef refs[6];
        for (int i = 0; i < 6; i++) refs[i] = { r.list + i, 1, false, i == 0 ? "a" : nullptr };
        int lists = r.queue.Lists(), count = r.queue.Count();
        Assert(!r.queue.PushLists(refs, 6));
        Assert(r.queue.Lists() == lists && r.queue.Count() == count);
        Assert(r.queue.ParamsInUse() == 0);
        Assert(r.queue.Dropped() == 6);                 // lists refused
        r.Drain();
        Assert(r.n_started == lists);                   // only the earlier ones run
    }
}

Fact("OpQueue PushLists Empty Lists Do Not Count Against The Ring")
{
    Rig r;
    for (int i = 0; i < OP_QUEUE_SIZE - 2; i++)
        Assert(r.Push(1));
    OpListRef refs[6] = { { r.list, 0 }, { r.list, 1 }, { r.list, 0 }, { r.list, 1 }, { r.list, 0 }, { r.list, 0 } };
    Assert(r.queue.PushLists(refs, 6));
    Assert(r.queue.Lists() == OP_QUEUE_SIZE);
}

Fact("OpQueue PushLists Gives Each List Its Own Param, Release Flag And Rolls Back On A Full Pool")
{
    Rig r;
    OpListRef refs[3] = { { r.list, 1, false, "one" }, { r.list + 1, 2, true, "two" }, { r.list + 3, 1 } };
    Assert(r.queue.PushLists(refs, 3));
    Assert(r.queue.ParamsInUse() == 2);
    r.Drain();
    Assert(strcmp(r.params[0], "one") == 0 && !r.releasing[0]);
    Assert(strcmp(r.params[1], "two") == 0 && strcmp(r.params[2], "two") == 0 && r.releasing[1] && r.releasing[2]);
    Assert(strcmp(r.params[3], "") == 0 && !r.releasing[3]);
    Assert(r.queue.ParamsInUse() == 0);

    // Pool: seven slots taken, the sequence wants two: refused, the one it got is given back
    Rig f;
    for (int i = 0; i < PARAM_SLOTS - 1; i++)
        Assert(f.Push(1, false, "p"));
    OpListRef need[2] = { { f.list, 1, false, "a" }, { f.list + 1, 1, false, "b" } };
    int lists = f.queue.Lists();
    Assert(!f.queue.PushLists(need, 2));
    Assert(f.queue.Lists() == lists && f.queue.ParamsInUse() == PARAM_SLOTS - 1);
    Assert(f.queue.ParamDropped() == 1);
    Assert(f.queue.Dropped() == 0);
    Assert(f.Push(1, false, "c"));                      // the slot came back
}

Fact("OpQueue PushList Is PushLists Of One")
{
    Rig r;
    Assert(r.queue.PushList(2, r.list, true, "z"));
    Assert(r.queue.Lists() == 1 && r.queue.Count() == 2 && r.queue.ParamsInUse() == 1);
    r.Drain();
    Assert(r.releasing[0] && r.releasing[1] && strcmp(r.params[1], "z") == 0);
}

Fact("OpQueue Ring Wraps Over Many Cycles With Params")
{
    Rig r;
    for (int i = 0; i < 5 * OP_QUEUE_SIZE; i++) {
        Assert(r.Push(2, i & 1, "w"));
        r.n_started = 0;
        r.Drain();
        Assert(r.n_started == 2 && r.started[0] == 100 && r.started[1] == 101);
        Assert(strcmp(r.params[1], "w") == 0 && r.releasing[0] == !!(i & 1));
    }
    Assert(r.queue.Dropped() == 0 && r.queue.ParamsInUse() == 0 && r.queue.Count() == 0);
}
