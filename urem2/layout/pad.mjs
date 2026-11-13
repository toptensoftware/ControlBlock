// Macro pad and encoder bindings. urem's GPIO buttons 0..5 are key1..key6, its encoder push key7 and the encoder
// enc1 (macro pad job names). urem bound the volume globally; ControlBlock binds per activity, so every activity
// spreads volumeBindings() into its own. The pad's six keys are 2 rows of 3: Escape, Up, Enter / Left, Down, Right.

import { withRepeat } from "./keys.mjs";
import { volumeScript, yamahaToggle } from "./scripts.mjs";

const KEYS = ["key1", "key2", "key3", "key4", "key5", "key6", "key7"];
export const PAD_CONTROLS = [...KEYS, "enc1"];

const KEY_EVENTS = ["press", "release", "tap", "long"];
const eventsOf = control => control === "enc1" ? ["turn"] : KEY_EVENTS;

// spec: { key1: { tap, long }, key2: { press, repeat }, enc1: { turn }, ... } -> [[control, { repeat, events }]], in
// control order, each checked (control, events, repeat). Shared by padBindings and padControls.
function padEntries(spec)
{
    for (const control of Object.keys(spec))
        if (!PAD_CONTROLS.includes(control))
            throw new Error(`unknown pad control ${JSON.stringify(control)}`);
    return PAD_CONTROLS.filter(c => spec[c] !== undefined).map(control => {
        const { repeat, ...events } = withRepeat(spec[control]);
        if (repeat !== undefined && control === "enc1")
            throw new Error(`${control} can't repeat`);
        for (const e of Object.keys(events))
            if (!eventsOf(control).includes(e))
                throw new Error(`unknown event ${JSON.stringify(e)} for ${control}`);
        return [control, { repeat, events }];
    });
}

// spec (above) -> layout bindings, in control order then event order (the repeat goes to padControls)
export const padBindings = spec => padEntries(spec).flatMap(([control, { events }]) =>
    eventsOf(control).filter(e => events[e] !== undefined).map(event => ({ control, event, do: events[event] })));

// spec (above) -> the activity's `pad` map: { key2: { repeat }, ... } for each repeating key, in control order
export const padControls = spec => Object.fromEntries(
    padEntries(spec).filter(([, { repeat }]) => repeat !== undefined).map(([control, { repeat }]) => [control, { repeat }]));

// urem volume.js: the encoder steps the Yamaha's volume, its push toggles mute
export const volumeBindings = ctx => padBindings({
    key7: { tap: yamahaToggle(ctx.target, "mute") },
    enc1: { turn: volumeScript(ctx.target) },
});

// urem's arrowRepeat on the pad arrows (atv, hdd)
export const ARROW_REPEAT = { initialDelay: 500, period: 100 };
