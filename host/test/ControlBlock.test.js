import test from "node:test";
import assert from "node:assert/strict";
import { ControlBlock, ControlBlockError } from "../src/ControlBlock.js";
import { Msg, Status, Reader, encodeIds } from "../src/protocol.js";
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
