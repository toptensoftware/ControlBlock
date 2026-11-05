#pragma once

// The network ops' executors (httpGet, httpPost, udpPacket, sendWol). Each turns
// its op into a NetRequest and hands it to ExecCtx::net; all are fire and forget
// at the op level, so each is Done as soon as the request is handed over (an
// error if Wi-Fi is down or the net queue refuses it: the op is dropped and the
// queue logs the error). A null ExecCtx::net makes them do nothing.

#include "op_exec.h"
#include "net_host.h"

OpStatus ExecHttpGet(ExecCtx &exec, const cfgdata::op *op, OpTask &task);
OpStatus ExecHttpPost(ExecCtx &exec, const cfgdata::op *op, OpTask &task);
OpStatus ExecUdpPacket(ExecCtx &exec, const cfgdata::op *op, OpTask &task);
OpStatus ExecSendWol(ExecCtx &exec, const cfgdata::op *op, OpTask &task);

// Hand a request to the net host (the ops above, and the script API, which sets `wait`
// to get an answer back): sets wait and the generation from the task. Done when handed
// over (or when there is no net host); Error with task.error if Wi-Fi is down or the
// queue refuses.
OpStatus NetSend(ExecCtx &exec, NetRequest &req, OpTask &task, bool wait);
