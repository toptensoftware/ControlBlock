// Steam (urem activity-steam.js): the HOT PC (launch Steam or the slideshow kiosk, shut down) and the photo
// slideshow (UDP commands, display element toggles): the pad page and two pages.

import { defineActivity } from "./activity.mjs";
import { key, muteKey, volUpKey, volDownKey } from "../keys.mjs";
import { icon, label, COLORS } from "../look.mjs";

export function steam(ctx)
{
    const { ops } = ctx;
    const show = cmd => ops.slideshow(cmd);
    const pc = (text, cmd) => key(label(text), ops.hotpc(cmd));
    const toggle = (text, el) => key(label(text), ops.slideshowToggle(el));
    const white = { color: COLORS.white }, orange = { color: COLORS.orange };

    return defineActivity(ctx, "steam", {
        pages: [
            // the pad page: the macro pad's keys on screen (urem bound key1 and key3 to nothing)
            [
                { ...key(icon("#previous-track", white), show("prevDay")), x: 0, y: 0 },
                { ...key(icon("#next-track", white), show("nextDay")), x: 1, y: 0 },
                { ...key(icon("#rewind", white), show("backward20"), { repeat: 100 }), x: 2, y: 0 },
                { ...key(icon("#forward", white), show("forward20"), { repeat: 100 }), x: 3, y: 0 },

                { ...key(icon("#pause", orange), show("pause")), x: 0, y: 1 },
                { ...key(icon("#play", orange), show("play")), x: 0, y: 2 },

                { ...key(icon("#info", { color: COLORS.green }), ops.slideshowToggle("all")), x: 3, y: 1 },

                { ...volUpKey(ctx), x: 4, y: 0 },
                { ...volDownKey(ctx), x: 4, y: 1 },
                { ...muteKey(ctx), x: 4, y: 2 },

                { ...key(icon("#arrow-up", white), show("voteUp")), x: 2, y: 1 },
                { ...key(icon("#arrow-left", white), show("prev")), x: 1, y: 2 },
                { ...key(icon("#arrow-down", white), show("voteDown")), x: 2, y: 2 },
                { ...key(icon("#arrow-right", white), show("next")), x: 3, y: 2 },
            ],
            // the slideshow's display element toggles
            [
                { ...toggle("Play State", "transport"), x: 0, y: 0 },
                { ...toggle("Date", "date"), x: 1, y: 0 },
                { ...toggle("Time", "time"), x: 2, y: 0 },
                { ...toggle("Rating", "rating"), x: 3, y: 0 },
                { ...toggle("Place", "location"), x: 4, y: 0 },
                { ...toggle("Other", "other"), x: 0, y: 1 },

                { ...pc("Steam", "launch-steam"), x: 2, y: 2 },
                { ...pc("Slide Show", "launch-kiosk"), x: 3, y: 2 },
                { ...pc("Shut Down", "shutdown"), x: 4, y: 2 },
            ],
        ],
        // the macro pad (urem bound key1 and key3 to nothing)
        pad: {
            key2: { press: show("voteUp") },
            key4: { press: show("prev") },
            key5: { press: show("voteDown") },
            key6: { press: show("next") },
        },
    });
}
