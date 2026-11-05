// Parity (docs/standalone-ops-plan.md step 7): everything an op can do, a script can do.
// Each op is run twice, once from a blob op and once from the equivalent Lua call, and the
// two must leave the same trace on the fake HID sink, net host and activity switch, in the
// same time. The coverage check fails when a new op code appears without a case here
// (`script` is the only op with no Lua call: nothing to call).

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "script_env.h"

using namespace SimpleLib;
using namespace script_env;

namespace
{
    // What an op did, as text
    void Describe(Rig &r, int polls, char *out, size_t cap)
    {
        size_t n = 0;
        auto add = [&](const char *fmt, auto... a) { n += snprintf(out + n, cap - n, fmt, a...); };
        add("polls=%d", polls);
        for (int i = 0; i < r.sent; i++) {
            add(" hid[");
            for (int b = 0; b < 8; b++)
                add("%02x", r.reports[i][b]);
            add("]");
        }
        for (int i = 0; i < r.n_switched; i++)
            add(" switch=%d", r.switched[i]);
        for (int i = 0; i < r.net.n_sent; i++) {
            const NetRequest &q = r.net.sent[i];
            add(" net{kind=%d wait=%d port=%u", (int)q.kind, (int)q.wait, (unsigned)q.port);
            add(" url=%s hdr=%s ct=%s", q.url ? q.url : "-", q.headers ? q.headers : "-", q.content_type ? q.content_type : "-");
            add(" host=%s", (q.kind == NetKind::Udp || q.kind == NetKind::Wol) ? q.Host() : "-");
            add(" body=");
            for (uint32_t b = 0; b < q.body_len; b++)
                add("%02x", q.body[b]);
            add(" mac=");
            if (q.kind == NetKind::Wol)
                for (int b = 0; b < 6; b++)
                    add("%02x", q.mac[b]);
            add("}");
        }
    }

    typedef void (*BlobFn)(Rig &r, OpStatus *st, int *polls);

    // Start `op` and poll to the end, the clock moving 1 ms per poll (as Rig::Run does)
    void RunBlob(Rig &r, const cfgdata::op *op, OpStatus *st, int *polls)
    {
        r.task = {};
        r.task.param = "";
        r.task.generation = 7;
        *st = OpStart(r.exec, op, r.task);
        for (*polls = 0; *st == OpStatus::Pending && *polls < 100000; (*polls)++) {
            r.now++;
            *st = OpPoll(r.exec, op, r.task);
        }
    }

    struct Case
    {
        const char *name;
        uint32_t    code;
        const char *lua;
        BlobFn      blob;
    };

    cfgdata::hidKeyOp Hid(uint8_t mode, uint8_t mods, uint8_t key)
    {
        cfgdata::hidKeyOp o = {};
        o.base.op = OP_HID_KEY; o.mode = mode; o.modifiers = mods; o.key = key;
        return o;
    }

    const Case CASES[] = {
        { "delay", OP_DELAY, "delay(50)",
          [](Rig &r, OpStatus *s, int *p) { cfgdata::delayOp o = {}; o.base.op = OP_DELAY; o.duration = 50; RunBlob(r, &o.base, s, p); } },
        { "switchActivity", OP_SWITCH_ACTIVITY, "switchActivity('Listen')",
          [](Rig &r, OpStatus *s, int *p) { cfgdata::switchActivityOp o = {}; o.base.op = OP_SWITCH_ACTIVITY; o.index = 1; RunBlob(r, &o.base, s, p); } },
        { "switchActivity none", OP_SWITCH_ACTIVITY, "switchActivity(nil)",
          [](Rig &r, OpStatus *s, int *p) { cfgdata::switchActivityOp o = {}; o.base.op = OP_SWITCH_ACTIVITY; o.index = CONFIG_ACTIVITY_NONE; RunBlob(r, &o.base, s, p); } },
        { "hidKey tap", OP_HID_KEY, "hid.key('ctrl+c')",
          [](Rig &r, OpStatus *s, int *p) { cfgdata::hidKeyOp o = Hid(HID_MODE_TAP, 0x01, 0x06); RunBlob(r, &o.base, s, p); } },
        { "hidKey down", OP_HID_KEY, "hid.key('shift+a', 'down')",
          [](Rig &r, OpStatus *s, int *p) { cfgdata::hidKeyOp o = Hid(HID_MODE_DOWN, 0x02, 0x04); RunBlob(r, &o.base, s, p); } },
        { "hidKey up", OP_HID_KEY, "hid.key('shift+a', 'down') hid.key('shift+a', 'up')",
          [](Rig &r, OpStatus *s, int *p) {
              cfgdata::hidKeyOp d = Hid(HID_MODE_DOWN, 0x02, 0x04), u = Hid(HID_MODE_UP, 0x02, 0x04);
              RunBlob(r, &d.base, s, p);
              RunBlob(r, &u.base, s, p);
          } },
        { "httpGet", OP_HTTP_GET, "http.get{ url = 'http://tv/x', headers = { A = 'b' }, reply = false }",
          [](Rig &r, OpStatus *s, int *p) { cfgdata::httpGetOp o = {}; o.base.op = OP_HTTP_GET; o.url = "http://tv/x"; o.headers = "A: b"; RunBlob(r, &o.base, s, p); } },
        { "httpPost", OP_HTTP_POST, "http.post('http://x/p', 'a=1')",
          [](Rig &r, OpStatus *s, int *p) {
              cfgdata::httpPostOp o = {};
              o.base.op = OP_HTTP_POST; o.url = "http://x/p"; o.data_count = 3; o.data = (uint8_t *)"a=1";
              o.contentType = "application/x-www-form-urlencoded";
              RunBlob(r, &o.base, s, p);
          } },
        { "httpPost json", OP_HTTP_POST, "http.post('http://x/p', '{}', 'application/json', { K = 'v' })",
          [](Rig &r, OpStatus *s, int *p) {
              cfgdata::httpPostOp o = {};
              o.base.op = OP_HTTP_POST; o.url = "http://x/p"; o.data_count = 2; o.data = (uint8_t *)"{}";
              o.contentType = "application/json"; o.headers = "K: v";
              RunBlob(r, &o.base, s, p);
          } },
        { "udpPacket", OP_UDP_PACKET, "udp.send('10.0.0.5', 9, 'hi')",
          [](Rig &r, OpStatus *s, int *p) {
              cfgdata::udpPacketOp o = {};
              o.base.op = OP_UDP_PACKET; o.host = "10.0.0.5"; o.port = 9; o.data_count = 2; o.data = (uint8_t *)"hi";
              RunBlob(r, &o.base, s, p);
          } },
        { "udpPacket hex", OP_UDP_PACKET, "udp.send('h.local', 7, { hex = 'de ad' })",
          [](Rig &r, OpStatus *s, int *p) {
              static uint8_t data[] = { 0xDE, 0xAD };
              cfgdata::udpPacketOp o = {};
              o.base.op = OP_UDP_PACKET; o.host = "h.local"; o.port = 7; o.data_count = 2; o.data = data;
              RunBlob(r, &o.base, s, p);
          } },
        { "sendWol", OP_SEND_WOL, "wol.send('AA:BB:CC:DD:EE:FF')",
          [](Rig &r, OpStatus *s, int *p) {
              cfgdata::sendWolOp o = {};
              o.base.op = OP_SEND_WOL;
              const uint8_t mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
              memcpy(o.mac, mac, 6);
              RunBlob(r, &o.base, s, p);
          } },
        { "sendWol broadcast", OP_SEND_WOL, "wol.send('11:22:33:44:55:66', '10.0.0.255')",
          [](Rig &r, OpStatus *s, int *p) {
              cfgdata::sendWolOp o = {};
              o.base.op = OP_SEND_WOL; o.broadcast = "10.0.0.255";
              const uint8_t mac[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
              memcpy(o.mac, mac, 6);
              RunBlob(r, &o.base, s, p);
          } },
    };
}

Fact("ScriptParity Every Op Does The Same From A Script As From A Blob")
{
    for (const Case &c : CASES) {
        Rig blob;
        OpStatus bs;
        int bp;
        c.blob(blob, &bs, &bp);
        char want[1024];
        Describe(blob, bp, want, sizeof(want));

        Rig lua;
        // The Lua run counts its polls the same way: the script's own start is not one
        cfgdata::scriptOp op = lua.Op(c.lua);
        lua.task = {};
        lua.task.param = "";
        lua.task.generation = 7;
        OpStatus ls = OpStart(lua.exec, &op.base, lua.task);
        int lp = 0;
        for (; ls == OpStatus::Pending && lp < 100000; lp++) {
            lua.now++;
            ls = OpPoll(lua.exec, &op.base, lua.task);
        }
        char got[1024];
        Describe(lua, lp, got, sizeof(got));

        if (bs != ls || strcmp(want, got) != 0) {
            printf("parity case '%s':\n  blob: %s\n  lua:  %s\n", c.name, want, got);
            Assert(false);
        }
    }
}

Fact("ScriptParity Has A Lua Call For Every Op")
{
    // Every op the registry knows, but `script`, has a case above
    for (uint32_t code = OP_DELAY; code <= OP_SCRIPT; code++) {
        Assert(OpFind(code) != nullptr);
        if (code == OP_SCRIPT)
            continue;
        bool covered = false;
        for (const Case &c : CASES)
            covered = covered || c.code == code;
        if (!covered)
            printf("op %u (%s) has no script parity case\n", (unsigned)code, OpFind(code)->name);
        Assert(covered);
    }
    // A new op code needs a registry entry, a Lua call and a case: this fails until it has them
    Assert(OpFind(OP_SCRIPT + 1) == nullptr);
}
