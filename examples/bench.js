// Link benchmark: request latency, pipelined throughput and image upload
// speed. Takes control of the device (which clears its layout: uploads need
// it), uploads and frees its own images, and releases control at the end so
// the device shows its own Home again.
//
//   node examples/bench.js       (from the repo root)

import { ControlBlock, encodeImage } from "../host/index.js";

const cb = await ControlBlock.open();
await cb.takeControl();
const size = cb.info.keyWidth;

// Latency: one request at a time
let t = performance.now();
const N = 200;
for (let i = 0; i < N; i++)
    await cb.ping();
console.log(`ping: ${((performance.now() - t) / N).toFixed(2)} ms round trip`);

// Throughput: pipelined pings with payload
const payload = new Uint8Array(cb.info.maxPayload).map((_, i) => i);
t = performance.now();
await Promise.all(Array.from({ length: 50 }, () => cb.ping(payload)));
let secs = (performance.now() - t) / 1000;
console.log(`ping ${payload.length} B x 50: ${(payload.length * 50 * 2 / 1024 / secs).toFixed(0)} KB/s (both ways)`);

// Uploads: a noisy (poorly compressing) image with 251 colours, under many IDs
const rgba = new Uint8Array(size * size * 4);
for (let i = 0; i < rgba.length; i += 4) {
    const c = ((i / 4) * 37) % 251;
    rgba.set([c, 255 - c, (c * 3) & 0xFF, 255], i);
}
const image = await encodeImage(rgba, size, size);
const ids = Array.from({ length: 40 }, (_, i) => 0xBE000000 + i);
t = performance.now();
await Promise.all(ids.map(id => cb.putImage(id, image)));
secs = (performance.now() - t) / 1000;
console.log(`upload ${ids.length} x ${(image.length / 1024).toFixed(1)} KB: ${(image.length * ids.length / 1024 / secs).toFixed(0)} KB/s`);

const before = await cb.getInfo();
await cb.freeImages(ids);
await new Promise(r => setTimeout(r, 200));
const after = await cb.getInfo();
console.log(`image memory: ${(before.imageUsed / 1024).toFixed(0)} KB -> ${(after.imageUsed / 1024).toFixed(0)} KB after freeing`);

await cb.releaseControl();
await cb.close();
