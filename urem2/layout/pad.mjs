// Macro pad and encoder bindings. urem's GPIO buttons 0..5 are key1..key6, its encoder push key7 and the encoder
// enc1 (macro pad job names). urem bound the volume globally; ControlBlock binds per activity, so every activity
// spreads volumeBindings() into its own.

import { withRepeat } from "./keys.mjs";
import { volumeScript, yamahaToggle } from "./scripts.mjs";

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
    [["key2", "up"], ["key4", "left"], ["key5", "down"], ["key6", "right"]]
        .map(([control, dir]) => [control, { press: opsFor(dir), repeat: ARROW_REPEAT }]));
