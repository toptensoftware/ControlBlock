// The network executors (httpGet, httpPost, udpPacket, sendWol): how each op
// turns into a NetRequest, and what happens when Wi-Fi is down or the queue
// refuses. All four are fire and forget: Done as soon as the request is handed
// over.

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "op_registry.h"
#include "net_env.h"

using namespace SimpleLib;

namespace
{
    struct Rig
    {
        uint32_t now = 1000;
        FakeNet  net;
        ExecCtx  exec = {};
        OpTask   task = {};

        Rig(bool with_net = true)
        {
            exec.ctx    = this;
            exec.now_ms = [](void *c) { return ((Rig *)c)->now; };
            exec.net    = with_net ? &net.host : nullptr;
            task.param      = "";
            task.generation = 42;
        }
    };

    const char *OK_URL = "http://tv.local/off";
}

Fact("ExecNet HttpGet Submits A Fire And Forget Get")
{
    Rig r;
    cfgdata::httpGetOp op = {};
    op.base.op = OP_HTTP_GET;
    op.url     = OK_URL;
    op.headers = "X-A: 1\r\nX-B: 2";

    Assert(OpStart(r.exec, &op.base, r.task) == OpStatus::Done);
    Assert(r.net.n_sent == 1);
    const NetRequest &q = r.net.sent[0];
    Assert(q.kind == NetKind::HttpGet);
    Assert(q.url == OK_URL);
    Assert(strcmp(q.headers, "X-A: 1\r\nX-B: 2") == 0);
    Assert(!q.wait);
    Assert(q.generation == 42);
}

Fact("ExecNet HttpGet Without Headers Passes Null")
{
    Rig r;
    cfgdata::httpGetOp op = {};
    op.base.op = OP_HTTP_GET;
    op.url     = OK_URL;
    Assert(OpStart(r.exec, &op.base, r.task) == OpStatus::Done);
    Assert(r.net.sent[0].headers == nullptr);
}

Fact("ExecNet HttpPost Carries The Body Content Type And Headers")
{
    Rig r;
    static uint8_t body[] = { 's', 't', 'a', 't', 'e', '=', 'o', 'f', 'f' };
    cfgdata::httpPostOp op = {};
    op.base.op     = OP_HTTP_POST;
    op.url         = OK_URL;
    op.data_count  = sizeof(body);
    op.data        = body;
    op.contentType = "application/x-www-form-urlencoded";
    op.headers     = "Authorization: x";

    Assert(OpStart(r.exec, &op.base, r.task) == OpStatus::Done);
    Assert(r.net.n_sent == 1);
    const NetRequest &q = r.net.sent[0];
    Assert(q.kind == NetKind::HttpPost);
    Assert(q.url == OK_URL);
    Assert(q.body == body && q.body_len == sizeof(body));
    Assert(strcmp(q.content_type, "application/x-www-form-urlencoded") == 0);
    Assert(strcmp(q.headers, "Authorization: x") == 0);
    Assert(!q.wait);
}

Fact("ExecNet HttpPost With An Empty Body Is Fine")
{
    Rig r;
    cfgdata::httpPostOp op = {};
    op.base.op     = OP_HTTP_POST;
    op.url         = OK_URL;
    op.contentType = "text/plain";
    Assert(OpStart(r.exec, &op.base, r.task) == OpStatus::Done);
    Assert(r.net.sent[0].body_len == 0);
}

Fact("ExecNet Udp Sends The Payload To The Host And Port")
{
    Rig r;
    static uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    cfgdata::udpPacketOp op = {};
    op.base.op    = OP_UDP_PACKET;
    op.host       = "audio.local";                      // a name: the worker resolves it
    op.port       = 4210;
    op.data_count = sizeof(data);
    op.data       = data;

    Assert(OpStart(r.exec, &op.base, r.task) == OpStatus::Done);
    Assert(r.net.n_sent == 1);
    const NetRequest &q = r.net.sent[0];
    Assert(q.kind == NetKind::Udp);
    Assert(strcmp(q.Host(), "audio.local") == 0);
    Assert(q.port == 4210);
    Assert(q.body == data && q.body_len == 4);
}

Fact("ExecNet Udp Port Zero Is An Error")
{
    Rig r;
    static uint8_t data[] = { 1 };
    cfgdata::udpPacketOp op = {};
    op.base.op    = OP_UDP_PACKET;
    op.host       = "1.2.3.4";
    op.port       = 0;
    op.data_count = 1;
    op.data       = data;
    Assert(OpStart(r.exec, &op.base, r.task) == OpStatus::Error);
    Assert(r.task.error && *r.task.error);
    Assert(r.net.n_sent == 0);
}

Fact("ExecNet Wol Uses The Op's Broadcast When It Has One")
{
    Rig r;
    r.net.has_broadcast = true;                         // present but not wanted
    cfgdata::sendWolOp op = {};
    op.base.op = OP_SEND_WOL;
    const uint8_t mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    memcpy(op.mac, mac, 6);
    op.broadcast = "10.0.0.255";

    Assert(OpStart(r.exec, &op.base, r.task) == OpStatus::Done);
    const NetRequest &q = r.net.sent[0];
    Assert(q.kind == NetKind::Wol);
    Assert(strcmp(q.Host(), "10.0.0.255") == 0);
    Assert(q.port == NET_WOL_PORT && q.port == 9);
    Assert(memcmp(q.mac, mac, 6) == 0);
    Assert(r.net.broadcast_asked == 0);
}

Fact("ExecNet Wol Defaults To The Subnet Broadcast")
{
    Rig r;
    r.net.has_broadcast = true;
    cfgdata::sendWolOp op = {};
    op.base.op = OP_SEND_WOL;
    Assert(OpStart(r.exec, &op.base, r.task) == OpStatus::Done);
    Assert(strcmp(r.net.sent[0].Host(), "192.168.1.255") == 0);
}

Fact("ExecNet Wol Falls Back To The Limited Broadcast")
{
    Rig r;
    r.net.has_broadcast = false;
    cfgdata::sendWolOp op = {};
    op.base.op = OP_SEND_WOL;
    Assert(OpStart(r.exec, &op.base, r.task) == OpStatus::Done);
    Assert(strcmp(r.net.sent[0].Host(), "255.255.255.255") == 0);
}

Fact("ExecNet Wol Request Survives Being Copied")
{
    // The default address lives inside the request, so a copy (into the queue) must still find it
    Rig r;
    cfgdata::sendWolOp op = {};
    op.base.op = OP_SEND_WOL;
    OpStart(r.exec, &op.base, r.task);
    NetRequest copy = r.net.sent[0];
    r.net.sent[0] = NetRequest{};
    Assert(strcmp(copy.Host(), "255.255.255.255") == 0);
}

Fact("ExecNet Ops Are Errors While Wi-Fi Is Down And Nothing Is Queued")
{
    Rig r;
    r.net.is_connected = false;

    cfgdata::httpGetOp g = {};
    g.base.op = OP_HTTP_GET;
    g.url     = OK_URL;
    Assert(OpStart(r.exec, &g.base, r.task) == OpStatus::Error);
    Assert(r.task.error && strstr(r.task.error, "wi-fi"));

    cfgdata::httpPostOp p = {};
    p.base.op     = OP_HTTP_POST;
    p.url         = OK_URL;
    p.contentType = "text/plain";
    r.task.error  = nullptr;
    Assert(OpStart(r.exec, &p.base, r.task) == OpStatus::Error);
    Assert(r.task.error);

    static uint8_t d[] = { 1 };
    cfgdata::udpPacketOp u = {};
    u.base.op    = OP_UDP_PACKET;
    u.host       = "1.2.3.4";
    u.port       = 9;
    u.data_count = 1;
    u.data       = d;
    Assert(OpStart(r.exec, &u.base, r.task) == OpStatus::Error);

    cfgdata::sendWolOp w = {};
    w.base.op = OP_SEND_WOL;
    Assert(OpStart(r.exec, &w.base, r.task) == OpStatus::Error);

    Assert(r.net.n_sent == 0);
}

Fact("ExecNet A Refused Submit Is An Error")
{
    Rig r;
    r.net.accepts = false;
    cfgdata::httpGetOp g = {};
    g.base.op = OP_HTTP_GET;
    g.url     = OK_URL;
    Assert(OpStart(r.exec, &g.base, r.task) == OpStatus::Error);
    Assert(r.task.error && strstr(r.task.error, "queue"));
}

Fact("ExecNet Without A Net Host The Ops Do Nothing")
{
    Rig r(false);
    cfgdata::httpGetOp g = {};
    g.base.op = OP_HTTP_GET;
    g.url     = OK_URL;
    Assert(OpStart(r.exec, &g.base, r.task) == OpStatus::Done);
    cfgdata::sendWolOp w = {};
    w.base.op = OP_SEND_WOL;
    Assert(OpStart(r.exec, &w.base, r.task) == OpStatus::Done);
}

Fact("ExecNet Ops Still Report Started And Poll Done")
{
    Rig r;
    int started = 0;
    r.exec.started = [](const cfgdata::op *, void *c) { (*(int *)c)++; };
    r.exec.ctx = &started;
    // now_ms reads r through ctx, so give it a clock that doesn't
    r.exec.now_ms = [](void *) { return (uint32_t)0; };

    cfgdata::httpGetOp g = {};
    g.base.op = OP_HTTP_GET;
    g.url     = OK_URL;
    Assert(OpStart(r.exec, &g.base, r.task) == OpStatus::Done);
    Assert(started == 1);
    Assert(OpPoll(r.exec, &g.base, r.task) == OpStatus::Done);
}
