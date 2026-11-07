#pragma once

// The executor protocol (docs/standalone-ops-plan.md section 1 E). Every op is
// run by an executor with three entry points:
//
//   start(ctx, op, task)  -> Done | Pending | Error
//   poll(ctx, op, task)   -> for a Pending op, until it is Done or Error
//   cancel(ctx, op, task) -> a Pending op is abandoned (takeover, release)
//
// `OpTask` is a small POD the queue owns for the one active op; the executor
// keeps its own state in it. `ExecCtx` holds the injected hosts. Executors are
// platform independent, single threaded (the owner serialises) and must not
// block: anything that waits returns Pending.

#include <stdint.h>
#include "layout_types.h"

class OpQueue;
class HidKeyboard;
class ScriptHost;                   // the Lua runtime (script_host.h)
struct NetHost;                     // the network executors' host (defined with them)

enum class OpStatus : uint8_t
{
    Done,
    Pending,
    Error,                          // OpTask::error says why
};

struct OpTask
{
    uint32_t    deadline;           // for the executor (delay, tap settle)
    uint32_t    request;            // for the executor (an outstanding request's id)
    uint32_t    generation;         // set by the queue: new for every started op, so a late reply can be told from a current one
    const char *param;              // the trigger's param; never null ("" when none); valid until the op finishes
    const char *error;              // set by an executor that returns Error
    bool        releasing;          // the op came from a `release` binding (a hidKey `hold` is up there, down elsewhere)
};

struct ExecCtx
{
    void *ctx;                                                  // for now_ms and started
    uint32_t (*now_ms)(void *ctx);                              // wrapping millisecond clock
    void (*started)(const layoutdata::op *op, void *ctx);          // every op that starts, after its executor's start (may be null)
    HidKeyboard *hid;                                           // null: hidKey does nothing
    NetHost     *net;                                           // null: network ops do nothing
    OpQueue     *queue;                                         // for ops that push more (script)
    ScriptHost  *script;                                        // null: `script` ops do nothing
    const layoutdata::layoutRoot *root;                            // the layout (a script looks up activities by name)
    // switchActivity: select activity `index` (-1 none). Null on success, else
    // why not (the op then ends in Error). Null function: the op does nothing.
    const char *(*switch_activity)(int index, void *ctx);

    uint32_t Now() const { return now_ms(ctx); }
};

// Wrapping clock: has `deadline` been reached?
inline bool OpReached(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

typedef OpStatus (*OpStartFn)(ExecCtx &exec, const layoutdata::op *op, OpTask &task);
typedef OpStatus (*OpPollFn)(ExecCtx &exec, const layoutdata::op *op, OpTask &task);
typedef void     (*OpCancelFn)(ExecCtx &exec, const layoutdata::op *op, OpTask &task);

struct OpDesc
{
    uint32_t    code;               // OP_*
    const char *name;
    OpStartFn   start;              // null: not built yet, the op is Done at once
    OpPollFn    poll;               // null: never Pending
    OpCancelFn  cancel;             // null: nothing to undo
};
