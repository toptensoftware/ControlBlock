// TV / HDD (urem activity-hdd.js): the Panasonic DVR over IR, three pages. urem's pager arrows (5, 10) are swipes
// here.

import { defineActivity } from "./activity.mjs";
import { key, transportKeys, muteKey } from "../keys.mjs";
import { icon, label, COLORS } from "../look.mjs";
import { padArrows } from "../pad.mjs";

export function hdd(ctx)
{
    const ir = code => ctx.ops.ir(`pana.${code}`);
    const irKey = (look, code) => key(look, ir(code));
    const white = { color: COLORS.white }, orange = { color: COLORS.orange }, green = { color: COLORS.green };
    const red = { color: COLORS.red };

    // urem numKey: "N" in bold 24px, raised 3 px, sending btnN
    const num = n => irKey(label(String(n), { font: "bold 24px sans-serif", offset: { y: -3 } }), `btn${n}`);
    // An icon with "CH" under or over it. urem's offsets are from the centre (node-buttonImage's default); ControlBlock
    // puts a label under an icon at the bottom, which pushed "CH" off the key.
    const ch = (iconLook, labelY, code) =>
        irKey({ ...iconLook, ...label("CH", { valign: "center", offset: { y: labelY } }) }, code);
    const square = color => icon("#square", { color, scale: 0.6 });

    return defineActivity(ctx, "hdd", {
        padPage: true,
        pages: [
            {
                ...transportKeys({ previous: ir("previousTrack"), next: ir("nextTrack"), rewind: ir("reverse"),
                                   forward: ir("forward"), play: ir("play"), pause: ir("pause"), stop: ir("stop") }),
                3: irKey(label("-10"), "skipBack10"),
                4: irKey(icon("#option", green), "option"),
                8: irKey(label("+60"), "skipForward60"),
                9: irKey(icon("#info", green), "info"),
                14: muteKey(ctx),
            },
            {
                1: num(1), 2: num(2), 3: num(3),
                6: num(4), 7: num(5), 8: num(6),
                11: num(7), 12: num(8), 13: num(9),
                14: num(0),
                4: ch(icon("#refresh", { ...orange, scale: 0.7, offset: { y: -10 } }), 13, "lastChannel"),
                9: irKey(icon("#guide", orange), "guide"),
            },
            {
                1: irKey(square(COLORS.red), "red"),
                2: irKey(square(COLORS.green), "green"),
                3: irKey(square(COLORS.yellow), "yellow"),
                4: irKey(square(COLORS.blue), "blue"),
                6: irKey(label("Sub- titles"), "sttl"),
                9: ch(icon("#arrow-up", { ...white, scale: 0.8, offset: { y: -13 } }), 3, "channelUp"),
                11: irKey(label("Drive Select"), "driveSelect"),
                12: irKey(icon("#delete", red), "delete"),
                13: irKey({ ...icon("#circle", { ...red, scale: 0.7, offset: { y: -10 } }),
                            ...label("Record", { valign: "bottom", offset: { y: -6 } }) }, "record"),
                14: ch(icon("#arrow-down", { ...white, scale: 0.8, offset: { y: 9 } }), -11, "channelDown"),
            },
        ],
        // all on press, as urem
        pad: { key1: { press: ir("return") }, key3: { press: ir("ok") }, ...padArrows(ir) },
    });
}
