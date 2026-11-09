// Steam (urem activity-steam.js): the HOT PC (launch Steam or the slideshow kiosk, shut down) and the photo
// slideshow (UDP commands, display element toggles), two pages.

import { defineActivity } from "./activity.mjs";
import { key, transportKeys } from "../keys.mjs";
import { icon, label, COLORS } from "../look.mjs";

export function steam(ctx)
{
    const { ops } = ctx;
    const show = cmd => ops.slideshow(cmd);
    const pc = (text, cmd) => key(label(text), ops.hotpc(cmd));
    const toggle = (text, el) => key(label(text), ops.slideshowToggle(el));
    const press = cmd => ({ press: show(cmd) });

    return defineActivity(ctx, "steam", {
        padPage: true,
        pages: [
            {
                ...transportKeys({ previous: show("prevDay"), next: show("nextDay"), rewind: show("backward20"),
                                   forward: show("forward20"), play: show("play"), pause: show("pause") },
                                 { repeat: 100 }),
                4: pc("Steam", "launch-steam"),
                9: pc("Slide Show", "launch-kiosk"),
                13: key(icon("#info", { color: COLORS.green }), ops.slideshowToggle("all")),
                14: pc("Shut Down", "shutdown"),
            },
            {
                1: toggle("Play State", "transport"),
                2: toggle("Date", "date"),
                3: toggle("Time", "time"),
                4: toggle("Rating", "rating"),
                6: toggle("Place", "location"),
                11: toggle("Other", "other"),
            },
        ],
        // urem bound key1 and key3 to nothing
        pad: { key2: press("voteUp"), key4: press("prev"), key5: press("voteDown"), key6: press("next") },
    });
}
