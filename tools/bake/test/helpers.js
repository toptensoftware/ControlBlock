// Shared test fixtures: a fake host (no disk, no wasmoon) for stage 1, and a
// fake renderer for stage 2.

import { resolve } from "../src/resolve.js";

// files: { "scripts/mute.lua": "...", "icons/a.svg": "<svg/>" }
export function fakeHost(files = {}, { luaCheck, icons } = {})
{
    return {
        ...(icons ? { hasIcon: name => icons.includes(name) } : {}),
        exists: p => Object.hasOwn(files, p),
        readText: p => { if (!Object.hasOwn(files, p)) throw new Error(`no such file ${p}`); return files[p]; },
        resolvePath: p => p,
        // A fake Lua checker: any source containing "SYNTAXERR" is bad
        luaCheck: luaCheck ?? (src => src.includes("SYNTAXERR") ? "script:1: unexpected symbol" : null),
    };
}

// Resolve a config and return everything
export function run(config, files, opts)
{
    return resolve(config, fakeHost(files, opts));
}

// Resolve, assert no errors, return the tree
export function ok(config, files, opts)
{
    const r = run(config, files, opts);
    if (r.errors.length)
        throw new Error("unexpected errors:\n" + r.errors.map(e => `${e.path}: ${e.message}`).join("\n"));
    return r.tree;
}

// The paths of the errors
export const errorPaths = r => r.errors.map(e => e.path);
export const errorText = r => r.errors.map(e => `${e.path}: ${e.message}`).join("\n");
export const warningText = r => r.warnings.map(e => `${e.path}: ${e.message}`).join("\n");

// A renderer that records calls and returns deterministic bytes from the definition
export function fakeRenderer()
{
    const calls = [];
    const fn = async def =>
    {
        calls.push(def);
        // 7 bytes: not a multiple of 4, to prove the pipeline doesn't rely on length
        return Buffer.from("IMG:" + JSON.stringify(def));
    };
    fn.calls = calls;
    return fn;
}

// Resolve a Home key whose `gesture` runs `ops`; return { r, ops } with the packed ops of the
// first generated binding (or undefined if resolution failed or the list is empty)
export function opsOf(ops, { gesture = "tap", files, actions, activities, devices } = {})
{
    const config = { version: 1, home: { keys: [ { [gesture]: ops } ] } };
    if (actions) config.actions = actions;
    if (activities) config.activities = activities;
    if (devices) config.devices = devices;
    const r = run(config, files);
    return { r, ops: r.tree?.home.bindings[0]?.ops };
}
