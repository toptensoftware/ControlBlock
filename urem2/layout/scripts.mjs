// urem2's script ops: the Yamaha toggles (mute, 7ch Stereo) and the encoder's volume script. URLs come from the
// tables/yamaha.mjs path builders on the target's base URL, injected as constants (layout/lua.mjs).

import { luaScript } from "./lua.mjs";
import { getStatus, setMute, setSoundProgram, setVolume } from "../tables/yamaha.mjs";

// Encoder volume (urem volume.js): per-detent step 5 under 8 ms between edges, 3 under 20 ms, else 1. A coalesced
// turn (|delta| > 1) multiplies the step, clamped at VOL_MAX_STEP (one request per event keeps the net queue short).
export const VOL_FAST_US = 8000;
export const VOL_MEDIUM_US = 20000;
export const VOL_STEPS = { fast: 5, medium: 3, slow: 1 };
export const VOL_MAX_STEP = 10;

// A Lua pattern escape (pattern magic characters get a %)
const luaPatternEscape = s => s.replace(/[\^$()%.[\]*+\-?]/g, "%$&");

// A Lua pattern for `"field": value` in compact or spaced JSON
const jsonFieldPattern = (field, value) =>
    `${luaPatternEscape(JSON.stringify(field))}%s*:%s*${luaPatternEscape(JSON.stringify(value))}`;

// which -> the getStatus field and value that mean "on", and the set paths for on and off
const TOGGLES = {
    mute: { field: "mute", on: true, onPath: setMute(true), offPath: setMute(false) },
    sevenCh: { field: "sound_program", on: "7ch_stereo", onPath: setSoundProgram("7ch_stereo"),
               offPath: setSoundProgram("surr_decoder") },
};

// which: "mute" | "sevenCh" -> a script op that reads getStatus and sets the opposite
export function yamahaToggle(target, which)
{
    if (!Object.hasOwn(TOGGLES, which))
        throw new Error(`unknown Yamaha toggle ${JSON.stringify(which)}`);
    const t = TOGGLES[which];
    return luaScript("yamaha-toggle.lua", {
        STATUS_URL: target.yamaha + getStatus(),
        ON_PATTERN: jsonFieldPattern(t.field, t.on),
        ON_URL: target.yamaha + t.onPath,
        OFF_URL: target.yamaha + t.offPath,
    });
}

// -> the enc1 turn script op (param "delta=<signed> interval=<us>")
export function volumeScript(target)
{
    const urls = dir => Array.from({ length: VOL_MAX_STEP }, (_, i) => target.yamaha + setVolume(dir, i + 1));
    return luaScript("volume.lua", {
        FAST_US: VOL_FAST_US,
        MEDIUM_US: VOL_MEDIUM_US,
        STEP_FAST: VOL_STEPS.fast,
        STEP_MEDIUM: VOL_STEPS.medium,
        STEP_SLOW: VOL_STEPS.slow,
        MAX_STEP: VOL_MAX_STEP,
        UP_URLS: urls("up"),
        DOWN_URLS: urls("down"),
    });
}
