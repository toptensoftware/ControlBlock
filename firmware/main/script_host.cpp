#include <stdio.h>
#include <string.h>
#include "script_host.h"
#include "op_registry.h"

#include "lua_include.h"

namespace
{
    const char *SCRIPTS_KEY = "cb_scripts";             // registry: source pointer -> { chunk, env } or false (bad)

    // The state is built inside a protected call: an out-of-memory in the middle of it must
    // come back as an error, not reach Lua's panic handler (which aborts)
    int OpenLibs(lua_State *L)
    {
        luaL_requiref(L, "_G", luaopen_base, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(L, 1);

        // Nothing that loads code from outside the script
        static const char *banned[] = { "dofile", "loadfile", "load" };
        for (const char *name : banned) {
            lua_pushnil(L);
            lua_setglobal(L, name);
        }

        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, SCRIPTS_KEY);

        ScriptApiOpen(L);
        return 0;
    }

    // Arguments: the host, the script op, the trigger param. Leaves the run's coroutine in the
    // host (m_thread / m_thread_ref); an error here (a compile error, no memory) comes back as
    // the pcall's error message.
    struct StartArgs
    {
        ScriptHost                 *host;
        const cfgdata::scriptOp    *op;
        const char                 *param;
        lua_State                 **thread;
        int                        *thread_ref;
        bool                        known_bad;     // set: it failed to compile before (already logged)
    };

    void SetupEnv(lua_State *L, int chunk)
    {
        lua_newtable(L);                                // env
        lua_newtable(L);                                // its metatable: globals fall through to the API and libraries
        lua_pushglobaltable(L);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "_G");                      // `_G` is the script's own globals, not the shared ones
        lua_pushvalue(L, -1);
        lua_setupvalue(L, chunk, 1);                    // the chunk's _ENV (pops the copy)
    }

    int DoStart(lua_State *L)
    {
        StartArgs *a = (StartArgs *)lua_touserdata(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, SCRIPTS_KEY);        // 2: cache
        lua_pushlightuserdata(L, (void *)a->op->source);       // 3: key
        lua_rawget(L, 2);                                       // 3: entry
        if (lua_isnil(L, 3)) {
            lua_pop(L, 1);
            if (luaL_loadbufferx(L, a->op->source, a->op->source_count, "=script", "t") != LUA_OK) {
                // Remember it is bad: not compiled (or logged) again
                lua_pushlightuserdata(L, (void *)a->op->source);
                lua_pushboolean(L, 0);
                lua_rawset(L, 2);
                return lua_error(L);                            // the message is on top
            }
            SetupEnv(L, 3);                                     // 3: chunk, 4: env
            lua_newtable(L);                                    // 5: entry
            lua_pushvalue(L, 3);
            lua_rawseti(L, 5, 1);
            lua_pushvalue(L, 4);
            lua_rawseti(L, 5, 2);
            lua_pushlightuserdata(L, (void *)a->op->source);
            lua_pushvalue(L, 5);
            lua_rawset(L, 2);
            lua_replace(L, 3);                                  // 3: entry
            lua_settop(L, 3);
        }
        if (lua_isboolean(L, 3)) {                              // known bad
            a->known_bad = true;
            lua_pushliteral(L, "script has a syntax error (see the first log line)");
            return lua_error(L);
        }

        lua_rawgeti(L, 3, 2);                                   // 4: env
        lua_pushstring(L, a->param ? a->param : "");
        lua_setfield(L, 4, "param");
        lua_rawgeti(L, 3, 1);                                   // 5: chunk

        lua_State *T = lua_newthread(L);                        // 6: the run's coroutine
        *a->thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);        // anchored (pops it)
        *a->thread = T;
        lua_xmove(L, T, 1);                                     // the chunk onto it
        lua_sethook(T, ScriptHostHook, LUA_MASKCOUNT, SCRIPT_HOOK_COUNT);
        return 0;
    }
}

void ScriptHost::Init(const ScriptPlatform &platform, const cfgdata::configRoot *root)
{
    m_platform = platform;
    m_root     = root;
    m_heap     = {};
    m_heap.ctx     = platform.ctx;
    m_heap.realloc = platform.realloc;
    m_heap.cap     = platform.heap_cap ? platform.heap_cap : SCRIPT_HEAP_CAP;
    m_L = m_thread = nullptr;
    m_running = false;
    m_wait = ScriptWait::None;
    m_runs = m_errors = 0;
    m_net_arena = nullptr;
    m_net_next = 0;
}

void ScriptHost::Log(const char *line)
{
    if (m_platform.log)
        m_platform.log(line, m_platform.ctx);
}

bool ScriptHost::EnsureState()
{
    if (m_L)
        return true;
    m_L = lua_newstate(LuaHeapAlloc, &m_heap);
    if (!m_L)
        return false;
    *(ScriptHost **)lua_getextraspace(m_L) = this;      // threads copy it: the API finds the host from any of them
    lua_pushcfunction(m_L, OpenLibs);
    if (lua_pcall(m_L, 0, 0, 0) != LUA_OK) {
        lua_close(m_L);
        m_L = nullptr;
        return false;
    }
    return true;
}

void ScriptHost::Close()
{
    if (m_running && m_exec)
        Cancel(*m_exec, m_sub);
    if (m_L)
        lua_close(m_L);
    m_L = m_thread = nullptr;
    m_running = false;
    if (m_net_arena && m_platform.realloc)
        m_platform.realloc(m_net_arena, 0, m_platform.ctx);
    m_net_arena = nullptr;
    m_net_slot = nullptr;
    m_net_next = 0;
}

void ScriptHost::EndRun()
{
    if (m_thread) {
        luaL_unref(m_L, LUA_REGISTRYINDEX, m_thread_ref);
        m_thread = nullptr;
    }
    m_running = false;
    m_wait = ScriptWait::None;
}

// The run is abandoned: whatever it waits on is cancelled
void ScriptHost::AbortRun(ExecCtx &exec)
{
    if (m_wait == ScriptWait::Op)
        OpCancel(exec, &m_op.base, m_sub);
    else if (m_wait == ScriptWait::Reply && exec.net && exec.net->abandon)
        exec.net->abandon(m_reply_gen, exec.net->ctx);
    EndRun();
}

void ScriptHostHook(lua_State *L, lua_Debug *)
{
    ScriptHost *h = *(ScriptHost **)lua_getextraspace(L);
    if (OpReached(h->Now(), h->SliceStart() + SCRIPT_SLICE_MS))
        luaL_error(L, "script ran too long without waiting");
}

OpStatus ScriptHost::Start(ExecCtx &exec, const cfgdata::scriptOp *op, OpTask &task)
{
    if (!m_platform.realloc)
        return OpStatus::Done;                          // scripts are off
    m_exec       = &exec;
    m_generation = task.generation;
    m_runs++;

    if (!EnsureState()) {
        Log("script: out of memory");
        m_errors++;
        task.error = "script: out of memory";
        return OpStatus::Error;
    }

    StartArgs a = { this, op, task.param, &m_thread, &m_thread_ref, false };
    m_thread = nullptr;
    lua_pushcfunction(m_L, DoStart);
    lua_pushlightuserdata(m_L, &a);
    if (lua_pcall(m_L, 1, 0, 0) != LUA_OK) {
        char line[160];
        snprintf(line, sizeof(line), "script: %s", lua_tostring(m_L, -1));
        lua_pop(m_L, 1);
        if (m_thread)                                   // anchored before the failure
            EndRun();
        if (!a.known_bad)
            Log(line);
        m_errors++;
        task.error = "script error";
        return OpStatus::Error;
    }

    m_running       = true;
    m_wait          = ScriptWait::None;
    m_net_error     = false;
    m_wall_deadline = Now() + SCRIPT_WALL_MS;
    return Resume(exec, task);
}

OpStatus ScriptHost::Resume(ExecCtx &, OpTask &task)
{
    m_slice_start = Now();
    int nres = 0;
    int st = lua_resume(m_thread, m_L, 0, &nres);
    if (st == LUA_OK) {
        EndRun();
        return OpStatus::Done;
    }
    if (st == LUA_YIELD)
        return OpStatus::Pending;

    char line[160];
    snprintf(line, sizeof(line), "script: %s", lua_tostring(m_thread, -1) ? lua_tostring(m_thread, -1) : "error");
    Log(line);
    m_errors++;
    EndRun();
    task.error = "script error";
    return OpStatus::Error;
}

OpStatus ScriptHost::Poll(ExecCtx &exec, OpTask &task)
{
    if (!m_running)
        return OpStatus::Done;
    m_exec = &exec;

    if (OpReached(Now(), m_wall_deadline)) {
        Log("script: timed out");
        m_errors++;
        AbortRun(exec);
        task.error = "script timed out";
        return OpStatus::Error;
    }

    if (m_wait == ScriptWait::Op) {
        OpStatus st = OpPoll(exec, &m_op.base, m_sub);
        if (st == OpStatus::Pending)
            return OpStatus::Pending;
        m_last       = st;
        m_last_error = st == OpStatus::Error ? m_sub.error : nullptr;
    } else if (m_wait == ScriptWait::Reply) {
        NetHost *net = exec.net;
        NetReply rep = {};
        if (!net || !net->take_reply(m_reply_gen, &rep, net->ctx))
            return OpStatus::Pending;
        m_reply = rep;
    }
    m_wait = ScriptWait::None;
    return Resume(exec, task);
}

void ScriptHost::Cancel(ExecCtx &exec, OpTask &)
{
    if (!m_running)
        return;
    m_exec = &exec;
    AbortRun(exec);
}

OpStatus ScriptHost::StartOp()
{
    m_sub            = {};
    m_sub.param      = "";
    m_sub.generation = m_generation;
    OpStatus st = OpStart(*m_exec, &m_op.base, m_sub);
    m_last       = st;
    m_last_error = st == OpStatus::Error ? m_sub.error : nullptr;
    if (st == OpStatus::Pending)
        m_wait = ScriptWait::Op;
    return st;
}

// ---- strings kept for network requests ----

void ScriptHost::NetSlotBegin()
{
    m_net_slot = nullptr;
    if (!m_net_arena)
        m_net_arena = (uint8_t *)m_platform.realloc(nullptr, (size_t)SCRIPT_NET_SLOTS * SCRIPT_NET_SLOT_BYTES, m_platform.ctx);
    if (!m_net_arena) {
        m_net_error = true;
        return;
    }
    m_net_slot = m_net_arena + (size_t)m_net_next * SCRIPT_NET_SLOT_BYTES;
    m_net_next = (m_net_next + 1) % SCRIPT_NET_SLOTS;
    m_net_used = 0;
}

uint8_t *ScriptHost::NetKeepBuffer(size_t len)
{
    if (!m_net_slot || m_net_used + len > SCRIPT_NET_SLOT_BYTES) {
        m_net_error = true;
        return nullptr;
    }
    uint8_t *p = m_net_slot + m_net_used;
    m_net_used += len;
    return p;
}

const char *ScriptHost::NetKeep(const void *data, size_t len, bool nul_terminate)
{
    uint8_t *p = NetKeepBuffer(len + (nul_terminate ? 1 : 0));
    if (!p)
        return nullptr;
    if (len)
        memcpy(p, data, len);
    if (nul_terminate)
        p[len] = 0;
    return (const char *)p;
}
