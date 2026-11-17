// Renders key faces and some of button-face's built-in icons, encodes
// them to rle565 (with alpha) and packs them into a blobpack for the
// firmware's test pages, plus the firmware's built-in UI assets:
//
//   firmware/assets/test_keys.bin    128x128 key faces and icons, then opaque 144x144
//                                    (1U) and 288x144 (2U) keys
//   firmware/assets/ui.bin           status bar font and pager pills
//   firmware/assets/render_lab.bin   render lab fonts, face and icon (CB_RENDER_LAB only)
//
// Run with `npm install && npm run build` in this folder.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import sharp from "sharp";
import { render } from "@toptensoftware/button-face/node";
import { packRle } from "rle565";
import { blobPack } from "rle565/blobpack.js";
import { packFont } from "rle565/font.js";

const ICON_SIZE = 80;   // icon area, centred in the 128x128 key cell
// The built-in icons are padded for small keys; render bigger and crop
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
    // No fill layer: the background stays transparent. Built-in icons are
    // drawn in #848484, which `colors` maps to the icon's colour.
    const { data } = await render(
        { layers: [{ type: "svg", src: "#" + name, colors: { "#848484": color } }] },
        RENDER_SIZE, RENDER_SIZE, { channels: 4 });
    const big = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
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
// Layout (keep in step with firmware/main/test_keys.h):
//   blobs 0..3            128x128 key faces (the `bg` image), one colour per
//                         test page plus a "pressed" face, padding as
//                         transparent edges
//   blobs 4..7            the same faces made opaque: drawn on COMP_BACKGROUND
//                         (black), so they cover the whole cell and the
//                         compositor can skip clearing it
//   blobs 8..             the icons above, centred in a 128x128 cell (the `fg`
//                         image)
//   then                  OPAQUE_KEYS_1U opaque 1U keys (144x144), then
//                         OPAQUE_KEYS_2U opaque 2U keys (288x144): one fully
//                         baked image per key (face, icon, label over black,
//                         8 px of black padding), as the redesigned compositor
//                         draws them (wip/wip-key-render-redesign.md)
//   last blob             names, NUL separated
// The 128 faces and icons go once the compositor draws opaque keys.

const KEY_SIZE = 128;   // matches TEST_KEYS_FACE_SIZE in firmware/main/test_keys.h
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

async function addKey(name, rgba, w = KEY_SIZE, h = KEY_SIZE)
{
    const info = { width: w, height: h, channels: 4 };
    const packed = await packRle({ data: rgba, info }, {});
    keyBlobs.push(pad4(packed.data));
    keyNames.push(name);
    await sharp(rgba, { raw: info }).png().toFile(path.join(pngDir, "key-" + name + ".png"));
    console.log(`${name.padEnd(14)} ${String(packed.packedSize).padStart(6)} bytes, ${w}x${h}, ` +
                `rows ${packed.firstRow}..${packed.firstRow + packed.rowCount - 1}`);
}

// A face's RGBA (KEY_SIZE square), optionally on an opaque background filling the cell
async function faceRgba(top, bottom, border, opaque)
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
    return await sharp(Buffer.from(svg)).ensureAlpha().raw().toBuffer();
}

async function addFace(name, top, bottom, border, opaque)
{
    await addKey(name, await faceRgba(top, bottom, border, opaque));
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

// Opaque keys: one fully opaque, fully baked image per key (KEY_CELL in
// firmware/main/key_geometry.h; the face stays 128 px like today's)
const OPAQUE_CELL = 144;
const OPAQUE_PAD = 8;           // (144 - 128) / 2
const OPAQUE_ICON = 64;
const OPAQUE_KEYS_1U = 15;      // TEST_KEYS_KEYS_1U in firmware/main/test_keys.h
const OPAQUE_KEYS_2U = 5;       // TEST_KEYS_KEYS_2U

async function addOpaqueKey(name, w, h, face, icon, label)
{
    const [, top, bottom, border] = face;
    const fw = w - 2 * OPAQUE_PAD, fh = h - 2 * OPAQUE_PAD;
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}">
        <defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="${top}"/><stop offset="1" stop-color="${bottom}"/>
        </linearGradient></defs>
        <rect width="${w}" height="${h}" fill="${BACKGROUND}"/>
        <rect x="${OPAQUE_PAD + 1}" y="${OPAQUE_PAD + 1}" width="${fw - 2}" height="${fh - 2}"
              rx="${FACE_RADIUS}" fill="url(#g)" stroke="${border}" stroke-width="2"/>
        <text x="${w / 2}" y="${h - OPAQUE_PAD - 14}" font-family="Arial, sans-serif" font-size="17"
              font-weight="bold" fill="#ffffff" text-anchor="middle">${label}</text>
    </svg>`;
    const iconPng = await sharp(path.join(pngDir, icon + ".png")).resize(OPAQUE_ICON, OPAQUE_ICON).png().toBuffer();
    const rgba = await sharp(await sharp(Buffer.from(svg))
            .composite([{ input: iconPng, left: (w - OPAQUE_ICON) >> 1, top: OPAQUE_PAD + 16 }])
            .flatten({ background: BACKGROUND })
            .png().toBuffer())
        .ensureAlpha().raw().toBuffer();
    for (let i = 3; i < rgba.length; i += 4)
        if (rgba[i] !== 255)
            throw new Error(`${name}: not opaque`);
    await addKey(name, rgba, w, h);
}

for (let i = 0; i < OPAQUE_KEYS_1U; i++)
{
    const icon = names[i % names.length];
    await addOpaqueKey("1u-" + icon, OPAQUE_CELL, OPAQUE_CELL, FACES[i % 3], icon, icon);
}
for (let i = 0; i < OPAQUE_KEYS_2U; i++)
{
    const icon = names[(i * 3 + 1) % names.length];
    await addOpaqueKey("2u-" + icon, OPAQUE_CELL * 2, OPAQUE_CELL, FACES[i % 3], icon, icon + " (wide key)");
}

keyBlobs.push(pad4(Buffer.from(keyNames.join("\0") + "\0")));
const keysPack = blobPack(keyBlobs);
fs.writeFileSync(keysOutFile, keysPack);
console.log(`\n${keysOutFile}: ${FACES.length * 2} faces + ${names.length} icons + ` +
            `${OPAQUE_KEYS_1U} 1U + ${OPAQUE_KEYS_2U} 2U opaque keys, ${keysPack.length} bytes`);


// ---------------------------------------------------------------------------
// Built-in UI assets: firmware/assets/ui.bin
//
// Layout (keep in step with firmware/main/ui_assets.h):
//   blob 0     status bar font: Inter SemiBold (fonts/, SIL OFL 1.1)
//   blob 1     pager pill for the current page (white)
//   blob 2     pager pill for the other pages (grey)
//   blobs 3-6  status icons: Wi-Fi connected, Wi-Fi off, busy, error

const STATUS_FONT = { file: "fonts/Inter-SemiBold.ttf", size: 20, subpixel: 4 };

// [name, width, height, colour]
const PAGER_PILLS = [
    ["pager-current", 24, 6, "#ffffff"],
    ["pager-other",   12, 6, "#606060"],
];

const uiOutFile = path.resolve(here, "../../firmware/assets/ui.bin");
const uiBlobs = [];

const font = packFont(path.join(here, STATUS_FONT.file),
                      { size: STATUS_FONT.size, subpixel: STATUS_FONT.subpixel, charset: "32-126" });
uiBlobs.push(pad4(font.data));
console.log(`\nstatus font    ${String(font.data.length).padStart(6)} bytes`);

for (const [name, w, h, color] of PAGER_PILLS)
{
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}">
        <rect width="${w}" height="${h}" rx="${h / 2}" fill="${color}"/>
    </svg>`;
    const rgba = await sharp(Buffer.from(svg)).ensureAlpha().raw().toBuffer();
    const packed = await packRle({ data: rgba, info: { width: w, height: h, channels: 4 } }, {});
    uiBlobs.push(pad4(packed.data));
    console.log(`${name.padEnd(14)} ${String(packed.packedSize).padStart(6)} bytes`);
}

// Status icons (blobs 3..6, in STATUS_ICON_* bit order: status_icons.h).
// Placeholders: to replace one, change its SVG here (any ~20 px square SVG
// with alpha) and re-run the build.
const STATUS_ICON = 20;
const STATUS_ICONS = [
    // Wi-Fi connected: the usual waves, white like the clock text
    ["wifi-connected", `<g fill="none" stroke="#ffffff" stroke-width="2.6" stroke-linecap="round">
        <path d="M2 8 Q10 0 18 8"/><path d="M5 11.5 Q10 6 15 11.5"/></g>
        <circle cx="10" cy="15.5" r="2.2" fill="#ffffff"/>`],
    // Wi-Fi not connected: a small dot
    ["wifi-off", `<circle cx="10" cy="14" r="3" fill="#a0a0a0"/>`],
    // Busy: a small white hourglass
    ["busy", `<g fill="none" stroke="#ffffff" stroke-width="1.8" stroke-linejoin="round" stroke-linecap="round">
        <path d="M5 2.5 H15"/><path d="M5 17.5 H15"/>
        <path d="M6.5 2.5 Q6.5 7.5 10 10 Q13.5 7.5 13.5 2.5"/><path d="M6.5 17.5 Q6.5 12.5 10 10 Q13.5 12.5 13.5 17.5"/></g>
        <path d="M7.8 17 Q8.3 13.8 10 12.8 Q11.7 13.8 12.2 17 Z" fill="#ffffff"/>`],
    // Error: a red circle with a white x
    ["error", `<circle cx="10" cy="10" r="8.5" fill="#e03030"/>
        <g stroke="#ffffff" stroke-width="2.2" stroke-linecap="round"><path d="M6.8 6.8 L13.2 13.2"/><path d="M13.2 6.8 L6.8 13.2"/></g>`],
];

for (const [name, body] of STATUS_ICONS)
{
    const s = STATUS_ICON;
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${s}" height="${s}" viewBox="0 0 20 20">${body}</svg>`;
    const rgba = await sharp(Buffer.from(svg)).ensureAlpha().raw().toBuffer();
    const packed = await packRle({ data: rgba, info: { width: s, height: s, channels: 4 } }, {});
    uiBlobs.push(pad4(packed.data));
    console.log(`${name.padEnd(14)} ${String(packed.packedSize).padStart(6)} bytes`);
}

const uiPack = blobPack(uiBlobs);
fs.writeFileSync(uiOutFile, uiPack);
console.log(`\n${uiOutFile}: ${uiPack.length} bytes`);


// ---------------------------------------------------------------------------
// Render lab assets: firmware/assets/render_lab.bin
// (wip/wip-on-device-render-feasibility.md; embedded only with -DCB_RENDER_LAB=ON)
//
// Layout (keep in step with firmware/main/render_lab_assets.h):
//   blobs 0-2  Inter SemiBold, ASCII: 20 px subpixel 1, 27 px subpixel 1, 27 px subpixel 4
//              (the default key label sizes: bake look.js, 11 and 15 px on a 72 px face at 128 px)
//   blob 3     a shaded grey key face with alpha, KEY_SIZE square (recolours well)
//   blob 4     an icon with alpha, ICON_SIZE square

const LAB_FONTS = [
    { size: 20, subpixel: 1 },
    { size: 27, subpixel: 1 },
    { size: 27, subpixel: 4 },
];
const LAB_FACE = ["#8c8c94", "#3c3c44", "#c0c0c8"];    // top, bottom, border
const LAB_ICON = "volume-up";

const labOutFile = path.resolve(here, "../../firmware/assets/render_lab.bin");
const labBlobs = [];

console.log("");
for (const { size, subpixel } of LAB_FONTS)
{
    const f = packFont(path.join(here, STATUS_FONT.file), { size, subpixel, charset: "32-126" });
    labBlobs.push(pad4(f.data));
    console.log(`lab font ${size} px sp${subpixel} ${String(f.data.length).padStart(6)} bytes`);
}

async function addLabImage(name, rgba, size)
{
    const packed = await packRle({ data: rgba, info: { width: size, height: size, channels: 4 } }, {});
    labBlobs.push(pad4(packed.data));
    console.log(`lab ${name.padEnd(10)} ${String(packed.packedSize).padStart(6)} bytes, ${size}x${size}`);
}

await addLabImage("face", await faceRgba(...LAB_FACE, false), KEY_SIZE);
await addLabImage("icon", await sharp(path.join(pngDir, LAB_ICON + ".png")).ensureAlpha().raw().toBuffer(), ICON_SIZE);

const labPack = blobPack(labBlobs);
fs.writeFileSync(labOutFile, labPack);
console.log(`\n${labOutFile}: ${labPack.length} bytes`);
