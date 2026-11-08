// Tests for the Steam activity (HOT PC and the photo slideshow): every slot and pad binding against urem's
// activity-steam.js (wip/complete-urem2-layout.md, Notes, Activity steam). From the repo root:
// node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { steam } from "./steam.mjs";
import { makeContext } from "../context.mjs";
import { activityMeta } from "../devices.mjs";
import { key } from "../keys.mjs";
import { icon, label, COLORS } from "../look.mjs";
import { volumeBindings } from "../pad.mjs";
import { targets } from "../../targets.mjs";
import { assertSlots, layoutWith } from "../../test/slots.mjs";
import { cleanLayout } from "../../test/resolve.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);
    const { ops } = ctx;
    const show = cmd => ops.slideshow(cmd);
    const white = { color: COLORS.white }, orange = { color: COLORS.orange };

    test(`${name}: steam meta and two pages`, () => {
        const { pages, bindings, ...meta } = steam(ctx);
        assert.deepEqual(meta, activityMeta(ctx, "steam"));
        assert.equal(pages.length, 2);
    });

    test(`${name}: steam page 1 (HOT PC, slideshow transport) matches urem`, () => {
        assertSlots(steam(ctx).pages[0].keys, {
            1: key(icon("#previous-track", white), show("prevDay")),
            2: key(icon("#next-track", white), show("nextDay")),
            4: key(label("Steam"), ops.hotpc("launch-steam")),
            6: key(icon("#rewind", white), show("backward20"), { repeat: 100 }),
            7: key(icon("#forward", white), show("forward20"), { repeat: 100 }),
            9: key(label("Slide Show"), ops.hotpc("launch-kiosk")),
            11: key(icon("#play", orange), show("play")),
            12: key(icon("#pause", orange), show("pause")),
            13: key(icon("#info", { color: COLORS.green }), ops.slideshowToggle("all")),
            14: key(label("Shut Down"), ops.hotpc("shutdown")),
        });
    });

    test(`${name}: steam page 2 (display element toggles) matches urem`, () => {
        const toggle = (text, el) => key(label(text), ops.slideshowToggle(el));
        assertSlots(steam(ctx).pages[1].keys, {
            1: toggle("Play State", "transport"),
            2: toggle("Date", "date"),
            3: toggle("Time", "time"),
            4: toggle("Rating", "rating"),
            6: toggle("Place", "location"),
            11: toggle("Other", "other"),
        });
    });

    test(`${name}: steam pad: key2/4/5/6 vote and step on press, key1/key3 unbound, then the volume`, () => {
        assert.deepEqual(steam(ctx).bindings, [
            { control: "key2", event: "press", do: show("voteUp") },
            { control: "key4", event: "press", do: show("prev") },
            { control: "key5", event: "press", do: show("voteDown") },
            { control: "key6", event: "press", do: show("next") },
            ...volumeBindings(ctx),
        ]);
    });

    test(`${name}: steam resolves clean`, async () => {
        const tree = await cleanLayout(layoutWith(ctx, { activities: { steam: steam(ctx) } }));
        assert.deepEqual(tree.activities[2].pages.map(p => p.keys.filter(k => k.normalFg).length), [10, 6]);
    });
}
