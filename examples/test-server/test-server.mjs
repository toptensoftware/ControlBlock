// Test server for the hardware test layouts in examples/ (net-test.json, activity-test.mjs, script-test.mjs).
//
// Run on the PC:  node examples/test-server/test-server.mjs
// HTTP on :8000, UDP listeners on :9 (WoL) and :9999. Allow node.exe through the Windows firewall.
// Each line shows the client's source port, so a repeated port = a reused connection.
// Tests: node --test examples/test-server/test-server.test.mjs

import http from "node:http";
import dgram from "node:dgram";
import { pathToFileURL } from "node:url";

const INDENT = "            ";

// Local time with milliseconds, for the latency checks (script-test.mjs "Latency op+script")
export function timestamp(d = new Date())
{
    const p = (n, w = 2) => String(n).padStart(w, "0");
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}

// Bytes as a Python-style literal: b'text\xNN'
export function repr(bytes)
{
    const esc = { 0x09: "\\t", 0x0a: "\\n", 0x0d: "\\r", 0x27: "\\'", 0x5c: "\\\\" };
    let s = "";
    for (const b of bytes)
        s += esc[b] ?? (b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : "\\x" + b.toString(16).padStart(2, "0"));
    return `b'${s}'`;
}

// " WoL magic, ..." for a magic packet (six 0xFF, then the MAC 16 times), else ""
function wolNote(data)
{
    if (data.length !== 102 || !data.subarray(0, 6).every(b => b === 0xff))
        return "";
    const mac = data.subarray(6, 12);
    let ok = true;
    for (let i = 6; i < 102; i++)
        ok &&= data[i] === mac[i % 6];
    const hex = [...mac].map(b => b.toString(16).padStart(2, "0")).join(":");
    return `  WoL magic, mac=${hex}, repeats ok=${ok ? "True" : "False"}`;
}

// Starts the HTTP server and UDP listeners. Resolves { httpServer, httpPort, udpPorts, close() }.
// Port 0 picks a free port (the tests); the timings are shortened by the tests too.
export async function startTestServer({
    httpPort = 8000, udpPorts = [9, 9999], host = "0.0.0.0", log = console.log,
    hangMs = 60000, slowMs = 3000, bigBytes = 5 * 1024 * 1024, now = () => new Date(),
} = {})
{
    const ts = () => timestamp(now());
    let muteState = "0";                    // /mute: GET reads it, POST "mute=0|1" sets it

    function reply(res, code, body = "ok", extra = {})
    {
        const buf = Buffer.from(body);
        res.writeHead(code, { "Content-Length": buf.length, ...extra });
        res.end(buf);
    }

    function show(req)
    {
        log(`${ts()} [${req.socket.remotePort}] ${req.method} ${req.url}`);
        const raw = req.rawHeaders;         // keeps the client's case and order
        for (let i = 0; i < raw.length; i += 2)
            if (!["host", "accept"].includes(raw[i].toLowerCase()))
                log(`${INDENT}${raw[i]}: ${raw[i + 1]}`);
    }

    // Runs fn after ms unless the client goes away first
    function later(res, ms, fn)
    {
        const t = setTimeout(fn, ms);
        res.on("close", () => clearTimeout(t));
    }

    function sendBig(res)
    {
        res.writeHead(200, { "Content-Length": bigBytes });
        const chunk = Buffer.alloc(65536, "x");
        let left = bigBytes;
        let done = false;
        res.on("finish", () => { done = true; log(`${ts()}${INDENT}/big fully sent`); });
        res.on("close", () => { if (!done) log(`${ts()}${INDENT}/big client went away`); });
        const pump = () => {
            while (left > 0 && !res.destroyed)
            {
                const n = Math.min(left, chunk.length);
                left -= n;
                if (left === 0)
                    res.end(chunk.subarray(0, n));
                else if (!res.write(chunk.subarray(0, n)))
                    return res.once("drain", pump);
            }
        };
        pump();
    }

    function onGet(req, res)
    {
        show(req);
        const path = req.url;
        if (path.startsWith("/redirect"))
            reply(res, 302, "", { Location: "/get?redirected=1" });
        else if (path.startsWith("/hang"))
            later(res, hangMs, () => req.socket.destroy());     // accepts, never answers
        else if (path.startsWith("/slow"))
            later(res, slowMs, () => reply(res, 200));
        else if (path.startsWith("/big"))
            sendBig(res);
        else if (path.startsWith("/get"))
            reply(res, 200);
        else if (path.startsWith("/mute"))
            reply(res, 200, `mute=${muteState}`);
        else
            reply(res, 404, "not found");
    }

    function onPost(req, res)
    {
        const chunks = [];
        req.on("data", c => chunks.push(c));
        req.on("end", () => {
            const body = Buffer.concat(chunks);
            show(req);
            log(`${INDENT}body (${body.length} bytes): ${repr(body)}`);
            if (req.url.startsWith("/mute") && body.subarray(0, 5).toString("latin1") === "mute=")
                muteState = body.subarray(5, 6).toString("latin1");
            reply(res, 200);
        });
    }

    const httpServer = http.createServer((req, res) => (req.method === "POST" ? onPost : onGet)(req, res));
    httpServer.keepAliveTimeout = 0;        // keep idle connections open, like the device expects
    await new Promise((resolve, reject) => {
        httpServer.once("error", reject);
        httpServer.listen(httpPort, host, resolve);
    });

    const sockets = await Promise.all(udpPorts.map(port => new Promise((resolve, reject) => {
        const s = dgram.createSocket("udp4");
        s.once("error", reject);
        s.on("message", (data, from) => {
            const port = s.address().port;
            log(`${ts()} UDP :${port} from ${from.address}:${from.port} ${data.length} bytes `
                + `${repr(data.subarray(0, 32))}${wolNote(data)}`);
        });
        s.bind(port, host, () => { s.setBroadcast(true); resolve(s); });
    })));

    return {
        httpServer,
        httpPort: httpServer.address().port,
        udpPorts: sockets.map(s => s.address().port),
        close()
        {
            httpServer.closeAllConnections();
            httpServer.close();
            for (const s of sockets)
                s.close();
        },
    };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href)
{
    await startTestServer();
    console.log("HTTP :8000, UDP :9 and :9999 - Ctrl+C to stop");
}
