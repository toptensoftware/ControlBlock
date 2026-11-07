#pragma once

// The op registry (docs/standalone-ops-plan.md section 1 D/E): one OpDesc per op
// code with its executor. Built in: delay, hidKey and switchActivity (which
// calls ExecCtx::switch_activity); the network ops are in exec_net. script has a
// name and no executor yet (it is Done at once): the Lua phase.
//
// OpStart also reports every op that starts to ExecCtx::started, after its
// executor has run (the device layer uses that to know an op ran).
// Platform independent.

#include "op_exec.h"
#include "op_queue.h"

// nullptr for a code outside OP_DELAY..OP_SCRIPT
const OpDesc *OpFind(uint32_t code);

// Run an op through its registry entry. An unknown op, or one with no
// executor, is Done.
OpStatus OpStart(ExecCtx &exec, const layoutdata::op *op, OpTask &task);
OpStatus OpPoll(ExecCtx &exec, const layoutdata::op *op, OpTask &task);
void     OpCancel(ExecCtx &exec, const layoutdata::op *op, OpTask &task);

// An OpQueue host that runs the registry's executors with `exec`, which must
// outlive the queue.
OpHost OpRegistryHost(ExecCtx *exec);
