// The Node image helpers: one CRC-32, one rle565 encode call and one key-image render
// (shared by the bake's renderer and the demos).
import test from "node:test";
import assert from "node:assert/strict";
import zlib from "node:zlib";
import { readRleHeader } from "rle565/unpack.js";
import { imageId, encodeImage, renderButtonImage, buttonKeyImage } from "../src/images.js";

test("imageId is the zlib CRC-32 of the data", () => {
    const data = new Uint8Array(1000).map((_, i) => (i * 2654435761) >>> 24);
    assert.equal(imageId(data), zlib.crc32(data) >>> 0);
    assert.notEqual(imageId(new Uint8Array(0)), 0);     // CRC 0 maps to 1 (0 means none)
});

test("encodeImage makes rle565 with the right size", async () => {
    const rgba = new Uint8Array(20 * 10 * 4).fill(200);
    const data = await encodeImage(rgba, 20, 10);
    assert.ok(data instanceof Uint8Array);
    assert.equal(Buffer.from(data).readUInt32LE(0), 0x35363552);    // "R565"
    const h = readRleHeader(Buffer.from(data));
    assert.equal(h.width, 20);
    assert.equal(h.height, 10);
});

test("renderButtonImage returns size x size RGBA; buttonKeyImage encodes it with its ID", { timeout: 60000 }, async () => {
    const def = { backColor: "transparent", svgFile: "#heart", svgForeColor: "red", text: "Hi", textColor: "white" };
    const rgba = await renderButtonImage(def, 64);
    assert.ok(rgba instanceof Uint8Array);
    assert.equal(rgba.length, 64 * 64 * 4);

    const key = await buttonKeyImage(def, 64);
    assert.deepEqual(key.data, await encodeImage(rgba, 64, 64));
    assert.equal(key.id, imageId(key.data));
    assert.equal(readRleHeader(Buffer.from(key.data)).width, 64);
});
