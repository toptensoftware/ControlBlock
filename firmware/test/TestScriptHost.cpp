// The Lua runtime behind the `script` op (script_host.cpp, script_api.cpp): the run
// model, the sandbox and limits, and the API that mirrors the ops. Scripts are run
// through the op registry like any op, against a fake clock, HID sink and net host.
// The config is the sample baked by host/src/bake (activities "Watch" = 0, "Listen" = 1).

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "script_env.h"

using namespace SimpleLib;
using namespace config_env;

using namespace script_env;

// ---- the run model ----

Fact("Script Runs To The End And Logs")
{
    Rig r;
    Assert(r.Run("log('hi', 1, true, nil)") == OpStatus::Done);
    Assert(strcmp(r.log, "hi\t1\ttrue\tnil") == 0);
    Assert(r.host.Runs() == 1 && r.host.Errors() == 0);
    Assert(!r.host.Running());
}

Fact("Script Print Is Log")
{
    Rig r;
    Assert(r.Run("print('x')") == OpStatus::Done);
    Assert(strcmp(r.log, "x") == 0);
}

Fact("Script Param Is A Global And Empty When Absent")
{
    Rig r;
    Assert(r.Run("log('[' .. param .. ']')", "velocity=10") == OpStatus::Done);
    Assert(strcmp(r.log, "[velocity=10]") == 0);
    Rig q;
    Assert(q.Run("log('[' .. param .. ']')", "") == OpStatus::Done);
    Assert(strcmp(q.log, "[]") == 0);
}

Fact("Script Globals Persist For One Source And Locals Do Not")
{
    Rig r;
    const char *counter = "n = (n or 0) + 1 local l = (l or 0) + 1 log(n, l)";
    Assert(r.Run(counter) == OpStatus::Done);
    Assert(r.Run(counter) == OpStatus::Done);
    Assert(strcmp(r.log, "1\t1|2\t1") == 0);
}

Fact("Script Globals Are Not Shared Between Scripts")
{
    Rig r;
    Assert(r.Run("shared = 5") == OpStatus::Done);
    Assert(r.Run("log(tostring(shared))") == OpStatus::Done);
    Assert(strcmp(r.log, "nil") == 0);
}

Fact("Script Syntax Error Is An Error Logged Once")
{
    Rig r;
    const char *bad = "this is not lua";
    Assert(r.Run(bad) == OpStatus::Error);
    Assert(r.task.error != nullptr);
    Assert(r.log_lines == 1 && r.Logged("script"));
    Assert(r.Run(bad) == OpStatus::Error);                  // cached as bad: no recompile, not logged again
    Assert(r.log_lines == 1);
    Assert(r.Run("log('still works')") == OpStatus::Done);
}

Fact("Script Runtime Error Ends The Run And Is Logged")
{
    Rig r;
    Assert(r.Run("error('boom')") == OpStatus::Error);
    Assert(r.Logged("boom"));
    Assert(r.host.Errors() == 1 && !r.host.Running());
    Assert(r.Run("log('next')") == OpStatus::Done);
}

Fact("Script Bytecode Is Rejected")
{
    Rig r;
    Assert(r.Run("\x1bLua\x54\x00") == OpStatus::Error);
    Assert(r.log_lines == 1);
}

Fact("Script Without A Host Or Platform Does Nothing")
{
    Rig r;
    r.exec.script = nullptr;
    Assert(r.Run("log('x')") == OpStatus::Done);
    Assert(r.log_lines == 0);
}

// ---- the sandbox and the limits ----

Fact("Script Sandbox Has No Files Os Loading Or Coroutines")
{
    Rig r;
    Assert(r.Run("log(type(io), type(os), type(load), type(loadfile), type(dofile), type(require), type(coroutine), type(debug), type(package))")
           == OpStatus::Done);
    Assert(strcmp(r.log, "nil\tnil\tnil\tnil\tnil\tnil\tnil\tnil\tnil") == 0);
}

Fact("Script Sandbox Keeps The Safe Libraries")
{
    Rig r;
    Assert(r.Run("log(string.rep('a', 3), #table.pack(1, 2), math.max(1, 5), utf8.char(65), tonumber('12') // 5)") == OpStatus::Done);
    Assert(strcmp(r.log, "aaa\t2\t5\tA\t2") == 0);
}

Fact("Script Memory Over The Cap Is An Error And The Host Survives")
{
    Rig r(64 * 1024);
    Assert(r.Run("local t = {} for i = 1, 1000000 do t[i] = i end") == OpStatus::Error);
    Assert(r.host.HeapRefused() > 0);
    Assert(r.host.HeapUsed() <= 64 * 1024);
    Assert(r.Run("log('ok')") == OpStatus::Done);
}

Fact("Script That Never Yields Is Stopped By The Instruction Budget")
{
    Rig r;
    r.auto_tick = 5;                                        // the clock moves as the hook reads it
    Assert(r.Run("while true do end") == OpStatus::Error);
    Assert(r.Logged("too long"));
    r.auto_tick = 0;
    Assert(r.Run("log('after')") == OpStatus::Done);
}

Fact("Script Wall Timeout Cancels A Run That Waits Too Long")
{
    Rig r;
    Assert(r.Begin("delay(60000)") == OpStatus::Pending);
    r.now += SCRIPT_WALL_MS - 100;
    Assert(r.PollIt() == OpStatus::Pending);
    r.now += 200;
    Assert(r.PollIt() == OpStatus::Error);
    Assert(r.Logged("timed out"));
    Assert(!r.host.Running());
}

// ---- the API: sub-ops ----

Fact("Script Delay Yields Until Its Time")
{
    Rig r;
    Assert(r.Begin("log('a') delay(50) log('b')") == OpStatus::Pending);
    Assert(strcmp(r.log, "a") == 0);
    r.now += 49;
    Assert(r.PollIt() == OpStatus::Pending);
    r.now += 1;
    Assert(r.PollIt() == OpStatus::Done);
    Assert(strcmp(r.log, "a|b") == 0);
}

Fact("Script Delay Checks Its Argument")
{
    Rig r;
    Assert(r.Run("log(delay(0))") == OpStatus::Done);
    Assert(r.Run("log(delay(60001))") == OpStatus::Done);
    Assert(r.Run("log(delay('x'))") == OpStatus::Done);
    Assert(strstr(r.log, "nil") != nullptr);
    Assert(r.started == 3 + 0);                             // only the three script ops: no delay op started
}

Fact("Script Hid Taps Run In Order With The Gaps Kept")
{
    Rig r;
    Assert(r.Run("hid.key('a') hid.key('b') hid.key('ctrl+c')") == OpStatus::Done);
    Assert(r.sent == 6);
    Assert(r.reports[0][0] == 0 && r.reports[0][2] == 0x04);        // a down
    Assert(r.reports[1][2] == 0);                                   // a up
    Assert(r.reports[2][2] == 0x05);                                // b down
    Assert(r.reports[3][2] == 0);
    Assert(r.reports[4][0] == 0x01 && r.reports[4][2] == 0x06);     // ctrl+c down
    Assert(r.reports[5][0] == 0 && r.reports[5][2] == 0);
}

Fact("Script Hid Down And Up Do Not Yield")
{
    Rig r;
    Assert(r.Begin("hid.key('shift', 'down') hid.key('a') hid.key('shift', 'up')") == OpStatus::Pending);
    Assert(r.sent == 2);                                            // shift down, a down: waiting out the tap
    Assert(r.reports[0][0] == 0x02);
}

Fact("Script Hid Rejects Hold Bad Modes And Bad Chords")
{
    Rig r;
    Assert(r.Run("log(hid.key('a', 'hold'))") == OpStatus::Done);
    Assert(r.Run("log(hid.key('a', 'nope'))") == OpStatus::Done);
    Assert(r.Run("log(hid.key('not a key'))") == OpStatus::Done);
    Assert(r.Run("log(hid.key(5))") == OpStatus::Done);
    Assert(r.sent == 0);
    Assert(r.log_lines == 4);
    Assert(!r.Logged("true"));
}

Fact("Script Hid Table Form")
{
    Rig r;
    Assert(r.Run("hid.key{ keys = 'x', mode = 'down' }") == OpStatus::Done);
    Assert(r.sent == 1 && r.reports[0][2] == 0x1b);
}

Fact("Script Cancel Mid Tap Lets Every Key Go")
{
    Rig r;
    Assert(r.Begin("hid.key('a') log('never')") == OpStatus::Pending);
    Assert(r.reports[r.sent - 1][2] == 0x04);
    r.CancelIt();
    Assert(r.reports[r.sent - 1][2] == 0 && r.reports[r.sent - 1][0] == 0);
    Assert(!r.host.Running());
    Assert(!r.Logged("never"));
}

Fact("Script SwitchActivity Looks Up The Name")
{
    Rig r;
    Assert(r.Run("log(switchActivity('Listen'))") == OpStatus::Done);
    Assert(r.n_switched == 1 && r.switched[0] == 1);
    Assert(strcmp(r.log, "true") == 0);
    Assert(r.Run("switchActivity(nil)") == OpStatus::Done);          // none
    Assert(r.n_switched == 2 && r.switched[1] == -1);
}

Fact("Script SwitchActivity Reports Unknown Names And Refusals")
{
    Rig r;
    Assert(r.Run("log(switchActivity('Nope'))") == OpStatus::Done);
    Assert(strstr(r.log, "nil") && r.n_switched == 0);
    r.switch_error = "activity switch refused: op queue full";
    Assert(r.Run("log(switchActivity('Watch'))") == OpStatus::Done);
    Assert(r.Logged("queue full"));
}

Fact("Script Millis Is The Clock")
{
    Rig r;
    r.now = 5000;
    Assert(r.Run("log(millis())") == OpStatus::Done);
    Assert(strstr(r.log, "50") == r.log);
}

// ---- the API: network ----

Fact("Script Http Post Is Fire And Forget With Its Strings Kept")
{
    Rig r;
    Assert(r.Run("http.post('http://avr/vol', 'delta=' .. (tonumber(param) // 10), nil, { ['X-A'] = '1' })", "40") == OpStatus::Done);
    Assert(r.net.n_sent == 1);
    const NetRequest &q = r.net.sent[0];
    Assert(q.kind == NetKind::HttpPost && !q.wait);
    Assert(strcmp(q.url, "http://avr/vol") == 0);
    Assert(q.body_len == 7 && memcmp(q.body, "delta=4", 7) == 0);
    Assert(strcmp(q.content_type, "application/x-www-form-urlencoded") == 0);
    Assert(strcmp(q.headers, "X-A: 1") == 0);
    // The script has finished and another runs: the queued request still reads
    Assert(r.Run("http.post('http://other/', 'zzzzzzz')") == OpStatus::Done);
    Assert(strcmp(q.url, "http://avr/vol") == 0 && memcmp(q.body, "delta=4", 7) == 0);
}

Fact("Script Http Requests Keep Their Strings Until The Ring Wraps")
{
    Rig r;
    Assert(r.Run("for i = 1, 3 do http.post('http://h/' .. i, 'b' .. i) end") == OpStatus::Done);
    Assert(r.net.n_sent == 3);
    Assert(strcmp(r.net.sent[0].url, "http://h/1") == 0);
    Assert(strcmp(r.net.sent[1].url, "http://h/2") == 0);
    Assert(strcmp(r.net.sent[2].url, "http://h/3") == 0);
    Assert(r.net.sent[0].url != r.net.sent[1].url);
}

Fact("Script Http Rejects Bad Urls And Oversize Bodies")
{
    Rig r;
    Assert(r.Run("log(http.post('https://x/', 'a'))") == OpStatus::Done);
    Assert(r.Run("log(http.post('ftp://x/', 'a'))") == OpStatus::Done);
    Assert(r.Run("log(http.post('http://x/', string.rep('a', 5000)))") == OpStatus::Done);
    Assert(r.Run("log(http.get(5))") == OpStatus::Done);
    Assert(r.net.n_sent == 0);
    Assert(r.log_lines == 4 && !r.Logged("true"));
}

Fact("Script Http Get Waits For The Reply And Returns Body And Status")
{
    Rig r;
    Assert(r.Begin("local b, s = http.get('http://tv/state', { A = 'b' }) log(b, s)") == OpStatus::Pending);
    Assert(r.net.n_sent == 1);
    const NetRequest &q = r.net.sent[0];
    Assert(q.kind == NetKind::HttpGet && q.wait && q.generation == 7);
    Assert(strcmp(q.url, "http://tv/state") == 0 && strcmp(q.headers, "A: b") == 0);

    Assert(r.PollIt() == OpStatus::Pending);                        // no reply yet
    static const uint8_t body[] = "hello";
    r.net.has_reply = true;
    r.net.reply_gen = 7;
    r.net.reply = {};
    r.net.reply.result   = NetResult::Ok;
    r.net.reply.status   = 200;
    r.net.reply.body     = body;
    r.net.reply.body_len = 5;
    Assert(r.PollIt() == OpStatus::Done);
    Assert(strcmp(r.log, "hello\t200") == 0);
}

Fact("Script Http Get Timeout And Failure Return Nil And A Reason")
{
    Rig r;
    Assert(r.Begin("log(http.get('http://tv/x'))") == OpStatus::Pending);
    r.net.has_reply = true;
    r.net.reply_gen = 7;
    r.net.reply = {};
    r.net.reply.result = NetResult::Timeout;
    Assert(r.PollIt() == OpStatus::Done);
    Assert(strstr(r.log, "nil\t") == r.log && r.Logged("timeout"));

    Rig q;
    Assert(q.Begin("log(http.get('http://tv/x'))") == OpStatus::Pending);
    q.net.has_reply = true;
    q.net.reply_gen = 7;
    q.net.reply = {};
    q.net.reply.result = NetResult::Failed;
    Assert(q.PollIt() == OpStatus::Done);
    Assert(q.Logged("failed"));
}

Fact("Script Http Get With Wi-Fi Down Or No Net Returns Nil")
{
    Rig r;
    r.net.is_connected = false;
    Assert(r.Run("log(http.get('http://tv/x'))") == OpStatus::Done);
    Assert(r.Logged("wi-fi"));
    Rig q(0, false);
    Assert(q.Run("log(http.get('http://tv/x'))") == OpStatus::Done);
    Assert(strstr(q.log, "nil") == q.log);
}

Fact("Script Http Get Reply False Is The Fire And Forget Op")
{
    Rig r;
    Assert(r.Run("log(http.get{ url = 'http://tv/x', reply = false })") == OpStatus::Done);
    Assert(r.net.n_sent == 1 && !r.net.sent[0].wait);
    Assert(strcmp(r.log, "true") == 0);
}

Fact("Script Cancel While Waiting For A Reply Abandons The Request")
{
    Rig r;
    Assert(r.Begin("http.get('http://tv/x') log('never')") == OpStatus::Pending);
    r.CancelIt();
    Assert(r.net.n_abandoned == 1 && r.net.abandoned[0] == 7);
    Assert(!r.host.Running());
    // A reply that comes later finds nobody
    r.net.has_reply = true;
    r.net.reply_gen = 7;
    Assert(r.Run("log('next')") == OpStatus::Done);
    Assert(!r.Logged("never"));
}

Fact("Script Udp Sends A String Or Hex")
{
    Rig r;
    Assert(r.Run("udp.send('10.0.0.5', 9, 'hi') udp.send('host.local', 7, { hex = 'DE ad 01' })") == OpStatus::Done);
    Assert(r.net.n_sent == 2);
    Assert(r.net.sent[0].kind == NetKind::Udp && r.net.sent[0].port == 9);
    Assert(strcmp(r.net.sent[0].host, "10.0.0.5") == 0 && r.net.sent[0].body_len == 2 && memcmp(r.net.sent[0].body, "hi", 2) == 0);
    Assert(strcmp(r.net.sent[1].host, "host.local") == 0 && r.net.sent[1].port == 7);
    Assert(r.net.sent[1].body_len == 3 && r.net.sent[1].body[0] == 0xDE && r.net.sent[1].body[1] == 0xAD && r.net.sent[1].body[2] == 0x01);
}

Fact("Script Udp Checks Its Arguments")
{
    Rig r;
    Assert(r.Run("log(udp.send('h', 0, 'x'))") == OpStatus::Done);
    Assert(r.Run("log(udp.send('h', 70000, 'x'))") == OpStatus::Done);
    Assert(r.Run("log(udp.send('', 9, 'x'))") == OpStatus::Done);
    Assert(r.Run("log(udp.send('h', 9, { hex = 'xyz' }))") == OpStatus::Done);
    Assert(r.Run("log(udp.send('h', 9, { hex = 'abc' }))") == OpStatus::Done);      // odd digits
    Assert(r.net.n_sent == 0 && r.log_lines == 5 && !r.Logged("true"));
}

Fact("Script Wol Sends A Magic Packet Request")
{
    Rig r;
    Assert(r.Run("wol.send('AA:BB:CC:DD:EE:FF') wol.send('11-22-33-44-55-66', '10.0.0.255')") == OpStatus::Done);
    Assert(r.net.n_sent == 2);
    Assert(r.net.sent[0].kind == NetKind::Wol && r.net.sent[0].mac[0] == 0xAA && r.net.sent[0].mac[5] == 0xFF);
    Assert(r.net.sent[0].port == NET_WOL_PORT);
    Assert(strcmp(r.net.sent[1].Host(), "10.0.0.255") == 0 && r.net.sent[1].mac[0] == 0x11);
}

Fact("Script Wol Rejects A Bad Mac")
{
    Rig r;
    Assert(r.Run("log(wol.send('nope'))") == OpStatus::Done);
    Assert(r.Run("log(wol.send('AA:BB:CC:DD:EE'))") == OpStatus::Done);
    Assert(r.net.n_sent == 0 && !r.Logged("true"));
}

Fact("Script Net Calls Report A Full Net Queue")
{
    Rig r;
    r.net.accepts = false;
    Assert(r.Run("log(http.post('http://x/', 'a'))") == OpStatus::Done);
    Assert(r.Logged("queue full"));
}

// ---- state ----

Fact("Script Close Frees The Heap And A New Run Starts Fresh")
{
    Rig r;
    const char *s = "n = (n or 0) + 1 log(n)";
    Assert(r.Run(s) == OpStatus::Done);
    Assert(r.host.HeapUsed() > 0);
    r.host.Close();
    Assert(r.host.HeapUsed() == 0);
    Assert(r.Run(s) == OpStatus::Done);
    Assert(strcmp(r.log, "1|1") == 0);
}
