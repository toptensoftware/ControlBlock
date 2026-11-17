// The render lab's path format (wip/wip-on-device-render-feasibility.md, step 5):
// what the bake would reduce a single-colour icon or a glyph outline to for the
// device's path rasteriser, and the test vector file the firmware's host tests
// read (firmware/test/data/path_vectors.bin).
//
// A path is non-zero filled, in pixels of its box (y down), absolute commands only:
//
//   u16 cmdCount, u16 pointCount
//   u8  cmds[cmdCount]          CMD.*: M 1 point, L 1, Q 2, C 3, Z 0
//   pad to 2 bytes
//   i16 points[pointCount][2]   x, y in 1/POINT_SCALE px (range about +-2048 px)
//
// little endian. Fixed point rather than float32: half the size, integers on the
// device until flattening, 1/16 px is far below what anti-aliasing can show.
//
// Vector file: "PVEC", u16 version 1, u16 count, then per entry (each part
// padded to 4 bytes): u16 w, u16 h, u16 nameLen, i16 x, u32 pathSize, name,
// path, mask (w * h coverage bytes, 0..255: the JS reference rasteriser's
// output for the stored, quantised path). `x` places a glyph's box in its
// run (glyph-<run>-<i> entries); 0 for everything else.

import { rasterizeGlyph } from "rle565/font.js";

export const CMD = { M: 0, L: 1, Q: 2, C: 3, Z: 4 };
export const CMD_POINTS = [1, 1, 2, 3, 0];
export const POINT_SCALE = 16;
export const VECTORS_MAGIC = 0x43455650;    // "PVEC" little endian
export const VECTORS_VERSION = 1;

// Parse an SVG path of absolute M/L/Q/C/Z (what Skia's toSVGString writes)
// into [{ cmd, pts: [x, y, ...] }]
export function parseSvgPath(d)
{
    const tokens = d.match(/[a-zA-Z]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?/g) || [];
    const out = [];
    let i = 0;
    while (i < tokens.length)
    {
        const letter = tokens[i++];
        if (!(letter in CMD))
            throw new Error(`unsupported path command '${letter}'`);
        let cmd = CMD[letter];
        const n = CMD_POINTS[cmd] * 2;
        // A letter may be followed by several point groups: repeats (after M, lines)
        do
        {
            const pts = [];
            for (let j = 0; j < n; j++)
            {
                const v = Number(tokens[i++]);
                if (!Number.isFinite(v))
                    throw new Error(`bad number in path near command ${out.length}`);
                pts.push(v);
            }
            out.push({ cmd, pts });
            if (cmd == CMD.M)
                cmd = CMD.L;
        }
        while (n > 0 && i < tokens.length && !/[a-zA-Z]/.test(tokens[i]));
    }
    return out;
}

export function encodePath(cmds)
{
    const pts = cmds.flatMap(c => c.pts);
    const cmdBytes = cmds.length + (cmds.length & 1);
    const buf = Buffer.alloc(4 + cmdBytes + pts.length * 2);
    buf.writeUInt16LE(cmds.length, 0);
    buf.writeUInt16LE(pts.length / 2, 2);
    cmds.forEach((c, i) => buf.writeUInt8(c.cmd, 4 + i));
    pts.forEach((v, i) => {
        const q = Math.round(v * POINT_SCALE);
        if (q < -32768 || q > 32767)
            throw new Error(`point ${v} out of the int16 range`);
        buf.writeInt16LE(q, 4 + cmdBytes + i * 2);
    });
    return buf;
}

export function decodePath(buf)
{
    const count = buf.readUInt16LE(0);
    const points = buf.readUInt16LE(2);
    let p = 4 + count + (count & 1);
    const out = [];
    for (let i = 0; i < count; i++)
    {
        const cmd = buf.readUInt8(4 + i);
        if (cmd >= CMD_POINTS.length)
            throw new Error(`bad command ${cmd}`);
        const pts = [];
        for (let j = 0; j < CMD_POINTS[cmd] * 2; j++, p += 2)
            pts.push(buf.readInt16LE(p) / POINT_SCALE);
        out.push({ cmd, pts });
    }
    if (p != 4 + count + (count & 1) + points * 4 || p != buf.length)
        throw new Error("point count doesn't match the commands");
    return out;
}

// The reference coverage mask (w x h) of a path: rle565's font rasteriser
// (exact signed area, non-zero), whose glyph paths are y up
const FONTKIT = ["moveTo", "lineTo", "quadraticCurveTo", "bezierCurveTo", "closePath"];
export function rasterizePath(cmds, w, h)
{
    const glyph = cmds.map(c => ({ command: FONTKIT[c.cmd], args: c.pts.map((v, i) => (i & 1) ? -v : v) }));
    const mask = new Uint8Array(w * h);
    const r = rasterizeGlyph(glyph, 1, 0);
    if (!r)
        return mask;
    for (let y = 0; y < r.height; y++)
    {
        const my = r.top + y;
        if (my < 0 || my >= h)
            continue;
        for (let x = 0; x < r.width; x++)
        {
            const mx = r.left + x;
            if (mx >= 0 && mx < w)
                mask[my * w + mx] = r.bitmap[y * r.width + x];
        }
    }
    return mask;
}

const pad4 = (n) => (n + 3) & ~3;

export function writeVectors(entries)
{
    const parts = [];
    const head = Buffer.alloc(8);
    head.writeUInt32LE(VECTORS_MAGIC, 0);
    head.writeUInt16LE(VECTORS_VERSION, 4);
    head.writeUInt16LE(entries.length, 6);
    parts.push(head);
    const padded = (b) => Buffer.concat([b, Buffer.alloc(pad4(b.length) - b.length)]);
    for (const e of entries)
    {
        const name = Buffer.from(e.name, "utf8");
        const h = Buffer.alloc(12);
        h.writeUInt16LE(e.w, 0);
        h.writeUInt16LE(e.h, 2);
        h.writeUInt16LE(name.length, 4);
        h.writeInt16LE(e.x ?? 0, 6);
        h.writeUInt32LE(e.path.length, 8);
        parts.push(h, padded(name), padded(Buffer.from(e.path)), padded(Buffer.from(e.mask)));
    }
    return Buffer.concat(parts);
}

export function readVectors(buf)
{
    if (buf.readUInt32LE(0) != VECTORS_MAGIC || buf.readUInt16LE(4) != VECTORS_VERSION)
        throw new Error("not a path vector file");
    const count = buf.readUInt16LE(6);
    const out = [];
    let p = 8;
    for (let i = 0; i < count; i++)
    {
        const w = buf.readUInt16LE(p), h = buf.readUInt16LE(p + 2);
        const nameLen = buf.readUInt16LE(p + 4), x = buf.readInt16LE(p + 6), pathSize = buf.readUInt32LE(p + 8);
        p += 12;
        const name = buf.toString("utf8", p, p + nameLen);
        p += pad4(nameLen);
        const path = buf.subarray(p, p + pathSize);
        p += pad4(pathSize);
        const mask = buf.subarray(p, p + w * h);
        p += pad4(w * h);
        out.push({ name, x, w, h, path, mask });
    }
    if (p != buf.length)
        throw new Error("trailing bytes in the vector file");
    return out;
}
