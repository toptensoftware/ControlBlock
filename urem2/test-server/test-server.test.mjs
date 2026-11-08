// Tests for the urem2 test server. From the repo root:  node --test "urem2/**/*.test.mjs"
// The server runs on free ports (0) on 127.0.0.1 with a short ATV connect delay and a captured log.

import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import dgram from "node:dgram";
import http from "node:http";
import { startUremServer, createFakeYamaha } from "./test-server.mjs";
import { YAMAHA_API, setPower, setInput, setVolume, setMute, setSoundProgram, getStatus } from "../tables/yamaha.mjs";
import { irPacket } from "../tables/irtx.mjs";
import { HOTPC_MAC, HOTPC_COMMANDS, SLIDESHOW_COMMANDS, displayElementsBody } from "../tables/hotpc.mjs";

const PORTS = { irtx: 0, atvd: 0, wol: 0, yamaha: 0, pc: 0, slideshow: 0 };
const CONNECT_MS = 150;

const lines = [];
let srv;

before(async () => {
    srv = await startUremServer({ ports: PORTS, host: "127.0.0.1", log: (...a) => lines.push(a.join(" ")),
                                  atvConnectMs: CONNECT_MS });
});

after(() => srv.close());

// Log lines added from now on
function mark()
{
    const start = lines.length;
    return () => lines.slice(start);
}

async function waitFor(pred, ms = 2000)
{
    const end = Date.now() + ms;
    while (Date.now() < end)
    {
        if (pred())
            return;
        await new Promise(r => setTimeout(r, 10));
    }
    assert.fail("timed out waiting");
}

const TS = String.raw`\d\d:\d\d:\d\d\.\d\d\d`;

function sendUdp(port, data)
{
    return new Promise((resolve, reject) => {
        const s = dgram.createSocket("udp4");
        s.send(data, port, "127.0.0.1", err => {
            s.close();
            err ? reject(err) : resolve();
        });
    });
}

// Sends one datagram and resolves the first new log line
async function udpLine(port, data)
{
    const seen = mark();
    await sendUdp(port, data);
    await waitFor(() => seen().length > 0);
    return seen()[0];
}

function wol(mac, count = 16, last = mac)
{
    const bytes = m => Buffer.from(m.split(":").map(h => parseInt(h, 16)));
    return Buffer.concat([Buffer.alloc(6, 0xff), ...Array(count - 1).fill(bytes(mac)), bytes(last)]);
}

// ---- skeleton: ports, irtx, WoL ----

test("ports are the bound ones (0 = a free port)", () => {
    assert.deepEqual(Object.keys(srv.ports).sort(), ["atvd", "irtx", "pc", "slideshow", "wol", "yamaha"]);
    for (const k of Object.keys(srv.ports))
        assert.ok(srv.ports[k] > 0, k);
});

test("irtx: a packet logs the code's name", async () => {
    assert.match(await udpLine(srv.ports.irtx, Buffer.from(irPacket("lg.powerOn"), "hex")),
                 new RegExp(`^${TS} IR lg\\.powerOn$`));
    assert.match(await udpLine(srv.ports.irtx, Buffer.from(irPacket("pana.guide", { repeat: true }), "hex")),
                 /^\S+ IR pana\.guide \(repeat\)$/);
});

test("irtx: a shared code logs every name", async () => {
    assert.match(await udpLine(srv.ports.irtx, Buffer.from(irPacket("atv.right"), "hex")),
                 /^\S+ IR atv\.right\/atv\.forward\/atv\.forward10x$/);
});

test("irtx: an unknown code and a malformed packet are logged as such", async () => {
    assert.match(await udpLine(srv.ports.irtx, Buffer.from(irPacket("NEC:0x12345678"), "hex")),
                 /^\S+ IR unknown NEC:0x12345678$/);
    assert.match(await udpLine(srv.ports.irtx, Buffer.from("hi")),
                 /^\S+ irtx: bad packet \(length 2, expected 17\) b'hi'$/);
});

test("WoL: the HOT PC's MAC is named, others logged raw", async () => {
    assert.match(await udpLine(srv.ports.wol, wol(HOTPC_MAC)),
                 new RegExp(`^${TS} HOT PC: wake \\(${HOTPC_MAC}\\)$`));
    assert.match(await udpLine(srv.ports.wol, wol("01:23:45:67:89:ab")), /^\S+ WoL: wake 01:23:45:67:89:ab$/);
    assert.match(await udpLine(srv.ports.wol, wol(HOTPC_MAC, 16, "01:23:45:67:89:ab")),
                 /^\S+ HOT PC: wake \(04:d4:c4:4e:da:2b\), bad repeats$/);
    assert.match(await udpLine(srv.ports.wol, Buffer.from("x")), /^\S+ WoL: not a magic packet \(1 bytes\) b'x'$/);
});

// ---- fake Apple TV daemon ----

// Sends a command from its own socket; resolves { reply, ms } (the reply must come back to that socket)
function atv(text)
{
    return new Promise((resolve, reject) => {
        const s = dgram.createSocket("udp4");
        const t = Date.now();
        const timer = setTimeout(() => { s.close(); reject(new Error(`no reply to ${text}`)); }, 3000);
        s.on("message", data => {
            clearTimeout(timer);
            s.close();
            resolve({ reply: data.toString("utf8"), ms: Date.now() - t });
        });
        s.bind(0, "127.0.0.1", () => s.send(text, srv.ports.atvd, "127.0.0.1"));
    });
}

// A command and its log line
async function atvLogged(text)
{
    const seen = mark();
    const r = await atv(text);
    await waitFor(() => seen().some(l => l.includes(" ATV: ")));
    return { ...r, line: seen().find(l => l.includes(" ATV: ")) };
}

async function atvConnect()
{
    if ((await atv("ping")).reply !== "pong")
        await waitFor(() => srv.atv.connected);
}

test("ATV: ping says connecting, then pong once connected", async () => {
    await atv("disconnect");
    const r = await atvLogged("ping");
    assert.equal(r.reply, "connecting");
    assert.match(r.line, new RegExp(`^${TS} ATV: ping -> connecting$`));
    assert.equal((await atv("ping")).reply, "connecting");      // still connecting: no second connect
    await new Promise(r => setTimeout(r, CONNECT_MS + 50));
    assert.equal((await atv("ping")).reply, "pong");
});

test("ATV: key and app reply ok and log in device terms", async () => {
    await atvConnect();
    let r = await atvLogged("key menu hold");
    assert.equal(r.reply, "ok");
    assert.ok(r.ms < CONNECT_MS, `${r.ms} ms while connected`);
    assert.match(r.line, /^\S+ ATV: key menu hold -> ok$/);
    assert.match((await atvLogged("key up")).line, /^\S+ ATV: key up single -> ok$/);       // the default action is shown
    assert.match((await atvLogged("app com.netflix.Netflix")).line, /^\S+ ATV: app Netflix \(com\.netflix\.Netflix\) -> ok$/);
    assert.match((await atvLogged("app org.example.other")).line, /^\S+ ATV: app org\.example\.other -> ok$/);
    assert.match((await atvLogged("text_set hi there")).line, /^\S+ ATV: text_set hi there -> ok$/);
    assert.match((await atvLogged("text_clear")).line, /^\S+ ATV: text_clear -> ok$/);
});

test("ATV: disconnect clears the connection", async () => {
    await atvConnect();
    const r = await atvLogged("disconnect");
    assert.equal(r.reply, "ok");
    assert.match(r.line, /^\S+ ATV: disconnect -> ok$/);
    assert.equal((await atv("ping")).reply, "connecting");
});

test("ATV: key and app while disconnected reply ok only after the connect delay", async () => {
    for (const cmd of ["key select", "app com.netflix.Netflix"])
    {
        await atvConnect();
        await atv("disconnect");
        const r = await atv(cmd);
        assert.equal(r.reply, "ok");
        assert.ok(r.ms >= CONNECT_MS - 20, `${cmd}: ${r.ms} ms`);
        assert.equal((await atv("ping")).reply, "pong");
    }
});

test("ATV: bad commands reply error: with the daemon's message", async () => {
    const cases = {
        "key nope": "error: Unknown key: 'nope'",
        "key menu triple": "error: unknown action 'triple' (use single, double, hold)",
        "launch x": "error: unknown command: 'launch'",
        "key": "error: key requires an argument",
        "app": "error: app requires an argument",
    };
    for (const [cmd, want] of Object.entries(cases))
    {
        const r = await atvLogged(cmd);
        assert.equal(r.reply, want, cmd);
        assert.ok(r.line.endsWith(`ATV: ${cmd} -> ${want}`), r.line);
    }
});

test("ATV: replies go to each sender's own port", async () => {
    await atvConnect();
    const [a, b] = await Promise.all([atv("key up"), atv("key nope")]);
    assert.equal(a.reply, "ok");
    assert.equal(b.reply, "error: Unknown key: 'nope'");
});

// ---- fake Yamaha, pure core (no sockets; the Lua tests drive it) ----

test("createFakeYamaha: getStatus is compact JSON of the state, logged", () => {
    const y = createFakeYamaha();
    const r = y.handle("GET", YAMAHA_API + getStatus());
    assert.equal(r.status, 200);
    assert.equal(r.type, "application/json");
    const json = JSON.parse(r.body);
    assert.equal(r.body, JSON.stringify(json));
    assert.equal(json.mute, false);
    assert.equal(json.sound_program, "surr_decoder");
    assert.equal(json.volume, y.state.volume);
    assert.equal(r.log, "Yamaha: getStatus (mute=false, sound_program=surr_decoder)");
});

test("createFakeYamaha: a setter changes the state; a full URL works too", () => {
    const y = createFakeYamaha();
    let r = y.handle("GET", "http://10.1.1.1:8080" + YAMAHA_API + setMute(true));
    assert.deepEqual([r.status, r.body, r.log], [200, '{"response_code":0}', "Yamaha: setMute true"]);
    assert.equal(y.state.mute, true);
    assert.equal(JSON.parse(y.handle("GET", YAMAHA_API + getStatus()).body).mute, true);
    r = y.handle("GET", YAMAHA_API + setVolume("up", 3));
    assert.equal(r.log, `Yamaha: setVolume up 3 -> ${y.state.volume}`);
});

test("createFakeYamaha: bad parameter is code 4, unknown path 404, state unchanged", () => {
    const y = createFakeYamaha();
    const before = { ...y.state };
    let r = y.handle("GET", YAMAHA_API + "/main/setMute?enable=1");
    assert.deepEqual([r.status, r.body], [200, '{"response_code":4}']);
    assert.match(r.log, /-> invalid parameter$/);
    r = y.handle("GET", YAMAHA_API + "/main/setSleep?sleep=30");
    assert.equal(r.status, 404);
    assert.equal(r.log, "Yamaha: unknown GET /YamahaExtendedControl/v1/main/setSleep?sleep=30 -> 404");
    assert.deepEqual(y.state, before);
});

test("createFakeYamaha: each instance has its own state", () => {
    const a = createFakeYamaha();
    const b = createFakeYamaha();
    a.handle("GET", YAMAHA_API + setMute(true));
    assert.equal(b.state.mute, false);
});

// ---- fake Yamaha over HTTP ----

const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
after(() => agent.destroy());

// One request over the keep-alive agent; resolves { status, body, json, localPort }
function request(port, method, path, { body, headers } = {})
{
    return new Promise((resolve, reject) => {
        const req = http.request({ host: "127.0.0.1", port, method, path, headers, agent }, res => {
            const chunks = [];
            res.on("data", c => chunks.push(c));
            res.on("end", () => {
                const text = Buffer.concat(chunks).toString("utf8");
                let json;
                try { json = JSON.parse(text); } catch { json = undefined; }
                resolve({ status: res.statusCode, body: text, json, localPort: req.socket.localPort });
            });
        });
        req.on("error", reject);
        req.end(body);
    });
}

const yamaha = path => request(srv.ports.yamaha, "GET", YAMAHA_API + path);

async function yamahaLogged(path)
{
    const seen = mark();
    const r = await yamaha(path);
    await waitFor(() => seen().length > 0);
    return { ...r, line: seen()[0] };
}

test("Yamaha: each setter changes the state and returns response_code 0", async () => {
    const steps = [
        [setPower(true), { power: "on" }, /Yamaha: setPower on$/],
        [setInput("hdmi2"), { input: "hdmi2" }, /Yamaha: setInput hdmi2$/],
        [setMute(true), { mute: true }, /Yamaha: setMute true$/],
        [setSoundProgram("7ch_stereo"), { sound_program: "7ch_stereo" }, /Yamaha: setSoundProgram 7ch_stereo$/],
        [setPower(false), { power: "standby" }, /Yamaha: setPower standby$/],
        [setMute(false), { mute: false }, /Yamaha: setMute false$/],
    ];
    for (const [path, change, logged] of steps)
    {
        const r = await yamahaLogged(path);
        assert.equal(r.status, 200, path);
        assert.equal(r.body, '{"response_code":0}');
        assert.match(r.line, new RegExp(`^${TS} ` + logged.source));
        for (const [k, v] of Object.entries(change))
            assert.equal(srv.yamaha[k], v, path);
    }
});

test("Yamaha: volume steps and clamps at 0 and max_volume", async () => {
    srv.yamaha.volume = 10;
    const r = await yamahaLogged(setVolume("up", 3));
    assert.equal(r.body, '{"response_code":0}');
    assert.equal(srv.yamaha.volume, 13);
    assert.match(r.line, /Yamaha: setVolume up 3 -> 13$/);
    await yamaha(setVolume("down"));
    assert.equal(srv.yamaha.volume, 12);
    await yamaha(setVolume("down", 5));
    await yamaha(setVolume("down", 5));
    await yamaha(setVolume("down", 5));
    assert.equal(srv.yamaha.volume, 0);
    srv.yamaha.volume = srv.yamaha.max_volume - 1;
    await yamaha(setVolume("up", 5));
    assert.equal(srv.yamaha.volume, srv.yamaha.max_volume);
    await yamaha("/main/setVolume?volume=up");                  // step defaults to 1
    assert.equal(srv.yamaha.volume, srv.yamaha.max_volume);
});

test("Yamaha: a bad parameter is response_code 4 and changes nothing", async () => {
    const before = { ...srv.yamaha };
    for (const path of ["/main/setInput?input=hdmi9", "/main/setPower?power=off", "/main/setMute?enable=1",
                        "/main/setVolume?volume=sideways", "/main/setVolume?volume=up&step=0",
                        "/main/setSoundProgram?program=straight", "/main/setInput"])
    {
        const r = await yamahaLogged(path);
        assert.equal(r.status, 200, path);
        assert.equal(r.body, '{"response_code":4}', path);
        assert.match(r.line, /Yamaha: .* -> invalid parameter$/, path);
    }
    assert.deepEqual(srv.yamaha, before);
});

test("Yamaha: an unknown path is a 404", async () => {
    let r = await yamahaLogged("/main/setSleep?sleep=30");
    assert.equal(r.status, 404);
    assert.match(r.line, /Yamaha: unknown GET \/YamahaExtendedControl\/v1\/main\/setSleep\?sleep=30 -> 404$/);
    r = await request(srv.ports.yamaha, "GET", "/other");
    assert.equal(r.status, 404);
});

test("Yamaha: getStatus is compact JSON with the real fields, reflecting the state", async () => {
    await yamaha(setInput("hdmi3"));
    await yamaha(setSoundProgram("surr_decoder"));
    await yamaha(setMute(false));
    const r = await yamahaLogged(getStatus());
    assert.equal(r.status, 200);
    assert.equal(r.body, JSON.stringify(r.json));                 // no spaces
    assert.ok(!r.body.includes(" "));
    for (const k of ["response_code", "power", "sleep", "volume", "max_volume", "mute", "input",
                     "distribution_enable", "sound_program", "surr_decoder_type", "direct", "enhancer",
                     "tone_control", "dialogue_level", "subwoofer_volume", "actual_volume"])
        assert.ok(k in r.json, k);
    assert.equal(r.json.response_code, 0);
    assert.equal(r.json.input, "hdmi3");
    assert.equal(r.json.sound_program, "surr_decoder");
    assert.equal(r.json.mute, false);
    assert.equal(r.json.volume, srv.yamaha.volume);
    assert.deepEqual(Object.keys(r.json.tone_control), ["mode", "bass", "treble"]);
    assert.deepEqual(Object.keys(r.json.actual_volume), ["mode", "value", "unit"]);
    assert.match(r.line, /Yamaha: getStatus \(mute=false, sound_program=surr_decoder\)$/);
});

test("Yamaha: a mute toggle done like the layout's Lua flips it twice", async () => {
    const toggle = async () => {
        const status = (await yamaha(getStatus())).body;
        const muted = /"mute"\s*:\s*true/.test(status);          // the Lua pattern '"mute"%s*:%s*true'
        await yamaha(setMute(!muted));
    };
    await yamaha(setMute(false));
    await toggle();
    assert.equal(srv.yamaha.mute, true);
    await toggle();
    assert.equal(srv.yamaha.mute, false);
});

test("Yamaha: keep-alive, requests reuse one connection", async () => {
    const a = await yamaha(getStatus());
    const b = await yamaha(setMute(false));
    assert.equal(a.localPort, b.localPort);
});

// ---- fake HOT PC and slideshow (one HTTP listener, plus the slideshow's UDP) ----

async function postLogged(path, body, headers = { "Content-Type": "application/json" })
{
    const seen = mark();
    const r = await request(srv.ports.pc, "POST", path, { body, headers });
    await waitFor(() => seen().length > 0);
    return { ...r, line: seen()[0] };
}

test("HOT PC: each command is logged and answered 200", async () => {
    for (const cmd of HOTPC_COMMANDS)
    {
        const r = await postLogged(`/${cmd}`, "{}");
        assert.equal(r.status, 200, cmd);
        assert.match(r.line, new RegExp(`^${TS} HOT PC: ${cmd}$`));
    }
});

test("Slideshow: display-elements toggles are logged and answered 200", async () => {
    let r = await postLogged("/display-elements", displayElementsBody("date"));
    assert.equal(r.status, 200);
    assert.match(r.line, new RegExp(`^${TS} Slideshow: toggle date$`));
    r = await postLogged("/display-elements", displayElementsBody("all"));
    assert.match(r.line, /Slideshow: toggle all$/);
});

test("PC HTTP: bad JSON, unknown element and unknown path are logged and refused", async () => {
    let r = await postLogged("/display-elements", "{nope");
    assert.equal(r.status, 400);
    assert.match(r.line, /Slideshow: display-elements bad body b'\{nope'$/);
    r = await postLogged("/display-elements", '{"weather":"toggle"}');
    assert.equal(r.status, 400);
    assert.match(r.line, /Slideshow: display-elements bad body b'\{"weather":"toggle"\}'$/);
    r = await postLogged("/display-elements", '{"date":"on"}');
    assert.equal(r.status, 400);
    r = await postLogged("/reboot", "{}");
    assert.equal(r.status, 404);
    assert.match(r.line, /PC: unknown POST \/reboot -> 404$/);
    r = await postLogged("/launch-steam/x", "{}");
    assert.equal(r.status, 404);
});

test("Slideshow: UDP commands are logged, unknown ones too", async () => {
    for (const cmd of SLIDESHOW_COMMANDS)
        assert.match(await udpLine(srv.ports.slideshow, Buffer.from(cmd)), new RegExp(`^${TS} Slideshow: ${cmd}$`));
    assert.match(await udpLine(srv.ports.slideshow, Buffer.from("rewind")),
                 /^\S+ Slideshow: unknown command b'rewind'$/);
});

test("close() frees the ports", async () => {
    const s = await startUremServer({ ports: PORTS, host: "127.0.0.1", log: () => {} });
    const ports = s.ports;
    await s.close();
    const again = await startUremServer({ ports, host: "127.0.0.1", log: () => {} });
    assert.deepEqual(again.ports, ports);
    await again.close();
});

test("a port that's taken fails the start", async () => {
    await assert.rejects(startUremServer({ ports: { ...PORTS, irtx: srv.ports.irtx }, host: "127.0.0.1",
                                           log: () => {} }), /EADDRINUSE/);
});
