// Checks the binpack behaviour docs/standalone.md relies on for key images:
// a Buffer object shared by several keys is stored once, 4-byte aligned, with
// every key pointing at the same copy; equal-content but distinct Buffer objects
// are NOT merged (so the bake step must hand out canonical Buffer objects).

import { test } from "node:test";
import assert from "node:assert/strict";
import { pack, unpack, registerType } from "../../../binpack/index.js";

registerType({
    name: "dedupeKey",
    fields: [
        { name: "normalBg", type: "length" },
        { name: "normalBg", type: "byte*", default: null },
        { name: "pressedBg", type: "length" },
        { name: "pressedBg", type: "byte*", default: null },
    ],
});

registerType({
    name: "dedupeRoot",
    fields: [
        { name: "keys", type: "length" },
        { name: "keys", type: "dedupeKey*" },
    ],
});

// Layout of the packed structs (32-bit pointers, 4-byte fields)
const KEY_SIZE = 16;
const readU32 = (bin, off) => bin.readUInt32LE(off);

// Root is at offset 0: [keys length][keys pointer]; key i is at keysPtr + i * KEY_SIZE
function keyFields(bin, i)
{
    const keysPtr = readU32(bin, 4);
    const k = keysPtr + i * KEY_SIZE;
    return {
        normalBgLen: readU32(bin, k),
        normalBgPtr: readU32(bin, k + 4),
        pressedBgLen: readU32(bin, k + 8),
        pressedBgPtr: readU32(bin, k + 12),
    };
}

// Odd-length, distinctive content so alignment padding and duplicates are detectable
const makeImage = (marker, len) => Buffer.concat([Buffer.from(marker), Buffer.alloc(len - marker.length, 0x5a)]);
const countOccurrences = (bin, marker) =>
{
    let n = 0, from = 0;
    while ((from = bin.indexOf(marker, from)) >= 0) { n++; from += marker.length; }
    return n;
};

test("a shared Buffer object is stored once and every key points at it", () => {
    const normal = makeImage("NORMALBG", 21);
    const pressed = makeImage("PRESSEDBG", 22);
    const keys = [];
    for (let i = 0; i < 15; i++)
        keys.push({ normalBg: normal, pressedBg: pressed });

    const { binary } = pack("dedupeRoot", { keys });

    assert.equal(countOccurrences(binary, "NORMALBG"), 1);
    assert.equal(countOccurrences(binary, "PRESSEDBG"), 1);

    const first = keyFields(binary, 0);
    for (let i = 1; i < 15; i++)
    {
        const k = keyFields(binary, i);
        assert.equal(k.normalBgPtr, first.normalBgPtr);
        assert.equal(k.pressedBgPtr, first.pressedBgPtr);
        assert.equal(k.normalBgLen, normal.length);
        assert.equal(k.pressedBgLen, pressed.length);
    }
});

test("distinct Buffer objects with equal content are not merged", () => {
    const keys = [];
    for (let i = 0; i < 3; i++)
        keys.push({ normalBg: makeImage("NORMALBG", 21), pressedBg: makeImage("PRESSEDBG", 22) });

    const { binary } = pack("dedupeRoot", { keys });

    assert.equal(countOccurrences(binary, "NORMALBG"), 3);
});

test("every distinct image starts on a 4-byte boundary", () => {
    // Odd lengths, so any missing padding shows
    const images = [makeImage("IMGA", 5), makeImage("IMGB", 7), makeImage("IMGC", 9), makeImage("IMGD", 13)];
    const keys = [
        { normalBg: images[0], pressedBg: images[1] },
        { normalBg: images[2], pressedBg: images[3] },
        { normalBg: images[0], pressedBg: images[3] },
    ];

    const { binary } = pack("dedupeRoot", { keys });

    for (let i = 0; i < keys.length; i++)
    {
        const k = keyFields(binary, i);
        assert.equal(k.normalBgPtr % 4, 0, `key ${i} normalBg pointer ${k.normalBgPtr}`);
        assert.equal(k.pressedBgPtr % 4, 0, `key ${i} pressedBg pointer ${k.pressedBgPtr}`);
    }
});

test("image data survives a round trip through the packed blob", () => {
    const normal = makeImage("NORMALBG", 21);
    const pressed = makeImage("PRESSEDBG", 22);
    const { binary } = pack("dedupeRoot", { keys: [
        { normalBg: normal, pressedBg: pressed },
        { normalBg: normal, pressedBg: pressed },
    ] });

    const root = unpack("dedupeRoot", binary);
    assert.equal(root.keys.length, 2);
    assert.deepEqual(Buffer.from(root.keys[0].normalBg), normal);
    assert.deepEqual(Buffer.from(root.keys[1].pressedBg), pressed);
});

test("an absent image packs as length 0 and a null pointer", () => {
    const normal = makeImage("NORMALBG", 21);
    const { binary } = pack("dedupeRoot", { keys: [
        { normalBg: normal },          // no pressedBg: uses the field default (null)
    ] });

    const k = keyFields(binary, 0);
    assert.equal(k.normalBgLen, normal.length);
    assert.equal(k.pressedBgLen, 0);
    assert.equal(k.pressedBgPtr, 0);
});

test("image offsets are 4-aligned, so they are 4-aligned on the device too", () => {
    // The device places the blob after a 32-byte header at a 4-aligned address, so
    // "offset % 4 == 0" in the blob is "address % 4 == 0" on the device.
    const { binary } = pack("dedupeRoot", { keys: [ { normalBg: makeImage("X", 5), pressedBg: makeImage("Y", 7) } ] });
    const k = keyFields(binary, 0);
    assert.equal(k.normalBgPtr % 4, 0);
    assert.equal(k.pressedBgPtr % 4, 0);
});

test("the blob length is a multiple of 4, so the relocation table after it is aligned", () => {
    // binpack pads the end of the blob (fixed in the submodule's finalize()); cli.js writes
    // the relocation table at HEADER_SIZE + binary.length.
    const { binary } = pack("dedupeRoot", { keys: [ { normalBg: makeImage("X", 5), pressedBg: makeImage("Y", 7) } ] });
    assert.equal(binary.length % 4, 0, `blob length ${binary.length}`);
});
