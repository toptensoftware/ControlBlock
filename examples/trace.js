// USB transfer trace (wip/wip-upload-throughput.md, step 5): times every write and read for
// ping and sink (bench-sink.mjs) messages of the device's maximum payload, sent pipelined, one
// at a time, and raw with 1-3 writes in flight, to find where the ~32 ms per 8 KB upload message
// goes. Takes control of the device and releases it at the end.
//
//   node examples/trace.js [--json file]       (from the repo root)

import { writeFileSync } from "node:fs";
import { ControlBlock, Msg, encodeMessage, encodePutImage, PUT_IMAGE_PREFIX } from "../host/index.js";
import { sinkSend } from "./bench-sink.mjs";
import { traceDevice, summarize, sendRaw, gateReads } from "./bench-trace.mjs";

const COUNT = 50;
const jsonAt = process.argv.indexOf("--json");
const jsonFile = jsonAt > 0 ? process.argv[jsonAt + 1] : null;

const cb = await ControlBlock.open();
await cb.takeControl();
const max = cb.info.maxPayload;
const pingPayload = new Uint8Array(max).map((_, i) => i);
const sinkData = new Uint8Array(max - PUT_IMAGE_PREFIX).map((_, i) => i * 37);
const sinkMessage = encodePutImage(0, sinkData.length, 0, sinkData);

// Raw sink messages (their BAD_ARG replies match no request and are ignored), then a ping so
// the device has read them all before the next case. The records stop before that ping.
// `holdReads`: no new read is submitted while the writes go out (the one already pending ends
// at the first reply), so the IN endpoint isn't polled; the ~50 x 16 B replies wait in the
// device's 2 KB transmit FIFO until the gate opens.
const rawSink = (depth, holdReads = false) => async trace => {
    const messages = Array.from({ length: COUNT }, (_, i) => encodeMessage(Msg.PUT_IMAGE, 0x8000 + i, sinkMessage));
    if (holdReads)
        gate.close();
    try {
        await sendRaw(cb.device, messages, depth);
    } finally {
        gate.open();
    }
    const records = [...trace.records];
    await cb.ping();
    return records;
};

const cases = [
    ["ping pipelined",    () => Promise.all(Array.from({ length: COUNT }, () => cb.ping(pingPayload)))],
    ["ping one at a time", async () => { for (let i = 0; i < COUNT; i++) await cb.ping(pingPayload); }],
    ["sink pipelined",    () => sinkSend(cb, COUNT)],
    ["sink one at a time", async () => { for (let i = 0; i < COUNT; i++) await sinkSend(cb, 1); }],
    ["raw sink, 1 write in flight",  rawSink(1)],
    ["raw sink, 2 writes in flight", rawSink(2)],
    ["raw sink, 3 writes in flight", rawSink(3)],
    ["raw sink, reads held",         rawSink(1, true)],
];

const ms = v => v.toFixed(2);
const range = s => s ? `${ms(s.median)}/${ms(s.max)}` : "-";
const dump = {};
const trace = traceDevice(cb.device);
const gate = gateReads(cb.device);      // after the tracer: a read is timed from when it's really submitted
console.log(`${COUNT} messages of ${max + 12} B per case; writes: median/max ms`);
for (const [name, run] of cases) {
    trace.reset();
    const result = await run(trace);
    const records = Array.isArray(result) && result[0]?.dir ? result : [...trace.records];
    const s = summarize(records);
    dump[name] = records;
    console.log(`${name}: ${s.out.kbps.toFixed(0)} KB/s out | ${s.out.count} writes, each ${range(s.out.duration)} ms, ` +
                `gaps ${range(s.out.gap)} ms, write in flight ${(s.out.busy * 100).toFixed(0)}% | ` +
                `${s.in.count} reads, ${(s.in.bytes / 1024).toFixed(1)} KB`);
}
gate.stop();
trace.stop();

if (jsonFile) {
    writeFileSync(jsonFile, JSON.stringify(dump));
    console.log(`records written to ${jsonFile}`);
}
await cb.releaseControl();
await cb.close();
