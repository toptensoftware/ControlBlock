import test from "node:test";
import assert from "node:assert/strict";
import { ControlBlock, ControlBlockError } from "../src/ControlBlock.js";
import { Msg, Status, Reader, encodeIds, crc32 } from "../src/protocol.js";
import { imageId, keyImage, encodeImage } from "../src/images.js";
import { readRleHeader } from "rle565/unpack.js";
import { FakeDevice } from "./FakeDevice.js";

async function connected(options)
{
    const dev = new FakeDevice(options);
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

test("reads wait briefly, so close() on an idle device returns promptly", async () => {
    // A pending transferIn can't be cancelled (usb 3.x), and close() and the
    // process exit wait for it: its timeout is the worst-case delay.
    const { dev, cb } = await connected();
    await new Promise(r => setTimeout(r, 120));         // let the idle read loop run
    assert.ok(dev.maxReadTimeout > 0);
    assert.ok(dev.maxReadTimeout <= 250, `read timeout ${dev.maxReadTimeout} ms`);
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

// --- Standalone config upload (docs/protocol.md "Configuration upload") ---

function blobOf(size, seed = 1)
{
    return new Uint8Array(size).map((_, i) => ((i + seed) * 2654435761) >>> 24);
}

test("uploadConfig sends begin, ordered chunks and commit, then the device restarts", async () => {
    const { dev, cb } = await connected({ maxPayload: 100, config: {} });
    const blob = blobOf(250);
    const progress = [];
    const r = await cb.uploadConfig(blob, { onProgress: (sent, total) => progress.push([sent, total]) });
    assert.deepEqual(r, { uploaded: true, size: 250, crc: crc32(blob) });

    const reqs = dev.requests.filter(m => m.type >= Msg.CONFIG_BEGIN && m.type <= Msg.CONFIG_COMMIT);
    assert.equal(reqs[0].type, Msg.CONFIG_BEGIN);
    const begin = new Reader(reqs[0].payload);
    assert.equal(begin.u32(), 250);
    assert.equal(begin.u32(), crc32(blob));
    const data = reqs.filter(m => m.type === Msg.CONFIG_DATA);
    assert.equal(data.length, 3);                       // 96 + 96 + 58
    let pos = 0;
    for (const c of data) {
        assert.equal(new Reader(c.payload).u32(), pos); // in order, back to back
        assert.ok(c.payload.length <= 100);
        pos += c.payload.length - 4;
    }
    assert.equal(pos, 250);
    assert.equal(reqs.at(-1).type, Msg.CONFIG_COMMIT);
    assert.deepEqual([...dev.config.stored.bytes], [...blob]);
    assert.deepEqual(progress.at(-1), [250, 250]);
    assert.ok(progress.every(([sent], i) => i === 0 || sent > progress[i - 1][0]));
    await cb.close();
});

test("uploadConfig keeps only a few chunks in flight", async () => {
    // Each chunk takes a while to answer; the library must not queue them all
    const { dev, cb } = await connected({ maxPayload: 100, config: { latencyMs: 10 } });
    await cb.uploadConfig(blobOf(96 * 40));             // 40 chunks
    assert.ok(dev.config.maxInflight > 1, "pipelined");
    assert.ok(dev.config.maxInflight <= 8, `window (saw ${dev.config.maxInflight})`);
    assert.equal(dev.config.stored.size, 96 * 40);
    await cb.close();
});

test("uploadConfig skips a config the device already has", async () => {
    const blob = blobOf(300);
    const { dev, cb } = await connected({ config: { stored: { size: 300, crc: crc32(blob) } } });
    const r = await cb.uploadConfig(blob);
    assert.deepEqual(r, { uploaded: false, size: 300, crc: crc32(blob) });
    assert.ok(!dev.requests.some(m => m.type === Msg.CONFIG_BEGIN));

    // Unless forced
    const forced = await cb.uploadConfig(blob, { force: true });
    assert.equal(forced.uploaded, true);
    assert.ok(dev.requests.some(m => m.type === Msg.CONFIG_BEGIN));
    await cb.close();
});

test("uploadConfig refuses what can't be right before sending anything", async () => {
    const { dev, cb } = await connected({ config: { capacity: 1000 } });
    await assert.rejects(cb.uploadConfig(new Uint8Array(0)), /Empty/);
    await assert.rejects(cb.uploadConfig(blobOf(1001)), /1000/);
    assert.ok(!dev.requests.some(m => m.type === Msg.CONFIG_BEGIN));
    await cb.close();
});

test("uploadConfig reports a begin the device refuses and sends no data", async () => {
    const { dev, cb } = await connected({ config: { capacity: 1 << 20 } });
    dev.config.capacity = 100;                          // less than the info said
    await assert.rejects(cb.uploadConfig(blobOf(500)),
                         err => err instanceof ControlBlockError && err.status === Status.NO_SPACE);
    assert.ok(!dev.requests.some(m => m.type === Msg.CONFIG_DATA || m.type === Msg.CONFIG_COMMIT));
    await cb.close();
});

test("uploadConfig stops at the first failed chunk and never commits", async () => {
    const { dev, cb } = await connected({ maxPayload: 100, config: { failAt: 96 * 3, latencyMs: 5 } });
    await assert.rejects(cb.uploadConfig(blobOf(96 * 30)),
                         err => err instanceof ControlBlockError && err.status === Status.IO_ERROR);
    const sent = dev.requests.filter(m => m.type === Msg.CONFIG_DATA).length;
    assert.ok(sent < 30, `stopped early (sent ${sent} of 30)`);
    assert.ok(!dev.requests.some(m => m.type === Msg.CONFIG_COMMIT));
    assert.equal(dev.config.stored, null);
    await cb.close();
});

test("uploadConfig lets the commit take longer than an ordinary request", async () => {
    const { dev, cb } = await connected({ config: { commitMs: 450 } });     // the connection's timeout is 300 ms
    const r = await cb.uploadConfig(blobOf(100));
    assert.equal(r.uploaded, true);
    assert.equal(dev.config.stored.size, 100);
    await cb.close();
});

test("a wrong CRC at the device is a BAD_CRC error", async () => {
    const { dev, cb } = await connected({ config: {} });
    // Corrupt what arrives: flip a byte in each chunk on its way
    const send = dev.transferOut.bind(dev);
    dev.transferOut = async (ep, data) => {
        const bytes = new Uint8Array(data);
        if (bytes[2] === Msg.CONFIG_DATA)
            bytes[bytes.length - 1] ^= 1;
        return send(ep, bytes);
    };
    await assert.rejects(cb.uploadConfig(blobOf(100)),
                         err => err instanceof ControlBlockError && err.status === Status.BAD_CRC);
    assert.equal(dev.config.stored, null);
    await cb.close();
});

test("after a commit the connection closes quietly and reconnect() finds the restarted device", async () => {
    const blob = blobOf(500);
    const { dev, cb } = await connected({ config: { restartMs: 120 } });
    const errors = [];
    cb.on("error", e => errors.push(e));
    await cb.uploadConfig(blob);

    // The device went away; the library reads that as a restart, not a failure
    const again = await cb.reconnect({
        timeoutMs: 3000, intervalMs: 20,
        opener: async () => { const c = new ControlBlock(dev, { timeoutMs: 300 }); await c.connect(); return c; },
    });
    assert.equal(cb.connected, false);
    assert.deepEqual(errors, []);
    assert.equal(again.info.configSize, 500);
    assert.equal(again.info.configCrc, crc32(blob));
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
