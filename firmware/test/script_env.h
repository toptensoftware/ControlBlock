#pragma once

// The rig the script tests share: a ScriptHost over the sample layout, driven through the op
// registry like any op, against a fake clock, HID sink and net host.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Core.h"
#include "layout_env.h"
#include "hid_keyboard.h"
#include "net_env.h"
#include "op_registry.h"
#include "script_host.h"

namespace script_env
{
    using namespace SimpleLib;
    using namespace layout_env;
}

namespace script_env
{
    struct Rig
    {
        List<uint8_t> bytes = LoadSample();
        const layoutdata::layoutRoot *root = nullptr;
        uint32_t        now = 1000;
        uint32_t        auto_tick = 0;          // added to the clock on every read (a script that never yields)
        FakeNet         net;
        HidKeyboard     hid;
        uint8_t         reports[32][8];
        int             sent = 0;
        ScriptHost      host;
        ExecCtx         exec = {};
        OpTask          task = {};
        char            log[2048] = {};
        int             log_lines = 0;
        int             switched[8];
        int             n_switched = 0;
        const char     *switch_error = nullptr;
        int             started = 0;            // every op that started, sub-ops included

        explicit Rig(size_t cap = 0, bool net_on = true)
        {
            root = Relocate(bytes);
            HidSink sink = {};
            sink.ctx  = this;
            sink.send = [](const uint8_t *r, void *c) {
                Rig *g = (Rig *)c;
                if (g->sent < 32)
                    memcpy(g->reports[g->sent++], r, 8);
            };
            hid.Init(sink);

            ScriptPlatform p = {};
            p.ctx      = this;
            p.heap_cap = cap;
            p.realloc  = [](void *ptr, size_t n, void *) -> void * {
                if (n == 0) { free(ptr); return nullptr; }
                return realloc(ptr, n);
            };
            p.log = [](const char *line, void *c) {
                Rig *g = (Rig *)c;
                if (g->log_lines++)
                    strncat(g->log, "|", sizeof(g->log) - strlen(g->log) - 1);
                strncat(g->log, line, sizeof(g->log) - strlen(g->log) - 1);
            };
            host.Init(p, root);

            exec.ctx    = this;
            exec.now_ms = [](void *c) { Rig *g = (Rig *)c; g->now += g->auto_tick; return g->now; };
            exec.started = [](const layoutdata::op *, void *c) { ((Rig *)c)->started++; };
            exec.hid    = &hid;
            exec.net    = net_on ? &net.host : nullptr;
            exec.script = &host;
            exec.root   = root;
            exec.switch_activity = [](int index, void *c) -> const char * {
                Rig *g = (Rig *)c;
                if (g->switch_error)
                    return g->switch_error;
                if (g->n_switched < 8)
                    g->switched[g->n_switched++] = index;
                return nullptr;
            };
            task.param      = "";
            task.generation = 7;
        }

        // The op for a source (the pointer is the script's identity, so use string literals)
        layoutdata::scriptOp Op(const char *src)
        {
            layoutdata::scriptOp op = {};
            op.base.op       = OP_SCRIPT;
            op.source        = src;
            op.source_count  = (uint32_t)strlen(src);
            return op;
        }

        // Start and drive to the end, the clock moving 1 ms per poll
        OpStatus Run(const char *src, const char *param = "", int max_polls = 100000)
        {
            layoutdata::scriptOp op = Op(src);
            task = {};
            task.param      = param;
            task.generation = 7;
            OpStatus st = OpStart(exec, &op.base, task);
            for (int i = 0; st == OpStatus::Pending && i < max_polls; i++) {
                now++;
                st = OpPoll(exec, &op.base, task);
            }
            return st;
        }

        // Start only
        OpStatus Begin(const char *src)
        {
            layoutdata::scriptOp op = Op(src);
            begun = op;
            task = {};
            task.param      = "";
            task.generation = 7;
            return OpStart(exec, &begun.base, task);
        }
        OpStatus PollIt() { return OpPoll(exec, &begun.base, task); }
        void CancelIt() { OpCancel(exec, &begun.base, task); }
        layoutdata::scriptOp begun = {};

        bool Logged(const char *s) const { return strstr(log, s) != nullptr; }
    };
}

