#pragma once

// The global op queue (docs/standalone.md "Actions"): a small ring of list
// references (a pointer to a binding's or hook's op array in the layout blob, its
// count, whether a release event queued it, its param slot) with one active op.
// The ops are never copied: the queue walks the current list op by op.
// Triggers push at the tail (PushLists); an activity switch pushes its lists
// next (PushListsNext), so they run before the rest of the list that switched.
// An op that pushed lists next can park (ParkActive) to wait for them: it
// steps aside, they run, and a resume marker (a ring entry with no ops) makes it
// active again before the rest of its list (a script's switchActivity()).
// Platform independent and single threaded: the device layer serialises Push
// (from triggers), Poll (from the op task) and Cancel.
//
// Each Poll() either polls the active op (a Pending one holds the queue) or
// starts the next. The queue knows nothing about what ops do: an OpHost runs
// them (the registry's executors in op_registry.h, or a fake in tests). Delay
// is just an executor that stays Pending.
//
// A trigger's `param` is copied into a ParamPool slot shared by the ops pushed
// for it and handed to each as OpTask::param.

#include <stdint.h>
#include "layout_types.h"
#include "op_exec.h"
#include "param_pool.h"

#define OP_QUEUE_SIZE 40          // list references, not ops (~16 bytes each)

struct OpHost
{
    void *ctx;
    OpStatus (*start)(const layoutdata::op *op, OpTask &task, void *ctx);
    OpStatus (*poll)(const layoutdata::op *op, OpTask &task, void *ctx);       // may be null: never Pending
    void     (*cancel)(const layoutdata::op *op, OpTask &task, void *ctx);     // may be null
};

// One list to queue: ops in the relocated blob (must stay valid until run), how
// many, whether a `release` event queued it, and the trigger param it shares
struct OpListRef
{
    layoutdata::op *const *ops = nullptr;
    uint32_t            count = 0;
    bool                release = false;
    const char         *param = nullptr;
};

class OpQueue
{
public:
    // Fresh: drops anything held or queued without telling the executors (Cancel first for that)
    void Init(const OpHost &host);

    // Add a list of `count` ops (an array of op pointers in the relocated blob,
    // which must stay valid until it has run) as one ring entry. A full ring,
    // or a list whose non-empty `param` finds the pool full, is dropped and
    // counted. An empty list succeeds and takes nothing.
    // `release`: the list was triggered by a `release` event (OpTask::releasing).
    bool PushList(uint32_t count, layoutdata::op *const *ops, bool release = false, const char *param = nullptr);

    // Add `n` lists in order, all or nothing: if the ring lacks room for every
    // non-empty one, or the param pool for their params, nothing is added (and
    // the refused lists are counted). Empty lists are skipped. PushList is this
    // for one.
    bool PushLists(const OpListRef *lists, int n);

    // As PushLists, but the lists run next: straight after the active op (or,
    // when idle, ahead of everything waiting), before the rest of its list and
    // every older ring entry. The activity switch uses it so its lists run
    // before the ops after switchActivity. The rest of the current list becomes
    // a ring entry of its own (same param and release flag), so the room check
    // counts one more entry when ops are left in it. Lists pushed next by the
    // same active op keep their order among themselves; one pushed by an op
    // that a next-push queued goes straight after that op (depth first).
    // The room check also keeps one entry free for a resume marker (ParkActive),
    // so any push that succeeds can be followed by a park.
    bool PushListsNext(const OpListRef *lists, int n);

    // Called by the active op (from its start or poll) after it pushed lists
    // next: when it then returns Pending it is parked instead of holding the
    // queue, the lists it pushed run, and it becomes the active op again (and is
    // polled) at a resume marker placed after them, before the rest of its list.
    // A script's switchActivity() waits for the switch's ops this way. Parked,
    // the op keeps its task and param. False (nothing changes) if the op pushed
    // nothing next since it started or last resumed, or an op is already parked
    // (one at a time). If the op finishes instead of returning Pending, the
    // request lapses and the marker is skipped.
    bool ParkActive();

    // Poll the active op, or start the next if there is none (or it finished).
    // True while there is work (an op started, or one is still Pending); false
    // when idle.
    bool Poll();

    // Takeover/release: tell the active op, then a parked one, to stop, then
    // drop them and everything queued, and free their params.
    void Cancel();

    bool     Active() const    { return m_active; }
    bool     Parked() const    { return m_parked; }         // an op waits for the lists it pushed next
    int      Count() const     { return m_waiting; }        // ops not yet started (rest of the current list included)
    int      Lists() const     { return m_count; }          // ring entries in use, resume markers included (the current list is not one)
    uint32_t Dropped() const   { return m_dropped; }        // lists refused because the ring was full
    uint32_t Errors() const    { return m_errors; }         // ops that ended in Error
    const char *LastError() const { return m_last_error; }  // null until one has
    int      ParamsInUse() const       { return m_params.InUse(); }
    uint32_t ParamDropped() const      { return m_params.Dropped(); }     // lists refused because the param pool was full
    uint32_t ParamTruncated() const    { return m_params.Truncated(); }

private:
    struct Entry
    {
        layoutdata::op *const *ops;     // null: a resume marker (count 0, no param)
        uint32_t            count;
        int8_t              slot;       // ParamPool slot, or PARAM_NONE
        bool                release;
    };

    // The op an executor runs, with its task and param slot (active or parked)
    struct Running
    {
        const layoutdata::op *op = nullptr;
        OpTask                task = {};
        int                   slot = PARAM_NONE;
    };

    void Finish(OpStatus st);           // the active op is over: count an error, free its param
    void Settle(OpStatus st);           // after the active op's start or poll: stays, parks or finishes
    bool StartNext();                   // the next op of the current list, else of the next list (resuming at a marker); false if none
    bool Resume();                      // the parked op is active again and polled

    // If the ring has room for the non-empty lists (`k` of them) plus `extra`
    // entries and the pool a param slot for each (into `slots`, one per
    // non-empty list), takes the slots; else refuses (counted) and takes nothing
    bool Reserve(const OpListRef *lists, int n, int extra, int8_t *slots, int &k);
    // Put the non-empty lists into the ring at offset `at` from m_head, moving the later entries back
    void Insert(int at, const OpListRef *lists, int n, const int8_t *slots, int k);
    void Insert(int at, const Entry &e);

    OpHost            m_host = {};
    Entry             m_ring[OP_QUEUE_SIZE] = {};
    int               m_head = 0, m_count = 0;
    Entry             m_cur = {};           // the list being walked
    uint32_t          m_cur_next = 0;       // its next op to start
    int               m_waiting = 0;        // ops not yet started: rest of m_cur plus every ring entry
    int               m_next_inserted = 0;  // ring entries PushListsNext put at the front since the last op started
    ParamPool         m_params;

    bool              m_active = false;
    Running           m_run;                // the active op
    bool              m_parked = false;
    Running           m_parked_run;         // the parked op
    bool              m_park_requested = false;
    uint32_t          m_generation = 0;

    uint32_t          m_dropped = 0, m_errors = 0;
    const char       *m_last_error = nullptr;
};
