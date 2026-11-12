// The command line end to end: spawned as a process (real renderer, real files), and
// in process through main() with a fake device for the commands that need one.
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { execFileSync, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { unpack } from "../../binpack/index.js";
import { ROOT_TYPE, crc32, WifiState, ClockSource } from "../../host/index.js";
import { FakeDevice, fakeOpener } from "../../host/test/FakeDevice.js";
import { exampleDir } from "../../host/test/bake/fixtures.js";
import { main } from "../cli.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const cliPath = path.join(here, "../cli.js");

// main() with captured output
async function run(argv, io = {})
{
    const out = [], err = [];
    const code = await main(argv, { stdout: l => out.push(l), stderr: l => err.push(l), ...io });
    return { code, out, err };
}

const fast = { timeoutMs: 3000, intervalMs: 10 };

test("cli: bakes a layout file with real images to a .bin", { timeout: 120000 }, () => {
    const { dir, file, cleanup } = exampleDir();
    try {
        const out = path.join(dir, "out.bin");
        execFileSync(process.execPath, [cliPath, "bake", file, "-o", out], { cwd: dir, encoding: "utf8" });
        const blob = fs.readFileSync(out);
        assert.equal(blob.readUInt32LE(0), 0x4B415042);
        assert.equal(blob.length % 4, 0);
        const root = unpack(ROOT_TYPE, blob, 32);
        // real rle565 data: starts with the "R565" signature
        const img = Buffer.from(root.home.keys[0].normal);
        assert.equal(img.readUInt32LE(0), 0x35363552);
    } finally {
        cleanup();
    }
});

test("cli: a bad layout prints every error with its path and exits 1", () => {
    const { dir, file, cleanup } = exampleDir({ layout: { version: 1, home: { keys: [{ iconScale: "x" }] }, oops: 1 } });
    try {
        const r = spawnSync(process.execPath, [cliPath, "bake", file], { cwd: dir, encoding: "utf8" });
        assert.equal(r.status, 1);
        assert.match(r.stderr, /home\.keys\[0\]\.iconScale/);
        assert.match(r.stderr, /oops: unknown property/);
        assert.equal(fs.existsSync(path.join(dir, "layout.bin")), false);
    } finally {
        cleanup();
    }
});

test("cli: usage on no args, exit 2", () => {
    const r = spawnSync(process.execPath, [cliPath], { encoding: "utf8" });
    assert.equal(r.status, 2);
    assert.match(r.stderr, /usage/i);
});

test("cli: bake writes <layout name>.bin next to the layout by default", { timeout: 120000 }, async () => {
    const { dir, file, cleanup } = exampleDir({ name: "living-room.json" });
    try {
        const r = await run(["bake", file]);
        assert.equal(r.code, 0, r.err.join("\n"));
        const out = path.join(dir, "living-room.bin");
        assert.deepEqual(r.out, [`${out}: ${fs.statSync(out).size} bytes`]);
    } finally {
        cleanup();
    }
});

test("cli: upload sends a baked .bin as it is and verifies it", async () => {
    const { dir, cleanup } = exampleDir();
    try {
        const blob = new Uint8Array(3000).map((_, i) => (i * 2654435761) >>> 24);
        const file = path.join(dir, "baked.bin");
        fs.writeFileSync(file, blob);
        const dev = new FakeDevice({ maxPayload: 1000, layout: { restartMs: 30 } });
        const opened = [];
        const r = await run(["upload", file, "--serial", "SERIAL1"], { open: fakeOpener(dev, { opened }), reconnect: fast });
        assert.equal(r.code, 0, r.err.join("\n"));
        assert.deepEqual([...dev.layout.stored.bytes], [...blob]);
        assert.equal(dev.layout.stored.crc, crc32(blob));
        assert.equal(opened[0].serial, "SERIAL1");
        assert.match(r.out.at(-1), /verified/);
    } finally {
        cleanup();
    }
});

test("cli: upload bakes a layout in memory and sends it; a failure exits 1", { timeout: 120000 }, async () => {
    const { dir, file, cleanup } = exampleDir();
    try {
        const dev = new FakeDevice({ layout: { restartMs: 30 } });
        const r = await run(["upload", file], { open: fakeOpener(dev), reconnect: fast });
        assert.equal(r.code, 0, r.err.join("\n"));
        assert.equal(r.out[0], `${file}: ${dev.layout.stored.size} bytes`);
        assert.deepEqual(fs.readdirSync(dir).filter(f => f.endsWith(".bin")), []);     // nothing written

        dev.layout.capacity = 100;
        const bad = await run(["upload", file, "--force"], { open: fakeOpener(dev), reconnect: fast });
        assert.equal(bad.code, 1);
        assert.match(bad.err.at(-1), /^error: .*at most 100/);
    } finally {
        cleanup();
    }
});

// --- wifi (docs/wifi.md) ---

const wifiAt = (state, reason = 0) => ({ state, reason, rssi: -50, ip: [10, 0, 0, 2], mac: [1, 2, 3, 4, 5, 0xab] });

test("cli: wifi set stores the network, waits for the connection and exits 0", async () => {
    const dev = new FakeDevice({ wifi: { status: wifiAt(WifiState.CONNECTED) } });
    const opened = [];
    const r = await run(["wifi", "set", "Home", "password1", "--serial", "S2"], { open: fakeOpener(dev, { opened }) });
    assert.equal(r.code, 0, r.err.join("\n"));
    assert.deepEqual([dev.wifi.ssid, dev.wifi.pass], ["Home", "password1"]);
    assert.equal(opened[0].serial, "S2");
    assert.deepEqual(r.out, [`stored "Home"`, "CONNECTED, 10.0.0.2, -50 dBm, MAC 01:02:03:04:05:ab"]);
});

test("cli: wifi status, get and clear print what the device says", async () => {
    const dev = new FakeDevice({ wifi: { ssid: "Home", pass: "pw", status: wifiAt(WifiState.RETRYING, 15) } });
    const open = fakeOpener(dev);
    assert.deepEqual((await run(["wifi", "status"], { open })).out, ["RETRYING, last disconnect reason 15, MAC 01:02:03:04:05:ab"]);
    assert.deepEqual((await run(["wifi", "get"], { open })).out, ["ssid: Home\npassword: pw"]);
    assert.deepEqual((await run(["wifi", "clear"], { open })).out, ["cleared"]);
    assert.equal(dev.wifi.ssid, "");
    assert.deepEqual((await run(["wifi", "get"], { open })).out, ["no network stored"]);
    dev.wifi.status = wifiAt(WifiState.NO_CREDENTIALS);
    assert.deepEqual((await run(["wifi", "status"], { open })).out, ["NO_CREDENTIALS"]);
});

test("cli: wifi wait gives up after the time and exits 1; no device exits 1", async () => {
    const dev = new FakeDevice({ wifi: { ssid: "Home", status: wifiAt(WifiState.CONNECTING) } });
    const r = await run(["wifi", "wait", "0.05"], { open: fakeOpener(dev) });
    assert.equal(r.code, 1);
    assert.deepEqual(r.out, ["CONNECTING, MAC 01:02:03:04:05:ab"]);

    const none = await run(["wifi", "status"], { open: async () => { throw new Error("No ControlBlock found"); } });
    assert.equal(none.code, 1);
    assert.deepEqual(none.err, ["error: No ControlBlock found"]);
});

// --- time (docs/protocol.md, Clock) ---

test("cli: time sets the device clock from this machine's, standalone, and exits 0", async () => {
    const dev = new FakeDevice({ clock: {} });
    const opened = [];
    const before = Date.now();
    const r = await run(["time", "--serial", "S3"], { open: fakeOpener(dev, { opened }) });
    assert.equal(r.code, 0, r.err.join("\n"));
    assert.equal(dev.hosted, false);                            // no takeover
    assert.ok(dev.clock.offset !== null && Math.abs(dev.clock.offset) <= Date.now() - before + 5);
    assert.equal(opened[0].serial, "S3");
    assert.equal(r.out.length, 1);
    assert.match(r.out[0], /^clock set to \d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$/);
    assert.equal((await run(["time", "set"], { open: fakeOpener(dev) })).code, 0);
});

test("cli: time get shows the clock, its source, the zone and the server", async () => {
    const dev = new FakeDevice({ clock: {} });
    const open = fakeOpener(dev);
    assert.deepEqual((await run(["time", "get"], { open })).out, ["time: not set", "time zone: UTC0", "ntp server: pool.ntp.org"]);
    Object.assign(dev.clock, { offset: Date.UTC(2026, 10, 8, 7, 59) - Date.now(), source: ClockSource.NTP, tz: "EST5EDT", server: "10.0.0.1" });
    const r = await run(["time", "get"], { open });
    assert.match(r.out[0], /^time: 2026-11-08T07:59:\d\d\.\d{3}Z \(set by NTP\)$/);
    assert.deepEqual(r.out.slice(1), ["time zone: EST5EDT", "ntp server: 10.0.0.1"]);
    dev.clock.source = ClockSource.HOST;
    assert.match((await run(["time", "get"], { open })).out[0], /\(set by the host\)$/);
    dev.clock.source = ClockSource.NONE;                        // known, but from before a restart
    assert.match((await run(["time", "get"], { open })).out[0], /^time: \S+Z$/);
});

test("cli: time zone and server store a setting, or put back the default", async () => {
    const dev = new FakeDevice({ clock: {} });
    const open = fakeOpener(dev);
    assert.deepEqual((await run(["time", "zone", "AEST-10AEDT,M10.1.0,M4.1.0/3"], { open })).out, ["time zone: AEST-10AEDT,M10.1.0,M4.1.0/3"]);
    assert.deepEqual((await run(["time", "server", "time.example.com"], { open })).out, ["ntp server: time.example.com"]);
    assert.deepEqual([dev.clock.tz, dev.clock.server], ["AEST-10AEDT,M10.1.0,M4.1.0/3", "time.example.com"]);
    assert.deepEqual((await run(["time", "zone", "--default"], { open })).out, ["time zone: UTC0"]);
    assert.deepEqual((await run(["time", "server", "--default"], { open })).out, ["ntp server: pool.ntp.org"]);
});

test("cli: a bad time zone exits 1 without sending it", async () => {
    const dev = new FakeDevice({ clock: {} });
    const r = await run(["time", "zone", "UTC 0"], { open: fakeOpener(dev) });
    assert.equal(r.code, 1);
    assert.match(r.err[0], /^error: timeZone "UTC 0" must be printable ASCII without spaces/);
    assert.equal(dev.clock.tz, "UTC0");
});

// --- name (docs/protocol.md, Device name) ---

test("cli: name shows the device name, set stores one or puts back the default", async () => {
    const dev = new FakeDevice({ name: {} });
    const open = fakeOpener(dev);
    assert.deepEqual((await run(["name"], { open })).out, ["name: controlblock-a1b2c3 (default)"]);
    const r = await run(["name", "set", "Den-TV"], { open });
    assert.equal(r.code, 0, r.err.join("\n"));
    assert.deepEqual(r.out, ["name: den-tv"]);
    assert.equal(dev.hosted, false);                            // no takeover
    assert.deepEqual((await run(["name", "get"], { open })).out, ["name: den-tv"]);
    assert.deepEqual((await run(["name", "set", "--default"], { open })).out, ["name: controlblock-a1b2c3 (default)"]);
});

test("cli: a bad name exits 1 without sending it", async () => {
    const dev = new FakeDevice({ name: {} });
    const r = await run(["name", "set", "a.b"], { open: fakeOpener(dev) });
    assert.equal(r.code, 1);
    assert.match(r.err[0], /^error: The name "a.b" must be/);
    assert.equal(dev.name.name, null);
});

test("cli: render writes one PNG per page and prints each file", { timeout: 120000 }, async () => {
    const { dir, file, cleanup } = exampleDir();
    try {
        const out = path.join(dir, "pages");
        const r = await run(["render", file, "-o", out]);
        assert.equal(r.code, 0, r.err.join("\n"));
        const names = ["00-home.png", "01-watch-tv-1.png", "02-listen-1.png"];
        assert.deepEqual(r.out, names.map(n => path.join(out, n)));
        assert.deepEqual(fs.readdirSync(out).sort(), names);
        const png = fs.readFileSync(path.join(out, names[0]));
        assert.equal(png.readUInt32BE(0), 0x89504E47);                          // the PNG signature
        assert.deepEqual([png.readUInt32BE(16), png.readUInt32BE(20)], [800, 480]);   // IHDR width, height

        // one activity, pressed: numbered as in the whole layout
        const one = await run(["render", file, "-o", out, "--page", "Listen", "--pressed", "--no-grid"]);
        assert.equal(one.code, 0, one.err.join("\n"));
        assert.deepEqual(one.out, [path.join(out, "02-listen-1.png")]);
    } finally {
        cleanup();
    }
});

test("cli: render refuses an unknown page and a bad layout with exit 1", { timeout: 120000 }, async () => {
    const { dir, file, cleanup } = exampleDir();
    try {
        const out = path.join(dir, "pages");
        const r = await run(["render", file, "-o", out, "--page", "Nope"]);
        assert.equal(r.code, 1);
        assert.match(r.err[0], /^error: no page or activity "Nope"/);
        assert.equal(fs.existsSync(out), false);
    } finally {
        cleanup();
    }
    const bad = exampleDir({ layout: { version: 1, home: { keys: [{ iconScale: "x" }] } } });
    try {
        const r = await run(["render", bad.file, "-o", path.join(bad.dir, "pages")]);
        assert.equal(r.code, 1);
        assert.match(r.err[0], /home\.keys\[0\]\.iconScale/);
    } finally {
        bad.cleanup();
    }
});
