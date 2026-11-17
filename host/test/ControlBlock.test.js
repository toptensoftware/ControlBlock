import test from "node:test";
import assert from "node:assert/strict";
import { ControlBlock, ControlBlockError } from "../src/ControlBlock.js";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
    Msg, Status, Reader, encodeIds, crc32, HEARTBEAT_INTERVAL_MS, WifiState, encodeWifiSet, encodeTimeSet, ClockSource,
    PROTOCOL_VERSION, KEYDEF_SIZE, KEY_EVENT_SIZE, KeyAction, encodeKeys, keySlot, keyPixelSize, encodePutImage, PUT_IMAGE_PREFIX,
    BPAK_SIGNATURE, BPAK_HEADER_SIZE,
} from "../src/protocol.js";
import { keyImage, encodeImage } from "../src/images.js";
import { FakeDevice, DEVICE, DISPLAY, displayPayload, layoutBlobOf } from "./FakeDevice.js";
import { BLOB_VERSION } from "../src/bake/consts.js";

const here = path.dirname(fileURLToPath(import.meta.url));

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

// The device's writes wait for the test (`release()` lets every write so far complete), and each
// transferIn and transferOut is logged ("in" when a read is submitted, "out"/"out done" for writes)
function slowWrites(dev)
{
    const log = [];
    let gate = [];
    const send = dev.transferOut.bind(dev), read = dev.transferIn.bind(dev);
    dev.transferOut = async (ep, data) => {
        log.push("out");
        await new Promise(r => gate.push(r));
        const r = await send(ep, data);
        log.push("out done");
        return r;
    };
    dev.transferIn = (...args) => { log.push("in"); return read(...args); };
    return { log, release() { const g = gate; gate = []; g.forEach(r => r()); }, waiting: () => gate.length };
}

const tick = ms => new Promise(r => setTimeout(r, ms));

// A PUT_IMAGE the device refuses (id 0): a bulk-data request with a 16-byte reply, like an upload chunk
const bulk = (cb, i) => cb.request(Msg.PUT_IMAGE, encodePutImage(0, 1, 0, Uint8Array.of(i)));

// Release every write until none is waiting; the log from the first write to the last one's end
async function drain(w)
{
    const first = w.log.indexOf("out");
    while (w.waiting()) {
        w.release();
        await tick(5);
    }
    return w.log.slice(first, w.log.lastIndexOf("out done"));
}

test("no read is submitted while bulk-data writes are queued (a pending read slows bulk OUT); reads resume when they drain", async () => {
    const dev = new FakeDevice({ hosted: true });
    const cb = new ControlBlock(dev, { timeoutMs: 2000, readHoldMs: 1000 });
    await cb.connect();
    const w = slowWrites(dev);
    const sent = Array.from({ length: 4 }, (_, i) => bulk(cb, i));
    // FakeDevice's idle reads end every 50 ms ("Cancelled") and the loop resubmits: held now
    await tick(120);
    assert.equal(w.waiting(), 1);
    const during = await drain(w);
    assert.ok(!during.includes("in"), w.log.join(", "));
    for (const r of await Promise.all(sent))            // the replies were read after
        assert.equal(r.status, Status.BAD_ARG);
    await cb.close();
});

test("reads aren't held while any other request is queued (its reply may be big: held, it would fill the device's FIFO)", async () => {
    const dev = new FakeDevice({ hosted: true });
    const cb = new ControlBlock(dev, { timeoutMs: 2000, readHoldMs: 1000 });
    await cb.connect();
    const w = slowWrites(dev);
    const sent = [bulk(cb, 1), cb.ping(new Uint8Array(100)), bulk(cb, 2)];
    await tick(120);                                    // FakeDevice's idle reads end and are resubmitted
    assert.ok(w.log.slice(w.log.indexOf("out")).includes("in"), w.log.join(", "));
    await drain(w);
    await Promise.all(sent);
    await cb.close();
});

test("a held read still goes out after readHoldMs while writes stay queued (the device's transmit FIFO must drain)", async () => {
    const dev = new FakeDevice({ hosted: true });
    const cb = new ControlBlock(dev, { timeoutMs: 2000, readHoldMs: 30 });
    await cb.connect();
    const w = slowWrites(dev);
    const sent = bulk(cb, 7);
    await tick(150);                                    // the write never completes meanwhile
    assert.equal(w.waiting(), 1);
    assert.ok(w.log.slice(w.log.indexOf("out")).includes("in"), w.log.join(", "));
    w.release();
    assert.equal((await sent).status, Status.BAD_ARG);
    await cb.close();
});

test("close() releases a held read", async () => {
    const dev = new FakeDevice({ hosted: true });
    const cb = new ControlBlock(dev, { timeoutMs: 2000, readHoldMs: 60000 });
    await cb.connect();
    const w = slowWrites(dev);
    bulk(cb, 1).catch(() => { });
    await tick(80);
    const closed = cb.close();
    w.release();
    await closed;                                       // the test runner would hang on a stuck loop
});

test("a message that ends on a whole USB packet is followed by a zero-length packet", async () => {
    // FakeDevice holds back a transfer that ends on a full packet until a short
    // packet or a ZLP ends it, like the device: without the ZLP these hang
    const { dev, cb } = await connected();
    const sizes = [];
    const send = dev.transferOut.bind(dev);
    dev.transferOut = async (ep, data) => { sizes.push(new Uint8Array(data).length); return send(ep, data); };

    for (const n of [52, 52 + 64, 52 + 512, 500, 4084, 4084 + 64])     // messages of 64, 128, 576, 512, 4096 and 4160 bytes
        assert.equal((await cb.ping(new Uint8Array(n))).length, n);
    assert.deepEqual(sizes, [64, 0, 128, 0, 576, 0, 512, 0, 4096, 0, 4160, 0]);

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
    await assert.rejects(cb.setKeys([{ id: 1, page: 0, normal: 99 }]),
                         err => err instanceof ControlBlockError && err.status === Status.NOT_FOUND);
    await cb.close();
});

test("putImage splits into chunks the device can take", async () => {
    // (not an image: a handler takes the chunks instead of the fake's image store)
    const { dev, cb } = await connected({ maxPayload: 100, handler: () => ({ status: Status.OK, data: new Uint8Array(0) }) });
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

test("a PUT_IMAGE for id 0 is refused (BAD_ARG), its data skipped, and the link stays in step", async () => {
    // The firmware skips such a message's data without touching the image store (State::Skip):
    // examples/bench-sink.mjs uses it to time the USB pipe alone
    const { dev, cb } = await connected();
    const data = new Uint8Array(cb.info.maxPayload - PUT_IMAGE_PREFIX).fill(0x5A);
    const rs = await Promise.all(Array.from({ length: 4 }, () =>
        cb.request(Msg.PUT_IMAGE, encodePutImage(0, data.length, 0, data))));
    assert.deepEqual(rs.map(r => r.status), [Status.BAD_ARG, Status.BAD_ARG, Status.BAD_ARG, Status.BAD_ARG]);
    assert.equal(dev.images.size, 0);
    assert.equal((await cb.ping(Uint8Array.of(7)))[0], 7);
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

// --- Keys and images (protocol v2), against FakeDevice's model of the firmware ---

// An opaque width x height key image of one colour (images.js's keyImage), cached
const images = new Map();
async function plain(width, height, shade = 1)
{
    const key = `${width}x${height}/${shade}`;
    if (!images.has(key)) {
        const rgba = new Uint8Array(width * height * 4);
        for (let i = 0; i < rgba.length; i += 4)
            rgba.set([shade * 40 & 0xFF, 80, 160, 255], i);
        images.set(key, await keyImage({ rgba, width, height }, width, height));
    }
    return images.get(key);
}

const statusOf = p => p.then(() => Status.OK, err => { if (!(err instanceof ControlBlockError)) throw err; return err.status; });

test("FakeDevice's geometry and wire sizes match the firmware's headers", () => {
    const header = name => fs.readFileSync(path.join(here, "../../firmware/main", name), "utf8");
    const define = (text, name) => {
        const m = text.match(new RegExp(`#define\\s+${name}\\s+(\\d+)`));
        assert.ok(m, `${name} not found`);
        return Number(m[1]);
    };
    const geometry = header("key_geometry.h"), screen = header("screen_layout.h"), proto = header("protocol.h"), keys = header("key_layout.h");
    assert.equal(DEVICE.cell, define(geometry, "KEY_CELL"));
    assert.equal(DEVICE.cols, define(geometry, "KEY_GRID_COLS"));
    assert.equal(DEVICE.rows, define(geometry, "KEY_GRID_ROWS"));
    assert.equal(DEVICE.quarter, DEVICE.cell / 4);              // KEY_QUARTER = KEY_CELL / 4
    assert.equal(DEVICE.maxKeys, define(geometry, "COMP_MAX_KEYS"));
    assert.equal(DEVICE.screenWidth, define(screen, "SCREEN_W"));
    assert.equal(DEVICE.screenHeight, define(screen, "SCREEN_H"));
    assert.equal(DEVICE.statusHeight, define(screen, "SCREEN_STATUS_H"));
    assert.match(screen, /#define SCREEN_PAGE_H\s+\(SCREEN_H - SCREEN_STATUS_H\)/);   // DEVICE.gridY assumes no pager bar
    assert.equal(DEVICE.maxPages, define(screen, "SCREEN_MAX_PAGES"));
    assert.equal(DEVICE.icon, define(keys, "KEY_ICON_SIZE"));
    assert.equal(PROTOCOL_VERSION, define(proto, "PROTO_VERSION"));
    assert.equal(KEYDEF_SIZE, define(proto, "PROTO_KEYDEF_SIZE"));
    assert.equal(KEY_EVENT_SIZE, define(proto, "PROTO_KEY_EVENT_SIZE"));
    assert.deepEqual([DEVICE.gridX, DEVICE.gridY, DEVICE.gridWidth, DEVICE.gridHeight], [40, 44, 720, 432]);
    // the layout blob header uploadLayout reads the version from (layout_blob.h)
    const blob = header("layout_blob.h");
    assert.equal(BPAK_HEADER_SIZE, define(blob, "BPAK_HEADER_SIZE"));
    assert.match(blob, new RegExp(`#define\\s+BPAK_SIGNATURE\\s+0x${BPAK_SIGNATURE.toString(16)}u`, "i"));
    assert.match(header("layout_types.gen.h"), /struct layoutRoot\s*\{\s*\/\*\s*0 \*\/\s*uint32_t version;/);   // the root's first field
    // the display timing limits of the 20 MHz build (display_timing.h)
    const timing = header("display_timing.h");
    const constant = name => {
        const m = timing.match(new RegExp(`constexpr uint32_t ${name}\\s*=\\s*(\\d+);`));
        assert.ok(m, `${name} not found`);
        return Number(m[1]);
    };
    assert.match(header("waveshare_rgb_lcd_port.h"), /#define DISPLAY_PIXEL_CLOCK_HZ\s+\(20 \* 1000 \* 1000\)/);
    assert.deepEqual([DISPLAY.defaultClockMhz, DISPLAY.maxClockMhz], [20, 20]);
    assert.equal(DISPLAY.minClockMhz, constant("DISPLAY_PCLK_MIN_MHZ"));
    assert.equal(DISPLAY.defaultLines, constant("DISPLAY_TIMING_V_TOTAL"));
    assert.equal(DISPLAY.minLines, constant("DISPLAY_TIMING_V_TOTAL"));
    assert.equal(DISPLAY.maxLines, constant("DISPLAY_V_TOTAL_HW_MAX"));
    assert.equal(DISPLAY.hTotal, constant("DISPLAY_TIMING_H_TOTAL"));
    assert.equal(DISPLAY.fillLines, constant("DISPLAY_TIMING_FILL_LINES"));
    assert.equal(displayPayload().length, define(proto, "PROTO_DISPLAY_INFO_SIZE"));
});

test("connect reads the v2 geometry", async () => {
    const { cb } = await connected();
    const i = cb.info;
    assert.equal(i.protocolVersion, 2);
    assert.deepEqual([i.keyWidth, i.quarter, i.gridX, i.gridY, i.gridWidth, i.gridHeight, i.maxKeys, i.maxImageWidth, i.maxImageHeight],
                     [144, 36, 40, 44, 720, 432, 32, 720, 432]);
    assert.deepEqual(keyPixelSize({ w: 2 }, i), { width: 288, height: 144 });
    await cb.close();
});

test("connect refuses a device that speaks another protocol version", async () => {
    const dev = new FakeDevice({ hosted: true });
    const send = dev.respond.bind(dev);
    dev.respond = (msg, status, data) => {
        if (msg.type === Msg.GET_INFO)
            data[0] = 1;                                        // a v1 device
        send(msg, status, data);
    };
    const cb = new ControlBlock(dev, { timeoutMs: 300 });
    await assert.rejects(cb.connect(), /protocol 1, expected 2/);
    await cb.close();
});

test("putImage: opaque images are stored (and replaced); anything else is BAD_IMAGE", async () => {
    const { dev, cb } = await connected();
    const one = await plain(144, 144), two = await plain(288, 144);
    await cb.putImage(one.id, one.data);
    await cb.putImage(two.id, two.data);
    assert.deepEqual(await cb.queryImages([one.id, two.id, 5]), [true, true, false]);
    assert.deepEqual([dev.images.get(two.id).width, dev.images.get(two.id).height], [288, 144]);
    assert.equal((await cb.getInfo()).imageCount, 2);

    const seeThrough = await encodeImage(new Uint8Array(36 * 36 * 4).fill(128), 36, 36);     // alpha 128
    assert.equal(await statusOf(cb.putImage(7, seeThrough)), Status.BAD_IMAGE);
    assert.equal(await statusOf(cb.putImage(8, Uint8Array.of(1, 2, 3, 4))), Status.BAD_IMAGE);
    const tooWide = await plain(721, 36);
    assert.equal(await statusOf(cb.putImage(9, tooWide.data)), Status.BAD_IMAGE);
    assert.deepEqual(await cb.queryImages([7, 8, 9]), [false, false, false]);

    // A refused replace keeps the old image; a good one changes it in place
    assert.equal(await statusOf(cb.putImage(one.id, seeThrough)), Status.BAD_IMAGE);
    assert.equal(dev.images.get(one.id).width, 144);
    await cb.putImage(one.id, two.data);
    assert.equal(dev.images.get(one.id).width, 288);
    await cb.close();
});

test("setKeys: keys by id with quarter-unit rects, images the key's size", async () => {
    const { dev, cb } = await connected();
    const one = await plain(144, 144), onePressed = await plain(144, 144, 2), two = await plain(288, 144), small = await plain(36, 72);
    await cb.syncImages([one, onePressed, two, small]);
    await cb.setPageCount(2);
    await cb.setKeys([
        { id: 1, page: 0, ...keySlot(0), normal: one, pressed: onePressed },
        { id: 2, page: 0, x: 1, y: 0, w: 2, normal: two.id },
        { id: 3, page: 0, x: 3.25, y: 1.5, w: 0.25, h: 0.5, normal: small, disabled: true },
        { id: 40, page: 1, ...keySlot(14) },                    // no image: black
    ]);
    assert.deepEqual(dev.keys(0), [
        { id: 1, page: 0, x: 0, y: 0, w: 4, h: 4, disabled: false, normal: one.id, pressed: onePressed.id },
        { id: 2, page: 0, x: 4, y: 0, w: 8, h: 4, disabled: false, normal: two.id, pressed: 0 },
        { id: 3, page: 0, x: 13, y: 6, w: 1, h: 2, disabled: true, normal: small.id, pressed: 0 },
    ]);
    assert.deepEqual(dev.keys(1), [{ id: 40, page: 1, x: 16, y: 8, w: 4, h: 4, disabled: false, normal: 0, pressed: 0 }]);

    // The wrong size, or an image the device doesn't have
    assert.equal(await statusOf(cb.setKeys([{ id: 4, page: 1, ...keySlot(0), normal: two }])), Status.BAD_IMAGE);
    assert.equal(await statusOf(cb.setKeys([{ id: 4, page: 1, ...keySlot(0), normal: one, pressed: two }])), Status.BAD_IMAGE);
    assert.equal(await statusOf(cb.setKeys([{ id: 4, page: 1, ...keySlot(0), normal: 12345 }])), Status.NOT_FOUND);
    assert.equal(dev.keys(1).length, 1);
    await cb.close();
});

test("setKeys refuses what the device refuses, and changes nothing", async () => {
    const { dev, cb } = await connected();
    await cb.setPageCount(2);
    await cb.setKeys([{ id: 1, page: 0, ...keySlot(0) }, { id: 2, page: 1, ...keySlot(0) }]);
    const before = [dev.keys(0), dev.keys(1)];
    const good = { id: 9, page: 0, ...keySlot(5) };
    const cases = {
        "overlap":                    [{ id: 10, page: 0, x: 0.75, y: 0 }],
        "overlap within the message": [{ id: 10, page: 0, ...keySlot(2) }, { id: 11, page: 0, x: 2.5, y: 0 }],
        "past the grid's right":      [{ id: 10, page: 0, x: 4.25, y: 0 }],
        "past the grid's bottom":     [{ id: 10, page: 0, x: 0, y: 2.5 }],
        "a page past the count":      [{ id: 10, page: 2, ...keySlot(0) }],
        "an id twice":                [{ id: 10, page: 0, ...keySlot(1) }, { id: 10, page: 0, ...keySlot(2) }],
        "removed and defined":        [{ id: 2, remove: true }, { id: 2, page: 0, ...keySlot(3) }],
        "an id on another page":      [{ id: 2, page: 0, ...keySlot(3) }],
        "too many keys on a page":    Array.from({ length: 33 }, (_, i) => ({ id: 100 + i, page: 1, x: (i % 20) / 4, y: 1 + Math.floor(i / 20) / 4, w: 0.25, h: 0.25 })),
    };
    for (const [name, keys] of Object.entries(cases)) {
        assert.equal(await statusOf(cb.setKeys(keys)), Status.BAD_ARG, name);
        assert.equal(await statusOf(cb.setKeys([good, ...keys])), Status.BAD_ARG, `${name}, with a good key`);
        assert.deepEqual([dev.keys(0), dev.keys(1)], before, name);
    }
    // 32 is fine (key 2 is already there)
    await cb.setKeys(cases["too many keys on a page"].slice(0, 31));
    assert.equal(dev.keys(1).length, 32);

    // What the library can't encode never reaches the device
    const sent = dev.requests.length;
    await assert.rejects(cb.setKeys([{ id: 0, page: 0 }]), RangeError);
    await assert.rejects(cb.setKeys([{ id: 5, page: 0, x: 0.1 }]), RangeError);
    assert.equal(dev.requests.length, sent);

    // Raw: an unknown flag, and a payload shorter than its count
    const raw = encodeKeys([{ id: 5, page: 0, ...keySlot(4) }]);
    raw[2 + 8] = 0x04;
    assert.equal((await cb.request(Msg.SET_KEYS, raw)).status, Status.BAD_ARG);
    assert.equal((await cb.request(Msg.SET_KEYS, raw.subarray(0, 21))).status, Status.BAD_LENGTH);
    assert.equal(dev.keys(0).length, 1);
    await cb.close();
});

test("setKeys edits keys by id: replace, move within a page, swap, remove; keys not named stay", async () => {
    const { dev, cb } = await connected();
    await cb.setPageCount(2);
    await cb.setKeys([{ id: 1, page: 0, ...keySlot(0) }, { id: 2, page: 0, ...keySlot(1) }, { id: 3, page: 1, ...keySlot(0) }]);
    const where = page => dev.keys(page).map(k => [k.id, k.x, k.y, k.w]);

    await cb.setKeys([{ id: 2, page: 0, ...keySlot(1), disabled: true }]);          // replace
    assert.deepEqual(where(0), [[1, 0, 0, 4], [2, 4, 0, 4]]);
    assert.equal(dev.keys(0)[1].disabled, true);

    await cb.setKeys([{ id: 1, page: 0, ...keySlot(1) }, { id: 2, page: 0, ...keySlot(0) }]);   // swap: checked on the result
    assert.deepEqual(where(0), [[1, 4, 0, 4], [2, 0, 0, 4]]);

    await cb.setKeys([{ id: 1, page: 0, x: 1, y: 0, w: 2 }, { id: 4, page: 0, ...keySlot(5) }, { id: 5, page: 1, ...keySlot(1) }]);
    assert.deepEqual(where(0), [[2, 0, 0, 4], [1, 4, 0, 8], [4, 0, 4, 4]]);
    assert.deepEqual(where(1), [[3, 0, 0, 4], [5, 4, 0, 4]]);

    await cb.removeKeys([2, 99]);                                                    // unknown ids are ignored
    assert.deepEqual(where(0), [[1, 4, 0, 8], [4, 0, 4, 4]]);
    await cb.setKeys([{ id: 3, remove: true }]);                                     // then it may go elsewhere
    await cb.setKeys([{ id: 3, page: 0, ...keySlot(14) }]);
    assert.deepEqual(where(1), [[5, 4, 0, 4]]);
    assert.deepEqual(where(0).at(-1), [3, 16, 8, 4]);
    await cb.setKeys([]);                                                            // nothing: fine
    await cb.close();
});

test("a freed image: the key showing it keeps it, a new key can't take it", async () => {
    const { dev, cb } = await connected();
    const one = await plain(144, 144);
    await cb.putImage(one.id, one.data);
    await cb.setKeys([{ id: 1, page: 0, ...keySlot(0), normal: one }]);
    await cb.freeImages([one.id]);
    assert.deepEqual(await cb.queryImages([one.id]), [false]);
    assert.equal(dev.keys(0)[0].normal, one.id);
    await cb.setKeys([{ id: 1, page: 0, ...keySlot(0), normal: one, disabled: true }]);     // re-sending its key is fine
    await cb.setKeys([{ id: 2, page: 0, ...keySlot(1) }]);                                  // so is an edit beside it
    assert.equal(await statusOf(cb.setKeys([{ id: 3, page: 0, ...keySlot(2), normal: one }])), Status.NOT_FOUND);
    await cb.close();
});

test("setPageCount drops removed pages' keys; setPage; reset and takeControl clear everything", async () => {
    const { dev, cb } = await connected({ hosted: false });
    await cb.takeControl();
    const one = await plain(144, 144);
    await cb.putImage(one.id, one.data);
    await cb.setPageCount(3);
    await cb.setKeys([{ id: 1, page: 2, ...keySlot(0), normal: one }, { id: 2, page: 0, ...keySlot(0) }]);
    await cb.setPage(2);
    assert.equal((await cb.getInfo()).currentPage, 2);
    assert.equal(await statusOf(cb.setPage(3)), Status.BAD_ARG);
    assert.equal(await statusOf(cb.setPageCount(9)), Status.BAD_ARG);

    await cb.setPageCount(2);
    assert.equal(cb.info.pageCount, 2);
    assert.equal((await cb.getInfo()).currentPage, 1);
    assert.deepEqual(dev.keys(2), []);
    await cb.setKeys([{ id: 1, page: 1, ...keySlot(0) }]);                          // its id is free again

    await cb.reset();
    assert.equal(dev.pageCount, 1);
    assert.deepEqual(dev.keys(0), []);
    assert.deepEqual(await cb.queryImages([one.id]), [false]);

    await cb.putImage(one.id, one.data);
    await cb.setStatusText({ left: "x" });
    await cb.takeControl();                                                          // again: like reset
    assert.deepEqual(await cb.queryImages([one.id]), [false]);
    assert.deepEqual(dev.statusText, ["", ""]);
    await cb.close();
});

test("key events carry the key's id and the page", async () => {
    const { dev, cb } = await connected();
    await cb.setPageCount(2);
    await cb.setKeys([{ id: 300, page: 1, x: 0, y: 0, w: 2 }, { id: 7, page: 0, ...keySlot(0), disabled: true }]);
    const got = new Promise(resolve => cb.on("key", resolve));
    dev.keyEvent(300, KeyAction.RELEASE, { timeMs: 1234, lost: true });
    assert.deepEqual(await got, { type: "key", seq: 0, id: 300, page: 1, action: KeyAction.RELEASE, lost: true, timeMs: 1234 });
    assert.throws(() => dev.keyEvent(7, KeyAction.PRESS), /disabled/);              // the device sends none for these
    assert.throws(() => dev.keyEvent(8, KeyAction.PRESS), /no such key/);
    await cb.close();
});

// --- Standalone layout upload (docs/protocol.md "Layout upload") ---

const blobOf = (size, seed = 1) => layoutBlobOf(size, { seed });

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

test("uploadLayout refuses a blob of another version than the device loads, sending nothing", async () => {
    const { dev, cb } = await connected({ layout: {} });
    try {
        assert.equal(cb.info.layoutBlobVersion, BLOB_VERSION);
        await assert.rejects(cb.uploadLayout(layoutBlobOf(500, { version: BLOB_VERSION + 1 })),
                             new RegExp(`version ${BLOB_VERSION + 1}.*loads version ${BLOB_VERSION}.*update the firmware`));
        await assert.rejects(cb.uploadLayout(layoutBlobOf(500, { version: BLOB_VERSION - 1 })),
                             new RegExp(`version ${BLOB_VERSION - 1}.*loads version ${BLOB_VERSION}.*re-bake`));
        await assert.rejects(cb.uploadLayout(new Uint8Array(500)), /not a layout blob/i);
        // even with force, and even when the device already has it (it refused it at boot)
        const old = layoutBlobOf(300, { version: BLOB_VERSION - 1 });
        dev.layout.stored = { size: 300, crc: crc32(old) };
        await assert.rejects(cb.uploadLayout(old, { force: true }), /version/);
        await assert.rejects(cb.uploadLayout(old), /version/);
        assert.ok(!dev.requests.some(m => m.type >= Msg.LAYOUT_BEGIN && m.type <= Msg.LAYOUT_COMMIT));

        const r = await cb.uploadLayout(layoutBlobOf(500));  // the matching version goes
        assert.equal(r.uploaded, true);
    } finally {
        await cb.close();
    }
});

test("uploadLayout doesn't check the version with firmware that doesn't report it", async () => {
    const { dev, cb } = await connected({ layout: {}, blobVersion: null });
    try {
        assert.equal(cb.info.layoutBlobVersion, undefined);
        const any = new Uint8Array(200).fill(7);             // not even a BPAK blob
        assert.equal((await cb.uploadLayout(any)).uploaded, true);
        assert.deepEqual([...dev.layout.stored.bytes], [...any]);
    } finally {
        await cb.close();                                    // a failure mustn't leave the read loop running
    }
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

test("getDisplayTiming, setDisplayTiming and resetDisplayTiming (experimental)", async () => {
    const { dev, cb } = await connected({ hosted: false, display: {} });
    try {
        const d = await cb.getDisplayTiming();
        assert.equal(d.clockMhz, 20);
        assert.equal(d.lines, 500);
        assert.equal(d.isDefault, true);
        assert.equal(d.fps, 48.8);

        await cb.setDisplayTiming({ clockMhz: 20, lines: 750 });
        assert.deepEqual(dev.display, { clockMhz: 20, lines: 750 });
        const s = await cb.getDisplayTiming();
        assert.equal(s.fps, 32.5);
        assert.equal(s.isDefault, false);

        // An omitted field is the device's default
        await cb.setDisplayTiming({ clockMhz: 16 });
        assert.deepEqual(dev.display, { clockMhz: 16, lines: 500 });

        await cb.resetDisplayTiming();
        assert.equal((await cb.getDisplayTiming()).isDefault, true);
        assert.deepEqual([...dev.requests.filter(m => m.type === Msg.DISPLAY_SET).at(-1).payload], [0, 0, 0]);
    } finally {
        await cb.close();
    }
});

test("setDisplayTiming refuses a timing outside the device's limits before sending it", async () => {
    const { dev, cb } = await connected({ hosted: false, display: {} });
    try {
        await assert.rejects(cb.setDisplayTiming({ clockMhz: 21, lines: 750 }), /clockMhz must be 16-20/);
        await assert.rejects(cb.setDisplayTiming({ clockMhz: 12 }), RangeError);
        await assert.rejects(cb.setDisplayTiming({ lines: 1025 }), /lines must be 500-1024/);
        await assert.rejects(cb.setDisplayTiming({ lines: 499 }), RangeError);
        assert.equal(dev.requests.filter(m => m.type === Msg.DISPLAY_SET).length, 0);
    } finally {
        await cb.close();
    }
});

test("display timing on firmware without it says so", async () => {
    const { cb } = await connected({ hosted: false });            // no `display`: the fake answers UNKNOWN_TYPE
    try {
        await assert.rejects(cb.getDisplayTiming(), err => {
            assert.ok(err instanceof ControlBlockError);
            assert.equal(err.status, Status.UNKNOWN_TYPE);
            assert.match(err.message, /firmware doesn't support display timing/);
            return true;
        });
        await assert.rejects(cb.setDisplayTiming({ lines: 750 }), /firmware doesn't support display timing/);
    } finally {
        await cb.close();
    }
});

test("setName and getName store and read the device name; empty is the default", async () => {
    const { dev, cb } = await connected({ hosted: false, name: {} });
    assert.deepEqual(await cb.getName(), { name: "controlblock-a1b2c3", isDefault: true });
    await cb.setName("Den-TV");
    assert.deepEqual(await cb.getName(), { name: "den-tv", isDefault: false });
    assert.equal(dev.name.name, "den-tv");
    await cb.setName("");
    assert.deepEqual(await cb.getName(), { name: "controlblock-a1b2c3", isDefault: true });
    await assert.rejects(cb.setName("a.b"), RangeError);
    assert.equal(dev.requests.filter(m => m.type === Msg.NAME_SET).length, 2);
    await cb.close();
});
