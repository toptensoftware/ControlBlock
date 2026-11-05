// The net worker's request queue: bounded, FIFO (so a GET waits behind queued
// posts), overflow drops the oldest fire-and-forget request, a waited request is
// never dropped, a late reply is ignored, a stuck request times out.

#include "UnitTesting.h"
#include "Core.h"
#include "net_queue.h"

using namespace SimpleLib;

namespace
{
    NetRequest Req(NetKind kind, uint32_t gen, bool wait = false)
    {
        NetRequest r = {};
        r.kind       = kind;
        r.generation = gen;
        r.wait       = wait;
        return r;
    }

    // Run the worker one step: take the next request and complete it
    bool Run(NetQueue &q, uint32_t now, NetRequest *out, NetResult res = NetResult::Ok)
    {
        if (!q.Next(now, out))
            return false;
        q.Complete(out->generation, res, 200, 10);
        return true;
    }
}

Fact("NetQueue Runs Requests In Order So A Get Waits Behind Posts")
{
    NetQueue q;
    q.Init();
    Assert(q.Submit(Req(NetKind::HttpPost, 1)));
    Assert(q.Submit(Req(NetKind::HttpPost, 2)));
    Assert(q.Submit(Req(NetKind::HttpGet,  3, true)));

    NetRequest r;
    Assert(Run(q, 0, &r) && r.generation == 1);
    Assert(Run(q, 0, &r) && r.generation == 2);
    Assert(Run(q, 0, &r) && r.generation == 3 && r.kind == NetKind::HttpGet);
    Assert(!q.Next(0, &r));
    Assert(q.Count() == 0);
}

Fact("NetQueue Hands Out One Request At A Time")
{
    NetQueue q;
    q.Init();
    q.Submit(Req(NetKind::HttpPost, 1));
    q.Submit(Req(NetKind::HttpPost, 2));
    NetRequest r;
    Assert(q.Next(0, &r) && r.generation == 1);
    Assert(q.Busy());
    Assert(!q.Next(0, &r));                             // 1 hasn't completed
    q.Complete(1, NetResult::Ok, 200, 0);
    Assert(!q.Busy());
    Assert(q.Next(0, &r) && r.generation == 2);
}

Fact("NetQueue Overflow Drops The Oldest Fire And Forget Request")
{
    NetQueue q;
    q.Init();
    for (uint32_t i = 1; i <= NET_QUEUE_SIZE; i++)
        Assert(q.Submit(Req(NetKind::HttpPost, i)));
    Assert(q.Count() == NET_QUEUE_SIZE);

    Assert(q.Submit(Req(NetKind::HttpPost, 100)));      // full: request 1 goes
    Assert(q.Count() == NET_QUEUE_SIZE);
    Assert(q.Dropped() == 1);

    NetRequest r;
    Assert(Run(q, 0, &r) && r.generation == 2);         // 1 is gone, order kept
    for (uint32_t i = 3; i <= NET_QUEUE_SIZE; i++)
        Assert(Run(q, 0, &r) && r.generation == i);
    Assert(Run(q, 0, &r) && r.generation == 100);
}

Fact("NetQueue Never Drops A Waited Request To Make Room")
{
    NetQueue q;
    q.Init();
    Assert(q.Submit(Req(NetKind::HttpGet, 1, true)));   // oldest, but waited on
    for (uint32_t i = 2; i <= NET_QUEUE_SIZE; i++)
        Assert(q.Submit(Req(NetKind::HttpPost, i)));

    Assert(q.Submit(Req(NetKind::HttpPost, 100)));      // drops 2, the oldest fire-and-forget
    Assert(q.Dropped() == 1);

    NetRequest r;
    Assert(Run(q, 0, &r) && r.generation == 1);         // the waited one survived
    Assert(Run(q, 0, &r) && r.generation == 3);
}

Fact("NetQueue Refuses A Request When Everything Queued Is Waited On")
{
    NetQueue q;
    q.Init();
    for (uint32_t i = 1; i <= NET_QUEUE_SIZE; i++)
        Assert(q.Submit(Req(NetKind::HttpGet, i, true)));

    Assert(!q.Submit(Req(NetKind::HttpPost, 100)));     // nothing to drop
    Assert(!q.Submit(Req(NetKind::HttpGet, 101, true)));
    Assert(q.Refused() == 2);
    Assert(q.Dropped() == 0);
    Assert(q.Count() == NET_QUEUE_SIZE);
}

Fact("NetQueue Gives A Waited Request Its Reply")
{
    NetQueue q;
    q.Init();
    q.Submit(Req(NetKind::HttpGet, 7, true));
    NetRequest r;
    Assert(q.Next(0, &r));

    NetReply rep;
    Assert(!q.TakeReply(7, &rep));                      // not done yet
    q.Complete(7, NetResult::Ok, 404, 123);
    Assert(q.TakeReply(7, &rep));
    Assert(rep.result == NetResult::Ok && rep.status == 404 && rep.body_len == 123 && !rep.truncated);
    Assert(!q.TakeReply(7, &rep));                      // taken once
    Assert(!q.TakeReply(8, &rep));                      // someone else's
}

Fact("NetQueue Clips A Big Response And Keeps The Status")
{
    NetQueue q;
    q.Init();
    q.Submit(Req(NetKind::HttpGet, 7, true));
    NetRequest r;
    q.Next(0, &r);
    q.Complete(7, NetResult::Ok, 200, 100000);

    NetReply rep;
    Assert(q.TakeReply(7, &rep));
    Assert(rep.status == 200);
    Assert(rep.body_len == NET_HTTP_MAX_RESPONSE && rep.truncated);
}

Fact("NetQueue Fire And Forget Leaves No Reply")
{
    NetQueue q;
    q.Init();
    q.Submit(Req(NetKind::HttpPost, 5));
    NetRequest r;
    q.Next(0, &r);
    q.Complete(5, NetResult::Ok, 200, 10);
    NetReply rep;
    Assert(!q.TakeReply(5, &rep));
}

Fact("NetQueue Drops A Late Reply After The Waiter Gave Up")
{
    NetQueue q;
    q.Init();
    q.Submit(Req(NetKind::HttpGet, 7, true));
    NetRequest r;
    q.Next(0, &r);                                      // in flight
    q.Abandon(7);                                       // the script was cancelled

    q.Complete(7, NetResult::Ok, 200, 5);               // the worker finishes later
    NetReply rep;
    Assert(!q.TakeReply(7, &rep));
    Assert(q.Late() == 1);
    Assert(!q.Busy());                                  // and the worker is free again

    // A new op that happens to come next is unaffected
    q.Submit(Req(NetKind::HttpGet, 8, true));
    Assert(q.Next(0, &r) && r.generation == 8);
    q.Complete(8, NetResult::Ok, 200, 5);
    Assert(q.TakeReply(8, &rep));
}

Fact("NetQueue Abandon Removes A Queued Waited Request And Keeps The Others")
{
    NetQueue q;
    q.Init();
    q.Submit(Req(NetKind::HttpPost, 1));
    q.Submit(Req(NetKind::HttpGet,  2, true));
    q.Submit(Req(NetKind::HttpPost, 3));
    q.Abandon(2);
    q.Abandon(1);                                       // fire and forget: abandon doesn't touch it
    Assert(q.Count() == 2);

    NetRequest r;
    Assert(Run(q, 0, &r) && r.generation == 1);
    Assert(Run(q, 0, &r) && r.generation == 3);
    Assert(!q.Next(0, &r));
}

Fact("NetQueue Times Out A Stuck Waited Request And Drops Its Late Completion")
{
    NetQueue q;
    q.Init();
    q.Submit(Req(NetKind::HttpGet, 7, true));
    NetRequest r;
    q.Next(1000, &r);

    q.Expire(1000 + NET_HTTP_TIMEOUT_MS - 1);
    NetReply rep;
    Assert(!q.TakeReply(7, &rep));                      // not yet
    q.Expire(1000 + NET_HTTP_TIMEOUT_MS);
    Assert(q.TakeReply(7, &rep));
    Assert(rep.result == NetResult::Timeout);

    Assert(q.Busy());                                   // the worker hasn't come back yet
    q.Complete(7, NetResult::Ok, 200, 5);               // when it does, the answer is too late
    Assert(!q.TakeReply(7, &rep));
    Assert(q.Late() == 1);
    Assert(!q.Busy());
}

Fact("NetQueue Expire Survives The Clock Wrapping And Ignores Fire And Forget")
{
    NetQueue q;
    q.Init();
    q.Submit(Req(NetKind::HttpGet, 7, true));
    NetRequest r;
    q.Next(0xFFFFFFF0u, &r);
    q.Expire(0xFFFFFFF0u + NET_HTTP_TIMEOUT_MS);        // wrapped
    NetReply rep;
    Assert(q.TakeReply(7, &rep) && rep.result == NetResult::Timeout);
    q.Complete(7, NetResult::Ok, 0, 0);

    q.Submit(Req(NetKind::HttpPost, 8));
    q.Next(0, &r);
    q.Expire(100000);                                   // nobody waits: nothing to report
    Assert(!q.TakeReply(8, &rep));
}

Fact("NetQueue Reinit Forgets Everything")
{
    NetQueue q;
    q.Init();
    q.Submit(Req(NetKind::HttpPost, 1));
    NetRequest r;
    q.Next(0, &r);
    q.Submit(Req(NetKind::HttpPost, 2));
    q.Init();
    Assert(q.Count() == 0 && !q.Busy() && q.Dropped() == 0);
    Assert(!q.Next(0, &r));
}

Fact("NetQueue A Waited Reply Carries The Body Clipped To The Cap")
{
    static uint8_t big[NET_HTTP_MAX_RESPONSE + 10];
    NetQueue q;
    q.Init();
    Assert(q.Submit(Req(NetKind::HttpGet, 5, true)));
    NetRequest r;
    Assert(q.Next(0, &r));
    q.Complete(5, NetResult::Ok, 200, sizeof(big), big);
    NetReply rep;
    Assert(q.TakeReply(5, &rep));
    Assert(rep.body == big && rep.truncated && rep.body_len == NET_HTTP_MAX_RESPONSE);

    Assert(q.Submit(Req(NetKind::HttpGet, 6, true)));
    Assert(q.Next(0, &r));
    q.Complete(6, NetResult::Ok, 204, 0, big);              // no body bytes: no body pointer
    Assert(q.TakeReply(6, &rep));
    Assert(rep.body == nullptr && rep.body_len == 0);
}
