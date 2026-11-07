// Tests for test-server.mjs. From the repo root:  node --test examples/test-server/
// The server runs on free ports with short delays and a captured log.

import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import dgram from "node:dgram";
import { startTestServer, repr, timestamp } from "./test-server.mjs";

const lines = [];
let srv;

before(async () => {
    srv = await startTestServer({
        httpPort: 0, udpPorts: [0, 0], host: "127.0.0.1",
        log: (...a) => lines.push(a.join(" ")),
        hangMs: 300, slowMs: 200, bigBytes: 200 * 1024 + 5,
    });
});

after(() => srv.close());

// One request; resolves { status, headers, body, localPort }
function request(method, path, { body, headers, agent } = {})
{
    return new Promise((resolve, reject) => {
        const req = http.request({ host: "127.0.0.1", port: srv.httpPort, method, path, headers, agent }, res => {
            const chunks = [];
            res.on("data", c => chunks.push(c));
            res.on("end", () => resolve({
                status: res.statusCode, headers: res.headers,
                body: Buffer.concat(chunks).toString("latin1"), localPort: req.socket.localPort,
            }));
        });
        req.on("error", reject);
        req.end(body);
    });
}

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

test("timestamp is local HH:MM:SS.mmm", () => {
    assert.equal(timestamp(new Date(2026, 10, 7, 9, 5, 3, 7)), "09:05:03.007");
});

test("repr quotes bytes like Python", () => {
    assert.equal(repr(Buffer.from("a=1&b=2")), "b'a=1&b=2'");
    assert.equal(repr(Buffer.from([0xde, 0xad, 0x00, 0x41, 0x0a, 0x5c, 0x27])), String.raw`b'\xde\xad\x00A\n\\\''`);
});

test("GET /get logs the request line and headers (case kept, Host and Accept skipped)", async () => {
    const seen = mark();
    const r = await request("GET", "/get?x=1", { headers: { "X-Test": "one", "Accept": "*/*", "X-Other": "two" } });
    assert.equal(r.status, 200);
    assert.equal(r.body, "ok");
    assert.equal(r.headers["content-length"], "2");
    const out = seen();
    assert.match(out[0], new RegExp(`^${TS} \\[${r.localPort}\\] GET /get\\?x=1$`));
    assert.ok(out.includes("            X-Test: one"));
    assert.ok(out.includes("            X-Other: two"));
    assert.ok(!out.some(l => /^\s+(host|accept):/i.test(l)));
});

test("GET /redirect is a 302 to /get?redirected=1", async () => {
    const r = await request("GET", "/redirect");
    assert.equal(r.status, 302);
    assert.equal(r.headers.location, "/get?redirected=1");
    assert.equal(r.body, "");
});

test("unknown path is a 404", async () => {
    const r = await request("GET", "/nope");
    assert.equal(r.status, 404);
    assert.equal(r.body, "not found");
});

test("GET /slow answers after the delay (prefix match keeps the query)", async () => {
    const seen = mark();
    const t = Date.now();
    const r = await request("GET", "/slow?n=3");
    assert.equal(r.status, 200);
    assert.ok(Date.now() - t >= 180);
    assert.match(seen()[0], / GET \/slow\?n=3$/);
});

test("GET /hang never answers", async () => {
    const t = Date.now();
    await assert.rejects(request("GET", "/hang"));
    assert.ok(Date.now() - t >= 280);
});

test("GET /big sends the whole body", async () => {
    const seen = mark();
    const r = await request("GET", "/big");
    assert.equal(r.status, 200);
    assert.equal(r.body.length, 200 * 1024 + 5);
    assert.equal(r.headers["content-length"], String(200 * 1024 + 5));
    assert.ok(/^x+$/.test(r.body));
    await waitFor(() => seen().some(l => new RegExp(`^${TS}            /big fully sent$`).test(l)));
});

test("GET /big logs a client that goes away", async () => {
    const big = await startTestServer({ httpPort: 0, udpPorts: [], host: "127.0.0.1", bigBytes: 64 * 1024 * 1024,
                                        log: (...a) => lines.push(a.join(" ")) });
    try
    {
        const seen = mark();
        await new Promise(resolve => {
            const req = http.get({ host: "127.0.0.1", port: big.httpPort, path: "/big" }, res => {
                res.once("data", () => { req.destroy(); resolve(); });
            });
            req.on("error", () => {});
        });
        await waitFor(() => seen().some(l => l.endsWith("/big client went away")));
        assert.ok(!seen().some(l => l.endsWith("/big fully sent")));
    }
    finally
    {
        big.close();
    }
});

test("POST logs the body and replies ok", async () => {
    const seen = mark();
    const r = await request("POST", "/post", { body: "a=1&b=2", headers: { "Content-Type": "application/x-www-form-urlencoded" } });
    assert.equal(r.status, 200);
    assert.equal(r.body, "ok");
    const out = seen();
    assert.match(out[0], / POST \/post$/);
    assert.ok(out.includes("            Content-Type: application/x-www-form-urlencoded"));
    assert.equal(out.at(-1), "            body (7 bytes): b'a=1&b=2'");
});

test("/mute: GET reads the state, POST mute=N sets it", async () => {
    assert.equal((await request("GET", "/mute")).body, "mute=0");
    await request("POST", "/mute", { body: "mute=1" });
    assert.equal((await request("GET", "/mute")).body, "mute=1");
    await request("POST", "/mute", { body: "other" });         // not mute=: ignored
    assert.equal((await request("GET", "/mute")).body, "mute=1");
    await request("POST", "/mute", { body: "mute=0" });
    assert.equal((await request("GET", "/mute")).body, "mute=0");
});

test("keep-alive: requests reuse one connection, and idle ones aren't timed out", async () => {
    assert.equal(srv.httpServer.keepAliveTimeout, 0);
    const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
    try
    {
        const a = await request("GET", "/get?a", { agent });
        const b = await request("POST", "/post", { agent, body: "x" });
        assert.equal(a.localPort, b.localPort);
    }
    finally
    {
        agent.destroy();
    }
});

function sendUdp(port, data)
{
    return new Promise((resolve, reject) => {
        const s = dgram.createSocket("udp4");
        s.send(data, port, "127.0.0.1", err => {
            const local = s.address().port;
            s.close();
            err ? reject(err) : resolve(local);
        });
    });
}

test("UDP logs each datagram on each port, cut to 32 bytes", async () => {
    for (const port of srv.udpPorts)
    {
        const seen = mark();
        const data = Buffer.concat([Buffer.from("hello "), Buffer.from([0xde, 0xad]), Buffer.alloc(40, 0x41)]);
        const from = await sendUdp(port, data);
        await waitFor(() => seen().length > 0);
        assert.match(seen()[0], new RegExp(`^${TS} UDP :${port} from 127\\.0\\.0\\.1:${from} 48 bytes `
                                           + String.raw`b'hello \\xde\\xadA{24}'$`));
    }
});

function wol(mac, repeats)
{
    return Buffer.concat([Buffer.alloc(6, 0xff), ...repeats.map(m => Buffer.from(m.split(":").map(h => parseInt(h, 16))))]);
}

test("UDP spots WoL magic and checks the repeats", async () => {
    const mac = "01:23:45:67:89:ab";
    const port = srv.udpPorts[0];

    let seen = mark();
    await sendUdp(port, wol(mac, Array(16).fill(mac)));
    await waitFor(() => seen().length > 0);
    assert.match(seen()[0], / 102 bytes .*  WoL magic, mac=01:23:45:67:89:ab, repeats ok=True$/);

    seen = mark();
    await sendUdp(port, wol(mac, [...Array(15).fill(mac), "01:23:45:67:89:ac"]));
    await waitFor(() => seen().length > 0);
    assert.match(seen()[0], /  WoL magic, mac=01:23:45:67:89:ab, repeats ok=False$/);

    seen = mark();
    await sendUdp(port, Buffer.alloc(102, 0x11));
    await waitFor(() => seen().length > 0);
    assert.doesNotMatch(seen()[0], /WoL/);
});
