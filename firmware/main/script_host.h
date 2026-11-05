#pragma once

// The Lua runtime behind the `script` op (docs/standalone-ops-plan.md section 2,
// docs/standalone.md "Actions"). One Lua state on a capped heap holds every
// script's compiled chunk and environment; a run is a coroutine that the op
// queue's one active slot drives:
//
//   Start  - compile (first run only; a syntax error ends the op in Error, once
//            logged), set the global `param`, resume. Done if the script ran to
//            the end, Pending if it yielded waiting on something.
//   Poll   - the thing it waits on (a sub-op, or an HTTP reply) is checked; when
//            it is over the script is resumed. The wall timeout is checked here.
//   Cancel - takeover/release: the sub-op is cancelled and the coroutine dropped.
//
// The script API (script_api.cpp) calls the same executors as the op queue
// (OpStart/OpPoll on a scratch op), so a script can do anything an op can.
// Platform independent and single threaded, like the executors: the owner
// serialises. Strings the network ops keep (url, body, ...) are copied into a ring
// of slots that outlives the script run.
//
// Sandbox: only base (without dofile, loadfile, load), string, table, math and utf8;
// text chunks only; `print` is `log`. An instruction-count hook aborts a script that
// runs SCRIPT_SLICE_MS without yielding; the heap cap bounds memory.

#include <stddef.h>
#include <stdint.h>
#include "config_blob.h"           // CONFIG_MAX_OP_DATA
#include "config_types.h"
#include "lua_heap.h"
#include "net_host.h"
#include "op_exec.h"

struct lua_State;
struct lua_Debug;

#define SCRIPT_HEAP_CAP        (256 * 1024)
#define SCRIPT_SLICE_MS        100              // longest a script may run between yields
#define SCRIPT_WALL_MS         30000            // longest a run may take in all (longer than one HTTP timeout)
#define SCRIPT_HOOK_COUNT      10000            // VM instructions between budget checks
#define SCRIPT_NET_SLOTS       12               // more than NET_QUEUE_SIZE + the one in flight
#define SCRIPT_NET_SLOT_BYTES  6144             // url, headers, content type and body of one request

struct ScriptPlatform
{
    void *ctx;
    void *(*realloc)(void *ptr, size_t size, void *ctx);    // size 0 frees. Null: scripts are off (ops do nothing)
    void (*log)(const char *line, void *ctx);               // script output and errors (may be null)
    size_t heap_cap;                                        // 0: SCRIPT_HEAP_CAP
};

// One sub-op's scratch storage: the call being made now (the executors read it as a blob op)
union ScriptOpData
{
    cfgdata::op               base;
    cfgdata::delayOp          delay;
    cfgdata::switchActivityOp sw;
    cfgdata::hidKeyOp         hid;
    cfgdata::httpGetOp        get;
    cfgdata::httpPostOp       post;
    cfgdata::udpPacketOp      udp;
    cfgdata::sendWolOp        wol;
};

// What the running script is waiting for
enum class ScriptWait : uint8_t
{
    None,
    Op,             // the scratch op is Pending (delay, hid tap)
    Reply,          // an HTTP reply (http.get)
};

class ScriptHost
{
public:
    void Init(const ScriptPlatform &platform, const cfgdata::configRoot *root);
    // Free the Lua state and everything cached (the config changed, takeover): a run in
    // progress must have been cancelled first
    void Close();

    OpStatus Start(ExecCtx &exec, const cfgdata::scriptOp *op, OpTask &task);
    OpStatus Poll(ExecCtx &exec, OpTask &task);
    void     Cancel(ExecCtx &exec, OpTask &task);

    bool     Running() const      { return m_running; }
    size_t   HeapUsed() const     { return m_heap.used; }
    size_t   HeapPeak() const     { return m_heap.peak; }
    unsigned HeapRefused() const  { return m_heap.refused; }
    uint32_t Runs() const         { return m_runs; }
    uint32_t Errors() const       { return m_errors; }

    // ---- for the script API (script_api.cpp) ----

    void Log(const char *line);
    uint32_t Now() const          { return m_exec->Now(); }
    uint32_t SliceStart() const   { return m_slice_start; }     // when the script was last resumed

    // Start the scratch op `m_op` through the registry. Done/Error: the caller returns its results
    // at once. Pending: the caller yields, and Poll resumes it when the op is over.
    OpStatus StartOp();
    // After a Pending StartOp / an HTTP submit, record what to wait for
    void SetWaiting(ScriptWait w)  { m_wait = w; }

    // A string kept for a network request: copied into the next slot of the ring; null when
    // the request does not fit. Call NetSlotBegin first, then NetKeep for each string.
    void        NetSlotBegin();
    const char *NetKeep(const void *data, size_t len, bool nul_terminate);
    uint8_t    *NetKeepBuffer(size_t len);          // for a body built in place

    ExecCtx                    *m_exec = nullptr;
    const cfgdata::configRoot  *m_root = nullptr;
    ScriptOpData                m_op = {};          // the scratch op the API fills in
    OpTask                      m_sub = {};         // its task
    OpStatus                    m_last = OpStatus::Done;    // how the last sub-op / reply ended
    const char                 *m_last_error = nullptr;     // and why (a string that outlives it)
    uint32_t                    m_reply_gen = 0;    // Reply: the waited request
    NetReply                    m_reply = {};       // Reply: its answer (body valid until the next request)
    uint32_t                    m_generation = 0;   // script op's OpTask::generation: waited requests carry it
    bool                        m_net_error = false;    // a net call could not be built (too big, ...)
    bool                        m_reply_result = false; // the pending call returns an HTTP reply, not true
    char                        m_message[96] = {}; // the text of the last API-level error

private:
    bool     EnsureState();
    bool     OpenState();
    bool     Compile(const cfgdata::scriptOp *op, int *env_index);  // pushes the env table; false: logged
    OpStatus Resume(ExecCtx &exec, OpTask &task);
    void     EndRun();
    void     AbortRun(ExecCtx &exec);

    ScriptPlatform  m_platform = {};
    LuaHeap         m_heap = {};
    lua_State      *m_L = nullptr;
    lua_State      *m_thread = nullptr;
    int             m_thread_ref = 0;
    bool            m_running = false;
    ScriptWait      m_wait = ScriptWait::None;
    uint32_t        m_slice_start = 0;
    uint32_t        m_wall_deadline = 0;
    uint32_t        m_runs = 0, m_errors = 0;

    uint8_t        *m_net_arena = nullptr;
    int             m_net_next = 0;
    size_t          m_net_used = 0;
    uint8_t        *m_net_slot = nullptr;

    friend struct ScriptApi;
};

// The instruction-count hook that enforces SCRIPT_SLICE_MS (script_host.cpp)
void ScriptHostHook(lua_State *L, lua_Debug *ar);

// Opens the script API in the state: delay, switchActivity, hid.key, http.get/post, udp.send,
// wol.send, log, millis (script_api.cpp)
void ScriptApiOpen(lua_State *L);
