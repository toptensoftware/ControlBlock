import test from "node:test";
import assert from "node:assert/strict";
import { ControlBlock, ControlBlockError } from "../src/ControlBlock.js";
import { Msg, Status, Reader, encodeIds, crc32, HEARTBEAT_INTERVAL_MS, WifiState, encodeWifiSet, encodeTimeSet, ClockSource } from "../src/protocol.js";
import { imageId, keyImage, encodeImage } from "../src/images.js";
import { readRleHeader } from "rle565/unpack.js";
import { FakeDevice } from "./FakeDevice.js";

// Most tests are of a hosted session; FakeDevice itself boots standalone, like the device
async function connected(options)
{
    const dev = new FakeDevice({ hosted: true, ...options });
    const cb = new ControlBlock(dev, { timeoutMs: 300 });
    await cb.connect();
    return { dev, cb };
}

test("connect resets the stream and reads the info", async () => {
    const { dev, cb } = await connected();
    assert.ok(dev.claimed);
    assert.equal(dev.resets, 1);
    assert.equal(cb.info.serial, "SERIAL1");
    await cb.close();
});

test("a read's timeout is well past the heartbeat, so an idle read never times out", async () => {
    // A cancelled (timed out) bulk IN read leaves the pipe deaf on Windows, so
    // reads must never time out while the device is alive: the device sends a
    // heartbeat every HEARTBEAT_INTERVAL_MS, and the timeout is a long way past it.
    // (A pending read can't be cancelled with usb 3.x, so close() and the process
    // exit wait for the next heartbeat: at most one interval.)
    const { dev, cb } = await connected();
    await new Promise(r => setTimeout(r, 120));         // let the idle read loop run
    assert.ok(dev.maxReadTimeout >= 3 * HEARTBEAT_INTERVAL_MS, `read timeout ${dev.maxReadTimeout} ms`);
    await cb.close();
});

test("heartbeat events are emitted", async () => {
    const { dev, cb } = await connected();
    const got = new Promise(resolve => cb.on("heartbeat", resolve));
    dev.event(Msg.HEARTBEAT, 5, Uint8Array.of(0xE8, 0x03, 0, 0));
    assert.deepEqual(await got, { type: "heartbeat", seq: 5, timeMs: 1000 });
    await cb.close();
});

test("pipelined requests resolve in order", async () => {
    const { dev, cb } = await connected();
    const pings = [];
    for (let i = 0; i < 20; i++)
        pings.push(cb.ping(Uint8Array.of(i)));
    const results = await Promise.all(pings);
    results.forEach((r, i) => assert.equal(r[0], i));
    await cb.close();
});

test("a message that ends on a whole USB packet is followed by a zero-length packet", async () => {
    // FakeDevice holds back a transfer that ends on a full packet until a short
    // packet or a ZLP ends it, like the device: without the ZLP these hang
    const { dev, cb } = await connected();
    const sizes = [];
    const send = dev.transferOut.bind(dev);
    dev.transferOut = async (ep, data) => { sizes.push(new Uint8Array(data).length); return send(ep, data); };

    for (const n of [52, 52 + 64, 52 + 512, 500])     // messages of 64, 128, 576 and 512 bytes
        assert.equal((await cb.ping(new Uint8Array(n))).length, n);
    assert.deepEqual(sizes, [64, 0, 128, 0, 576, 0, 512, 0]);

    sizes.length = 0;
    await cb.ping(new Uint8Array(53));                  // 65 bytes: a short packet ends it
    assert.deepEqual(sizes, [65]);
    await cb.close();
});

test("uploadLayout works when the last chunk ends on a whole USB packet", async () => {
    // The case found on the hardware: a 53464-byte blob, whose last LAYOUT_DATA was 4352 bytes
    const { dev, cb } = await connected({ layout: {} });
    const r = await cb.uploadLayout(blobOf(53464));
    assert.equal(r.uploaded, true);
    assert.equal(dev.layout.stored.size, 53464);
    await cb.close();
});

test("a failed request throws ControlBlockError", async () => {
    const { cb } = await connected({ handler: () => ({ status: Status.NOT_FOUND, data: new Uint8Array(0) }) });
    await assert.rejects(cb.setKeys([{ page: 0, key: 0, bg: 99 }]),
                         err => err instanceof ControlBlockError && err.status === Status.NOT_FOUND);
    await cb.close();
});

test("putImage splits into chunks the device can take", async () => {
    const { dev, cb } = await connected({ maxPayload: 100 });
    const data = new Uint8Array(250).map((_, i) => i);
    await cb.putImage(42, data);

    const chunks = dev.requests.filter(m => m.type === Msg.PUT_IMAGE);
    assert.equal(chunks.length, 3);                 // 88 + 88 + 74
    const joined = [];
    for (const c of chunks) {
        const r = new Reader(c.payload);
        assert.equal(r.u32(), 42);
        assert.equal(r.u32(), 250);
        assert.equal(r.u32(), joined.length);       // in order, back to back
        joined.push(...c.payload.subarray(12));
        assert.ok(c.payload.length <= 100);
    }
    assert.deepEqual(joined, [...data]);
    await cb.close();
});

test("syncImages uploads only what's missing", async () => {
    const have = new Set([1, 3]);
    const handler = msg => {
        if (msg.type === Msg.QUERY_IMAGES) {
            const r = new Reader(msg.payload);
            const n = r.u16();
            const bits = new Uint8Array(2 + Math.ceil(n / 8));
            bits[0] = n;
            for (let i = 0; i < n; i++)
                if (have.has(r.u32()))
                    bits[2 + (i >> 3)] |= 1 << (i & 7);
            return { status: Status.OK, data: bits };
        }
        return { status: Status.OK, data: new Uint8Array(0) };
    };
    const { dev, cb } = await connected({ handler });
    const n = await cb.syncImages([1, 2, 3, 4].map(id => ({ id, data: Uint8Array.of(id) })));
    assert.equal(n, 2);
    const ids = dev.requests.filter(m => m.type === Msg.PUT_IMAGE).map(m => new Reader(m.payload).u32());
    assert.deepEqual(ids.sort(), [2, 4]);
    await cb.close();
});

test("setStatusText sends only the fields given", async () => {
    const { dev, cb } = await connected();
    const sent = () => dev.requests.filter(m => m.type === Msg.SET_STATUS_TEXT).map(m => [...m.payload]);

    await cb.setStatusText({ left: "AppleTV" });
    assert.deepEqual(sent(), [[0, 7, ...new TextEncoder().encode("AppleTV")]]);

    await cb.setStatusText({ right: "" });                      // clears
    await cb.setStatusText({ left: "A", right: "B" });
    assert.deepEqual(sent().slice(1), [[1, 0], [0, 1, 65], [1, 1, 66]]);

    await cb.setStatusText({});
    assert.equal(sent().length, 4);

    // Nothing is sent if any field is bad
    await assert.rejects(cb.setStatusText({ left: "ok", right: "x".repeat(48) }), RangeError);
    await assert.rejects(cb.setStatusText({ middle: "x" }), TypeError);
    assert.equal(sent().length, 4);
    await cb.close();
});

test("a rejected status text throws ControlBlockError", async () => {
    const { cb } = await connected({ handler: () => ({ status: Status.BAD_ARG, data: new Uint8Array(0) }) });
    await assert.rejects(cb.setStatusText({ left: "x" }),
                         err => err instanceof ControlBlockError && err.status === Status.BAD_ARG);
    await cb.close();
});

test("events are emitted", async () => {
    const { dev, cb } = await connected();
    const got = new Promise(resolve => cb.on("pageChanged", resolve));
    dev.event(Msg.PAGE_CHANGED, 1, Uint8Array.of(2, 0));
    assert.deepEqual(await got, { type: "pageChanged", seq: 1, page: 2, cause: 0 });
    await cb.close();
});

test("wake events are emitted", async () => {
    const { dev, cb } = await connected();
    const got = new Promise(resolve => cb.on("wake", resolve));
    dev.event(Msg.WAKE, 3, Uint8Array.of(0, 0, 0, 0, 0xE8, 0x03, 0, 0));
    assert.deepEqual(await got, { type: "wake", seq: 3, lost: false, timeMs: 1000 });
    await cb.close();
});

test("setBacklight sends on or off and info follows", async () => {
    const { dev, cb } = await connected();
    assert.equal(cb.info.backlight, true);
    await cb.setBacklight(false);
    assert.equal(cb.info.backlight, false);
    await cb.setBacklight(true);
    assert.equal(cb.info.backlight, true);
    const sent = dev.requests.filter(m => m.type === Msg.SET_BACKLIGHT).map(m => [...m.payload]);
    assert.deepEqual(sent, [[0], [1]]);
    await cb.close();
});

test("a reconnecting host learns the backlight is off", async () => {
    const { dev, cb } = await connected({ backlight: false });
    assert.equal(cb.info.backlight, false);
    await cb.setBacklight(true);
    assert.equal((await cb.getInfo()).backlight, true);
    assert.equal(dev.backlight, true);
    await cb.close();
});

test("a failed setBacklight throws and leaves info alone", async () => {
    const { cb } = await connected({ handler: () => ({ status: Status.BAD_LENGTH, data: new Uint8Array(0) }) });
    await assert.rejects(cb.setBacklight(false), err => err instanceof ControlBlockError);
    assert.equal(cb.info.backlight, true);
    await cb.close();
});

test("a lost response times out and resets the stream", async () => {
    const { dev, cb } = await connected({ handler: () => null });
    await assert.rejects(cb.reset(), /timed out/);
    await new Promise(r => setTimeout(r, 20));
    assert.equal(dev.resets, 2);

    // A late response for the old request is ignored; new requests still work
    const old = dev.requests.find(m => m.type === Msg.RESET);
    dev.respond(old, Status.OK);
    assert.deepEqual([...await cb.ping(Uint8Array.of(5))], [5]);
    await cb.close();
});

test("key images are rle565 with a stable content ID", async () => {
    const icon = new Uint8Array(80 * 80 * 4);
    for (let i = 0; i < icon.length; i += 4)
        icon.set([255, 128, 0, (i / 4) % 80 < 40 ? 255 : 0], i);
    const a = await keyImage(icon, 80, 80, 128);
    const b = await keyImage(icon, 80, 80, 128);
    const h = readRleHeader(Buffer.from(a.data));
    assert.equal(h.width, 128);
    assert.equal(h.height, 128);
    assert.equal(a.id, b.id);
    assert.notEqual(a.id, 0);
    assert.equal(a.id, imageId(a.data));

    const other = await encodeImage(new Uint8Array(128 * 128 * 4).fill(255), 128, 128);
    assert.notEqual(imageId(other), a.id);
});

// --- Standalone layout upload (docs/protocol.md "Layout upload") ---

function blobOf(size, seed = 1)
{
    return new Uint8Array(size).map((_, i) => ((i + seed) * 2654435761) >>> 24);
}

test("uploadLayout sends begin, ordered chunks and commit, then the device restarts", async () => {
    const { dev, cb } = await connected({ maxPayload: 100, layout: {} });
    const blob = blobOf(250);
    const progress = [];
    const r = await cb.uploadLayout(blob, { onProgress: (sent, total) => progress.push([sent, total]) });
    assert.deepEqual(r, { uploaded: true, size: 250, crc: crc32(blob) });

    const reqs = dev.requests.filter(m => m.type >= Msg.LAYOUT_BEGIN && m.type <= Msg.LAYOUT_COMMIT);
    assert.equal(reqs[0].type, Msg.LAYOUT_BEGIN);
    const begin = new Reader(reqs[0].payload);
    assert.equal(begin.u32(), 250);
    assert.equal(begin.u32(), crc32(blob));
    const data = reqs.filter(m => m.type === Msg.LAYOUT_DATA);
    assert.equal(data.length, 3);                       // 96 + 96 + 58
    let pos = 0;
    for (const c of data) {
        assert.equal(new Reader(c.payload).u32(), pos); // in order, back to back
        assert.ok(c.payload.length <= 100);
        pos += c.payload.length - 4;
    }
    assert.equal(pos, 250);
    assert.equal(reqs.at(-1).type, Msg.LAYOUT_COMMIT);
    assert.deepEqual([...dev.layout.stored.bytes], [...blob]);
    assert.deepEqual(progress.at(-1), [250, 250]);
    assert.ok(progress.every(([sent], i) => i === 0 || sent > progress[i - 1][0]));
    await cb.close();
});

test("uploadLayout keeps only a few chunks in flight", async () => {
    // Each chunk takes a while to answer; the library must not queue them all
    const { dev, cb } = await connected({ maxPayload: 100, layout: { latencyMs: 10 } });
    await cb.uploadLayout(blobOf(96 * 40));             // 40 chunks
    assert.ok(dev.layout.maxInflight > 1, "pipelined");
    assert.ok(dev.layout.maxInflight <= 8, `window (saw ${dev.layout.maxInflight})`);
    assert.equal(dev.layout.stored.size, 96 * 40);
    await cb.close();
});

test("uploadLayout skips a layout the device already has", async () => {
    const blob = blobOf(300);
    const { dev, cb } = await connected({ layout: { stored: { size: 300, crc: crc32(blob) } } });
    const r = await cb.uploadLayout(blob);
    assert.deepEqual(r, { uploaded: false, size: 300, crc: crc32(blob) });
    assert.ok(!dev.requests.some(m => m.type === Msg.LAYOUT_BEGIN));

    // Unless forced
    const forced = await cb.uploadLayout(blob, { force: true });
    assert.equal(forced.uploaded, true);
    assert.ok(dev.requests.some(m => m.type === Msg.LAYOUT_BEGIN));
    await cb.close();
});

test("uploadLayout refuses what can't be right before sending anything", async () => {
    const { dev, cb } = await connected({ layout: { capacity: 1000 } });
    await assert.rejects(cb.uploadLayout(new Uint8Array(0)), /Empty/);
    await assert.rejects(cb.uploadLayout(blobOf(1001)), /1000/);
    assert.ok(!dev.requests.some(m => m.type === Msg.LAYOUT_BEGIN));
    await cb.close();
});

test("uploadLayout reports a begin the device refuses and sends no data", async () => {
    const { dev, cb } = await connected({ layout: { capacity: 1 << 20 } });
    dev.layout.capacity = 100;                          // less than the info said
    await assert.rejects(cb.uploadLayout(blobOf(500)),
                         err => err instanceof ControlBlockError && err.status === Status.NO_SPACE);
    assert.ok(!dev.requests.some(m => m.type === Msg.LAYOUT_DATA || m.type === Msg.LAYOUT_COMMIT));
    await cb.close();
});

test("uploadLayout stops at the first failed chunk and never commits", async () => {
    const { dev, cb } = await connected({ maxPayload: 100, layout: { failAt: 96 * 3, latencyMs: 5 } });
    await assert.rejects(cb.uploadLayout(blobOf(96 * 30)),
                         err => err instanceof ControlBlockError && err.status === Status.IO_ERROR);
    const sent = dev.requests.filter(m => m.type === Msg.LAYOUT_DATA).length;
    assert.ok(sent < 30, `stopped early (sent ${sent} of 30)`);
    assert.ok(!dev.requests.some(m => m.type === Msg.LAYOUT_COMMIT));
    assert.equal(dev.layout.stored, null);
    await cb.close();
});

test("uploadLayout lets the commit take longer than an ordinary request", async () => {
    const { dev, cb } = await connected({ layout: { commitMs: 450 } });     // the connection's timeout is 300 ms
    const r = await cb.uploadLayout(blobOf(100));
    assert.equal(r.uploaded, true);
    assert.equal(dev.layout.stored.size, 100);
    await cb.close();
});

test("a wrong CRC at the device is a BAD_CRC error", async () => {
    const { dev, cb } = await connected({ layout: {} });
    // Corrupt what arrives: flip a byte in each chunk on its way
    const send = dev.transferOut.bind(dev);
    dev.transferOut = async (ep, data) => {
        const bytes = new Uint8Array(data);
        if (bytes[2] === Msg.LAYOUT_DATA)
            bytes[bytes.length - 1] ^= 1;
        return send(ep, bytes);
    };
    await assert.rejects(cb.uploadLayout(blobOf(100)),
                         err => err instanceof ControlBlockError && err.status === Status.BAD_CRC);
    assert.equal(dev.layout.stored, null);
    await cb.close();
});

test("after a commit the connection closes quietly and reconnect() finds the restarted device", async () => {
    const blob = blobOf(500);
    const { dev, cb } = await connected({ layout: { restartMs: 120 } });
    const errors = [];
    cb.on("error", e => errors.push(e));
    await cb.uploadLayout(blob);

    // The device went away; the library reads that as a restart, not a failure
    const again = await cb.reconnect({
        timeoutMs: 3000, intervalMs: 20,
        opener: async () => { const c = new ControlBlock(dev, { timeoutMs: 300 }); await c.connect(); return c; },
    });
    assert.equal(cb.connected, false);
    assert.deepEqual(errors, []);
    assert.equal(again.info.layoutSize, 500);
    assert.equal(again.info.layoutCrc, crc32(blob));
    await again.close();
});

test("waitOpen retries until the device is back, then gives up", async () => {
    let tries = 0;
    const found = await ControlBlock.waitOpen({
        intervalMs: 5, timeoutMs: 1000,
        opener: async () => { if (++tries < 4) throw new Error("No ControlBlock found"); return "device"; },
    });
    assert.equal(found, "device");
    assert.equal(tries, 4);

    await assert.rejects(ControlBlock.waitOpen({
        intervalMs: 5, timeoutMs: 60,
        opener: async () => { throw new Error("No ControlBlock found"); },
    }), /did not come back.*No ControlBlock found/);
});

// --- Wi-Fi (docs/wifi.md) ---

test("setWifi sends the credentials and info follows", async () => {
    const { dev, cb } = await connected({ wifi: {} });
    assert.equal(cb.info.wifiCredentials, false);
    await cb.setWifi("Home", "password1");
    assert.equal(cb.info.wifiCredentials, true);
    const sent = dev.requests.filter(m => m.type === Msg.WIFI_SET);
    assert.equal(sent.length, 1);
    assert.deepEqual([...sent[0].payload], [...encodeWifiSet("Home", "password1")]);
    assert.equal((await cb.getInfo()).wifiCredentials, true);
    await cb.close();
});

test("getWifi reads the credentials back", async () => {
    const { cb } = await connected({ wifi: {} });
    assert.deepEqual(await cb.getWifi(), { ssid: "", password: "", ssidBytes: new Uint8Array(0) });
    await cb.setWifi("Home", "password1");
    const got = await cb.getWifi();
    assert.equal(got.ssid, "Home");
    assert.equal(got.password, "password1");
    await cb.setWifi("Cafe");                       // open network
    assert.equal((await cb.getWifi()).password, "");
    await cb.close();
});

test("clearWifi sends an empty ssid and info follows", async () => {
    const { dev, cb } = await connected({ wifi: { ssid: "Home", pass: "password1" } });
    assert.equal(cb.info.wifiCredentials, true);
    await cb.clearWifi();
    assert.equal(cb.info.wifiCredentials, false);
    assert.deepEqual([...dev.requests.filter(m => m.type === Msg.WIFI_SET)[0].payload], [0, 0]);
    assert.equal((await cb.getWifi()).ssid, "");
    await cb.close();
});

test("setWifi refuses invalid credentials before sending anything", async () => {
    const { dev, cb } = await connected({ wifi: {} });
    await assert.rejects(cb.setWifi("Home", "short"), RangeError);
    await assert.rejects(cb.setWifi("", "password1"), TypeError);
    assert.equal(dev.requests.filter(m => m.type === Msg.WIFI_SET).length, 0);
    assert.equal(cb.info.wifiCredentials, false);
    await cb.close();
});

test("a device that refuses setWifi throws and leaves info alone", async () => {
    const { cb } = await connected({ handler: () => ({ status: Status.IO_ERROR, data: new Uint8Array(0) }) });
    await assert.rejects(cb.setWifi("Home", "password1"), err => err instanceof ControlBlockError && err.status === Status.IO_ERROR);
    assert.equal(cb.info.wifiCredentials, false);
    await cb.close();
});

test("wifiStatus reports the connection", async () => {
    const { dev, cb } = await connected({ wifi: {} });
    assert.equal((await cb.wifiStatus()).stateName, "NO_CREDENTIALS");
    dev.wifi.status = { state: WifiState.CONNECTED, reason: 0, rssi: -61, ip: [192, 168, 0, 9], mac: [1, 2, 3, 4, 5, 6] };
    const st = await cb.wifiStatus();
    assert.deepEqual(st, { state: 2, stateName: "CONNECTED", reason: 0, rssi: -61, ip: "192.168.0.9", mac: "01:02:03:04:05:06" });
    await cb.close();
});

const wifiAt = state => ({ state, reason: 0, rssi: -50, ip: [10, 0, 0, 2], mac: [1, 2, 3, 4, 5, 6] });

test("waitWifiConnected: true once connected, onStatus once per change", async () => {
    const { dev, cb } = await connected({ wifi: { ssid: "Home", status: wifiAt(WifiState.CONNECTING) } });
    setTimeout(() => { dev.wifi.status = wifiAt(WifiState.CONNECTED); }, 40);
    const seen = [];
    assert.equal(await cb.waitWifiConnected({ timeoutMs: 2000, intervalMs: 5, onStatus: st => seen.push(st.stateName) }), true);
    assert.deepEqual(seen, ["CONNECTING", "CONNECTED"]);
    await cb.close();
});

test("waitWifiConnected: false straight away with no credentials", async () => {
    const { cb } = await connected({ wifi: {} });
    const seen = [];
    assert.equal(await cb.waitWifiConnected({ timeoutMs: 2000, onStatus: st => seen.push(st.stateName) }), false);
    assert.deepEqual(seen, ["NO_CREDENTIALS"]);
    await cb.close();
});

test("waitWifiConnected: false at the deadline", async () => {
    const { cb } = await connected({ wifi: { ssid: "Home", status: wifiAt(WifiState.RETRYING) } });
    const start = Date.now();
    assert.equal(await cb.waitWifiConnected({ timeoutMs: 60, intervalMs: 10 }), false);
    assert.ok(Date.now() - start >= 60);
    await cb.close();
});

test("wifiState events are emitted", async () => {
    const { dev, cb } = await connected();
    const got = new Promise(resolve => cb.on("wifiState", resolve));
    dev.event(Msg.WIFI_STATE, 8, Uint8Array.of(2, 0, 0, 0, 192, 168, 1, 5));
    const ev = await got;
    assert.equal(ev.state, WifiState.CONNECTED);
    assert.equal(ev.ip, "192.168.1.5");
    await cb.close();
});

// --- Take control and release (docs/protocol.md, docs/standalone.md) ---

test("a device boots standalone and refuses layout requests until takeControl", async () => {
    const { dev, cb } = await connected({ hosted: false });
    assert.equal(cb.info.hosted, false);
    await assert.rejects(cb.setPageCount(2), err => err instanceof ControlBlockError && err.status === Status.NOT_HOSTED);
    await assert.rejects(cb.reset(), err => err.status === Status.NOT_HOSTED);
    await cb.ping();                                        // the rest still works
    await cb.setBacklight(false);
    await cb.takeControl();
    await cb.setPageCount(2);
    assert.equal(dev.requests.filter(m => m.type === Msg.TAKE_CONTROL).length, 1);
    await cb.close();
});

test("takeControl sends TAKE_CONTROL and info follows", async () => {
    const { dev, cb } = await connected({ hosted: false });
    await cb.takeControl();
    assert.equal(cb.info.hosted, true);
    assert.equal(dev.hosted, true);
    assert.equal((await cb.getInfo()).hosted, true);
    await cb.close();
});

test("takeControl clears what the device had, like reset", async () => {
    const { dev, cb } = await connected({ hosted: false });
    dev.model = "home";
    await cb.takeControl();
    assert.equal(dev.model, "empty");
    await cb.close();
});

test("releaseControl sends RELEASE_CONTROL; the device is standalone with its light on", async () => {
    const { dev, cb } = await connected({ hosted: true });
    await cb.setBacklight(false);
    await cb.releaseControl();
    assert.equal(cb.info.hosted, false);
    assert.equal(cb.info.backlight, true);
    assert.equal(dev.hosted, false);
    assert.equal(dev.model, "home");
    await assert.rejects(cb.setPageCount(2), err => err.status === Status.NOT_HOSTED);
    await cb.close();
});

test("a refused takeControl throws and leaves info alone", async () => {
    const { cb } = await connected({ hosted: false, controlStatus: Status.IO_ERROR });
    await assert.rejects(cb.takeControl(), err => err instanceof ControlBlockError);
    assert.equal(cb.info.hosted, false);
    await cb.close();
});

test("a refused releaseControl throws and leaves info alone", async () => {
    const { cb } = await connected({ hosted: true, controlStatus: Status.IO_ERROR });
    await assert.rejects(cb.releaseControl(), err => err instanceof ControlBlockError);
    assert.equal(cb.info.hosted, true);
    await cb.close();
});

test("a reconnecting host learns the device is still hosted", async () => {
    const { cb } = await connected({ hosted: true });
    assert.equal(cb.info.hosted, true);
    await cb.close();
});

// --- Clock (docs/protocol.md, Clock) ---

const ms2026 = Date.UTC(2026, 10, 8, 7, 59, 1, 234);

test("setTime sends the time given (default now) and info follows; no takeControl needed", async () => {
    const { dev, cb } = await connected({ hosted: false, clock: {} });
    assert.equal(cb.info.timeSet, false);
    await cb.setTime(new Date(ms2026));
    assert.equal(cb.info.timeSet, true);
    const sent = dev.requests.filter(m => m.type === Msg.TIME_SET);
    assert.deepEqual([...sent[0].payload], [...encodeTimeSet(ms2026)]);
    assert.equal((await cb.getInfo()).timeSet, true);

    const before = Date.now();
    await cb.setTime();
    const at = Number(new DataView(dev.requests.filter(m => m.type === Msg.TIME_SET)[1].payload.buffer).getBigInt64(0, true));
    assert.ok(at >= before && at <= Date.now());
    await cb.close();
});

test("getTime reads the clock, the time zone and the NTP server", async () => {
    const { dev, cb } = await connected({ hosted: false, clock: {} });
    assert.deepEqual(await cb.getTime(), { known: false, source: ClockSource.NONE, sourceName: "NONE", time: null, timeZone: "UTC0", ntpServer: "pool.ntp.org" });
    await cb.setTime(new Date(ms2026));
    const t = await cb.getTime();
    assert.equal(t.known, true);
    assert.equal(t.sourceName, "HOST");
    assert.ok(Math.abs(t.time.getTime() - ms2026) < 1000);         // the fake's clock runs on
    dev.clock.source = ClockSource.NTP;
    assert.equal((await cb.getTime()).source, ClockSource.NTP);
    await cb.close();
});

test("setClock sets either or both settings; empty is the default", async () => {
    const { dev, cb } = await connected({ hosted: false, clock: {} });
    await cb.setClock({ timeZone: "AEST-10AEDT,M10.1.0,M4.1.0/3" });
    assert.deepEqual([dev.clock.tz, dev.clock.server], ["AEST-10AEDT,M10.1.0,M4.1.0/3", "pool.ntp.org"]);
    await cb.setClock({ ntpServer: "time.example.com" });
    assert.deepEqual([dev.clock.tz, dev.clock.server], ["AEST-10AEDT,M10.1.0,M4.1.0/3", "time.example.com"]);
    assert.equal((await cb.getTime()).ntpServer, "time.example.com");
    await cb.setClock({ timeZone: "", ntpServer: "" });
    assert.deepEqual([dev.clock.tz, dev.clock.server], ["UTC0", "pool.ntp.org"]);
    await cb.close();
});

test("setTime and setClock refuse bad values before sending anything", async () => {
    const { dev, cb } = await connected({ clock: {} });
    await assert.rejects(cb.setTime(new Date(0)), RangeError);
    await assert.rejects(cb.setClock({ timeZone: "UTC 0" }), RangeError);
    await assert.rejects(cb.setClock({}), TypeError);
    assert.equal(dev.requests.filter(m => m.type === Msg.TIME_SET || m.type === Msg.CLOCK_SET).length, 0);
    assert.equal(cb.info.timeSet, false);
    await cb.close();
});

test("a device that refuses the time throws a ControlBlockError and info is left alone", async () => {
    const { cb } = await connected({ handler: () => ({ status: Status.BAD_ARG, data: new Uint8Array(0) }) });
    await assert.rejects(cb.setTime(new Date(ms2026)), err => err instanceof ControlBlockError && err.status === Status.BAD_ARG);
    assert.equal(cb.info.timeSet, false);
    await assert.rejects(cb.setClock({ timeZone: "UTC0" }), err => err.status === Status.BAD_ARG);
    await cb.close();
});

test("connect sends the PC's time only when asked to", async () => {
    const plain = new FakeDevice({ clock: {} });
    const cb1 = new ControlBlock(plain, { timeoutMs: 300 });
    await cb1.connect();
    assert.equal(plain.requests.filter(m => m.type === Msg.TIME_SET).length, 0);
    assert.equal(cb1.info.timeSet, false);
    await cb1.close();

    const dev = new FakeDevice({ clock: {} });
    const cb = new ControlBlock(dev, { timeoutMs: 300 });
    const before = Date.now();
    const info = await cb.connect({ setTime: true });
    assert.deepEqual(dev.requests.map(m => m.type), [Msg.GET_INFO, Msg.TIME_SET]);
    assert.equal(info.timeSet, true);
    assert.ok(dev.clock.offset !== null && Math.abs(dev.clock.offset) <= Date.now() - before + 5);
    await cb.close();
});
