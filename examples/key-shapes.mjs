// Standalone layout showing the key shapes (docs/layout-format.md, Keys): a 2U span, a 2x2 key, a tall key, a
// grouped Volume -/+ (one picture across two keys), half-height and half-width keys, and a small key placed off the
// whole-unit grid with an id of its own. Every key types something different over the USB HID keyboard, so with a
// text editor focused on the PC each press shows which key fired. Bake and upload from the repo root:
//     node cli/cli.js upload examples/key-shapes.mjs
//
// Positions and sizes are in grid units (1U = one 144 px cell, steps of 0.25U); the grid is 5 x 3:
//
//     +-----------+-----+-----+-----+
//     |   Wide    | 1U  |  -  Vol  +|     row 0 (Volume: one picture, two keys)
//     +-----+-----+-----+--+--+-----+
//     |  A  |           |L |R |     |     row 1 (A over B: half height; L, R: half width)
//     |  B  |    Big    |  |  |     |
//     +-----+           +--+--+Tall |
//     | Esc |           | (q) |     |     row 2 (q: 0.5U square, 0.25U in from the cell's corner)
//     +-----+-----------+-----+-----+

import { flatten } from "./test-common.mjs";

const type = keys => ({ op: "hidKey", keys });
const VOL_ICON = { x: 0, y: -7 };

const layout = {
    version: 1,
    defaults: { color: "#22262e", iconColor: "white", pressed: { color: "#3a86ff" } },
    home: {
        keys: [
            // slot 0, two units wide (it covers slot 1, so entry 1 is null)
            { w: 2, label: "Wide (w)", tap: type("w") },
            null,
            { label: "1U (1)", tap: type("1") },
            // slots 3-4: one "Volume" picture split into two keys, each pressing alone. The group's label goes along
            // the bottom and the keys' icons sit above it, as on a 1U key with both.
            { w: 2, label: "Volume", labelVAlign: "bottom", color: "#2a9d8f", keys: [
                { icon: "#volume-down", iconOffset: VOL_ICON, tap: type("minus") },
                { x: 1, icon: "#volume-up", iconOffset: VOL_ICON, tap: type("plus") },
            ] },
            // slot 5 holds two half-height keys
            { x: 0, y: 1, h: 0.5, label: "A", tap: type("a") },
            { x: 0, y: 1.5, h: 0.5, label: "B", tap: type("b") },
            // two units square over slots 6, 7, 11, 12
            { x: 1, y: 1, w: 2, h: 2, label: "Big (g)", color: "#8338ec", tap: type("g") },
            // slot 8 holds two half-width keys
            { x: 3, y: 1, w: 0.5, label: "L", tap: type("left") },
            { x: 3.5, y: 1, w: 0.5, label: "R", tap: type("right") },
            // two units tall over slots 9 and 14
            { x: 4, y: 1, h: 2, label: "Tall (t)", color: "#e76f51", tap: type("t") },
            { x: 0, y: 2, label: "Esc", tap: type("esc") },
            // half a unit square, a quarter in from slot 13's corner, with a stable id of its own
            { id: 500, x: 3.25, y: 2.25, w: 0.5, h: 0.5, label: "q", tap: type("q") },
        ],
    },
};

export default flatten(layout);
