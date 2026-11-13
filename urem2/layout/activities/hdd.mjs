// TV / HDD (urem activity-hdd.js): the Panasonic DVR over IR, the pad page and three pages. urem's pager arrows are
// swipes here.

import { defineActivity } from "./activity.mjs";
import { key, muteKey, volUpKey, volDownKey } from "../keys.mjs";
import { icon, label, COLORS } from "../look.mjs";
import { ARROW_REPEAT } from "../pad.mjs";

export function hdd(ctx)
{
    const ir = code => ctx.ops.ir(`pana.${code}`);
    const irKey = (look, code, opts) => key(look, ir(code), opts);
    const white = { color: COLORS.white }, orange = { color: COLORS.orange }, green = { color: COLORS.green };
    const red = { color: COLORS.red };
    const repeat = ARROW_REPEAT;

    // urem numKey: "N" in bold 24px, raised 3 px, sending btnN
    const num = n => irKey(label(String(n), { font: "bold 24px sans-serif", offset: { y: -3 } }), `btn${n}`);
    // An icon with "CH" under or over it. urem's offsets are from the centre (node-buttonImage's default); ControlBlock
    // puts a label under an icon at the bottom, which pushed "CH" off the key.
    const ch = (iconLook, labelY, code) =>
        irKey({ ...iconLook, ...label("CH", { valign: "center", offset: { y: labelY } }) }, code);
    const square = color => icon("#square", { color, scale: 0.6 });

    return defineActivity(ctx, "hdd", {
        pages: [
            // the pad page: the macro pad's keys on screen
            [
                // Functions
                { ...irKey(icon("#list", orange), "videoMenu"), x: 1, y: 0 },
                { ...irKey(icon("#guide", orange), "guide" ), x: 2, y: 0 },
                { ...irKey(icon("#function", orange), "functionMenu" ), x: 3, y: 0 },

                // Audio
                { ...volUpKey(ctx), x: 4, y: 0 },
                { ...volDownKey(ctx), x: 4, y: 1 },
                { ...muteKey(ctx), x: 4, y: 2 },

                // Modes
                { ...irKey(icon("#option", green), "option"), x: 0, y: 1 },
                { ...irKey(icon("#info", green), "info"), x: 0, y: 2 },

                // Misc
                { ...irKey(label("Sub- titles"), "sttl"), x: 0, y: 0 },

                // Navigation
                { ...irKey(icon("#cross", red), "return"), x: 1, y: 1 },
                { ...irKey(icon("#arrow-up", white), "up", { repeat }), x: 2, y: 1 },
                { ...irKey(icon("#check", green), "ok"), x: 3, y: 1 },
                { ...irKey(icon("#arrow-left", white), "left", { repeat }), x: 1, y: 2 },
                { ...irKey(icon("#arrow-down", white), "down", { repeat }), x: 2, y: 2 },
                { ...irKey(icon("#arrow-right", white), "right", { repeat }), x: 3, y: 2 },
            ],
            // transport and skips
            [
                { ...irKey(icon("#previous-track", white), "previousTrack"), x: 1, y: 0 },
                { ...irKey(icon("#next-track", white), "nextTrack"), x: 2, y: 0 },
                { ...irKey(label("-10"), "skipBack10"), x: 3, y: 0 },
                { ...irKey(icon("#rewind", white), "reverse"), x: 1, y: 1 },
                { ...irKey(icon("#forward", white), "forward"), x: 2, y: 1 },
                { ...irKey(label("+60"), "skipForward60"), x: 3, y: 1 },
                { ...irKey(icon("#play", orange), "play"), x: 1, y: 2 },
                { ...irKey(icon("#pause", orange), "pause"), x: 2, y: 2 },
                { ...irKey(icon("#stop", orange), "stop"), x: 3, y: 2 },

                { ...volUpKey(ctx), x: 4, y: 0 },
                { ...volDownKey(ctx), x: 4, y: 1 },
                { ...muteKey(ctx), x: 4, y: 2 },
            ],
            // numbers, last channel, guide
            [
                { ...ch(icon("#refresh", { ...orange, scale: 0.7, offset: { y: -10 } }), 13, "lastChannel"), x: 0, y: 0 },
                { ...irKey(icon("#guide", orange), "guide"), x: 0, y: 1 },

                { ...num(1), x: 1, y: 2 },
                { ...num(2), x: 2, y: 2 },
                { ...num(3), x: 3, y: 2 },
                { ...num(4), x: 1, y: 1 },
                { ...num(5), x: 2, y: 1 },
                { ...num(6), x: 3, y: 1 },
                { ...num(7), x: 1, y: 0 },
                { ...num(8), x: 2, y: 0 },
                { ...num(9), x: 3, y: 0 },
                { ...num(0), x: 0, y: 2 },

                { ...volUpKey(ctx), x: 4, y: 0 },
                { ...volDownKey(ctx), x: 4, y: 1 },
                { ...muteKey(ctx), x: 4, y: 2 },
            ],
            // colour keys, subtitles, channel, record
            [
                { ...irKey(square(COLORS.red), "red"), x: 1, y: 0 },
                { ...irKey(square(COLORS.green), "green"), x: 2, y: 0 },
                { ...irKey(square(COLORS.yellow), "yellow"), x: 3, y: 0 },
                { ...irKey(square(COLORS.blue), "blue"), x: 4, y: 0 },
                { ...ch(icon("#arrow-up", { ...white, scale: 0.8, offset: { y: -13 } }), 3, "channelUp"), x: 4, y: 1 },
                { ...irKey(label("Drive Select"), "driveSelect"), x: 1, y: 2 },
                { ...irKey(icon("#delete", red), "delete"), x: 2, y: 2 },
                { ...irKey({ ...icon("#circle", { ...red, scale: 0.7, offset: { y: -10 } }),
                             ...label("Record", { valign: "bottom", offset: { y: -6 } }) }, "record"), x: 3, y: 2 },
                { ...ch(icon("#arrow-down", { ...white, scale: 0.8, offset: { y: 9 } }), -11, "channelDown"), x: 4, y: 2 },
            ],
        ],
        // the macro pad: all on press, as urem
        pad: {
            key1: { press: ir("return") },
            key2: { press: ir("up"), repeat },
            key3: { press: ir("ok") },
            key4: { press: ir("left"), repeat },
            key5: { press: ir("down"), repeat },
            key6: { press: ir("right"), repeat },
        },
    });
}
