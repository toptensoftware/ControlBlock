// Transfer tracing for examples/trace.js (wip/wip-upload-throughput.md, step 5): times every USB
// write and read the library makes, summarises them, and sends raw messages with several writes
// in flight (the library keeps one at a time).

import { ENDPOINT, USB_PACKET_SIZE } from "../host/index.js";

// Wrap `device`'s transferOut and transferIn to record { dir, start, end, bytes } (ms, from
// performance.now()) for each one that completes. A read already pending when tracing starts
// isn't recorded. stop() puts the device's own methods back.
export function traceDevice(device)
{
    const records = [];
    const timed = (dir, bytesOf) => original => async (...args) => {
        const start = performance.now();
        const r = await original(...args);
        records.push({ dir, start, end: performance.now(), bytes: bytesOf(args, r) });
        return r;
    };
    const restore = [
        patch(device, "transferOut", timed("out", args => args[1].length)),
        patch(device, "transferIn", timed("in", (args, r) => r?.data?.byteLength ?? 0)),
    ];
    return {
        records,
        reset() { records.length = 0; },
        stop() { restore.forEach(r => r()); },
    };
}

// Hold new transferIn calls while closed (open at first), so no read is pending on the bus: a
// read already submitted runs on. open() submits the held ones. stop() puts the method back.
export function gateReads(device)
{
    let waiters = null;         // null: open
    const restore = patch(device, "transferIn", original => async (...args) => {
        if (waiters)
            await new Promise(resolve => waiters.push(resolve));
        return original(...args);
    });
    return {
        close() { waiters ??= []; },
        open() { const w = waiters ?? []; waiters = null; w.forEach(r => r()); },
        held() { return waiters?.length ?? 0; },
        stop() { this.open(); restore(); },
    };
}

// Replace device[name] by make(original) (the original bound to the device); returns a function
// that puts back what was there (an own property, or the prototype's method)
function patch(device, name, make)
{
    const own = Object.prototype.hasOwnProperty.call(device, name);
    const original = device[name];
    device[name] = make(original.bind(device));
    return () => {
        if (own) device[name] = original;
        else delete device[name];
    };
}

function stats(values)
{
    if (!values.length)
        return null;
    const v = [...values].sort((a, b) => a - b);
    const mid = v.length >> 1;
    return { min: v[0], median: v.length % 2 ? v[mid] : (v[mid - 1] + v[mid]) / 2, max: v[v.length - 1] };
}

// Writes: count, bytes, span (first start to last end), KB/s over the span, durations, the gap
// from one write's end to the next one's start (0 when they overlap) and the share of the span
// with a write in flight. Reads: count and bytes.
export function summarize(records)
{
    const outs = records.filter(r => r.dir === "out").sort((a, b) => a.start - b.start);
    const ins  = records.filter(r => r.dir === "in");
    const bytes = outs.reduce((n, r) => n + r.bytes, 0);
    const spanMs = outs.length ? Math.max(...outs.map(r => r.end)) - outs[0].start : 0;

    const gaps = [];
    let busy = 0, coveredTo = -Infinity;
    for (let i = 0; i < outs.length; i++) {
        const r = outs[i];
        if (i > 0)
            gaps.push(Math.max(0, r.start - coveredTo));
        busy += Math.max(0, r.end - Math.max(r.start, coveredTo));
        coveredTo = Math.max(coveredTo, r.end);
    }
    return {
        out: {
            count: outs.length, bytes, spanMs,
            kbps: spanMs > 0 ? bytes / 1024 / (spanMs / 1000) : 0,
            duration: stats(outs.map(r => r.end - r.start)),
            gap: stats(gaps),
            busy: spanMs > 0 ? Math.min(1, busy / spanMs) : 0,
        },
        in: { count: ins.length, bytes: ins.reduce((n, r) => n + r.bytes, 0) },
    };
}

// Send `messages` (encoded, whole) with up to `depth` transferOuts in flight, in order. A message
// of a whole number of packets needs a ZLP straight after it, which only depth 1 can promise.
export async function sendRaw(device, messages, depth)
{
    if (depth > 1 && messages.some(m => m.length % USB_PACKET_SIZE === 0))
        throw new Error("sendRaw: a message of a whole number of packets needs a ZLP; use depth 1");
    const out = async bytes => {
        const r = await device.transferOut(ENDPOINT, bytes);
        if (r.status !== "ok")
            throw new Error(`transferOut: ${r.status}`);
    };
    let next = 0;
    const lane = async () => {
        while (next < messages.length) {
            const m = messages[next++];
            await out(m);
            if (m.length % USB_PACKET_SIZE === 0)
                await out(new Uint8Array(0));
        }
    };
    await Promise.all(Array.from({ length: Math.min(depth, messages.length) }, lane));
}
