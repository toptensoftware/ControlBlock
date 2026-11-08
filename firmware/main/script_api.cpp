// The Lua API of the `script` op (docs/standalone-ops-plan.md section 3): one function per
// op, built on the same executors the op queue uses (a scratch op in the ScriptHost,
// run through OpStart/OpPoll). A call that has to wait (a delay, a key tap, an HTTP
// reply) yields the script's coroutine; ScriptHost::Poll resumes it when the wait is over.
//
//   delay(ms)                              yields; true
//   switchActivity(name | nil)             waits for the switch's ops (the script parks in the
//                                          op queue); true, or nil, err (refused if they hold a script)
//   hid.key(keys [, mode])                 mode tap (default), down, up; a tap yields
//   http.get(url [, headers])              yields; body, status  (or nil, err)
//   http.get{ url=, headers=, reply=false } the fire and forget op; true
//   http.post(url, body [, contentType [, headers]])    true
//   udp.send(host, port, data)             data: a string or { hex = "de ad" }
//   wol.send(mac [, broadcast])
//   log(...), print(...), millis(), and the global `param`
//
// A call takes its arguments positionally or as one table of named fields. A failure
// returns nil, reason; it does not raise. Header tables are { Name = "value" }.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "exec_net.h"
#include "hid_chord.h"
#include "hid_keyboard.h"
#include "script_host.h"

#include "lua_include.h"

#define FORM_CONTENT_TYPE "application/x-www-form-urlencoded"
#define DELAY_MAX_MS      60000

namespace
{
    ScriptHost *H(lua_State *L)
    {
        return *(ScriptHost **)lua_getextraspace(L);
    }

    int Fail(lua_State *L, const char *msg)
    {
        lua_pushnil(L);
        lua_pushstring(L, msg);
        return 2;
    }

    int FailF(lua_State *L, const char *fmt, ...)
    {
        char buf[96];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        return Fail(L, buf);
    }

    // How a finished sub-op answers: true, or nil and why
    int Results(lua_State *L)
    {
        ScriptHost *h = H(L);
        if (h->m_last == OpStatus::Error)
            return Fail(L, h->m_last_error ? h->m_last_error : "error");
        lua_pushboolean(L, 1);
        return 1;
    }

    int ReplyResults(lua_State *L)
    {
        ScriptHost *h = H(L);
        switch (h->m_reply.result) {
        case NetResult::Ok:
            if (h->m_reply.body && h->m_reply.body_len)
                lua_pushlstring(L, (const char *)h->m_reply.body, h->m_reply.body_len);
            else
                lua_pushliteral(L, "");
            lua_pushinteger(L, h->m_reply.status);
            return 2;
        case NetResult::Timeout:
            return Fail(L, "timeout");
        default:
            return Fail(L, "failed");
        }
    }

    int Continue(lua_State *L, int, lua_KContext)
    {
        return Results(L);
    }

    int ContinueReply(lua_State *L, int, lua_KContext)
    {
        return ReplyResults(L);
    }

    // Run the scratch op; a Pending one yields the script until it is over
    int RunOp(lua_State *L)
    {
        ScriptHost *h = H(L);
        if (h->StartOp() == OpStatus::Pending)
            return lua_yieldk(L, 0, 0, Continue);
        return Results(L);
    }

    // ---- arguments: positional, or one table of named fields ----

    // Push argument `i` (or field `name` of a table passed alone); nil when absent. Returns its
    // index. `n` is the call's argument count (lua_gettop at entry: the pushes change it).
    int Arg(lua_State *L, int n, int i, const char *name)
    {
        if (n == 1 && lua_istable(L, 1))
            lua_getfield(L, 1, name);
        else if (i <= n)
            lua_pushvalue(L, i);
        else
            lua_pushnil(L);
        return lua_gettop(L);
    }

    bool IsStr(lua_State *L, int idx) { return lua_type(L, idx) == LUA_TSTRING; }

    // An integer in [lo, hi] (a float with an integral value counts); strings do not
    bool GetInt(lua_State *L, int idx, lua_Integer lo, lua_Integer hi, lua_Integer *out)
    {
        int ok = 0;
        if (lua_type(L, idx) != LUA_TNUMBER)
            return false;
        lua_Integer v = lua_tointegerx(L, idx, &ok);
        if (!ok || v < lo || v > hi)
            return false;
        *out = v;
        return true;
    }

    // A { Name = "value" } table as "Name: value" lines joined by CRLF, kept for the request.
    // Null with *ok false on a bad table; null with *ok true for none. (A plain buffer, not a
    // luaL_Buffer: that one puts a box on the stack, which lua_next must not see.)
    const char *Headers(lua_State *L, int idx, bool *ok)
    {
        ScriptHost *h = H(L);
        *ok = true;
        if (lua_isnil(L, idx))
            return nullptr;
        if (!lua_istable(L, idx)) {
            *ok = false;
            return nullptr;
        }
        char buf[1024];
        size_t len = 0;
        int n = 0;
        int base = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, idx)) {                          // key at base+1, value at base+2
            int vt = lua_type(L, base + 2);
            if (lua_type(L, base + 1) != LUA_TSTRING || (vt != LUA_TSTRING && vt != LUA_TNUMBER)) {
                lua_settop(L, base);
                *ok = false;
                return nullptr;
            }
            size_t kl, vl;
            const char *k = lua_tolstring(L, base + 1, &kl);
            const char *v = lua_tolstring(L, base + 2, &vl);   // a number value is converted in place (not the key)
            if (len + kl + vl + 4 >= sizeof(buf)) {
                lua_settop(L, base);
                *ok = false;
                return nullptr;
            }
            if (n++) {
                buf[len++] = '\r';
                buf[len++] = '\n';
            }
            memcpy(buf + len, k, kl); len += kl;
            buf[len++] = ':';
            buf[len++] = ' ';
            memcpy(buf + len, v, vl); len += vl;
            lua_pop(L, 1);                                  // the value; the key stays for lua_next
        }
        if (!n)
            return nullptr;
        const char *kept = h->NetKeep(buf, len, true);
        if (!kept)
            *ok = false;
        return kept;
    }

    bool HttpUrl(const char *s) { return strncmp(s, "http://", 7) == 0 && s[7] != 0; }

    // ---- the functions ----

    int Delay(lua_State *L)
    {
        int n = lua_gettop(L);
        lua_Integer ms;
        Arg(L, n, 1, "ms");
        if (!GetInt(L, lua_gettop(L), 1, DELAY_MAX_MS, &ms))
            return Fail(L, "delay: ms must be a number from 1 to 60000");
        ScriptHost *h = H(L);
        h->m_op.delay.base.op  = OP_DELAY;
        h->m_op.delay.duration = (uint32_t)ms;
        return RunOp(L);
    }

    int SwitchActivity(lua_State *L)
    {
        int n = lua_gettop(L);
        ScriptHost *h = H(L);
        int a = Arg(L, n, 1, "activity");
        uint32_t index = LAYOUT_ACTIVITY_NONE;
        if (!lua_isnil(L, a)) {
            if (!IsStr(L, a))
                return Fail(L, "switchActivity: expected an activity name or nil");
            const char *name = lua_tostring(L, a);
            bool found = false;
            if (h->m_root)
                for (uint32_t i = 0; i < h->m_root->activities_count; i++)
                    if (strcmp(h->m_root->activities[i].name, name) == 0) {
                        index = i;
                        found = true;
                        break;
                    }
            if (!found)
                return Fail(L, "unknown activity");
        }
        h->m_op.sw.base.op = OP_SWITCH_ACTIVITY;
        h->m_op.sw.index   = index;
        // The switch's ops run next; the script waits for them (parked in the op queue)
        OpStatus st = h->StartOp();
        if (st == OpStatus::Pending || (st == OpStatus::Done && h->ParkForSwitch()))
            return lua_yieldk(L, 0, 0, Continue);
        return Results(L);
    }

    int HidKey(lua_State *L)
    {
        int n = lua_gettop(L);
        ScriptHost *h = H(L);
        int k = Arg(L, n, 1, "keys");
        int m = Arg(L, n, 2, "mode");
        if (!IsStr(L, k))
            return Fail(L, "hid.key: expected a key chord such as 'ctrl+c'");

        uint8_t mode = HID_MODE_TAP;
        if (!lua_isnil(L, m)) {
            const char *s = IsStr(L, m) ? lua_tostring(L, m) : "";
            if (strcmp(s, "tap") == 0)        mode = HID_MODE_TAP;
            else if (strcmp(s, "down") == 0)  mode = HID_MODE_DOWN;
            else if (strcmp(s, "up") == 0)    mode = HID_MODE_UP;
            else if (strcmp(s, "hold") == 0)  return Fail(L, "hid.key: hold is only for press and release bindings");
            else                              return Fail(L, "hid.key: mode must be tap, down or up");
        }

        size_t len;
        const char *text = lua_tolstring(L, k, &len);
        uint8_t mods, key;
        if (const char *err = HidParseChord(text, len, &mods, &key))
            return FailF(L, "hid.key: %s", err);
        h->m_op.hid.base.op   = OP_HID_KEY;
        h->m_op.hid.mode      = mode;
        h->m_op.hid.modifiers = mods;
        h->m_op.hid.key       = key;
        return RunOp(L);
    }

    int HttpGet(lua_State *L)
    {
        int n = lua_gettop(L);
        ScriptHost *h = H(L);
        int u  = Arg(L, n, 1, "url");
        int hd = Arg(L, n, 2, "headers");
        int rp = Arg(L, n, 3, "reply");
        if (!IsStr(L, u) || !HttpUrl(lua_tostring(L, u)))
            return Fail(L, "http.get: url must be a string starting with http://");
        bool reply = !(lua_isboolean(L, rp) && !lua_toboolean(L, rp));

        h->NetSlotBegin();
        size_t ul;
        const char *ustr = lua_tolstring(L, u, &ul);
        const char *url = h->NetKeep(ustr, ul, true);
        bool ok;
        const char *headers = Headers(L, hd, &ok);
        if (!ok || !url || h->m_net_error)
            return Fail(L, ok ? "http.get: request too large" : "http.get: headers must be a table of strings");

        if (!reply) {
            h->m_op.get.base.op  = OP_HTTP_GET;
            h->m_op.get.url      = url;
            h->m_op.get.headers  = headers;
            return RunOp(L);
        }

        // Waited: the reply comes back to this call
        ExecCtx &exec = *h->m_exec;
        if (!exec.net)
            return Fail(L, "no network");
        NetRequest req = {};
        req.kind    = NetKind::HttpGet;
        req.url     = url;
        req.headers = headers;
        h->m_sub            = {};
        h->m_sub.param      = "";
        h->m_sub.generation = h->m_generation;
        if (NetSend(exec, req, h->m_sub, true) == OpStatus::Error)
            return Fail(L, h->m_sub.error ? h->m_sub.error : "error");
        h->m_reply_gen = h->m_generation;
        h->SetWaiting(ScriptWait::Reply);
        return lua_yieldk(L, 0, 0, ContinueReply);
    }

    int HttpPost(lua_State *L)
    {
        int n = lua_gettop(L);
        ScriptHost *h = H(L);
        int u  = Arg(L, n, 1, "url");
        int b  = Arg(L, n, 2, "body");
        int ct = Arg(L, n, 3, "contentType");
        int hd = Arg(L, n, 4, "headers");
        if (!IsStr(L, u) || !HttpUrl(lua_tostring(L, u)))
            return Fail(L, "http.post: url must be a string starting with http://");
        if (!IsStr(L, b))
            return Fail(L, "http.post: body must be a string");
        if (!lua_isnil(L, ct) && !IsStr(L, ct))
            return Fail(L, "http.post: contentType must be a string");
        size_t bl;
        const char *body = lua_tolstring(L, b, &bl);
        if (bl > LAYOUT_MAX_OP_DATA)
            return Fail(L, "http.post: body is too large");

        h->NetSlotBegin();
        size_t ul;
        const char *ustr = lua_tolstring(L, u, &ul);
        const char *url = h->NetKeep(ustr, ul, true);
        const char *data = h->NetKeep(body, bl, false);
        size_t cl = 0;
        const char *cts = lua_isnil(L, ct) ? FORM_CONTENT_TYPE : lua_tolstring(L, ct, &cl);
        if (lua_isnil(L, ct))
            cl = strlen(cts);
        const char *ctype = h->NetKeep(cts, cl, true);
        bool ok;
        const char *headers = Headers(L, hd, &ok);
        if (!ok || h->m_net_error)
            return Fail(L, ok ? "http.post: request too large" : "http.post: headers must be a table of strings");

        h->m_op.post.base.op     = OP_HTTP_POST;
        h->m_op.post.url         = url;
        h->m_op.post.data_count  = (uint32_t)bl;
        h->m_op.post.data        = (uint8_t *)data;
        h->m_op.post.contentType = ctype;
        h->m_op.post.headers     = headers;
        return RunOp(L);
    }

    int HexValue(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    int UdpSend(lua_State *L)
    {
        int n = lua_gettop(L);
        ScriptHost *h = H(L);
        int ho = Arg(L, n, 1, "host");
        int po = Arg(L, n, 2, "port");
        int da = Arg(L, n, 3, "data");
        lua_Integer port;
        if (!IsStr(L, ho) || lua_rawlen(L, ho) == 0)
            return Fail(L, "udp.send: host must be a name or address");
        if (!GetInt(L, po, 1, 65535, &port))
            return Fail(L, "udp.send: port must be 1 to 65535");

        h->NetSlotBegin();
        size_t hl;
        const char *hs = lua_tolstring(L, ho, &hl);
        const char *host = h->NetKeep(hs, hl, true);
        const uint8_t *data = nullptr;
        uint32_t dl = 0;
        if (IsStr(L, da)) {
            size_t n;
            const char *s = lua_tolstring(L, da, &n);
            if (n == 0 || n > LAYOUT_MAX_OP_DATA)
                return Fail(L, "udp.send: data must be 1 to 4096 bytes");
            data = (const uint8_t *)h->NetKeep(s, n, false);
            dl = (uint32_t)n;
        } else if (lua_istable(L, da)) {
            lua_getfield(L, da, "hex");
            if (!IsStr(L, lua_gettop(L)))
                return Fail(L, "udp.send: data table needs a hex string");
            size_t n;
            const char *s = lua_tolstring(L, lua_gettop(L), &n);
            int digits = 0;
            for (size_t i = 0; i < n; i++) {
                if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')
                    continue;
                if (HexValue(s[i]) < 0)
                    return Fail(L, "udp.send: hex must be hex digits");
                digits++;
            }
            if (digits == 0 || (digits & 1) || digits / 2 > LAYOUT_MAX_OP_DATA)
                return Fail(L, "udp.send: hex must be an even number of digits, 1 to 4096 bytes");
            uint8_t *out = h->NetKeepBuffer((size_t)digits / 2);
            int pos = 0, hi = -1;
            for (size_t i = 0; out && i < n; i++) {
                int v = HexValue(s[i]);
                if (v < 0)
                    continue;
                if (hi < 0) {
                    hi = v;
                } else {
                    out[pos++] = (uint8_t)(hi << 4 | v);
                    hi = -1;
                }
            }
            data = out;
            dl = (uint32_t)digits / 2;
        } else {
            return Fail(L, "udp.send: data must be a string or { hex = '...' }");
        }
        if (h->m_net_error)
            return Fail(L, "udp.send: request too large");

        h->m_op.udp.base.op     = OP_UDP_PACKET;
        h->m_op.udp.host        = host;
        h->m_op.udp.port        = (uint16_t)port;
        h->m_op.udp.data_count  = dl;
        h->m_op.udp.data        = (uint8_t *)data;
        return RunOp(L);
    }

    // "AA:BB:CC:DD:EE:FF" or with dashes
    bool ParseMac(const char *s, size_t len, uint8_t mac[6])
    {
        if (len != 17)
            return false;
        for (int i = 0; i < 6; i++) {
            const char *p = s + i * 3;
            int hi = HexValue(p[0]), lo = HexValue(p[1]);
            if (hi < 0 || lo < 0 || (i < 5 && p[2] != ':' && p[2] != '-'))
                return false;
            mac[i] = (uint8_t)(hi << 4 | lo);
        }
        return true;
    }

    int WolSend(lua_State *L)
    {
        int n = lua_gettop(L);
        ScriptHost *h = H(L);
        int m = Arg(L, n, 1, "mac");
        int b = Arg(L, n, 2, "broadcast");
        uint8_t mac[6];
        size_t ml = 0;
        const char *ms = IsStr(L, m) ? lua_tolstring(L, m, &ml) : "";
        if (!ParseMac(ms, ml, mac))
            return Fail(L, "wol.send: mac must look like AA:BB:CC:DD:EE:FF");
        const char *bcast = nullptr;
        if (!lua_isnil(L, b)) {
            if (!IsStr(L, b) || lua_rawlen(L, b) == 0)
                return Fail(L, "wol.send: broadcast must be an address");
            h->NetSlotBegin();
            size_t bl;
            const char *bs = lua_tolstring(L, b, &bl);
            bcast = h->NetKeep(bs, bl, true);
            if (h->m_net_error)
                return Fail(L, "wol.send: request too large");
        }
        h->m_op.wol.base.op = OP_SEND_WOL;
        memcpy(h->m_op.wol.mac, mac, 6);
        h->m_op.wol.broadcast = bcast;
        return RunOp(L);
    }

    int Log(lua_State *L)
    {
        int n = lua_gettop(L);                              // before the buffer puts a placeholder on the stack
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        for (int i = 1; i <= n; i++) {
            if (i > 1)
                luaL_addchar(&b, '\t');
            size_t len;
            const char *s = luaL_tolstring(L, i, &len);     // pushes the string
            luaL_addvalue(&b);
            (void)s;
        }
        luaL_pushresult(&b);
        H(L)->Log(lua_tostring(L, -1));
        return 0;
    }

    int Millis(lua_State *L)
    {
        lua_pushinteger(L, (lua_Integer)H(L)->Now());
        return 1;
    }

    void Register(lua_State *L, const char *lib, const luaL_Reg *fns)
    {
        lua_newtable(L);
        luaL_setfuncs(L, fns, 0);
        lua_setglobal(L, lib);
    }
}

void ScriptApiOpen(lua_State *L)
{
    lua_pushcfunction(L, Delay);          lua_setglobal(L, "delay");
    lua_pushcfunction(L, SwitchActivity); lua_setglobal(L, "switchActivity");
    lua_pushcfunction(L, Log);            lua_setglobal(L, "log");
    lua_pushcfunction(L, Log);            lua_setglobal(L, "print");
    lua_pushcfunction(L, Millis);         lua_setglobal(L, "millis");

    static const luaL_Reg hid[]  = { { "key", HidKey }, { nullptr, nullptr } };
    static const luaL_Reg http[] = { { "get", HttpGet }, { "post", HttpPost }, { nullptr, nullptr } };
    static const luaL_Reg udp[]  = { { "send", UdpSend }, { nullptr, nullptr } };
    static const luaL_Reg wol[]  = { { "send", WolSend }, { nullptr, nullptr } };
    Register(L, "hid", hid);
    Register(L, "http", http);
    Register(L, "udp", udp);
    Register(L, "wol", wol);
}
