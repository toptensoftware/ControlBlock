// Stage 1: the user's config -> a plain, fully specified tree shaped like the device structs
// (docs/config-format.md "What the pipeline produces"). Synchronous and pure: everything that
// touches the world comes in through `host`:
//   exists(path) -> bool       resolvePath(rel) -> path       readText(path) -> string
//   luaCheck(source) -> null | error message
// Never throws on bad input: problems are collected with their JSON paths.
// Images are left as definitions (stage 2 renders them).

import { BLOB_VERSION, FILE_VERSION, KEYS_PER_PAGE, KEY_COLS, GESTURES, LONG_PRESS_MIN, LONG_PRESS_MAX } from "./consts.js";
import { Diagnostics, childPath } from "./diag.js";
import { t, required, checkProps } from "./check.js";
import { resolveOpList } from "./ops.js";
import { BUTTON_LOOK_SPEC, DEFAULTS_SPEC, inheritLook, lookImages } from "./look.js";

const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);
const isNote = name => name.startsWith("_") || name.startsWith("$");
const realNames = map => Object.keys(map ?? {}).filter(name => !isNote(name));
const nonEmptyString = t.string({ nonEmpty: true });
const HOOKS = ["willDeactivate", "willActivate", "didActivate", "didDeactivate"];

const BUTTON_SPEC = { ...BUTTON_LOOK_SPEC, ...Object.fromEntries([...GESTURES, "action"].map(g => [g, t.any])) };
const BINDING_SPEC = { control: required(nonEmptyString), event: required(nonEmptyString), do: required(t.any) };
const emptyKey = () => ({ normalBg: null, normalFg: null, pressedBg: null, pressedFg: null, longPressMs: 0 });

const fileVersion = (v, p, d) =>
    t.integer()(v, p, d) && (v === FILE_VERSION || (d.error(p, `unsupported version ${v} (this tool reads version ${FILE_VERSION})`), false));

// The properties of `value` that pass `spec` (errors recorded), or null if it isn't an object
const section = (value, spec, path, diag) => checkProps(value, spec, path, diag).value;

export function resolve(config, host)
{
    const diag = new Diagnostics();
    const tree = build(config, host, diag);
    return { tree, errors: diag.errors, warnings: diag.warnings };
}

function build(config, host, diag)
{
    const root = section(config, {
        version: required(fileVersion),
        settings: t.any, defaults: t.any, devices: t.map(t.any), actions: t.map(t.any),
        home: required(t.any), activities: t.map(t.any),
    }, "", diag) ?? {};

    const settings = section(root.settings ?? {}, { longPressMs: t.integer(LONG_PRESS_MIN, LONG_PRESS_MAX) }, "settings", diag) ?? {};
    const rc = {
        diag, host,
        actions: root.actions ?? {},
        activityIndex: new Map(realNames(root.activities).map((name, i) => [name, i])),
        deviceNames: realNames(root.devices),
        usedActions: new Set(),
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

    warnUnused(tree, root, rc);
    return tree;
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
    return resolveKeys(h.keys, "home.keys", [rootDefaults, defaultsOf(h, "home")], 0, rc);
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
        }, path, rc.diag) ?? {};

        const defaults = defaultsOf(a, path);
        if (a.pages?.length === 0)
            rc.diag.error(childPath(path, "pages"), "an activity needs at least one page");

        const bindings = [];
        const pages = (a.pages ?? []).map((page, p) => {
            const pagePath = childPath(childPath(path, "pages"), p);
            const pg = section(page, { defaults: t.any, keys: t.any }, pagePath, rc.diag) ?? {};
            // The strip: Home is page 0, so this activity's page p starts at column KEY_COLS * (p + 1)
            const r = resolveKeys(pg.keys, childPath(pagePath, "keys"), [rootDefaults, defaults, defaultsOf(pg, pagePath)], KEY_COLS * (p + 1), rc);
            bindings.push(...r.bindings);
            return { keys: r.keys };
        });
        bindings.push(...resolveBindings(a.bindings ?? [], childPath(path, "bindings"), rc));

        const hooks = a.hooks ?? {};
        return {
            name,
            displayName: a.displayName ?? null,
            devices: (a.devices ?? []).map((d, i) => deviceIndexOf(d, childPath(childPath(path, "devices"), i), rc)),
            pages, bindings,
            ...Object.fromEntries(HOOKS.map(h => [h, resolveOpList(hooks[h], childPath(childPath(path, "hooks"), h), rc, "hook")])),
        };
    });
}

function deviceIndexOf(name, path, rc)
{
    const index = rc.deviceNames.indexOf(name);
    if (index < 0)
        rc.diag.error(path, `unknown device "${name}"`);
    return Math.max(index, 0);
}


function resolveBindings(list, path, rc)
{
    const out = [];
    list.forEach((b, i) => {
        const p = childPath(path, i);
        if (!checkProps(b, BINDING_SPEC, p, rc.diag).ok)
            return;
        if (b.control.startsWith("touch["))
            return void rc.diag.error(childPath(p, "control"), "touch keys are bound by the buttons themselves; use a gesture on the button");
        out.push({ control: b.control, event: b.event, ops: resolveOpList(b.do, childPath(p, "do"), rc, b.event) });
    });
    return out;
}

// ------------------------------------------------------------ keys

// -> { keys: key[15], bindings }. `chain`: defaults blocks from the root down; `colBase`: the
// column of this page's first key on the page strip.
function resolveKeys(list, path, chain, colBase, rc)
{
    const { diag } = rc;
    const keys = [], bindings = [], seen = new Map();
    const entries = list === undefined ? [] : Array.isArray(list) ? list : (diag.error(path, "expected an array"), []);
    if (entries.length > KEYS_PER_PAGE)
        diag.error(path, `at most ${KEYS_PER_PAGE} keys per page, got ${entries.length}`);

    entries.slice(0, KEYS_PER_PAGE).forEach((button, i) => {
        const kp = childPath(path, i);
        const b = button === null ? null : section(button, BUTTON_SPEC, kp, diag);
        if (!b)
            return void keys.push(emptyKey());

        // Gestures -> touch bindings
        const control = `touch[${colBase + (i % KEY_COLS)},${Math.floor(i / KEY_COLS)}]`;
        if (b.action !== undefined && b.tap !== undefined)
            diag.error(kp, "action and tap are the same gesture; set only one");
        const gestures = { ...b, tap: b.tap ?? b.action };
        const tapPath = b.tap !== undefined ? "tap" : "action";
        const keyBindings = [];
        for (const g of GESTURES)
        {
            const ops = resolveOpList(gestures[g], childPath(kp, g === "tap" ? tapPath : g), rc, g);
            if (ops.length)
                keyBindings.push({ control, event: g, ops });
        }
        if (b.long !== undefined && b.tap === undefined && b.action === undefined)
            diag.warn(kp, "has a long gesture but no tap");
        bindings.push(...keyBindings);

        // Look -> images
        const { press, release, tap, long, action, ...own } = b;
        const look = inheritLook(chain, own);
        const key = { ...lookImages(look, rc, kp), longPressMs: look.longPressMs ?? 0 };
        keys.push(key);

        // Identical buttons on one page
        if (!isBlank(key, keyBindings))
        {
            const signature = JSON.stringify([key, keyBindings.map(x => [x.event, x.ops])]);
            if (seen.has(signature))
                diag.warn(kp, `is the same as ${childPath(path, seen.get(signature))}`);
            else
                seen.set(signature, i);
        }
    });

    while (keys.length < KEYS_PER_PAGE)
        keys.push(emptyKey());
    return { keys, bindings };
}

const isBlank = (key, bindings) => bindings.length === 0 && !key.normalBg && !key.normalFg && !key.pressedBg && !key.pressedFg;

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
