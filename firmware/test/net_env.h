#pragma once

// A fake NetHost for the executor tests: records what the executors submit, and
// lets a test say whether Wi-Fi is up, whether the queue accepts, and what the
// netif's subnet broadcast is.

#include <string.h>
#include "net_host.h"

struct FakeNet
{
    NetRequest  sent[16];
    int         n_sent      = 0;
    bool        is_connected = true;
    bool        accepts      = true;
    bool        has_broadcast = false;      // the netif has a subnet broadcast
    char        broadcast_addr[NET_ADDR_LEN] = "192.168.1.255";
    int         broadcast_asked = 0;
    bool        has_reply = false;          // take_reply hands out `reply` once
    NetReply    reply = {};
    uint32_t    reply_gen = 0;
    uint32_t    abandoned[8] = {};
    int         n_abandoned = 0;
    NetHost     host;

    FakeNet()
    {
        host = {};
        host.ctx = this;
        host.connected = [](void *c) { return ((FakeNet *)c)->is_connected; };
        host.broadcast = [](char out[NET_ADDR_LEN], void *c) {
            FakeNet *f = (FakeNet *)c;
            f->broadcast_asked++;
            if (!f->has_broadcast)
                return false;
            strcpy(out, f->broadcast_addr);
            return true;
        };
        host.take_reply = [](uint32_t gen, NetReply *out, void *c) {
            FakeNet *f = (FakeNet *)c;
            if (!f->has_reply || f->reply_gen != gen)
                return false;
            *out = f->reply;
            f->has_reply = false;
            return true;
        };
        host.abandon = [](uint32_t gen, void *c) {
            FakeNet *f = (FakeNet *)c;
            if (f->n_abandoned < 8)
                f->abandoned[f->n_abandoned++] = gen;
        };
        host.submit = [](const NetRequest &r, void *c) {
            FakeNet *f = (FakeNet *)c;
            if (!f->accepts)
                return false;
            if (f->n_sent < 16)
                f->sent[f->n_sent++] = r;
            return true;
        };
    }
};
