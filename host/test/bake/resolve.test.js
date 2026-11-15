// Stage 1: structure, names -> indices, generated bindings, validation and warnings.
import { test } from "node:test";
import assert from "node:assert/strict";
import { run, ok, errorText, errorPaths, warningText } from "./helpers.js";
import { OP, BLOB_VERSION } from "../../src/bake/consts.js";

const delay = ms => ({ op: "delay", ms });
const act = (extra = {}) => ({ pages: [{ keys: [] }], ...extra });

test("a minimal layout resolves to an empty Home", () => {
    const t = ok({ version: 1, home: {} });
    assert.equal(t.version, BLOB_VERSION);
    assert.equal(BLOB_VERSION, 3);
    assert.equal(t.longPressMs, 0);
    assert.deepEqual(t.home.keys, []);
    assert.deepEqual(t.home.bindings, []);
    assert.deepEqual(t.devices, []);
    assert.deepEqual(t.activities, []);
});

test("root: version and home are required; unknown properties and bad version are errors", () => {
    assert.match(errorText(run({ home: {} })), /version.*required/);
    assert.match(errorText(run({ version: 1 })), /home.*required/);
    assert.match(errorText(run({ version: 2, home: {} })), /version/);
    assert.match(errorText(run({ version: 1, home: {}, hoem: 1 })), /hoem: unknown property/);
    assert.match(errorText(run(null)), /object/);
    assert.equal(run({ version: 1, home: {}, _note: "x", $schema: "y" }).errors.length, 0);
});

test("settings.longPressMs goes to the root; a key's own value wins", () => {
    const t = ok({ version: 1, settings: { longPressMs: 600 }, home: { keys: [{ longPressMs: 800 }, {}] } });
    assert.equal(t.longPressMs, 600);
    assert.equal(t.home.keys[0].longPressMs, 800);
    assert.equal(t.home.keys[1].longPressMs, 0);   // 0 = use the root's
    assert.match(errorText(run({ version: 1, settings: { longPressMs: 5 }, home: {} })), /longPressMs/);
    assert.match(errorText(run({ version: 1, settings: { nope: 1 }, home: {} })), /settings\.nope/);
});

test("binding order: page, key, then press/release/tap/long; the activity's own bindings follow", () => {
    const t = ok({
        version: 1, home: {},
        activities: { A: {
            pages: [{ keys: [{ long: delay(4), tap: delay(3), release: delay(2), press: delay(1) }, { tap: delay(5) }] }],
            bindings: [{ control: "key1", event: "tap", do: delay(6) }],
        } },
    });
    assert.deepEqual(t.activities[0].bindings.map(b => `${b.control}/${b.event}`),
        ["touch[16]/press", "touch[16]/release", "touch[16]/tap", "touch[16]/long", "touch[17]/tap", "key1/tap"]);
    assert.deepEqual(t.activities[0].bindings.map(b => b.ops[0].duration), [1, 2, 3, 4, 5, 6]);
});

test("action is an alias for tap; both together is an error", () => {
    const t = ok({ version: 1, home: { keys: [{ action: delay(7) }] } });
    assert.deepEqual(t.home.bindings.map(b => b.event), ["tap"]);
    assert.match(errorText(run({ version: 1, home: { keys: [{ action: delay(1), tap: delay(2) }] } })), /home\.keys\[0\]: .*action.*tap/);
});

test("devices and activities keep file order; names become indices", () => {
    const t = ok({
        version: 1, home: {},
        devices: { tv: { turnOn: delay(1), turnOff: [delay(2)] }, avr: {} },
        activities: {
            Watch: act({ displayName: "TV", devices: ["avr", "tv"], hooks: { didActivate: delay(3), willDeactivate: [delay(4), delay(5)] } }),
            Listen: act(),
        },
    });
    assert.deepEqual(t.devices.map(d => d.name), ["tv", "avr"]);
    assert.equal(t.devices[0].turnOn[0].duration, 1);
    assert.deepEqual(t.devices[1].turnOn, []);
    assert.deepEqual(t.activities.map(a => a.name), ["Watch", "Listen"]);
    assert.equal(t.activities[0].displayName, "TV");
    assert.equal(t.activities[1].displayName, null);
    assert.deepEqual(t.activities[0].devices, [1, 0]);
    assert.deepEqual(t.activities[0].didActivate.map(o => o.duration), [3]);
    assert.deepEqual(t.activities[0].willDeactivate.map(o => o.duration), [4, 5]);
    assert.deepEqual(t.activities[0].willActivate, []);
    assert.deepEqual(t.activities[0].didDeactivate, []);
    assert.equal(t.activities[0].pages.length, 1);
    assert.deepEqual(t.activities[0].pages[0].keys, []);
    assert.deepEqual(t.activities[0].controls, []);     // pad control timings (none yet)
});

test("activity errors: no pages, unknown device, unknown hook, unknown property", () => {
    assert.match(errorText(run({ version: 1, home: {}, activities: { A: { pages: [] } } })), /activities\.A\.pages: .*at least one/);
    assert.match(errorText(run({ version: 1, home: {}, activities: { A: {} } })), /pages.*required/);
    assert.match(errorText(run({ version: 1, home: {}, activities: { A: act({ devices: ["ghost"] }) } })), /ghost/);
    assert.match(errorText(run({ version: 1, home: {}, activities: { A: act({ hooks: { didStuff: [] } }) } })), /hooks\.didStuff/);
    assert.match(errorText(run({ version: 1, home: {}, activities: { A: act({ extra: 1 }) } })), /activities\.A\.extra/);
    assert.match(errorText(run({ version: 1, home: {}, activities: { "": act() } })), /name/);
});

test("Home has only keys and defaults", () => {
    assert.match(errorText(run({ version: 1, home: { bindings: [] } })), /home\.bindings: unknown property/);
    assert.match(errorText(run({ version: 1, home: { pages: [] } })), /home\.pages/);
});

test("activity bindings: control/event/do; touch controls are not allowed", () => {
    const t = ok({ version: 1, home: {}, activities: { A: act({ bindings: [{ control: "enc1", event: "turn", do: delay(1) }] }) } });
    assert.deepEqual(t.activities[0].bindings.map(b => [b.control, b.event]), [["enc1", "turn"]]);
    const bad = b => errorText(run({ version: 1, home: {}, activities: { A: act({ bindings: [b] }) } }));
    assert.match(bad({ control: "touch[1,1]", event: "tap", do: delay(1) }), /activities\.A\.bindings\[0\]\.control: .*touch/);
    assert.match(bad({ event: "tap", do: delay(1) }), /control.*required/);
    assert.match(bad({ control: "m", do: delay(1) }), /event.*required/);
    assert.match(bad({ control: "m", event: "tap" }), /do.*required/);
});

test("a binding with an empty op list is kept (it still consumes the trigger)", () => {
    const t = ok({ version: 1, home: {}, activities: { A: act({ bindings: [{ control: "m", event: "tap", do: [] }] }) } });
    assert.equal(t.activities[0].bindings.length, 1);
    assert.deepEqual(t.activities[0].bindings[0].ops, []);
});

test("switchActivity ops see all activity names, including later ones", () => {
    const t = ok({
        version: 1,
        home: { keys: [{ tap: { op: "switchActivity", activity: "Late" } }] },
        activities: { Early: act(), Late: act() },
    });
    assert.deepEqual(t.home.bindings[0].ops, [{ op: OP.switch_activity, index: 1 }]);
});

test("all errors are collected, not just the first", () => {
    const r = run({
        version: 1,
        home: { keys: [{ icon: "#x", iconScale: "a" }, { tap: { op: "delay", ms: -1 } }] },
        activities: { A: act({ devices: ["ghost"] }) },
        bogus: 1,
    });
    assert.deepEqual(errorPaths(r).sort(), ["activities.A.devices[0]", "bogus", "home.keys[0].iconScale", "home.keys[1].tap.ms"]);
});

test("the result is plain data: tree has no undefined and survives a JSON round trip for non-Buffer parts", () => {
    const t = ok({ version: 1, home: { keys: [{ color: "red", icon: "#a", label: "x", tap: delay(1) }] } });
    const copy = JSON.parse(JSON.stringify(t));
    assert.deepEqual(copy.home.keys[0], t.home.keys[0]);
});

test("resolve does not modify its input", () => {
    const cfg = { version: 1, defaults: { color: "red" }, actions: { a: delay(1) },
                  home: { keys: [{ tap: { use: "a" }, icon: "#x" }] }, activities: { A: act({ devices: [] }) } };
    const before = JSON.stringify(cfg);
    ok(cfg);
    assert.equal(JSON.stringify(cfg), before);
});

// ---------------------------------------------------------------- warnings

test("warning: long without tap", () => {
    const r = run({ version: 1, home: { keys: [{ long: delay(1) }] } });
    assert.equal(r.errors.length, 0);
    assert.match(warningText(r), /home\.keys\[0\]: .*long.*tap/);
    assert.equal(run({ version: 1, home: { keys: [{ long: delay(1), tap: delay(2) }] } }).warnings.length, 0);
});

test("warning: unused device and unused action", () => {
    const r = run({ version: 1, home: {}, devices: { tv: {} }, actions: { a: [] } });
    assert.match(warningText(r), /devices\.tv: .*no activity/);
    assert.match(warningText(r), /actions\.a: .*never used/);
    const used = run({ version: 1, home: { keys: [{ tap: { use: "a" } }] }, devices: { tv: {} }, actions: { a: [] },
                       activities: { A: act({ devices: ["tv"] }) } });
    assert.equal(used.warnings.length, 0, warningText(used));
});

test("warning: two buttons on a page with identical look and actions", () => {
    const b = () => ({ icon: "#x", tap: delay(1) });
    const r = run({ version: 1, home: { keys: [b(), b()] } });
    assert.match(warningText(r), /home\.keys\[1\]: .*same as.*\[0\]/);
    // empty buttons are not flagged, nor are different actions
    assert.equal(run({ version: 1, home: { keys: [{}, {}, { icon: "#x", tap: delay(1) }, { icon: "#x", tap: delay(2) }] } }).warnings.length, 0);
});

// ---------------------------------------------------------------- pad controls

const padAct = (pad, bindings = []) => ({ version: 1, home: {}, activities: { A: act({ pad, bindings }) } });
const bind = (control, event, ops = delay(1)) => ({ control, event, do: ops });
const hold = { op: "hidKey", keys: "a", mode: "hold" };

test("an activity's pad map becomes its controls table, in control order", () => {
    const t = ok(padAct({ key3: { repeat: 100 }, key1: { longPressMs: 800 }, key12: { repeat: { initialDelay: 500, period: 100 } } },
                        [bind("key1", "long"), bind("key3", "press"), bind("key12", "tap")]));
    assert.deepEqual(t.activities[0].controls, [
        { control: "key1", longPressMs: 800, repeatDelayMs: 0, repeatMs: 0 },
        { control: "key3", longPressMs: 0, repeatDelayMs: 100, repeatMs: 100 },
        { control: "key12", longPressMs: 0, repeatDelayMs: 500, repeatMs: 100 },
    ]);
    assert.deepEqual(ok({ version: 1, home: {}, activities: { A: act() } }).activities[0].controls, []);
    assert.deepEqual(ok(padAct({ _note: "x" })).activities[0].controls, []);
});

test("pad names are key1..key32; entries take repeat and longPressMs only", () => {
    for (const name of ["key0", "key33", "key", "enc1", "Key1", "key01"])
        assert.match(errorText(run(padAct({ [name]: { repeat: 100 } }))), new RegExp(`activities\\.A\\.pad\\.${name}: .*key1\\.\\.key32`));
    assert.match(errorText(run(padAct({ key1: { rate: 1 } }, [bind("key1", "tap")]))), /activities\.A\.pad\.key1\.rate: unknown property/);
    assert.match(errorText(run(padAct({ key1: { repeat: 20 } }, [bind("key1", "tap")]))), /activities\.A\.pad\.key1\.repeat: must be between/);
    assert.match(errorText(run(padAct({ key1: { longPressMs: 5 } }, [bind("key1", "tap")]))), /activities\.A\.pad\.key1\.longPressMs/);
    assert.match(errorText(run(padAct({ key1: 100 }))), /activities\.A\.pad\.key1: expected an object/);
    assert.match(errorText(run(padAct([]))), /activities\.A\.pad/);
    assert.equal(run(padAct({ key32: { repeat: 100 } }, [bind("key32", "tap")])).errors.length, 0);
});

test("pad errors: a repeating control has no long press (binding or longPressMs) and no hidKey hold under press", () => {
    assert.match(errorText(run(padAct({ key3: { repeat: 100 } }, [bind("key3", "tap"), bind("key3", "long")]))),
        /activities\.A\.bindings\[1\]: .*repeat/);
    assert.match(errorText(run(padAct({ key3: { repeat: 100, longPressMs: 800 } }, [bind("key3", "tap")]))),
        /activities\.A\.pad\.key3\.longPressMs: .*repeat/);
    assert.match(errorText(run(padAct({ key3: { repeat: 100 } }, [bind("key3", "press", hold)]))),
        /activities\.A\.bindings\[0\]: .*hold/);
    // another control, a non-repeating one, or a hold under release may
    const fine = run(padAct({ key3: { repeat: 100 } }, [bind("key3", "tap"), bind("key4", "long"), bind("key4", "press", hold), bind("key3", "release", hold)]));
    assert.equal(fine.errors.length, 0, errorText(fine));
    assert.equal(run(padAct({ key3: { longPressMs: 900 } }, [bind("key3", "long")])).errors.length, 0);
});

test("warning: a pad entry for a control with no binding in the activity", () => {
    const r = run(padAct({ key3: { repeat: 100 } }, [bind("key4", "tap")]));
    assert.equal(r.errors.length, 0);
    assert.match(warningText(r), /activities\.A\.pad\.key3: .*no binding/);
    assert.equal(run(padAct({ key3: { repeat: 100 } }, [bind("key3", "tap")])).warnings.length, 0);
});

test("pad is only for activities", () => {
    assert.match(errorText(run({ version: 1, home: { pad: {} } })), /home\.pad: unknown property/);
});
