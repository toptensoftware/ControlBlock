// Writes firmware/test/data/path_vectors.bin: paths in the render lab's path
// format (path-format.mjs) with the JS reference rasteriser's mask for each,
// for the firmware's C path rasteriser tests (TestPathRaster.cpp).
// wip/wip-on-device-render-feasibility.md, step 5. Run from the repo root:
//
//   node tools/render-lab/gen-path-vectors.mjs
//
// Contents: button-face's built-in icons that are plain single-colour path
// fills, reduced the way the bake would (Skia Path2D: asWinding, simplify,
// fitted to an ICON px box) to absolute M/L/Q/C/Z; a few hand-made shapes;
// and some Inter SemiBold glyph runs straight from their outlines.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Path2D } from "@napi-rs/canvas";
import * as fontkit from "fontkit";
import { icons } from "@toptensoftware/button-face";
import { CMD, parseSvgPath, encodePath, decodePath, rasterizePath, writeVectors } from "./path-format.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const OUT = path.resolve(here, "../../firmware/test/data/path_vectors.bin");
const FONT = path.resolve(here, "../test-icons/fonts/Inter-SemiBold.ttf");
const ICON = 80;                    // icon box (KEY_ICON_SIZE)
const GLYPH_PX = 27;                // the larger default label size
const GLYPH_RUNS = ["Vol+", "55%", "Ag"];
const PER_GLYPH = ["55%"];          // also one entry a glyph (glyph-<run>-<i>), for a path layer a glyph

const entries = [];
const skipped = [];

function add(name, w, h, cmds, x = 0)
{
    const bin = encodePath(cmds);
    // The reference is made from the stored (quantised) path, so the device
    // test compares rasterisers, not rounding
    entries.push({ name, x, w, h, path: bin, mask: rasterizePath(decodePath(bin), w, h) });
}

// --- Built-in icons ----------------------------------------------------------

// Why an icon can't be a plain path fill, or null if it can
function unsupported(svg)
{
    const body = svg.replace(/<\?xml[^>]*>/, "").replace(/<sodipodi:namedview[\s\S]*?\/>/, "").replace(/<defs[^>]*\/>/, "");
    const elems = new Set([...body.matchAll(/<([a-zA-Z:]+)[\s>/]/g)].map(m => m[1]));
    const others = [...elems].filter(e => e != "svg" && e != "path");
    const why = [];
    if (others.length) why.push("elements " + others.join(","));
    if (/Gradient/.test(svg)) why.push("gradient");
    if (/transform=/.test(svg)) why.push("transform");
    if (/stroke="(?!none)|stroke-width="/.test(svg)) why.push("stroke");
    if (/style=/.test(svg)) why.push("style");
    const colours = new Set([...svg.matchAll(/fill="([^"]+)"/g)].map(m => m[1]).filter(c => c != "none"));
    if (colours.size > 1) why.push("colours " + [...colours].join(","));
    return why.length ? why.join("; ") : null;
}

for (const [name, svg] of Object.entries(icons))
{
    const why = unsupported(svg);
    if (why)
    {
        skipped.push([name, why]);
        continue;
    }
    const vb = svg.match(/viewBox="([^"]+)"/)[1].trim().split(/[\s,]+/).map(Number);
    const scale = ICON / Math.max(vb[2], vb[3]);
    const p = new Path2D();
    for (const [, d] of svg.matchAll(/\sd="([^"]+)"/g))
        p.addPath(new Path2D(d));
    if (/evenodd/.test(svg))
        p.setFillType(1);           // FillType.EvenOdd: asWinding turns it into non-zero
    const fitted = p.asWinding().simplify().transform({
        a: scale, b: 0, c: 0, d: scale,
        e: -vb[0] * scale + (ICON - vb[2] * scale) / 2,
        f: -vb[1] * scale + (ICON - vb[3] * scale) / 2,
    });
    add("icon-" + name, ICON, ICON, parseSvgPath(fitted.toSVGString()));
}

// --- Hand-made shapes ----------------------------------------------------------

// A circle of four cubics, clockwise (dir 1) or anticlockwise (-1)
function circle(cx, cy, r, dir = 1)
{
    const k = 0.5522847498 * r;
    const pts = [[cx + r, cy], [cx, cy + r * dir], [cx - r, cy], [cx, cy - r * dir]];
    const out = [{ cmd: CMD.M, pts: pts[0] }];
    for (let i = 0; i < 4; i++)
    {
        const [x0, y0] = pts[i], [x1, y1] = pts[(i + 1) % 4];
        // Tangents: at a point on the circle, perpendicular to its radius
        const t0 = [-(y0 - cy) / r * k * dir, (x0 - cx) / r * k * dir];
        const t1 = [-(y1 - cy) / r * k * dir, (x1 - cx) / r * k * dir];
        out.push({ cmd: CMD.C, pts: [x0 + t0[0], y0 + t0[1], x1 - t1[0], y1 - t1[1], x1, y1] });
    }
    out.push({ cmd: CMD.Z, pts: [] });
    return out;
}

add("shape-square", ICON, ICON, parseSvgPath("M10.5 10.5L69.5 10.5L69.5 69.5L10.5 69.5Z"));
add("shape-circle", ICON, ICON, circle(40, 40, 33.3));
add("shape-ring", ICON, ICON, [...circle(40, 40, 36), ...circle(40, 40, 24, -1)]);
add("shape-diagonal", ICON, ICON, parseSvgPath("M4 74L74 4L74.7 4.7L4.7 74.7Z"));
add("shape-quads", ICON, ICON, parseSvgPath("M5 40Q40 -20 75 40Q40 100 5 40Z"));
// Partly outside the box on every side (clipping)
add("shape-overhang", ICON, ICON, parseSvgPath("M-10 30L40 -12L91 35L45 95Z"));

// --- Glyph runs from the font's outlines ----------------------------------------

const font = fontkit.openSync(FONT);
const gscale = GLYPH_PX / font.unitsPerEm;
const baseline = Math.ceil(font.ascent * gscale) + 2;
const gh = baseline + Math.ceil(-font.descent * gscale) + 2;
for (const text of GLYPH_RUNS)
{
    const run = font.layout(text);
    const cmds = [];
    const glyphs = [];              // per glyph: its commands in run pixels, and its ink's x range
    let pen = 2 / gscale;
    run.glyphs.forEach((g, i) => {
        const X = (x) => (pen + x) * gscale, Y = (y) => baseline - y * gscale;
        const own = [];
        for (const c of g.path.commands)
        {
            const cmd = { moveTo: CMD.M, lineTo: CMD.L, quadraticCurveTo: CMD.Q, bezierCurveTo: CMD.C, closePath: CMD.Z }[c.command];
            own.push({ cmd, pts: c.args.map((v, j) => (j & 1) ? Y(v) : X(v)) });
        }
        cmds.push(...own);
        if (own.length)
            glyphs.push({ cmds: own, x0: Math.floor(X(g.bbox.minX)) - 1, x1: Math.ceil(X(g.bbox.maxX)) + 1 });
        pen += run.positions[i].xAdvance;
    });
    const w = Math.ceil(pen * gscale) + 2;
    add("glyphs-" + text, w, gh, cmds);
    if (PER_GLYPH.includes(text))
        glyphs.forEach((g, i) => {
            // The glyph's box: its ink plus a pixel, full line height, at x0 in the run
            const x0 = Math.max(0, g.x0), x1 = Math.min(w, g.x1);
            const moved = g.cmds.map(c => ({ cmd: c.cmd, pts: c.pts.map((v, j) => (j & 1) ? v : v - x0) }));
            add(`glyph-${text}-${i}`, x1 - x0, gh, moved, x0);
        });
}

// --- Write and report ------------------------------------------------------------

fs.writeFileSync(OUT, writeVectors(entries));
let total = 0;
console.log("path            size (B)  cmds   box");
for (const e of entries)
{
    total += e.path.length;
    console.log(`${e.name.padEnd(22)} ${String(e.path.length).padStart(6)} ${String(e.path.readUInt16LE(0)).padStart(5)}   ${e.w}x${e.h}`);
}
console.log(`\n${entries.length} paths, ${total} B of path data; ${OUT}`);
console.log(`\nskipped ${skipped.length} icons:`);
for (const [name, why] of skipped)
    console.log(`  ${name.padEnd(20)} ${why}`);
