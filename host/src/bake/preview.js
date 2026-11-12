// Page previews: each page of a resolved layout (stage 1's tree, images not yet rendered) drawn as
// the screen shows it, for checking a layout without the device. Each key's image (the bake's own
// composition: createPixelRenderer) at its rect, optionally over a faint dashed grid of the 1U
// slots numbered 0-14, and the page's name in a plain status bar. Node only (sharp).

import sharp from "sharp";
import { createPixelRenderer } from "./render-node.js";
import { IMAGES } from "./tree.js";
import {
    SCREEN_W, SCREEN_H, SCREEN_STATUS_H, KEY_GRID_X0, KEY_GRID_Y0, KEY_CELL, KEY_QUARTER, KEY_COLS, KEY_ROWS, GRID_SLOTS,
} from "./consts.js";

const GRID_TOP = SCREEN_STATUS_H + KEY_GRID_Y0;     // the grid's origin on the screen
const STATUS_BG = "#181818", TITLE_COLOR = "#c0c0c0", GRID_COLOR = "#2a2a2a", SLOT_COLOR = "#3a3a3a";

const toBuffer = a => Buffer.from(a.buffer, a.byteOffset, a.byteLength);
const xml = s => s.replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;" })[c]);

// Grid line i of n starting at `origin`, on pixel centres inside the grid
const line = (origin, i, n) => origin + i * KEY_CELL + (i === n ? -0.5 : 0.5);

function gridSvg()
{
    let lines = "", labels = "";
    const right = line(KEY_GRID_X0, KEY_COLS, KEY_COLS), bottom = line(GRID_TOP, KEY_ROWS, KEY_ROWS);
    for (let c = 0; c <= KEY_COLS; c++)
        lines += `<line x1="${line(KEY_GRID_X0, c, KEY_COLS)}" y1="${GRID_TOP}" x2="${line(KEY_GRID_X0, c, KEY_COLS)}" y2="${bottom}"/>`;
    for (let r = 0; r <= KEY_ROWS; r++)
        lines += `<line x1="${KEY_GRID_X0}" y1="${line(GRID_TOP, r, KEY_ROWS)}" x2="${right}" y2="${line(GRID_TOP, r, KEY_ROWS)}"/>`;
    for (let i = 0; i < GRID_SLOTS; i++)
        labels += `<text x="${KEY_GRID_X0 + (i % KEY_COLS) * KEY_CELL + 6}" y="${GRID_TOP + Math.floor(i / KEY_COLS) * KEY_CELL + 18}">${i}</text>`;
    return `<g stroke="${GRID_COLOR}" stroke-width="1" stroke-dasharray="4 4">${lines}</g>
        <g fill="${SLOT_COLOR}" font-family="sans-serif" font-size="13">${labels}</g>`;
}

const statusSvg = title => `<rect width="${SCREEN_W}" height="${SCREEN_STATUS_H}" fill="${STATUS_BG}"/>
    <text x="12" y="${Math.round(SCREEN_STATUS_H * 0.65)}" fill="${TITLE_COLOR}" font-family="sans-serif" font-size="18">${xml(title)}</text>`;

const svg = (body, height) => Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${SCREEN_W}" height="${height}">${body}</svg>`);

// Every page of the tree, in order: { index, name, title, activity, page, keys }. `name` is a
// stable short name ("home", "<activity>-<n>"); `title` is what the status bar shows.
function pagesOf(tree)
{
    const list = [{ name: "home", title: "Home", activity: null, page: 0, keys: tree.home.keys }];
    for (const a of tree.activities)
        for (const [p, page] of a.pages.entries())
            list.push({
                name: `${a.name}-${p + 1}`,
                title: `${a.displayName ?? a.name}${a.pages.length > 1 ? ` — page ${p + 1} of ${a.pages.length}` : ""}`,
                activity: a.name, page: p, keys: page.keys,
            });
    return list.map((p, index) => ({ index, ...p }));
}

// The tree's pages as screen images: [{ index, name, title, activity, page, rgba, width, height }]
// (opaque RGBA, SCREEN_W x SCREEN_H). Options: `state` "normal" or "pressed" (the key look to
// draw), `grid` (default true), `page` ("home" or an activity's name: only those pages; anything
// else throws), `pixels` (spec -> RGBA; default a new createPixelRenderer()).
export async function renderPages(tree, { state = "normal", grid = true, page, pixels = createPixelRenderer() } = {})
{
    if (!IMAGES.includes(state))
        throw new Error(`state must be one of ${IMAGES.join(", ")} (got "${state}")`);
    let pages = pagesOf(tree);
    if (page !== undefined)
    {
        pages = pages.filter(p => (p.activity ?? "home") === page);
        if (!pages.length)
            throw new Error(`no page or activity "${page}" (home, or one of: ${tree.activities.map(a => a.name).join(", ")})`);
    }

    const background = grid ? [{ input: svg(gridSvg(), SCREEN_H), left: 0, top: 0 }] : [];
    return Promise.all(pages.map(async ({ keys, ...p }) => {
        const inputs = [...background];
        for (const key of keys)
        {
            const spec = key[state] ?? key.normal;
            if (!spec)
                continue;
            inputs.push({ input: toBuffer(await pixels(spec)), raw: { width: spec.width, height: spec.height, channels: 4 },
                          left: KEY_GRID_X0 + key.rect[0] * KEY_QUARTER, top: GRID_TOP + key.rect[1] * KEY_QUARTER });
        }
        inputs.push({ input: svg(statusSvg(p.title), SCREEN_STATUS_H), left: 0, top: 0 });
        const out = await sharp({ create: { width: SCREEN_W, height: SCREEN_H, channels: 4, background: { r: 0, g: 0, b: 0, alpha: 1 } } })
            .composite(inputs).raw().toBuffer();
        return { ...p, rgba: new Uint8Array(out.buffer, out.byteOffset, out.byteLength), width: SCREEN_W, height: SCREEN_H };
    }));
}

// A page from renderPages (or any { rgba, width, height }) as a PNG Buffer
export function encodePng({ rgba, width, height })
{
    return sharp(toBuffer(rgba), { raw: { width, height, channels: 4 } }).png().toBuffer();
}
