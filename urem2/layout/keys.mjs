// Touch key helpers: a key is a look (layout/look.mjs) plus a tap (and maybe a long) op list, and the single keys
// shared by Home and the activities. Each helper makes one key with no position: a page places it with
// `{ ...helper(...), x, y }` (plus `w`, `h` when not 1U).

import { icon, label, COLORS } from "./look.mjs";
import { yamahaToggle } from "./scripts.mjs";
import { setVolume } from "../tables/yamaha.mjs";

const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);
const positiveInt = n => Number.isInteger(n) && n > 0;

function checkRepeat(r)
{
    const ok = positiveInt(r) ||
        (isObject(r) && Object.keys(r).length === 2 && positiveInt(r.initialDelay) && positiveInt(r.period));
    if (!ok)
        throw new Error(`repeat must be a period in ms or { initialDelay, period }, got ${JSON.stringify(r)}`);
}

// urem's auto-repeat (`repeat: 100` or `{ initialDelay, period }`), kept where urem had it: the bake's `repeat`
// has urem's meaning (a number is both the initial delay and the period). The one place that checks it: an
// undefined repeat is dropped, and a repeat with a long gesture throws (the bake refuses it).
export function withRepeat(spec)
{
    const { repeat, ...rest } = spec;
    if (repeat === undefined)
        return rest;
    checkRepeat(repeat);
    if (rest.long !== undefined)
        throw new Error("repeat and long can't both be on one key");
    return { ...rest, repeat };
}

// look: from icon()/label() (spread together for both); ops: the tap's op or op list
export function key(look, ops, opts = {})
{
    for (const k of Object.keys(opts))
        if (k !== "long" && k !== "repeat")
            throw new Error(`unknown key option ${JSON.stringify(k)}`);
    return withRepeat({ ...look, tap: ops, ...(opts.long !== undefined && { long: opts.long }), repeat: opts.repeat });
}

const purple = { color: COLORS.purple };

export const sevenChKey = ctx => key(label("7ch Stereo", purple), yamahaToggle(ctx.target, "sevenCh"));
export const muteKey = ctx => key(icon("#volume-mute", purple), yamahaToggle(ctx.target, "mute"));

// One Yamaha volume step (the on-screen Vol+/Vol-: a tap has no encoder delta)
export const volumeStep = (ctx, dir) => ctx.ops.yamaha(setVolume(dir, 1));
// Vol+/Vol- step again every period while held (repeats are skipped while the previous request is still queued)
export const VOLUME_REPEAT = { initialDelay: 500, period: 100 };
export const volUpKey = ctx => key(icon("#volume-up", purple), volumeStep(ctx, "up"), { repeat: VOLUME_REPEAT });
export const volDownKey = ctx => key(icon("#volume-down", purple), volumeStep(ctx, "down"), { repeat: VOLUME_REPEAT });
