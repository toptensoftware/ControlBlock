// Renders key faces and some of node-buttonImage's built-in icons, encodes
// them to rle565 (with alpha) and packs them into a blobpack for the
// firmware's test pages:
//
//   firmware/assets/test_keys.bin    128x128 key faces and icons
//
// Run with `npm install && npm run build` in this folder.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import sharp from "sharp";
import { ButtonImage } from "@toptensoftware/node-buttonImage";
import { packRle } from "rle565";
import { blobPack } from "rle565/blobpack.js";

const ICON_SIZE = 80;   // icon area, centred in the 128x128 key cell
// The built-in icons are padded for Stream Deck keys; render bigger and crop
// the centre so they fill more of the icon area.
const RENDER_SIZE = 112;

// [built-in icon, colour]
const ICONS = [
    ["home",         "#ffffff"],
    ["settings",     "#c0c0c0"],
    ["play",         "#40e040"],
    ["pause",        "#ffc020"],
    ["volume-up",    "#40a0ff"],
    ["volume-mute",  "#ff4040"],
    ["power",        "#ff3030"],
    ["heart",        "#ff4080"],
    ["star",         "#ffd700"],
    ["apple-tv",     "#ffffff"],
    ["steam",        "#80c0ff"],
    ["refresh",      "#40ffc0"],
    ["next-track",   "#ffffff"],
    ["info",         "#a0a0ff"],
];

const here = path.dirname(fileURLToPath(import.meta.url));
const pngDir = path.resolve(here, "out");

// Blobs must start 4-byte aligned for the device renderer
function pad4(buf)
{
    const extra = (4 - (buf.length & 3)) & 3;
    return extra ? Buffer.concat([buf, Buffer.alloc(extra)]) : buf;
}

// Render each icon at ICON_SIZE into out/<name>.png; the key icons below
// are made from these
const names = [];
fs.mkdirSync(pngDir, { recursive: true });

for (const [name, color] of ICONS)
{
    const bi = new ButtonImage({});
    bi.backColor = "transparent";
    bi.svgFile = "#" + name;
    bi.svgForeColor = color;

    const big = await bi.render(RENDER_SIZE, RENDER_SIZE, 4);
    const crop = (RENDER_SIZE - ICON_SIZE) >> 1;
    await sharp(big, { raw: { width: RENDER_SIZE, height: RENDER_SIZE, channels: 4 } })
        .extract({ left: crop, top: crop, width: ICON_SIZE, height: ICON_SIZE })
        .png()
        .toFile(path.join(pngDir, name + ".png"));
    names.push(name);
}


// ---------------------------------------------------------------------------
// Compositor test keys: firmware/assets/test_keys.bin
//
// Every image is a full 128x128 key cell (COMP_KEY_SIZE). Layout (keep in
// step with firmware/main/test_keys.h):
//   blobs 0..3            key faces (the `bg` image), one colour per test page
//                         plus a "pressed" face, padding as transparent edges
//   blobs 4..7            the same faces made opaque: drawn on COMP_BACKGROUND
//                         (black), so they cover the whole cell and the
//                         compositor can skip clearing it
//   blobs 8..             the icons above, centred in the cell (the `fg` image)
//   last blob             names, NUL separated

const KEY_SIZE = 128;   // matches COMP_KEY_SIZE in firmware/main/compositor.h
const FACE_SIZE = 120;  // 4 px transparent padding all round
const FACE_RADIUS = 14;
const BACKGROUND = "#000000";   // COMP_BACKGROUND in firmware/main/page_compositor.h

// [name, top colour, bottom colour, border colour]
const FACES = [
    ["face-red",     "#a03838", "#601818", "#d07070"],
    ["face-green",   "#38903c", "#185a1c", "#70c070"],
    ["face-blue",    "#3850a8", "#182c68", "#7090e0"],
    ["face-pressed", "#e0e0e8", "#9090a0", "#ffffff"],
];

const keysOutFile = path.resolve(here, "../../firmware/assets/test_keys.bin");
const keyBlobs = [];
const keyNames = [];

async function addKey(name, rgba)
{
    const info = { width: KEY_SIZE, height: KEY_SIZE, channels: 4 };
    const packed = await packRle({ data: rgba, info }, {});
    keyBlobs.push(pad4(packed.data));
    keyNames.push(name);
    await sharp(rgba, { raw: info }).png().toFile(path.join(pngDir, "key-" + name + ".png"));
    console.log(`${name.padEnd(14)} ${String(packed.packedSize).padStart(6)} bytes, ` +
                `rows ${packed.firstRow}..${packed.firstRow + packed.rowCount - 1}`);
}

// A face, optionally on an opaque background filling the cell
async function addFace(name, top, bottom, border, opaque)
{
    const o = (KEY_SIZE - FACE_SIZE) / 2;
    const back = opaque ? `<rect width="${KEY_SIZE}" height="${KEY_SIZE}" fill="${BACKGROUND}"/>` : "";
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${KEY_SIZE}" height="${KEY_SIZE}">
        <defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="${top}"/><stop offset="1" stop-color="${bottom}"/>
        </linearGradient></defs>
        ${back}
        <rect x="${o + 1}" y="${o + 1}" width="${FACE_SIZE - 2}" height="${FACE_SIZE - 2}"
              rx="${FACE_RADIUS}" fill="url(#g)" stroke="${border}" stroke-width="2"/>
    </svg>`;
    const rgba = await sharp(Buffer.from(svg)).ensureAlpha().raw().toBuffer();
    await addKey(name, rgba);
}

console.log("");
for (const opaque of [false, true])
    for (const [name, top, bottom, border] of FACES)
        await addFace(opaque ? name + "-opaque" : name, top, bottom, border, opaque);

for (const name of names)
{
    const pad = (KEY_SIZE - ICON_SIZE) / 2;
    const rgba = await sharp(path.join(pngDir, name + ".png"))
        .ensureAlpha()
        .extend({ top: pad, bottom: pad, left: pad, right: pad, background: { r: 0, g: 0, b: 0, alpha: 0 } })
        .raw()
        .toBuffer();
    await addKey(name, rgba);
}

keyBlobs.push(pad4(Buffer.from(keyNames.join("\0") + "\0")));
const keysPack = blobPack(keyBlobs);
fs.writeFileSync(keysOutFile, keysPack);
console.log(`\n${keysOutFile}: ${FACES.length * 2} faces + ${names.length} icons, ${keysPack.length} bytes`);
