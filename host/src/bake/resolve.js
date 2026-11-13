// Stage 1: the user's layout -> a plain, fully specified tree shaped like the device structs
// (docs/layout-format.md "What the pipeline produces"). Synchronous and pure: everything that
// touches the world comes in through `host`:
//   exists(path) -> bool       resolvePath(rel) -> path       readText(path) -> string
//   luaCheck(source) -> null | error message
// Never throws on bad input: problems are collected with their JSON paths.
// Images are left as specs (layer definitions at the key's pixel size; stage 2 renders them).

import { BLOB_VERSION, FILE_VERSION, LONG_PRESS_MIN, LONG_PRESS_MAX, MAX_CONTROLS } from "./consts.js";
import { Diagnostics, childPath } from "./diag.js";
import { t, required, checkProps } from "./check.js";
import { resolveOpList } from "./ops.js";
import { DEFAULTS_SPEC } from "./look.js";
import { resolvePage, assignIds, touchBindings, repeatCheck, timingOf, checkRepeatGesture } from "./keys.js";
import { setKeyPath } from "./tree.js";

const isNote = name => name.startsWith("_") || name.startsWith("$");
const realNames = map => Object.keys(map ?? {}).filter(name => !isNote(name));
const nonEmptyString = t.string({ nonEmpty: true });
const HOOKS = ["willDeactivate", "willActivate", "didActivate", "didDeactivate"];

const BINDING_SPEC = { control: required(nonEmptyString), event: required(nonEmptyString), do: required(t.any) };
const longPressMs = t.integer(LONG_PRESS_MIN, LONG_PRESS_MAX);
const PAD_SPEC = { repeat: repeatCheck, longPressMs };

// A macro pad key's number (key1..key32), or 0 for any other name
const padKeyNumber = name => {
    const m = /^key([1-9][0-9]?)$/.exec(name);
    return m && Number(m[1]) <= MAX_CONTROLS ? Number(m[1]) : 0;
};

const fileVersion = (v, p, d) =>
    t.integer()(v, p, d) && (v === FILE_VERSION || (d.error(p, `unsupported version ${v} (this tool reads version ${FILE_VERSION})`), false));

// The properties of `value` that pass `spec` (errors recorded), or null if it isn't an object
const section = (value, spec, path, diag) => checkProps(value, spec, path, diag).value;

export function resolve(layout, host)
{
    const diag = new Diagnostics();
    const tree = build(layout, host, diag);
    return { tree, errors: diag.errors, warnings: diag.warnings };
}

function build(layout, host, diag)
{
    const root = section(layout, {
        version: required(fileVersion),
        settings: t.any, defaults: t.any, devices: t.map(t.any), actions: t.map(t.any),
        home: required(t.any), activities: t.map(t.any),
    }, "", diag) ?? {};

    const settings = section(root.settings ?? {}, { longPressMs }, "settings", diag) ?? {};
    const rc = {
        diag, host,
        actions: root.actions ?? {},
        activityIndex: new Map(realNames(root.activities).map((name, i) => [name, i])),
        deviceNames: realNames(root.devices),
        usedActions: new Set(),
        pages: [],              // every page's resolved keys, in layout order (Home first)
        touchOwners: [],        // { owner, keys }: Home and each activity, whose bindings start with its keys' touch bindings
    };
    // A defaults block (validated here, once) or {}
    const defaultsOf = (owner, path) =>
        owner?.defaults === undefined ? {} : section(owner.defaults, DEFAULTS_SPEC, childPath(path, "defaults"), diag) ?? {};

    const rootDefaults = defaultsOf(root, "");
    checkActions(rc);

    const tree = {
        version: BLOB_VERSION,
        longPressMs: settings.longPressMs ?? 0,
        home: resolveHome(root.home, rootDefaults, rc, defaultsOf),
        devices: resolveDevices(root.devices ?? {}, rc),
        activities: resolveActivities(root.activities ?? {}, rootDefaults, rc, defaultsOf),
    };

    finishKeys(rc);
    warnUnused(tree, root, rc);
    return tree;
}

// Once every page is resolved: key ids (unique across the layout), then the touch bindings by id
function finishKeys(rc)
{
    assignIds(rc.pages, rc.diag);
    for (const { owner, keys } of rc.touchOwners)
    {
        owner.bindings = [...touchBindings(keys), ...owner.bindings];
        for (const { key, path } of keys)
            setKeyPath(key, path);
    }
}

// One page's keys, numbered across the layout (Home is page 0) -> its resolved keys
function resolvePageKeys(list, path, chain, rc)
{
    const keys = resolvePage(list, path, chain, rc.pages.length, rc);
    rc.pages.push(keys);
    return keys;
}

// Every action is checked on its own (in a throwaway "used" set, so only real uses count)
function checkActions(rc)
{
    const used = rc.usedActions;
    rc.usedActions = new Set();
    for (const name of Object.keys(rc.actions))
        if (!name.startsWith("_") && !name.startsWith("$"))
            resolveOpList(rc.actions[name], childPath("actions", name), rc, undefined, [name]);
    rc.usedActions = used;
}

// ------------------------------------------------------------ home and activities

function resolveHome(home, rootDefaults, rc, defaultsOf)
{
    const h = section(home ?? {}, { defaults: t.any, keys: t.any }, "home", rc.diag) ?? {};
    const keys = resolvePageKeys(h.keys, "home.keys", [rootDefaults, defaultsOf(h, "home")], rc);
    const out = { keys: keys.map(k => k.key), bindings: [] };
    rc.touchOwners.push({ owner: out, keys });
    return out;
}

function resolveDevices(devices, rc)
{
    return Object.entries(devices).filter(([name]) => !isNote(name)).map(([name, device]) => {
        const path = childPath("devices", name);
        const d = section(device, { turnOn: t.any, turnOff: t.any }, path, rc.diag) ?? {};
        if (name === "")
            rc.diag.error(path, "device names must not be empty");
        return {
            name,
            turnOn: resolveOpList(d.turnOn, childPath(path, "turnOn"), rc, "hook"),
            turnOff: resolveOpList(d.turnOff, childPath(path, "turnOff"), rc, "hook"),
        };
    });
}

function resolveActivities(activities, rootDefaults, rc, defaultsOf)
{
    return Object.entries(activities).filter(([name]) => !isNote(name)).map(([name, activity]) => {
        const path = childPath("activities", name);
        if (name === "")
            rc.diag.error(path, "activity names must not be empty");
        const a = section(activity, {
            displayName: nonEmptyString,
            devices: t.array(nonEmptyString),
            defaults: t.any,
            hooks: t.object(Object.fromEntries(HOOKS.map(h => [h, t.any]))),
            pages: required(t.array(t.any)),
            bindings: t.array(t.any),
            pad: t.map(t.any),
        }, path, rc.diag) ?? {};

        const defaults = defaultsOf(a, path);
        if (a.pages?.length === 0)
            rc.diag.error(childPath(path, "pages"), "an activity needs at least one page");

        const touchKeys = [];
        const pages = (a.pages ?? []).map((page, p) => {
            const pagePath = childPath(childPath(path, "pages"), p);
            const pg = section(page, { defaults: t.any, keys: t.any }, pagePath, rc.diag) ?? {};
            const keys = resolvePageKeys(pg.keys, childPath(pagePath, "keys"), [rootDefaults, defaults, defaultsOf(pg, pagePath)], rc);
            touchKeys.push(...keys);
            return { keys: keys.map(k => k.key) };
        });
        // the touch bindings go in front once the keys have ids (finishKeys)
        const pad = resolvePad(a.pad ?? {}, childPath(path, "pad"), rc);
        const bindings = resolveBindings(a.bindings ?? [], childPath(path, "bindings"), rc, pad);
        for (const { control, path: p } of pad)
            if (!bindings.some(b => b.control === control.control))
                rc.diag.warn(p, `${control.control} has no binding in this activity`);

        const hooks = a.hooks ?? {};
        const out = {
            name,
            displayName: a.displayName ?? null,
            devices: (a.devices ?? []).map((d, i) => deviceIndexOf(d, childPath(childPath(path, "devices"), i), rc)),
            pages, bindings,
            controls: pad.map(c => c.control),
            ...Object.fromEntries(HOOKS.map(h => [h, resolveOpList(hooks[h], childPath(childPath(path, "hooks"), h), rc, "hook")])),
        };
        rc.touchOwners.push({ owner: out, keys: touchKeys });
        return out;
    });
}

function deviceIndexOf(name, path, rc)
{
    const index = rc.deviceNames.indexOf(name);
    if (index < 0)
        rc.diag.error(path, `unknown device "${name}"`);
    return Math.max(index, 0);
}


// An activity's `pad` map (macro pad keys' timing) -> [{ control (a controlTiming), path }] in key order
function resolvePad(pad, path, rc)
{
    return realNames(pad).flatMap(name => {
        const p = childPath(path, name);
        const n = padKeyNumber(name);
        if (!n)
            return rc.diag.error(p, `pad entries are macro pad keys, key1..key${MAX_CONTROLS}`), [];
        const own = section(pad[name], PAD_SPEC, p, rc.diag);
        return own ? [{ n, path: p, control: { control: name, ...timingOf(own, own.longPressMs, p, rc.diag) } }] : [];
    }).sort((a, b) => a.n - b.n);
}

// `pad`: the activity's resolvePad result (a repeating control's bindings are checked against it)
function resolveBindings(list, path, rc, pad)
{
    const repeating = new Set(pad.filter(c => c.control.repeatMs).map(c => c.control.control));
    const out = [];
    list.forEach((b, i) => {
        const p = childPath(path, i);
        if (!checkProps(b, BINDING_SPEC, p, rc.diag).ok)
            return;
        if (b.control.startsWith("touch["))
            return void rc.diag.error(childPath(p, "control"), "touch keys are bound by the buttons themselves; use a gesture on the button");
        const ops = resolveOpList(b.do, childPath(p, "do"), rc, b.event);
        if (repeating.has(b.control))
            checkRepeatGesture(b.event, ops, p, rc.diag);
        out.push({ control: b.control, event: b.event, ops });
    });
    return out;
}

// ------------------------------------------------------------ warnings

function warnUnused(tree, root, rc)
{
    const usedDevices = new Set(tree.activities.flatMap(a => a.devices));
    tree.devices.forEach((d, i) => {
        if (!usedDevices.has(i))
            rc.diag.warn(childPath("devices", d.name), "used by no activity");
    });
    for (const name of Object.keys(rc.actions))
        if (!isNote(name) && !rc.usedActions.has(name))
            rc.diag.warn(childPath("actions", name), "never used");
}
