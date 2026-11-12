// The Node image helpers: one CRC-32, one rle565 encode call, layers rendered with
// node-buttonImage or from SVG, and key images composed over black (protocol v2: every key
// image is fully opaque and exactly the key's size).
import test from "node:test";
import assert from "node:assert/strict";
import zlib from "node:zlib";
import { readRleHeader, unpackRle } from "rle565/unpack.js";
import {
    imageId, encodeImage, renderButtonImage, buttonLayer, svgLayer, centreImage, composeOverBlack, clipLayer, keyImage, FACE_MARGIN,
} from "../src/images.js";

// A width x height RGBA layer of one colour
function solid(width, height, [r, g, b, a])
{
    const rgba = new Uint8Array(width * height * 4);
    for (let i = 0; i < rgba.length; i += 4)
        rgba.set([r, g, b, a], i);
    return { rgba, width, height };
}

const pixel = (rgba, width, x, y) => [...rgba.subarray((y * width + x) * 4, (y * width + x) * 4 + 4)];

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

test("renderButtonImage returns width x height RGBA (square by default); buttonLayer wraps it as a layer", { timeout: 60000 }, async () => {
    const def = { backColor: "transparent", svgFile: "#heart", svgForeColor: "red", text: "Hi", textColor: "white" };
    const rgba = await renderButtonImage(def, 64);
    assert.ok(rgba instanceof Uint8Array);
    assert.equal(rgba.length, 64 * 64 * 4);
    assert.equal((await renderButtonImage(def, 96, 48)).length, 96 * 48 * 4);

    const layer = await buttonLayer(def, 64);
    assert.deepEqual(layer, { rgba, width: 64, height: 64 });
});

test("svgLayer renders an SVG at the size given", { timeout: 60000 }, async () => {
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><rect x="0" y="0" width="5" height="10" fill="#ff0000"/></svg>`;
    const layer = await svgLayer(svg, 40, 20);
    assert.equal(layer.width, 40);
    assert.equal(layer.height, 20);
    assert.equal(layer.rgba.length, 40 * 20 * 4);
    assert.deepEqual(pixel(layer.rgba, 40, 2, 10).slice(0, 1), [255]);   // left half red
    assert.equal(pixel(layer.rgba, 40, 38, 10)[3], 0);                  // right half transparent
});

test("centreImage pads with transparency to any size", async () => {
    const { rgba } = solid(2, 2, [10, 20, 30, 255]);
    const out = await centreImage(rgba, 2, 2, 6, 4);
    assert.equal(out.length, 6 * 4 * 4);
    assert.deepEqual(pixel(out, 6, 2, 1), [10, 20, 30, 255]);
    assert.deepEqual(pixel(out, 6, 0, 0), [0, 0, 0, 0]);
    assert.equal((await centreImage(rgba, 2, 2, 4)).length, 4 * 4 * 4);   // square by default
    await assert.rejects(centreImage(rgba, 2, 2, 1, 4), /bigger/);
});

test("composeOverBlack: layers in order over black, centred unless placed; always opaque", async () => {
    const face = solid(4, 4, [255, 0, 0, 255]);
    const icon = { ...solid(2, 2, [0, 0, 255, 128]), x: 0, y: 0 };
    const out = await composeOverBlack([face, icon], 8, 6);
    assert.equal(out.length, 8 * 6 * 4);
    for (let i = 3; i < out.length; i += 4)
        assert.equal(out[i], 255, "opaque everywhere");
    assert.deepEqual(pixel(out, 8, 0, 5), [0, 0, 0, 255]);      // uncovered: black
    assert.deepEqual(pixel(out, 8, 2, 1), [255, 0, 0, 255]);    // the face, centred at (2, 1)
    const blend = pixel(out, 8, 1, 1);                          // the half-transparent icon over black
    assert.ok(blend[2] > 100 && blend[2] < 160 && blend[0] === 0, `blend ${blend}`);
    assert.deepEqual(pixel(out, 8, 0, 2), [0, 0, 0, 255]);      // the icon covers rows 0..1 only

    // No layers: all black. A layer that doesn't fit throws.
    assert.deepEqual(pixel(await composeOverBlack([], 2, 2), 2, 1, 1), [0, 0, 0, 255]);
    await assert.rejects(composeOverBlack([solid(9, 1, [1, 1, 1, 255])], 8, 6), /doesn't fit/);
    await assert.rejects(composeOverBlack([{ ...solid(2, 2, [1, 1, 1, 255]), x: 7, y: 0 }], 8, 6), /doesn't fit/);
});

test("keyImage is one opaque rle565 image at the key's size, with a stable content ID", async () => {
    const face = solid(128, 128, [40, 80, 160, 255]);
    const icon = solid(80, 80, [255, 128, 0, 200]);
    const a = await keyImage([face, icon], 144, 144);
    const b = await keyImage([face, icon], 144, 144);
    assert.equal(a.id, b.id);
    assert.notEqual(a.id, 0);
    assert.equal(a.id, imageId(a.data));
    assert.equal(a.width, 144);
    assert.equal(a.height, 144);

    const img = unpackRle(Buffer.from(a.data));
    assert.equal(img.width, 144);
    assert.equal(img.height, 144);
    for (let i = 3; i < img.data.length; i += 4)
        assert.equal(img.data[i], 255, "every pixel opaque, as the device requires");
    assert.deepEqual(pixel(img.data, 144, FACE_MARGIN - 1, 72), [0, 0, 0, 255]);     // the black margin round the face
    assert.notDeepEqual(pixel(img.data, 144, FACE_MARGIN, 72), [0, 0, 0, 255]);

    // A span: any size; a single layer needn't be in an array
    const wide = await keyImage(face, 288, 144);
    assert.equal(readRleHeader(Buffer.from(wide.data)).width, 288);
    assert.notEqual(wide.id, a.id);
});

test("FACE_MARGIN: a 128 face centred in a 144 cell (the bake's FACE_SIZE and KEY_CELL)", () => {
    assert.equal(144 - 2 * FACE_MARGIN, 128);
});

test("clipLayer: the part of a layer inside a width x height image (null if none), so a group's picture can be split", async () => {
    // a 4x2 layer, each pixel's red = its index
    const rgba = new Uint8Array(4 * 2 * 4);
    for (let i = 0; i < 8; i++)
        rgba.set([i, 0, 0, 255], i * 4);
    const layer = { rgba, width: 4, height: 2 };

    assert.strictEqual(clipLayer({ ...layer, x: 0, y: 0 }, 4, 2).rgba, rgba);       // fits: unchanged
    const right = clipLayer({ ...layer, x: -2, y: 0 }, 2, 2);                       // the right half
    assert.deepEqual([right.x, right.y, right.width, right.height], [0, 0, 2, 2]);
    assert.deepEqual([0, 1, 2, 3].map(i => right.rgba[i * 4]), [2, 3, 6, 7]);
    const corner = clipLayer({ ...layer, x: 1, y: 1 }, 3, 3);                       // off the right and bottom
    assert.deepEqual([corner.x, corner.y, corner.width, corner.height], [1, 1, 2, 2]);
    assert.deepEqual([0, 1, 2, 3].map(i => corner.rgba[i * 4]), [0, 1, 4, 5]);
    assert.equal(clipLayer({ ...layer, x: 5, y: 0 }, 4, 2), null);                  // nothing inside
    assert.equal(clipLayer({ ...layer, x: 0, y: -2 }, 4, 2), null);
});
