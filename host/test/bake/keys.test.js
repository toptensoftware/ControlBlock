// Stage 1, keys: placement on the grid (by slot, or by x/y/w/h in units of 0.25), ids, rect checks,
// groups (one picture split into keys) and the touch bindings by id (docs/layout-format.md "Keys").
import { test } from "node:test";
import assert from "node:assert/strict";
import { run, ok, errorText, warningText } from "./helpers.js";
import { exampleKeys } from "./fixtures.js";

const delay = ms => ({ op: "delay", ms });
const home = keys => ({ version: 1, home: { keys } });
// [id, x, y, w, h] of each key (the rect in quarter units, as in the blob)
const rects = keys => keys.map(k => [k.id, ...k.rect]);
const errs = keys => errorText(run(home(keys)));

test("a plain grid: entry i is slot i (row-major, 5x3), a 1U key with id i + 1; null leaves the slot empty", () => {
    const t = ok(home([{}, null, { label: "c" }, ...new Array(11).fill(null), { label: "o" }]));
    assert.deepEqual(rects(t.home.keys), [[1, 0, 0, 4, 4], [3, 8, 0, 4, 4], [15, 16, 8, 4, 4]]);
    assert.equal(t.home.keys[0].longPressMs, 0);
});

test("plain grid ids continue across pages: Home is page 0, then every activity's pages in file order", () => {
    const t = ok({ version: 1, home: { keys: [{}] }, activities: {
        A: { pages: [{ keys: [{}] }, { keys: [null, {}] }] },
        B: { pages: [{ keys: [null, null, {}] }] },
    } });
    assert.deepEqual(t.home.keys.map(k => k.id), [1]);
    assert.deepEqual(t.activities[0].pages.map(p => p.keys.map(k => k.id)), [[16], [32]]);
    assert.deepEqual(t.activities[1].pages[0].keys.map(k => k.id), [48]);
});

test("x, y, w, h place and size a key in grid units, multiples of 0.25 (quarter counts in the tree)", () => {
    const t = ok(home([{ x: 1.5, y: 0.25, w: 2, h: 0.75 }, { x: 0, y: 2, w: 5, h: 1 }]));
    assert.deepEqual(t.home.keys.map(k => k.rect), [[6, 1, 8, 3], [0, 8, 20, 4]]);
});

test("w and h without x and y: a span from the entry's slot", () => {
    const t = ok(home([{ w: 2 }, null, { h: 2 }, { w: 2, h: 0.5 }]));
    assert.deepEqual(rects(t.home.keys), [[1, 0, 0, 8, 4], [3, 8, 0, 4, 8], [4, 12, 0, 8, 2]]);
});

test("ids: a key on a whole-unit corner gets that slot's id; a layout may name its own", () => {
    const t = ok(home([null, null, { x: 3, y: 1 }, { id: 500 }, { x: 0, y: 2, id: 2 }]));
    // (3, 1) is slot 8: id 9; entry 3 keeps slot 3's place but names id 500
    assert.deepEqual(rects(t.home.keys), [[9, 12, 4, 4, 4], [500, 12, 0, 4, 4], [2, 0, 8, 4, 4]]);
});

test("ids: other keys get the lowest ids above every page's plain grid ids, in layout order", () => {
    const t = ok({ version: 1, home: { keys: [{ x: 0.5, y: 0, w: 0.5, h: 0.5 }, { x: 1, y: 0.5, h: 0.5 }] },
                   activities: { A: { pages: [{ keys: [{ id: 31 }] }] } } });
    // 2 pages: plain grid ids 1..30; 31 is taken by name, so 32 and 33
    assert.deepEqual(t.home.keys.map(k => k.id), [32, 33]);
});

test("ids are unique across the whole layout", () => {
    const r = run({ version: 1, home: { keys: [{ id: 7 }] }, activities: { A: { pages: [{ keys: [{ id: 7 }] }] } } });
    assert.match(errorText(r), /activities\.A\.pages\[0\]\.keys\[0\]\.id: id 7 is already used by home\.keys\[0\]/);
    // a named id may not take another key's plain grid id
    assert.match(errs([{}, { id: 1 }]), /home\.keys\[1\]\.id: id 1 is already used by home\.keys\[0\]/);
    assert.match(errs([{ id: 0 }]), /home\.keys\[0\]\.id/);
    assert.match(errs([{ id: 65536 }]), /home\.keys\[0\]\.id/);
    assert.match(errs([{ id: 1.5 }]), /home\.keys\[0\]\.id/);
});

test("rects: whole quarters, inside the 5x3 grid, no overlaps", () => {
    assert.match(errs([{ x: 0.3, y: 0 }]), /home\.keys\[0\]\.x: .*0\.25/);
    assert.match(errs([{ w: 0 }]), /home\.keys\[0\]\.w/);
    assert.match(errs([{ x: -1, y: 0 }]), /home\.keys\[0\]\.x/);
    assert.match(errs([{ h: "1" }]), /home\.keys\[0\]\.h/);
    assert.match(errs([{ x: 4.5, y: 0 }]), /home\.keys\[0\]: .*outside the 5x3 grid/);
    assert.match(errs([null, null, null, null, { w: 2 }]), /home\.keys\[4\]: .*outside/);
    assert.match(errs([{ x: 0, y: 2.5, h: 1 }]), /home\.keys\[0\]: .*outside/);
    assert.match(errs([{ w: 2 }, {}]), /home\.keys\[1\]: overlaps home\.keys\[0\]/);
    assert.match(errs([{}, { x: 0.75, y: 0.75 }]), /home\.keys\[1\]: overlaps home\.keys\[0\]/);
    assert.equal(run(home([{ w: 0.5 }, { x: 0.5, y: 0, w: 0.5 }])).errors.length, 0);   // abutting is fine
});

test("x and y go together; entries past the 15 slots need them; at most 32 keys a page", () => {
    assert.match(errs([{ x: 1 }]), /home\.keys\[0\]: x and y/);
    assert.match(errs([{ y: 1 }]), /home\.keys\[0\]: x and y/);
    const sixteen = new Array(15).fill(null).concat([{}]);
    assert.match(errs(sixteen), /home\.keys\[15\]: .*x and y/);
    sixteen[15] = { x: 0, y: 0 };
    assert.equal(run(home(sixteen)).errors.length, 0);

    // 33 quarter keys: one too many
    const quarters = n => Array.from({ length: n }, (_, i) => ({ x: (i % 20) / 4, y: Math.floor(i / 20) / 4, w: 0.25, h: 0.25 }));
    assert.equal(run(home(quarters(32))).errors.length, 0);
    assert.match(errs(quarters(33)), /home\.keys: at most 32 keys a page, got 33/);
    assert.match(errs([5]), /home\.keys\[0\]/);
    assert.match(errorText(run({ version: 1, home: { keys: {} } })), /home\.keys/);
});

test("touch bindings name the key's id: touch[id]", () => {
    const t = ok({ version: 1,
        home: { keys: [{ tap: delay(1) }, null, null, null, null, null, null, { tap: delay(2) }, { id: 900, x: 4, y: 2, tap: delay(3) }] },
        activities: { A: { pages: [{ keys: [null, null, null, null, { tap: delay(4) }] }, { keys: [{ long: delay(5), tap: delay(6) }] }] } } });
    assert.deepEqual(t.home.bindings.map(b => [b.control, b.event, b.ops[0].duration]),
        [["touch[1]", "tap", 1], ["touch[8]", "tap", 2], ["touch[900]", "tap", 3]]);
    assert.deepEqual(t.activities[0].bindings.map(b => `${b.control}/${b.event}`), ["touch[20]/tap", "touch[31]/tap", "touch[31]/long"]);
});

// ---------------------------------------------------------------- groups

test("a group: one picture split into keys, each with its own rect (relative to the group), id and gestures", () => {
    const t = ok(home([null, null, null, null, {
        h: 2, label: "Volume", color: "#333333",
        keys: [ { label: "+", tap: delay(1) }, { y: 1, label: "-", tap: delay(2) } ],
    }]));
    assert.deepEqual(rects(t.home.keys), [[5, 16, 0, 4, 4], [10, 16, 4, 4, 4]]);
    assert.deepEqual(t.home.bindings.map(b => b.control), ["touch[5]", "touch[10]"]);
});

test("group members: inside the group, not overlapping each other; 0.25 steps", () => {
    const group = keys => [{ w: 2, color: "red", keys }];
    assert.equal(run(home(group([{ w: 0.5 }, { x: 0.5, w: 1.5 }]))).errors.length, 0);
    assert.match(errs(group([{ x: 1.5 }])), /home\.keys\[0\]\.keys\[0\]: .*outside its group/);
    assert.match(errs(group([{}, { x: 0.5 }])), /home\.keys\[0\]\.keys\[1\]: overlaps home\.keys\[0\]\.keys\[0\]/);
    assert.match(errs(group([{ x: 0.1 }])), /home\.keys\[0\]\.keys\[0\]\.x/);
    assert.match(errs(group([])), /home\.keys\[0\]\.keys: at least 1/);
    // the group is one item on the page: nothing may overlap it
    assert.match(errs([...group([{}]), { x: 1.5, y: 0 }]), /home\.keys\[1\]: overlaps home\.keys\[0\]/);
    // members count towards the page's 32 keys
    const many = Array.from({ length: 33 }, (_, i) => ({ x: (i % 20) / 4, y: Math.floor(i / 20) / 4, w: 0.25, h: 0.25 }));
    assert.match(errorText(run(home([{ w: 5, h: 3, keys: many }]))), /home\.keys: at most 32 keys a page, got 33/);
});

test("a group draws the face: its keys may not set one; a group has no gestures, id or nested groups", () => {
    const g = (member, extra = {}) => errs([{ w: 2, keys: [member], ...extra }]);
    assert.match(g({ color: "red" }), /home\.keys\[0\]\.keys\[0\]\.color: .*group/);
    assert.match(g({ face: "square" }), /home\.keys\[0\]\.keys\[0\]\.face: .*group/);
    assert.match(g({ image: { normalBg: null } }), /home\.keys\[0\]\.keys\[0\]\.image: .*group/);
    assert.match(g({ pressed: { color: "red" } }), /home\.keys\[0\]\.keys\[0\]\.pressed\.color: .*group/);
    assert.match(g({}, { tap: delay(1) }), /home\.keys\[0\]\.tap: unknown property/);
    assert.match(g({}, { id: 4 }), /home\.keys\[0\]\.id: unknown property/);
    assert.match(g({ keys: [{}] }), /home\.keys\[0\]\.keys\[0\]\.keys: unknown property/);
});

test("warning: part of a group no key covers (it shows black)", () => {
    const r = run(home([{ w: 2, color: "red", keys: [{}] }]));
    assert.equal(r.errors.length, 0);
    assert.match(warningText(r), /home\.keys\[0\]: .*not covered/);
});

test("group members inherit the group's look but not its label or icon", () => {
    const [a] = ok(home([{ w: 2, label: "Vol", icon: "#x", iconColor: "red", labelColor: "blue", keys: [{ icon: "#add" }, { x: 1, label: "-" }] }])).home.keys;
    const fgs = a.normal.layers.filter(l => l.layer === "fg").map(l => l.def);
    assert.equal(fgs.length, 2);                                   // the group's, then the key's
    assert.equal(fgs[0].text, "Vol");
    assert.equal(fgs[1].svgFile, "#add");
    assert.equal(fgs[1].svgForeColor, "red");                      // inherited from the group
    assert.equal(fgs[1].text, undefined);                          // the group's label isn't the key's
});

test("the key placement example (for docs/layout-format.md) resolves", () => {
    const t = ok(exampleKeys());
    assert.deepEqual(rects(t.home.keys), [
        [1, 0, 0, 8, 4],          // Watch TV: 2U, slot 0
        [3, 8, 0, 4, 4],          // slot 2
        [5, 16, 0, 4, 4],         // Volume group, top half: +
        [10, 16, 4, 4, 4],        //                bottom half: -
        [6, 0, 4, 4, 2],          // two half-height keys in slot 5
        [16, 0, 6, 4, 2],
        [200, 4, 8, 12, 4],       // a 3U key with its own id
    ]);
    assert.deepEqual(t.home.bindings.map(b => b.control), ["touch[1]", "touch[3]", "touch[5]", "touch[10]", "touch[6]", "touch[16]", "touch[200]"]);
});
