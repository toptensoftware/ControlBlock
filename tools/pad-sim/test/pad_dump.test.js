import { test } from "node:test";
import assert from "node:assert/strict";
import * as W from "../src/pad_wire.js";
import { PadDump, classifyError, formatEvent, formatStats } from "../src/pad_dump.js";

function rig() {
    let t = 0;
    const events = [];
    const dump = new PadDump({ now: () => t, onEvent: e => events.push(e) });
    const rec = {
        dump, events,
        advance(ms) { t += ms; },
        state(seq, keys, encs = []) {
            dump.feed(W.encodePacket(seq, W.PAD_CMD_STATE, W.buildState({ keys, encoders: encs })));
        },
        hello(seq, k = 6, e = 1) {
            dump.feed(W.encodePacket(seq, W.PAD_CMD_HELLO, W.buildHello({ version: 1, keyCount: k, encoderCount: e })));
        },
        types() { return events.map(e => e.type); },
        take() { return events.splice(0); },
    };
    return rec;
}
const enc = (position, interval = 6500) => ({ position, interval });

test("hello is reported", () => {
    const r = rig();
    r.hello(0, 6, 1);
    assert.deepEqual(r.events, [{ type: "hello", t: 0, seq: 0, version: 1, keyCount: 6, encoderCount: 1 }]);
});

test("first state is a baseline and fires nothing, held keys are listed", () => {
    const r = rig();
    r.state(1, 0b101, [enc(10)]);
    assert.equal(r.events.length, 1);
    assert.equal(r.events[0].type, "baseline");
    assert.deepEqual(r.events[0].keysDown, [1, 3]);
    r.state(2, 0b101, [enc(10)]);
    assert.equal(r.events.length, 1);
    assert.equal(r.dump.stats().repeats, 1);
});

test("key down and up come from diffing states (key1..key32)", () => {
    const r = rig();
    r.state(1, 0);
    r.take();
    r.state(2, 0x80000001);
    assert.deepEqual(r.take().map(e => [e.type, e.index, e.down]), [["key", 1, true], ["key", 32, true]]);
    r.state(3, 0x80000000);
    assert.deepEqual(r.take().map(e => [e.type, e.index, e.down]), [["key", 1, false]]);
});

test("encoder turn: signed delta, interval, wraps across 2^32", () => {
    const r = rig();
    r.state(1, 0, [enc(100), enc(0xFFFFFFFE)]);
    r.take();
    r.state(2, 0, [enc(97, 4000), enc(2, 9000)]);
    const turns = r.take();
    assert.deepEqual(turns.map(e => [e.type, e.encoder, e.delta, e.interval]), [["turn", 1, -3, 4000], ["turn", 2, 4, 9000]]);
});

test("a huge delta is a reset, not a turn", () => {
    const r = rig();
    r.state(1, 0, [enc(0)]);
    r.take();
    r.state(2, 0, [enc(40000)]);
    assert.deepEqual(r.take().map(e => e.type), ["reset"]);
    r.state(3, 0, [enc(40003)]);
    assert.deepEqual(r.take().map(e => [e.type, e.delta]), [["turn", 3]]);
});

test("hello clears the baseline", () => {
    const r = rig();
    r.state(1, 0, [enc(0)]);
    r.hello(2);
    r.take();
    r.state(3, 1, [enc(500)]);
    assert.deepEqual(r.take().map(e => e.type), ["baseline"]);
});

test("changed encoder count rebaselines", () => {
    const r = rig();
    r.state(1, 0, [enc(0)]);
    r.take();
    r.state(2, 1, [enc(5), enc(5)]);
    assert.deepEqual(r.take().map(e => e.type), ["baseline"]);
});

test("offline after 300 ms of silence, then a fresh baseline", () => {
    const r = rig();
    r.state(1, 0);
    r.take();
    r.advance(299);
    r.dump.tick();
    assert.equal(r.events.length, 0);
    r.advance(2);
    r.dump.tick();
    assert.deepEqual(r.take().map(e => e.type), ["offline"]);
    r.advance(1000);
    r.dump.tick();
    assert.equal(r.events.length, 0);            // only once
    r.state(2, 1);
    assert.deepEqual(r.take().map(e => e.type), ["baseline"]);
});

test("seq gaps are counted; a backwards seq is a reset, not a gap", () => {
    const r = rig();
    r.state(10, 0);
    r.state(11, 0);
    r.state(14, 0);
    assert.equal(r.dump.stats().seqGaps, 2);
    r.state(0, 0);
    assert.equal(r.dump.stats().seqGaps, 2);
    assert.equal(r.dump.stats().seqResets, 1);
    r.state(1, 0);
    assert.equal(r.dump.stats().seqGaps, 2);
});

test("bad payload lengths and unknown commands are counted and reported", () => {
    const r = rig();
    r.dump.feed(W.encodePacket(1, W.PAD_CMD_STATE, Buffer.alloc(5)));
    r.dump.feed(W.encodePacket(2, W.PAD_CMD_HELLO, Buffer.alloc(2)));
    r.dump.feed(W.encodePacket(3, 9, Buffer.alloc(2)));
    assert.deepEqual(r.types(), ["bad", "bad", "unknown"]);
    const s = r.dump.stats();
    assert.equal(s.badPayload, 2);
    assert.equal(s.unknown, 1);
    assert.equal(s.ok, 3);
});

test("decoder errors are named", () => {
    const r = rig();
    const good = W.encodePacket(1, W.PAD_CMD_STATE, W.buildState({ keys: 3, encoders: [] }));
    const flipped = Buffer.from(good);
    flipped[7] ^= 0x10;
    r.dump.feed(flipped);                              // crc
    r.dump.feed(good.subarray(0, 8));                  // truncated, then a new start signal
    r.dump.feed(good);
    r.dump.feed(W.encodePacket(1, 1, Buffer.alloc(40)));   // too large
    const noTerm = Buffer.from(good);
    noTerm[noTerm.length - 1] = 0x11;
    r.dump.feed(noTerm);                               // terminator
    r.dump.feed(Buffer.from([0xAA, 0xAA, 0xAA, 0x07])); // separator
    r.dump.feed(Buffer.from([0xAA, 0xAA, 0x07]));       // stuff
    const e = r.dump.stats().errors;
    assert.equal(e.crc, 1);
    assert.equal(e.newSignal, 1);
    assert.equal(e.tooLarge, 1);
    assert.equal(e.terminator, 1);
    assert.equal(e.separator, 1);
    assert.equal(e.stuff, 1);
    assert.equal(r.dump.stats().ok, 1);
    assert.equal(r.events.filter(x => x.type === "error").length, 6);
});

test("classifyError names every decoder message", () => {
    assert.equal(classifyError("packet discarded: new packet signal detected"), "newSignal");
    assert.equal(classifyError("discarded packet: invalid stuff byte 0x5"), "stuff");
    assert.equal(classifyError("discarded packet: expected separator byte, received 0x1"), "separator");
    assert.equal(classifyError("discarded packet: length exceeded limit (40 > 28)"), "tooLarge");
    assert.equal(classifyError("discarded packet: checksum mismatch (recv: 0x1 expected: 0x2)"), "crc");
    assert.equal(classifyError("discarded packet: missing terminator byte"), "terminator");
    assert.equal(classifyError("something else"), "other");
});

test("bytes can arrive one at a time and in odd chunks", () => {
    const r = rig();
    const all = Buffer.concat([
        W.encodePacket(0, W.PAD_CMD_HELLO, W.buildHello({ version: 1, keyCount: 6, encoderCount: 0 })),
        W.encodePacket(1, W.PAD_CMD_STATE, W.buildState({ keys: 0, encoders: [] })),
        W.encodePacket(2, W.PAD_CMD_STATE, W.buildState({ keys: 4, encoders: [] })),
    ]);
    for (let i = 0; i < all.length; i++) r.dump.feed(all.subarray(i, i + 1));
    assert.deepEqual(r.types(), ["hello", "baseline", "key"]);
    assert.equal(r.events[2].index, 3);
});

test("noise before and between packets does not hide them", () => {
    const r = rig();
    r.dump.feed(Buffer.from([1, 2, 3, 0xAA, 0, 0x55]));
    r.state(1, 0);
    r.dump.feed(Buffer.from([0xAA, 0xAA, 9, 9, 9]));
    r.state(2, 1);
    assert.deepEqual(r.events.filter(e => e.type !== "error").map(e => e.type), ["baseline", "key"]);
});

test("formatEvent gives readable lines", () => {
    assert.equal(formatEvent({ type: "key", t: 1234, index: 3, down: true }), "   1.234  key3 down");
    assert.equal(formatEvent({ type: "key", t: 1234, index: 3, down: false }), "   1.234  key3 up");
    assert.equal(formatEvent({ type: "turn", t: 0, encoder: 1, delta: -3, interval: 6500 }), "   0.000  enc1 turn -3 (interval 6.5 ms)");
    assert.equal(formatEvent({ type: "turn", t: 0, encoder: 2, delta: 1, interval: 65535 }), "   0.000  enc2 turn +1 (interval 65.5 ms, saturated)");
    assert.match(formatEvent({ type: "hello", t: 0, seq: 0, version: 1, keyCount: 6, encoderCount: 1 }), /HELLO version 1, 6 keys, 1 encoders/);
    assert.match(formatEvent({ type: "baseline", t: 0, keysDown: [] }), /baseline/);
    assert.match(formatEvent({ type: "baseline", t: 0, keysDown: [1, 3] }), /held: key1 key3/);
    assert.match(formatEvent({ type: "error", t: 0, name: "crc", message: "m" }), /error crc/);
    assert.match(formatEvent({ type: "offline", t: 0 }), /offline/);
});

test("formatStats lists counts", () => {
    const r = rig();
    r.state(1, 0);
    r.state(2, 0);
    const line = formatStats(r.dump.stats());
    assert.match(line, /ok 2/);
    assert.match(line, /repeats 1/);
    assert.match(line, /gaps 0/);
    assert.match(line, /errors none/);
});
