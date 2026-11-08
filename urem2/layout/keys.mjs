// Touch key helpers: a key is a look (layout/look.mjs) plus a tap (and maybe a long) op list, and the sound keys
// shared by Home and the activities.

import { icon, label, COLORS } from "./look.mjs";
import { yamahaToggle } from "./scripts.mjs";

const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);
const positiveInt = n => Number.isInteger(n) && n > 0;

function checkRepeat(r)
{
    const ok = positiveInt(r) ||
        (isObject(r) && Object.keys(r).length === 2 && positiveInt(r.initialDelay) && positiveInt(r.period));
    if (!ok)
        throw new Error(`repeat must be a period in ms or { initialDelay, period }, got ${JSON.stringify(r)}`);
}

// urem's auto-repeat (`repeat: 100` or `{ initialDelay, period }`), kept where urem had it. ControlBlock has no
// repeat gesture yet, so this is the one place that checks it and drops it (later: emit the repeat gesture here).
export function withRepeat(spec)
{
    const { repeat, ...rest } = spec;
    if (repeat !== undefined)
        checkRepeat(repeat);
    return rest;
}

// look: from icon()/label() (spread together for both); ops: the tap's op or op list
export function key(look, ops, opts = {})
{
    for (const k of Object.keys(opts))
        if (k !== "long" && k !== "repeat")
            throw new Error(`unknown key option ${JSON.stringify(k)}`);
    return withRepeat({ ...look, tap: ops, ...(opts.long !== undefined && { long: opts.long }), repeat: opts.repeat });
}

const SLOTS = 15;

// { index: key } -> a page's 15 row-major keys (index = row * 5 + col, urem's [col, row]), null where empty
export function grid(slots)
{
    const keys = Array(SLOTS).fill(null);
    for (const [i, k] of Object.entries(slots))
    {
        const n = Number(i);
        if (!/^\d+$/.test(i) || n >= SLOTS)
            throw new Error(`slot ${JSON.stringify(i)} is not 0..${SLOTS - 1}`);
        keys[n] = k;
    }
    return keys;
}

// urem's transport row, the same on atv, hdd and steam: slot, icon, colour. `stop` is optional (steam's slot 13 is
// its own); `repeat` (urem's) goes on rewind and forward.
const TRANSPORT = {
    previous: [1, "#previous-track", COLORS.white],
    next: [2, "#next-track", COLORS.white],
    rewind: [6, "#rewind", COLORS.white],
    forward: [7, "#forward", COLORS.white],
    play: [11, "#play", COLORS.orange],
    pause: [12, "#pause", COLORS.orange],
    stop: [13, "#stop", COLORS.orange],
};
const REPEATED = ["rewind", "forward"];

// opsByName: { previous, next, rewind, forward, play, pause, stop? } -> { slot: key }
export function transportKeys(opsByName, { repeat } = {})
{
    for (const name of Object.keys(opsByName))
        if (!Object.hasOwn(TRANSPORT, name))
            throw new Error(`unknown transport key ${JSON.stringify(name)}`);
    const slots = {};
    for (const [name, [slot, iconName, color]] of Object.entries(TRANSPORT))
    {
        if (!Object.hasOwn(opsByName, name) && name === "stop")
            continue;
        if (opsByName[name] === undefined)
            throw new Error(`transport key ${JSON.stringify(name)} has no ops`);
        slots[slot] = key(icon(iconName, { color }), opsByName[name], { repeat: REPEATED.includes(name) ? repeat : undefined });
    }
    return slots;
}

export const sevenChKey = ctx => key(label("7ch Stereo", { color: COLORS.purple }), yamahaToggle(ctx.target, "sevenCh"));
export const muteKey = ctx => key(icon("#volume-mute", { color: COLORS.purple }), yamahaToggle(ctx.target, "mute"));
