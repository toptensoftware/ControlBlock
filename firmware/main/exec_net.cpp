#include "exec_net.h"
#include "net_host.h"
#include <string.h>

// The request can't go out: an external failure (the error icon), and the op ends in Error
static OpStatus net_refused(NetHost *net, OpTask &task, const char *why)
{
    task.error = why;
    if (net->external_error)
        net->external_error(net->ctx);
    return OpStatus::Error;
}

// The checks and hand-over every network op shares
OpStatus NetSend(ExecCtx &exec, NetRequest &req, OpTask &task, bool wait)
{
    NetHost *net = exec.net;
    if (!net)
        return OpStatus::Done;
    if (!net->connected(net->ctx))
        return net_refused(net, task, "wi-fi not connected");
    req.wait       = wait;
    req.generation = task.generation;
    if (!net->submit(req, net->ctx))
        return net_refused(net, task, "net queue full");
    return OpStatus::Done;
}

namespace
{
    OpStatus Send(ExecCtx &exec, NetRequest &req, OpTask &task)
    {
        return NetSend(exec, req, task, false);         // ops are fire and forget
    }
}

OpStatus ExecHttpGet(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    const layoutdata::httpGetOp *g = (const layoutdata::httpGetOp *)op;
    NetRequest req = {};
    req.kind    = NetKind::HttpGet;
    req.url     = g->url;
    req.headers = g->headers;
    return Send(exec, req, task);
}

OpStatus ExecHttpPost(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    const layoutdata::httpPostOp *p = (const layoutdata::httpPostOp *)op;
    NetRequest req = {};
    req.kind         = NetKind::HttpPost;
    req.url          = p->url;
    req.headers      = p->headers;
    req.content_type = p->contentType;                  // the bake fills in the form default
    req.body         = p->data;
    req.body_len     = p->data_count;
    return Send(exec, req, task);
}

OpStatus ExecUdpPacket(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    const layoutdata::udpPacketOp *u = (const layoutdata::udpPacketOp *)op;
    if (u->port == 0) {
        task.error = "udp port 0";
        return OpStatus::Error;
    }
    NetRequest req = {};
    req.kind     = NetKind::Udp;
    req.host     = u->host;                             // maybe a name: the worker resolves it
    req.port     = u->port;
    req.body     = u->data;
    req.body_len = u->data_count;
    return Send(exec, req, task);
}

OpStatus ExecSendWol(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    const layoutdata::sendWolOp *w = (const layoutdata::sendWolOp *)op;
    NetRequest req = {};
    req.kind = NetKind::Wol;
    req.port = NET_WOL_PORT;
    memcpy(req.mac, w->mac, sizeof(req.mac));
    if (w->broadcast) {
        req.host = w->broadcast;
    } else if (exec.net && !exec.net->broadcast(req.addr, exec.net->ctx)) {
        // No subnet broadcast known: some routers drop this, but it is the standard fallback
        strcpy(req.addr, NET_BROADCAST_DEFAULT);
    }
    return Send(exec, req, task);
}
