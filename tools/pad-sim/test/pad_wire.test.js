import { test } from "node:test";
import assert from "node:assert/strict";
import { packet_encode, packet_decoder } from "../../../PacketEncoder/packenc.js";
import * as W from "../src/pad_wire.js";

// Golden wire bytes: keep in step with pad/common/pad_golden.md (the C++ test asserts the same arrays)
const GOLDEN_STATE = [0xAA, 0xAA, 0xAA, 0x00, 0x05, 0x01, 0x0A, 0x05, 0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00, 0x64, 0x19, 0xB3, 0x86, 0x92, 0x41, 0x55];
const GOLDEN_HELLO = [0xAA, 0xAA, 0xAA, 0x00, 0x00, 0x02, 0x03, 0x01, 0x06, 0x01, 0xD5, 0x86, 0xDE, 0xD7, 0x55];

function decodeAll(bytes, maxlength = W.PAD_PAYLOAD_MAX_BYTES) {
    const packets = [];
    const errors = [];
    const rx = packet_decoder((seq, cmd, p) => packets.push({ seq, cmd, payload: Buffer.from(p) }), e => errors.push(e), maxlength);
    for (const b of bytes) rx(b);
    return { packets, errors };
}

test("constants match pad_protocol.h", () => {
    assert.equal(W.PAD_MAX_KEYS, 32);
    assert.equal(W.PAD_MAX_ENCODERS, 4);
    assert.equal(W.PAD_CMD_STATE, 1);
    assert.equal(W.PAD_CMD_HELLO, 2);
    assert.equal(W.PAD_PROTOCOL_VERSION, 1);
    assert.equal(W.PAD_STATE_REPEAT_MS, 100);
    assert.equal(W.PAD_OFFLINE_TIMEOUT_MS, 300);
    assert.equal(W.PAD_ENCODER_MIN_GAP_MS, 5);
    assert.equal(W.PAD_HELLO_COUNT, 3);
    assert.equal(W.PAD_HELLO_GAP_MS, 50);
    assert.equal(W.PAD_STATE_MAX_BYTES, 28);
    assert.equal(W.PAD_HELLO_BYTES, 3);
    assert.equal(W.PAD_PAYLOAD_MAX_BYTES, 28);
});

test("STATE payload sizes", () => {
    for (let n = 0; n <= 4; n++) {
        const st = { keys: 0, encoders: Array.from({ length: n }, () => ({ position: 0, interval: 0 })) };
        assert.equal(W.buildState(st).length, 4 + 6 * n);
        assert.equal(W.stateBytes(n), 4 + 6 * n);
    }
});

test("golden STATE packet", () => {
    const st = { keys: 5, encoders: [{ position: 0x1234, interval: 6500 }] };
    const pkt = W.encodePacket(5, W.PAD_CMD_STATE, W.buildState(st));
    assert.deepEqual([...pkt], GOLDEN_STATE);
    const { packets, errors } = decodeAll(GOLDEN_STATE);
    assert.equal(errors.length, 0);
    assert.equal(packets.length, 1);
    assert.equal(packets[0].seq, 5);
    assert.equal(packets[0].cmd, W.PAD_CMD_STATE);
    assert.deepEqual(W.parseState(packets[0].payload), st);
});

test("golden HELLO packet", () => {
    const h = { version: 1, keyCount: 6, encoderCount: 1 };
    const pkt = W.encodePacket(0, W.PAD_CMD_HELLO, W.buildHello(h));
    assert.deepEqual([...pkt], GOLDEN_HELLO);
    const { packets } = decodeAll(GOLDEN_HELLO);
    assert.equal(packets.length, 1);
    assert.equal(packets[0].cmd, W.PAD_CMD_HELLO);
    assert.deepEqual(W.parseHello(packets[0].payload), h);
});

test("STATE round trip for 0..4 encoders, including stuffing patterns", () => {
    for (let n = 0; n <= 4; n++) {
        const st = {
            keys: 0xAAAAAAAA,
            encoders: Array.from({ length: n }, (_, i) => ({ position: i % 2 ? 0xFFFFFFFF : 0xAAAAAAAA, interval: i % 2 ? 65535 : 0xAAAA })),
        };
        const pkt = W.encodePacket(0xAAAA, W.PAD_CMD_STATE, W.buildState(st));
        const { packets, errors } = decodeAll(pkt);
        assert.equal(errors.length, 0);
        assert.equal(packets.length, 1);
        assert.deepEqual(W.parseState(packets[0].payload), st);
    }
});

test("little endian layout", () => {
    const b = W.buildState({ keys: 0x01020304, encoders: [{ position: 0x0A0B0C0D, interval: 0x1122 }] });
    assert.deepEqual([...b], [4, 3, 2, 1, 0x0D, 0x0C, 0x0B, 0x0A, 0x22, 0x11]);
});

test("parseState rejects bad lengths", () => {
    for (const len of [0, 1, 3, 5, 9, 11, 15, 17, 21, 23, 27, 29, 34]) {
        assert.equal(W.parseState(Buffer.alloc(len)), null, `len ${len}`);
    }
    // 5 encoders
    assert.equal(W.parseState(Buffer.alloc(34)), null);
    for (const len of [4, 10, 16, 22, 28]) assert.notEqual(W.parseState(Buffer.alloc(len)), null);
});

test("buildState rejects out-of-range encoder counts", () => {
    const enc = { position: 0, interval: 0 };
    assert.throws(() => W.buildState({ keys: 0, encoders: [enc, enc, enc, enc, enc] }));
});

test("hello bad lengths", () => {
    for (const len of [0, 1, 2, 4, 6]) assert.equal(W.parseHello(Buffer.alloc(len)), null);
});

test("values are truncated to their wire widths", () => {
    const b = W.buildState({ keys: 0x1FFFFFFFF, encoders: [{ position: -1, interval: 70000 }] });
    const st = W.parseState(b);
    assert.equal(st.keys, 0xFFFFFFFF);
    assert.equal(st.encoders[0].position, 0xFFFFFFFF);
    assert.equal(st.encoders[0].interval, 65535);   // saturates, not wraps
});

test("decoder resyncs after noise and delivers later packets", () => {
    const stream = [0x12, 0xAA, 0x00, 0x55, 0xAA, 0xAA, 0x77, ...GOLDEN_HELLO.slice(0, 9), ...GOLDEN_STATE, 0x00, 0x01, ...GOLDEN_HELLO];
    const { packets } = decodeAll(stream);
    assert.deepEqual(packets.map(p => p.cmd), [W.PAD_CMD_STATE, W.PAD_CMD_HELLO]);
});

test("payload over the decoder buffer is rejected", () => {
    const pkt = W.encodePacket(1, W.PAD_CMD_STATE, Buffer.alloc(40));
    const { packets, errors } = decodeAll(pkt);
    assert.equal(packets.length, 0);
    assert.equal(errors.length, 1);
});

test("corrupt packets are never delivered (flip fuzz)", () => {
    let seed = 1234;
    const rnd = () => (seed = (seed * 1664525 + 1013904223) >>> 0) / 0x100000000;
    const st = { keys: 0xDEADBEEF, encoders: [{ position: 77, interval: 5000 }, { position: 9, interval: 100 }] };
    const good = W.encodePacket(7, W.PAD_CMD_STATE, W.buildState(st));
    for (let i = 0; i < 2000; i++) {
        const bad = Buffer.from(good);
        bad[Math.floor(rnd() * bad.length)] ^= 1 << Math.floor(rnd() * 8);
        const { packets } = decodeAll(bad);
        for (const p of packets) {
            assert.equal(p.seq, 7);
            assert.deepEqual(W.parseState(p.payload), st);
        }
    }
});

test("encodePacket matches packet_encode directly", () => {
    const out = [];
    packet_encode(b => out.push(b), 5, 1, Buffer.from([1, 2, 3]));
    assert.deepEqual([...W.encodePacket(5, 1, Buffer.from([1, 2, 3]))], out);
});
