// bench-trace.mjs: the transfer tracer, its summary and the raw sender with several writes in flight.
// From the repo root:  node --test "examples/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { ControlBlock } from "../host/index.js";
import { FakeDevice } from "../host/test/FakeDevice.js";
import { traceDevice, summarize, sendRaw, gateReads } from "./bench-trace.mjs";

test("summarize: rate, durations, gaps between writes and busy share", () => {
    // Three 1000 B writes: 0-2, 3-5, 9-11 ms; two reads
    const records = [
        { dir: "out", start: 0, end: 2, bytes: 1000 },
        { dir: "in",  start: 0, end: 4, bytes: 16 },
        { dir: "out", start: 3, end: 5, bytes: 1000 },
        { dir: "out", start: 9, end: 11, bytes: 1000 },
        { dir: "in",  start: 4, end: 10, bytes: 32 },
    ];
    const s = summarize(records);
    assert.equal(s.out.count, 3);
    assert.equal(s.out.bytes, 3000);
    assert.equal(s.out.spanMs, 11);
    assert.equal(s.out.kbps, 3000 / 1024 / 0.011);
    assert.deepEqual(s.out.duration, { min: 2, median: 2, max: 2 });
    assert.deepEqual(s.out.gap, { min: 1, median: 2.5, max: 4 });     // 3-2, 9-5
    assert.equal(s.out.busy, 6 / 11);
    assert.equal(s.in.count, 2);
    assert.equal(s.in.bytes, 48);
});

test("summarize: overlapping writes (several in flight) count as no gap, busy capped by the span", () => {
    const s = summarize([
        { dir: "out", start: 0, end: 4, bytes: 10 },
        { dir: "out", start: 1, end: 6, bytes: 10 },
    ]);
    assert.deepEqual(s.out.gap, { min: 0, median: 0, max: 0 });
    assert.equal(s.out.busy, 1);
});

test("summarize: nothing recorded", () => {
    const s = summarize([]);
    assert.equal(s.out.count, 0);
    assert.equal(s.out.kbps, 0);
    assert.equal(s.out.duration, null);
    assert.equal(s.out.gap, null);
});

test("traceDevice records the library's writes and reads, and stop() puts the device back", async () => {
    const dev = new FakeDevice({ hosted: true });
    const original = dev.transferOut;
    const cb = new ControlBlock(dev, { timeoutMs: 300 });
    await cb.connect();
    try {
        const trace = traceDevice(dev);
        await cb.ping(new Uint8Array(100));     // its reply comes on the read pending from before
        await cb.ping(new Uint8Array(100));     // this one's on a traced read
        const outs = trace.records.filter(r => r.dir === "out");
        assert.equal(outs.length, 2);
        assert.equal(outs[0].bytes, 112);       // header + payload
        assert.ok(outs[0].end >= outs[0].start);
        assert.ok(trace.records.some(r => r.dir === "in" && r.bytes >= 112));
        trace.reset();
        assert.equal(trace.records.length, 0);
        trace.stop();
        assert.equal(dev.transferOut, original);
    } finally {
        await cb.close();
    }
});

test("gateReads holds new reads until opened; a read already pending isn't held", async () => {
    const calls = [];
    const device = { transferIn: async (ep, n) => { calls.push(n); return { status: "ok" }; } };
    const gate = gateReads(device);
    await device.transferIn(1, 10);                 // open by default
    gate.close();
    let done = false;
    const held = device.transferIn(1, 20).then(() => { done = true; });
    await new Promise(r => setImmediate(r));
    assert.deepEqual(calls, [10]);
    assert.equal(done, false);
    assert.equal(gate.held(), 1);
    gate.open();
    await held;
    assert.deepEqual(calls, [10, 20]);
    gate.stop();
    assert.equal(Object.prototype.hasOwnProperty.call(device, "transferIn"), true);    // its own method put back
});

// A device whose writes complete when the test says, recording how many were in flight
function slowDevice()
{
    const d = { sent: [], inFlight: 0, maxInFlight: 0, waiting: [] };
    d.transferOut = (ep, bytes) => {
        d.sent.push(bytes.length);
        d.inFlight++;
        d.maxInFlight = Math.max(d.maxInFlight, d.inFlight);
        return new Promise(resolve => d.waiting.push(() => { d.inFlight--; resolve({ status: "ok", bytesWritten: bytes.length }); }));
    };
    d.finishAll = async () => {
        while (d.waiting.length || d.inFlight) {
            const w = d.waiting.splice(0);
            w.forEach(f => f());
            await new Promise(r => setImmediate(r));
        }
    };
    return d;
}

test("sendRaw keeps at most `depth` writes in flight, in order", async () => {
    const d = slowDevice();
    const messages = [10, 11, 12, 13, 14].map(n => new Uint8Array(n));
    const done = sendRaw(d, messages, 3);
    await new Promise(r => setImmediate(r));
    assert.equal(d.inFlight, 3);
    await d.finishAll();
    await done;
    assert.deepEqual(d.sent, [10, 11, 12, 13, 14]);
    assert.equal(d.maxInFlight, 3);
});

test("sendRaw at depth 1 ends a whole number of packets with a ZLP", async () => {
    const d = slowDevice();
    const done = sendRaw(d, [new Uint8Array(128), new Uint8Array(5)], 1);
    await d.finishAll();
    await done;
    assert.deepEqual(d.sent, [128, 0, 5]);
});

test("sendRaw refuses a message that needs a ZLP when several writes are in flight", async () => {
    await assert.rejects(sendRaw(slowDevice(), [new Uint8Array(64)], 2), /ZLP/);
});

test("sendRaw fails on a write that doesn't complete ok", async () => {
    const d = { transferOut: async () => ({ status: "stall" }) };
    await assert.rejects(sendRaw(d, [new Uint8Array(5)], 2), /stall/);
});
