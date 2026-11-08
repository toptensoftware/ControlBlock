// Test server for the urem2 layout: stands in for the devices it controls and logs what each one received, in
// device terms ("IR lg.powerOn", "Yamaha: setInput hdmi2", "HOT PC: wake (...)").
//
// Run on the PC whose IP is SERVER in examples/test-common.mjs:  node urem2/test-server/test-server.mjs
// Ports come from targets.test (urem2/targets.mjs), so the layout's test entry and the server can't disagree.
// Allow node.exe through the Windows firewall. It can't run alongside examples/test-server (both bind UDP :9).
// Tests (from the repo root): node --test "urem2/**/*.test.mjs"

import http from "node:http";
import dgram from "node:dgram";
import { pathToFileURL } from "node:url";
import { timestamp, repr, wolMac } from "../../examples/test-server/test-server.mjs";
import { targets } from "../targets.mjs";
import { decodeIrtxPacket } from "../tables/irtx.mjs";
import { irNames } from "../tables/ir.mjs";
import { HOTPC_MAC, HOTPC_COMMANDS, SLIDESHOW_COMMANDS, SLIDESHOW_ELEMENTS } from "../tables/hotpc.mjs";
import { ATV_APPS, parseAtvCommand } from "../tables/appletv.mjs";
import { YAMAHA_API, YAMAHA_INPUTS, YAMAHA_PROGRAMS } from "../tables/yamaha.mjs";

const WOL_PORT = 9;

// Default ports, from the test target
export const DEFAULT_PORTS = {
    irtx: targets.test.irtx.port,                   // UDP, irtx IR blaster
    wol: WOL_PORT,                                  // UDP, Wake-on-LAN
    atvd: targets.test.atvd.port,                   // UDP, Apple TV daemon (text commands, replies)
    yamaha: Number(new URL(targets.test.yamaha).port),     // HTTP, Yamaha receiver
    pc: targets.test.hotpc.port,                    // HTTP, HOT PC and slideshow routes (= slideshow.httpPort)
    slideshow: targets.test.slideshow.udpPort,      // UDP, slideshow commands
};

// The fake Apple TV daemon: replies like atvdaemon.py (to the sender's address and port). connected is set
// atvConnectMs after a ping (or a key/app/text command, which wait for it, like the daemon's connect retry) and
// cleared by disconnect.
function fakeAtv({ say, atvConnectMs })
{
    let connected = false;
    let connecting = null;                  // { promise, timer } while a connect is in flight

    function connect()
    {
        if (!connecting)
        {
            let timer;
            const promise = new Promise(resolve => {
                timer = setTimeout(() => {
                    connected = true;
                    connecting = null;
                    say("ATV: (connected)");
                    resolve();
                }, atvConnectMs);
            });
            connecting = { promise, timer };
        }
        return connecting.promise;
    }

    async function run(c)
    {
        if (c.error)
            return `error: ${c.error}`;
        if (c.cmd === "ping")
        {
            if (connected)
                return "pong";
            connect();
            return "connecting";
        }
        if (c.cmd === "disconnect")
        {
            connected = false;
            return "ok";
        }
        if (!connected)
            await connect();
        return "ok";
    }

    function describe(text, c)
    {
        if (c.cmd === "key")
            return `key ${c.key} ${c.action}`;
        if (c.cmd === "app")
        {
            const name = Object.keys(ATV_APPS).find(n => ATV_APPS[n] === c.arg);
            return name ? `app ${name} (${c.arg})` : `app ${c.arg}`;
        }
        if (c.cmd)
            return c.arg === undefined ? c.cmd : `${c.cmd} ${c.arg}`;
        return text.trim();
    }

    async function onMessage(data, from, socket)
    {
        const text = data.toString("utf8");
        const c = parseAtvCommand(text);
        const reply = await run(c);
        say(`ATV: ${describe(text, c)} -> ${reply}`);
        try
        {
            socket.send(reply, from.port, from.address, () => {});
        }
        catch
        {
            // closed while the reply waited for the connect
        }
    }

    return {
        onMessage,
        get connected() { return connected; },
        stop() { if (connecting) clearTimeout(connecting.timer); },
    };
}

// Binds a UDP socket; onMessage(data, from, socket)
function udpListener(port, host, onMessage)
{
    return new Promise((resolve, reject) => {
        const s = dgram.createSocket("udp4");
        s.once("error", reject);
        s.on("message", (data, from) => onMessage(data, from, s));
        s.bind(port, host, () => {
            s.off("error", reject);
            s.setBroadcast(true);
            resolve({ port: s.address().port, close: () => new Promise(r => s.close(r)) });
        });
    });
}

// Starts an HTTP server; handler(req, res, body) once the body is in
function httpListener(port, host, handler)
{
    const server = http.createServer((req, res) => {
        const chunks = [];
        req.on("data", c => chunks.push(c));
        req.on("end", () => handler(req, res, Buffer.concat(chunks)));
    });
    server.keepAliveTimeout = 0;            // keep idle connections open, like the device expects
    return new Promise((resolve, reject) => {
        server.once("error", reject);
        server.listen(port, host, () => {
            server.off("error", reject);
            resolve({
                port: server.address().port,
                close: () => new Promise(r => { server.closeAllConnections(); server.close(r); }),
            });
        });
    });
}

function reply(res, code, body, type = "text/plain")
{
    const buf = Buffer.from(typeof body === "string" ? body : JSON.stringify(body));
    res.writeHead(code, { "Content-Type": type, "Content-Length": buf.length });
    res.end(buf);
}

// The fake Yamaha receiver (YamahaExtendedControl, the main zone calls urem uses), with no sockets: the HTTP listener
// wraps it, and the layout's Lua tests call it directly. state is live (the tests read it).
// handle(method, url) -> { status, type, body, log }; url is a path or a full URL. Setters reply
// {"response_code":0}, a bad parameter {"response_code":4}, an unknown path 404.
export function createFakeYamaha()
{
    const state = { power: "standby", input: "hdmi1", volume: 60, max_volume: 161, mute: false,
                    sound_program: "surr_decoder" };
    const oneOf = (list, v) => list.includes(v) ? v : null;

    // name -> fn(query) that applies it and returns the log text, or null for a bad parameter
    const setters = {
        setPower(q)
        {
            const p = oneOf(["on", "standby"], q.get("power"));
            return p && (state.power = p);
        },
        setInput(q)
        {
            const i = oneOf(YAMAHA_INPUTS, q.get("input"));
            return i && (state.input = i);
        },
        setMute(q)
        {
            const e = oneOf(["true", "false"], q.get("enable"));
            if (e)
                state.mute = e === "true";
            return e;
        },
        setSoundProgram(q)
        {
            const p = oneOf(YAMAHA_PROGRAMS, q.get("program"));
            return p && (state.sound_program = p);
        },
        setVolume(q)
        {
            const dir = oneOf(["up", "down"], q.get("volume"));
            const step = /^\d+$/.test(q.get("step") ?? "1") ? Number(q.get("step") ?? "1") : 0;
            if (!dir || step < 1)
                return null;
            state.volume = Math.max(0, Math.min(state.max_volume, state.volume + (dir === "up" ? step : -step)));
            return `${dir} ${step} -> ${state.volume}`;
        },
    };

    function status()
    {
        return {
            response_code: 0, power: state.power, sleep: 0, volume: state.volume, max_volume: state.max_volume,
            mute: state.mute, input: state.input, distribution_enable: true, sound_program: state.sound_program,
            surr_decoder_type: "auto", direct: false, enhancer: true,
            tone_control: { mode: "manual", bass: 0, treble: 0 }, dialogue_level: 0, subwoofer_volume: 0,
            actual_volume: { mode: "db", value: (state.volume - state.max_volume) / 2, unit: "dB" },
        };
    }

    const JSON_TYPE = "application/json";
    const json = (obj, log) => ({ status: 200, type: JSON_TYPE, body: JSON.stringify(obj), log });

    function handle(method, rawUrl)
    {
        const url = new URL(rawUrl, "http://yamaha");
        const prefix = YAMAHA_API + "/main/";
        const name = url.pathname.startsWith(prefix) ? url.pathname.slice(prefix.length) : null;
        if (name === "getStatus")
            return json(status(), `Yamaha: getStatus (mute=${state.mute}, sound_program=${state.sound_program})`);
        if (!Object.hasOwn(setters, name ?? ""))
            return { status: 404, type: "text/plain", body: "not found",
                     log: `Yamaha: unknown ${method} ${url.pathname}${url.search} -> 404` };
        const done = setters[name](url.searchParams);
        if (done === null)
            return json({ response_code: 4 },
                        `Yamaha: ${[name, ...url.searchParams.values()].join(" ")} -> invalid parameter`);
        return json({ response_code: 0 }, `Yamaha: ${name} ${done}`);
    }

    return { state, handle };
}

// The fake Yamaha on an HTTP listener
function fakeYamahaHttp({ say })
{
    const yamaha = createFakeYamaha();

    function onRequest(req, res)
    {
        const r = yamaha.handle(req.method, req.url);
        say(r.log);
        reply(res, r.status, r.body, r.type);
    }

    return { state: yamaha.state, onRequest };
}

// The HOT PC's and the slideshow's HTTP routes, on one listener (same port in the test target; paths don't overlap):
// POST /<HOTPC_COMMANDS> and POST /display-elements {"<element>":"toggle", ...}
function fakePcHttp({ say })
{
    function toggles(body)
    {
        let obj;
        try { obj = JSON.parse(body.toString("utf8")); } catch { return null; }
        if (!obj || typeof obj !== "object" || Array.isArray(obj))
            return null;
        const els = Object.keys(obj);
        const ok = els.length && els.every(el => SLIDESHOW_ELEMENTS.includes(el) && obj[el] === "toggle");
        return ok ? els : null;
    }

    function onRequest(req, res, body)
    {
        const path = new URL(req.url, "http://pc").pathname;
        const name = path.slice(1);
        if (req.method === "POST" && HOTPC_COMMANDS.includes(name))
        {
            say(`HOT PC: ${name}`);
            return reply(res, 200, "ok");
        }
        if (req.method === "POST" && name === "display-elements")
        {
            const els = toggles(body);
            if (!els)
            {
                say(`Slideshow: display-elements bad body ${repr(body.subarray(0, 64))}`);
                return reply(res, 400, "bad body");
            }
            say(`Slideshow: toggle ${els.join(", ")}`);
            return reply(res, 200, "ok");
        }
        say(`PC: unknown ${req.method} ${req.url} -> 404`);
        reply(res, 404, "not found");
    }

    return { onRequest };
}

// The slideshow's UDP text commands
function onSlideshowUdp(say, data)
{
    const cmd = data.toString("utf8").trim();
    say(SLIDESHOW_COMMANDS.includes(cmd) ? `Slideshow: ${cmd}` : `Slideshow: unknown command ${repr(data.subarray(0, 32))}`);
}

// Starts every listener. Resolves { ports, close() }; port 0 picks a free port (the tests).
export async function startUremServer({
    ports = DEFAULT_PORTS, host = "0.0.0.0", log = console.log, now = () => new Date(), atvConnectMs = 1500,
} = {})
{
    ports = { ...DEFAULT_PORTS, ...ports };
    const say = text => log(`${timestamp(now())} ${text}`);
    const atv = fakeAtv({ say, atvConnectMs });
    const yamaha = fakeYamahaHttp({ say });
    const pc = fakePcHttp({ say });

    function onIrtx(data)
    {
        const p = decodeIrtxPacket(data);
        if (p.error)
            return say(`irtx: bad packet (${p.error}) ${repr(data.subarray(0, 32))}`);
        const names = irNames(p.protocol, p.code);
        const what = names.length ? names.join("/") : `unknown ${p.protocol}:0x${p.code.toString(16)}`;
        say(`IR ${what}${p.repeat ? " (repeat)" : ""}`);
    }

    function onWol(data)
    {
        const w = wolMac(data);
        if (!w)
            return say(`WoL: not a magic packet (${data.length} bytes) ${repr(data.subarray(0, 32))}`);
        const bad = w.ok ? "" : ", bad repeats";
        say(w.mac === HOTPC_MAC ? `HOT PC: wake (${w.mac})${bad}` : `WoL: wake ${w.mac}${bad}`);
    }

    const listeners = {
        irtx: () => udpListener(ports.irtx, host, onIrtx),
        wol: () => udpListener(ports.wol, host, onWol),
        atvd: () => udpListener(ports.atvd, host, atv.onMessage),
        yamaha: () => httpListener(ports.yamaha, host, yamaha.onRequest),
        pc: () => httpListener(ports.pc, host, pc.onRequest),
        slideshow: () => udpListener(ports.slideshow, host, data => onSlideshowUdp(say, data)),
    };

    // Bind one at a time; if one fails, close the ones already open
    const open = [];
    const bound = {};
    const closeAll = () => { atv.stop(); return Promise.all(open.map(l => l.close())); };
    try
    {
        for (const [name, start] of Object.entries(listeners))
        {
            const l = await start();
            open.push(l);
            bound[name] = l.port;
        }
    }
    catch (e)
    {
        await closeAll();
        throw e;
    }

    return { ports: bound, atv, yamaha: yamaha.state, close: closeAll };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href)
{
    const { ports } = await startUremServer();
    console.log(Object.entries(ports).map(([k, v]) => `${k} :${v}`).join(", ") + " - Ctrl+C to stop");
}
