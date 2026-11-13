// Tests for the TV/HDD activity (Panasonic DVR over IR): every slot and pad binding against urem's
// activity-hdd.js (wip/complete-urem2-layout.md, Notes, Activity hdd). From the repo root:
// node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { hdd } from "./hdd.mjs";
import { makeContext } from "../context.mjs";
import { activityMeta } from "../devices.mjs";
import { key, grid, muteKey } from "../keys.mjs";
import { icon, label, COLORS } from "../look.mjs";
import { volumeBindings, padPage, ARROW_REPEAT } from "../pad.mjs";
import { targets } from "../../targets.mjs";
import { assertSlots, layoutWith } from "../../test/slots.mjs";
import { cleanLayout } from "../../test/resolve.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);
    const ir = code => ctx.ops.ir(`pana.${code}`);
    const white = { color: COLORS.white }, orange = { color: COLORS.orange }, green = { color: COLORS.green };
    const red = { color: COLORS.red };
    const num = n => key(label(String(n), { font: "bold 24px sans-serif", offset: { y: -3 } }), ir(`btn${n}`));
    const square = color => icon("#square", { color, scale: 0.6 });

    test(`${name}: hdd meta, the pad page and three pages`, () => {
        const { pages, bindings, pad, ...meta } = hdd(ctx);
        assert.deepEqual(meta, activityMeta(ctx, "hdd"));
        assert.deepEqual(pad, { key2: { repeat: ARROW_REPEAT }, key4: { repeat: ARROW_REPEAT },
                                key5: { repeat: ARROW_REPEAT }, key6: { repeat: ARROW_REPEAT } });
        assert.equal(pages.length, 4);
    });

    test(`${name}: hdd page 1 matches urem`, () => {
        assertSlots(hdd(ctx).pages[1].keys, {
            1: key(icon("#previous-track", white), ir("previousTrack")),
            2: key(icon("#next-track", white), ir("nextTrack")),
            3: key(label("-10"), ir("skipBack10")),
            4: key(icon("#option", green), ir("option")),
            6: key(icon("#rewind", white), ir("reverse")),
            7: key(icon("#forward", white), ir("forward")),
            8: key(label("+60"), ir("skipForward60")),
            9: key(icon("#info", green), ir("info")),
            11: key(icon("#play", orange), ir("play")),
            12: key(icon("#pause", orange), ir("pause")),
            13: key(icon("#stop", orange), ir("stop")),
            14: muteKey(ctx),
        });
    });

    test(`${name}: hdd page 2 (numbers, last channel, guide) matches urem`, () => {
        assertSlots(hdd(ctx).pages[2].keys, {
            1: num(1), 2: num(2), 3: num(3),
            6: num(4), 7: num(5), 8: num(6),
            11: num(7), 12: num(8), 13: num(9),
            14: num(0),
            4: key({ ...icon("#refresh", { ...orange, scale: 0.7, offset: { y: -10 } }), ...label("CH", { valign: "center", offset: { y: 13 } }) },
                   ir("lastChannel")),
            9: key(icon("#guide", orange), ir("guide")),
        });
    });

    test(`${name}: hdd page 3 (colours, subtitles, channel, record) matches urem`, () => {
        assertSlots(hdd(ctx).pages[3].keys, {
            1: key(square(COLORS.red), ir("red")),
            2: key(square(COLORS.green), ir("green")),
            3: key(square(COLORS.yellow), ir("yellow")),
            4: key(square(COLORS.blue), ir("blue")),
            6: key(label("Sub- titles"), ir("sttl")),
            9: key({ ...icon("#arrow-up", { ...white, scale: 0.8, offset: { y: -13 } }), ...label("CH", { valign: "center", offset: { y: 3 } }) },
                   ir("channelUp")),
            11: key(label("Drive Select"), ir("driveSelect")),
            12: key(icon("#delete", red), ir("delete")),
            13: key({ ...icon("#circle", { ...red, scale: 0.7, offset: { y: -10 } }),
                      ...label("Record", { valign: "bottom", offset: { y: -6 } }) }, ir("record")),
            14: key({ ...icon("#arrow-down", { ...white, scale: 0.8, offset: { y: 9 } }), ...label("CH", { valign: "center", offset: { y: -11 } }) },
                    ir("channelDown")),
        });
    });

    test(`${name}: hdd pad: return/up/ok/left/down/right on press, then the volume`, () => {
        assert.deepEqual(hdd(ctx).bindings, [
            { control: "key1", event: "press", do: ir("return") },
            { control: "key2", event: "press", do: ir("up") },
            { control: "key3", event: "press", do: ir("ok") },
            { control: "key4", event: "press", do: ir("left") },
            { control: "key5", event: "press", do: ir("down") },
            { control: "key6", event: "press", do: ir("right") },
            ...volumeBindings(ctx),
        ]);
    });

    test(`${name}: hdd resolves clean`, async () => {
        const tree = await cleanLayout(layoutWith(ctx, { activities: { hdd: hdd(ctx) } }));
        assert.deepEqual(tree.activities[1].pages.map(p => p.keys.filter(k => k.normal).length), [9, 12, 12, 10]);
    });
}

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);
    const ir = code => ctx.ops.ir(`pana.${code}`);

    test(`${name}: hdd pad page: cross return, tick ok, Panasonic arrows`, () => {
        const page = hdd(ctx).pages[0].keys;
        assert.deepEqual([1, 2, 3, 6, 7, 8].map(s => page[s].tap),
                         ["return", "up", "ok", "left", "down", "right"].map(ir));
        const repeat = ARROW_REPEAT;
        assert.deepEqual(page, grid(padPage(ctx, { key1: { tap: ir("return") }, key2: { tap: ir("up"), repeat },
            key3: { tap: ir("ok") }, key4: { tap: ir("left"), repeat }, key5: { tap: ir("down"), repeat },
            key6: { tap: ir("right"), repeat } })));
        assert.deepEqual([2, 6, 7, 8].map(s => page[s].repeat), Array(4).fill(ARROW_REPEAT));
    });
}
