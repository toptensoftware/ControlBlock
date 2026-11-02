import test from "node:test";
import assert from "node:assert/strict";
import {
    encodeMessage, MessageParser, encodeKeys, encodeIds, encodePutImage, decodeInfo, decodeQuery, decodeEvent,
    Msg, Flags, KeyFlags, HEADER_SIZE, KEYDEF_SIZE, MAGIC, statusName, Status,
    encodeStatusText, StatusText, STATUS_TEXT_MAX_BYTES,
} from "../src/protocol.js";
import { infoPayload } from "./FakeDevice.js";

test("a message survives being split anywhere", () => {
    const a = encodeMessage(Msg.PING, 7, Uint8Array.of(1, 2, 3));
    const b = encodeMessage(Msg.RESET, 8);
    const stream = new Uint8Array([...a, ...b]);
    for (const piece of [1, 2, 5, 13, 100]) {
        const p = new MessageParser();
        const msgs = [];
        for (let i = 0; i < stream.length; i += piece)
            msgs.push(...p.push(stream.subarray(i, i + piece)));
        assert.equal(msgs.length, 2);
        assert.deepEqual([...msgs[0].payload], [1, 2, 3]);
        assert.equal(msgs[0].seq, 7);
        assert.equal(msgs[1].type, Msg.RESET);
        assert.equal(msgs[1].payload.length, 0);
    }
});

test("the header is little endian with the CB magic", () => {
    const m = encodeMessage(0x21, 0x1234, new Uint8Array(5), Flags.RESPONSE);
    assert.equal(m.length, HEADER_SIZE + 5);
    assert.deepEqual([...m.subarray(0, HEADER_SIZE)], [0x43, 0x42, 0x21, 0x01, 0x34, 0x12, 0, 0, 5, 0, 0, 0]);
    assert.equal(m[0] | m[1] << 8, MAGIC);
});

test("the parser skips garbage to the next message", () => {
    const p = new MessageParser();
    const good = encodeMessage(Msg.PING, 1, Uint8Array.of(9));
    const msgs = p.push(new Uint8Array([0xFF, 0x00, 0x13, ...good]));
    assert.equal(msgs.length, 1);
    assert.equal(msgs[0].payload[0], 9);
    assert.equal(p.resyncs, 1);
});

test("key definitions match the firmware's KeyDef", () => {
    const b = encodeKeys([
        { page: 2, key: 14, bg: 0x11223344, fg: 5, pressedBg: 6 },
        { page: 0, key: 1, bg: 9, empty: true },
        { page: 1, key: 3, fg: 7, disabled: true },
    ]);
    assert.equal(b.length, 2 + 3 * KEYDEF_SIZE);
    const v = new DataView(b.buffer, b.byteOffset);
    assert.equal(v.getUint16(0, true), 3);
    assert.equal(v.getUint8(2), 2);
    assert.equal(v.getUint8(3), 14);
    assert.equal(v.getUint16(4, true), 0);
    assert.equal(v.getUint32(6, true), 0x11223344);
    assert.equal(v.getUint32(10, true), 5);
    assert.equal(v.getUint32(14, true), 6);
    assert.equal(v.getUint32(18, true), 0);
    assert.equal(v.getUint16(2 + KEYDEF_SIZE + 2, true), KeyFlags.EMPTY);
    assert.equal(v.getUint32(2 + KEYDEF_SIZE + 4, true), 0);      // empty: IDs zeroed
    assert.equal(v.getUint16(2 + 2 * KEYDEF_SIZE + 2, true), KeyFlags.DISABLED);
});

test("ids and image chunks", () => {
    const ids = encodeIds([1, 0xFFFFFFFF]);
    assert.deepEqual([...ids], [2, 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF]);
    const put = encodePutImage(3, 100, 40, Uint8Array.of(7, 8));
    assert.deepEqual([...put], [3, 0, 0, 0, 100, 0, 0, 0, 40, 0, 0, 0, 7, 8]);
});

test("status text is a field, a length and UTF-8", () => {
    assert.deepEqual([...encodeStatusText(StatusText.LEFT, "Hi")], [0, 2, 72, 105]);
    assert.deepEqual([...encodeStatusText(StatusText.RIGHT, "")], [1, 0]);
    assert.deepEqual([...encodeStatusText(StatusText.LEFT, "é")], [0, 2, 0xC3, 0xA9]);       // bytes, not characters
    assert.equal(encodeStatusText(StatusText.RIGHT, "x".repeat(STATUS_TEXT_MAX_BYTES)).length, 2 + STATUS_TEXT_MAX_BYTES);
    assert.throws(() => encodeStatusText(StatusText.LEFT, "x".repeat(STATUS_TEXT_MAX_BYTES + 1)), RangeError);
    assert.throws(() => encodeStatusText(StatusText.LEFT, "é".repeat(24)), RangeError);       // 48 bytes
    assert.throws(() => encodeStatusText(StatusText.LEFT, "a\0b"), TypeError);
    assert.throws(() => encodeStatusText(StatusText.LEFT, 12), TypeError);
});

test("get info decodes every field", () => {
    const info = decodeInfo(infoPayload({ maxPayload: 4096, pageCount: 3 }));
    assert.equal(info.protocolVersion, 1);
    assert.equal(info.maxPayload, 4096);
    assert.equal(info.imageCount, 3);
    assert.equal(info.screenWidth, 800);
    assert.equal(info.cols, 5);
    assert.equal(info.rows, 3);
    assert.equal(info.pageCount, 3);
    assert.equal(info.keyWidth, 128);
    assert.equal(info.iconWidth, 80);
    assert.equal(info.firmwareVersion, "1.0");
    assert.equal(info.serial, "SERIAL1");
});

test("query results are a bitmap", () => {
    assert.deepEqual(decodeQuery(Uint8Array.of(10, 0, 0b10000101, 0b10)),
                     [true, false, true, false, false, false, false, true, false, true]);
});

test("events", () => {
    const key = decodeEvent({ type: Msg.KEY, seq: 4, payload: Uint8Array.of(1, 7, 2, 1, 0x10, 0x27, 0, 0) });
    assert.deepEqual(key, { type: "key", seq: 4, page: 1, key: 7, action: 2, lost: true, timeMs: 10000 });
    const page = decodeEvent({ type: Msg.PAGE_CHANGED, seq: 5, payload: Uint8Array.of(3, 1) });
    assert.deepEqual(page, { type: "pageChanged", seq: 5, page: 3, cause: 1 });
    assert.equal(decodeEvent({ type: 0x99, seq: 0, payload: new Uint8Array(0) }), null);
});

test("status names", () => {
    assert.equal(statusName(Status.NOT_FOUND), "NOT_FOUND");
    assert.equal(statusName(77), "STATUS_77");
});
