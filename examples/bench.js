// Link benchmark: request latency, pipelined throughput and image upload
// speed. Takes control of the device (which clears its keys: uploads need
// it), uploads and frees its own images, and releases control at the end so
// the device shows its own Home again.
//
//   node examples/bench.js       (from the repo root)

import { ControlBlock, encodeImage, keyImage, keyPixelSize, svgLayer, buttonLayer, FACE_MARGIN } from "../host/index.js";
import { sinkSend } from "./bench-sink.mjs";

const cb = await ControlBlock.open();
await cb.takeControl();
const { width, height } = keyPixelSize({ w: 1, h: 1 }, cb.info);      // a 1U key's images

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

// Sink: full-size PUT_IMAGEs the device refuses and skips (bench-sink.mjs), pipelined like an
// upload: the USB pipe's one-way rate without the image path (PSRAM copy, check_image)
t = performance.now();
const sunk = await sinkSend(cb, 50);
secs = (performance.now() - t) / 1000;
console.log(`sink ${cb.info.maxPayload} B x 50: ${(sunk / 1024 / secs).toFixed(0)} KB/s (one way, nothing stored)`);

// Uploads: a noisy (poorly compressing) opaque 1U image with 251 colours, under many IDs
const rgba = new Uint8Array(width * height * 4);
for (let i = 0; i < rgba.length; i += 4) {
    const c = ((i / 4) * 37) % 251;
    rgba.set([c, 255 - c, (c * 3) & 0xFF, 255], i);
}
const image = await encodeImage(rgba, width, height);
const ids = Array.from({ length: 40 }, (_, i) => 0xBE000000 + i);
t = performance.now();
await Promise.all(ids.map(id => cb.putImage(id, image)));
secs = (performance.now() - t) / 1000;
console.log(`upload ${ids.length} x ${(image.length / 1024).toFixed(1)} KB: ${(image.length * ids.length / 1024 / secs).toFixed(0)} KB/s`);

// A page of realistic keys: 15 keys x 2 states (face, icon and label over black), all different
const fw = width - 2 * FACE_MARGIN, fh = height - 2 * FACE_MARGIN;
const face = async colour => svgLayer(`<svg xmlns="http://www.w3.org/2000/svg" width="${fw}" height="${fh}">
    <rect x="1" y="1" width="${fw - 2}" height="${fh - 2}" rx="14" fill="${colour}" stroke="#7090e0" stroke-width="2"/></svg>`, fw, fh);
const faces = [await face("#283c80"), await face("#c0c0d0")];
const keys = [];
for (let k = 0; k < 15; k++) {
    const icon = await buttonLayer({ backColor: "transparent", svgFile: "#star", svgForeColor: "#ffd700", svgTranslate: { y: -7 },
                                     text: `Key ${k}`, textColor: "white", textVAlign: "bottom", font: "bold 11px sans-serif" }, fw, fh);
    for (const f of faces)
        keys.push(await keyImage([f, icon], width, height));
}
const pageBytes = keys.reduce((n, k) => n + k.data.length, 0);
t = performance.now();
await Promise.all(keys.map(k => cb.putImage(k.id, k.data)));
secs = (performance.now() - t) / 1000;
console.log(`a page of keys: ${keys.length} images, ${(pageBytes / 1024).toFixed(1)} KB in ${(secs * 1000).toFixed(0)} ms ` +
            `(${(pageBytes / 1024 / secs).toFixed(0)} KB/s; ${(pageBytes / keys.length / 1024).toFixed(1)} KB per image)`);

const before = await cb.getInfo();
await cb.freeImages([...ids, ...keys.map(k => k.id)]);
await new Promise(r => setTimeout(r, 200));
const after = await cb.getInfo();
console.log(`image memory: ${(before.imageUsed / 1024).toFixed(0)} KB -> ${(after.imageUsed / 1024).toFixed(0)} KB after freeing`);

await cb.releaseControl();
await cb.close();
