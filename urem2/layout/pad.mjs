// Macro pad and encoder bindings. urem's GPIO buttons 0..5 are key1..key6, its encoder push key7 and the encoder
// enc1 (macro pad job names). urem bound the volume globally; ControlBlock binds per activity, so every activity
// spreads volumeBindings() into its own.

import { withRepeat, key, muteKey } from "./keys.mjs";
import { icon, COLORS } from "./look.mjs";
import { volumeScript, yamahaToggle } from "./scripts.mjs";
import { setVolume } from "../tables/yamaha.mjs";

// The pad's six keys (2 rows of 3: Escape, Up, Enter / Left, Down, Right): the pad page slot, icon and colour, and the
// arrow direction
const PAD_KEYS = {
    key1: { slot: 1, icon: "#cross", color: COLORS.red },
    key2: { slot: 2, icon: "#arrow-up", color: COLORS.white, dir: "up" },
    key3: { slot: 3, icon: "#check", color: COLORS.green },
    key4: { slot: 6, icon: "#arrow-left", color: COLORS.white, dir: "left" },
    key5: { slot: 7, icon: "#arrow-down", color: COLORS.white, dir: "down" },
    key6: { slot: 8, icon: "#arrow-right", color: COLORS.white, dir: "right" },
};

const KEYS = ["key1", "key2", "key3", "key4", "key5", "key6", "key7"];
export const PAD_CONTROLS = [...KEYS, "enc1"];

const KEY_EVENTS = ["press", "release", "tap", "long"];
const eventsOf = control => control === "enc1" ? ["turn"] : KEY_EVENTS;

// spec: { key1: { tap, long }, key2: { press, repeat }, enc1: { turn }, ... } -> layout bindings, in control order
// then event order
export function padBindings(spec)
{
    for (const control of Object.keys(spec))
        if (!PAD_CONTROLS.includes(control))
            throw new Error(`unknown pad control ${JSON.stringify(control)}`);
    return PAD_CONTROLS.filter(c => spec[c] !== undefined).flatMap(control => {
        const events = withRepeat(spec[control]);
        for (const e of Object.keys(events))
            if (!eventsOf(control).includes(e))
                throw new Error(`unknown event ${JSON.stringify(e)} for ${control}`);
        return eventsOf(control).filter(e => events[e] !== undefined).map(event => ({ control, event, do: events[event] }));
    });
}

// urem volume.js: the encoder steps the Yamaha's volume, its push toggles mute
export const volumeBindings = ctx => padBindings({
    key7: { tap: yamahaToggle(ctx.target, "mute") },
    enc1: { turn: volumeScript(ctx.target) },
});

// urem's arrowRepeat on the pad arrows (atv, hdd)
export const ARROW_REPEAT = { initialDelay: 500, period: 100 };

// urem's pad arrows: key2 up, key4 left, key5 down, key6 right, on press with the arrow repeat.
// opsFor(direction) -> the op(s) for "up" | "left" | "down" | "right"
export const padArrows = opsFor => Object.fromEntries(
    Object.entries(PAD_KEYS).filter(([, p]) => p.dir)
        .map(([control, { dir }]) => [control, { press: opsFor(dir), repeat: ARROW_REPEAT }]));

// One Yamaha volume step (the on-screen Vol+/Vol-: a tap has no encoder delta)
export const volumeStep = (ctx, dir) => ctx.ops.yamaha(setVolume(dir, 1));

// The pad page (on-screen stand-in for the pad): spec (as padBindings) -> { slot: key }. Each pad key in the pad's
// 2x3 shape, its tap from the spec's tap or press (touch keys have no press), its long kept; unbound keys empty.
// Plus Vol+ (4), Vol- (9) and mute (14).
export function padPage(ctx, spec)
{
    const purple = { color: COLORS.purple };
    const slots = {};
    for (const [control, p] of Object.entries(PAD_KEYS))
    {
        const s = spec[control];
        const tap = s?.tap ?? s?.press;
        if (tap !== undefined)
            slots[p.slot] = key(icon(p.icon, { color: p.color }), tap,
                                { long: s.long, repeat: s.repeat });
    }
    slots[4] = key(icon("#volume-up", purple), volumeStep(ctx, "up"));
    slots[9] = key(icon("#volume-down", purple), volumeStep(ctx, "down"));
    slots[14] = muteKey(ctx);
    return slots;
}
