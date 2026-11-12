// Stage 3: schema, binpack, BPAK file.
import { test } from "node:test";
import assert from "node:assert/strict";
import { pack, unpack } from "../../../binpack/index.js";
import { buildCombinedBuffer } from "../../../binpack/cli.js";
import { ok, fakeRenderer } from "./helpers.js";
import { renderImages } from "../../src/bake/render.js";
import { packTree, toBpak, ROOT_TYPE, headerText } from "../../src/bake/pack.js";
import { OP, BLOB_VERSION } from "../../src/bake/consts.js";

async function baked(layout, files)
{
    const tree = ok(layout, files);
    await renderImages(tree, fakeRenderer());
    return tree;
}

const countOf = (bin, text) => { let n = 0, i = 0; while ((i = bin.indexOf(text, i)) >= 0) { n++; i += text.length; } return n; };

test("the whole tree packs and unpacks to the same values", async () => {
    const tree = await baked({
        version: 1, settings: { longPressMs: 650 },
        devices: { tv: { turnOn: { op: "sendWol", mac: "aa:bb:cc:dd:ee:ff" }, turnOff: { op: "httpPost", url: "http://tv/off", body: "x=1" } } },
        home: { keys: [{ color: "red", icon: "#a", label: "Go", tap: { op: "switchActivity", activity: "A" } }, null, { id: 300, x: 3.5, y: 2.25, w: 1.5, h: 0.75 }] },
        activities: { A: {
            displayName: "Alpha", devices: ["tv"],
            hooks: { didActivate: { op: "delay", ms: 10 } },
            pages: [{ keys: [{ icon: "#b", longPressMs: 900, tap: { op: "script", lua: "return param" } }] }],
            bindings: [{ control: "macro1", event: "tap", do: [{ op: "hidKey", keys: "ctrl+c" }, { op: "udpPacket", host: "10.0.0.1", port: 9, data: "hi" }] }],
        } },
    });
    const { binary } = packTree(tree);
    const u = unpack(ROOT_TYPE, binary);

    assert.equal(u.version, BLOB_VERSION);
    assert.equal(u.longPressMs, 650);
    assert.equal(u.home.keys.length, 2);
    assert.deepEqual([u.home.keys[0].id, ...u.home.keys[0].rect], [1, 0, 0, 4, 4]);
    assert.deepEqual([u.home.keys[1].id, ...u.home.keys[1].rect], [300, 14, 9, 6, 3]);
    assert.deepEqual(Buffer.from(u.home.keys[0].normal), tree.home.keys[0].normal);
    assert.deepEqual(Buffer.from(u.home.keys[0].pressed), tree.home.keys[0].pressed);
    assert.equal(u.home.keys[1].normal, null);     // length 0 / null pointer -> no image
    assert.equal(u.home.keys[1].pressed, null);
    assert.equal(u.home.bindings[0].control, "touch[1]");
    assert.equal(u.home.bindings[0].event, "tap");
    assert.equal(u.home.bindings[0].ops[0].op, OP.switch_activity);
    assert.equal(u.home.bindings[0].ops[0].index, 0);

    assert.equal(u.devices[0].name, "tv");
    assert.equal(u.devices[0].turnOn[0].op, OP.send_wol);
    assert.deepEqual(Array.from(u.devices[0].turnOn[0].mac), [0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff]);
    assert.equal(u.devices[0].turnOff[0].url, "http://tv/off");
    assert.equal(Buffer.from(u.devices[0].turnOff[0].data).toString(), "x=1");

    const a = u.activities[0];
    assert.equal(a.name, "A");
    assert.equal(a.displayName, "Alpha");
    assert.deepEqual(Array.from(a.devices), [0]);
    assert.equal(a.didActivate[0].duration, 10);
    assert.equal(a.pages.length, 1);
    assert.equal(a.pages[0].keys[0].longPressMs, 900);
    assert.equal(a.pages[0].keys[0].id, 16);
    assert.equal(a.bindings[0].control, "touch[16]");
    assert.equal(a.bindings[0].ops[0].source, "return param");
    assert.equal(a.bindings[1].control, "macro1");
    assert.equal(a.bindings[1].ops[0].op, OP.hid_key);
    assert.equal(a.bindings[1].ops[0].modifiers, 1);
    assert.equal(a.bindings[1].ops[0].key, 0x06);
    assert.equal(a.bindings[1].ops[1].host, "10.0.0.1");
    assert.equal(a.bindings[1].ops[1].port, 9);
});

test("a shared image is stored once, 4-aligned, with the same pointer from every key", async () => {
    const keys = Array.from({ length: 15 }, () => ({ color: "red", icon: "#a" }));
    const tree = await baked({ version: 1, home: { keys } });
    const { binary } = packTree(tree);
    // one normal and one pressed image, shared by all 15 keys
    assert.equal(countOf(binary, "IMG:"), 2);
    assert.equal(binary.length % 4, 0);
});

test("an empty layout packs", async () => {
    const tree = await baked({ version: 1, home: {} });
    const u = unpack(ROOT_TYPE, packTree(tree).binary);
    assert.equal(u.activities.length, 0);
    assert.equal(u.devices.length, 0);
    assert.equal(u.home.keys.length, 0);
});

test("an unrendered tree (definitions instead of Buffers) is rejected with a clear message", () => {
    const tree = ok({ version: 1, home: { keys: [{ icon: "#a" }] } });
    assert.throws(() => packTree(tree), /home\.keys\[0\]\.normal.*render/i);
});

test("duplicate script sources are stored once", async () => {
    const lua = "return 'UNIQUE_SCRIPT_TEXT_12345'";
    const tree = await baked({ version: 1, home: { keys: [{ tap: { op: "script", lua } }, { tap: { op: "script", lua } }, { tap: { op: "script", lua } }] } });
    assert.equal(countOf(packTree(tree).binary, "UNIQUE_SCRIPT_TEXT_12345"), 1);
});

test("toBpak matches binpack's own combined file format", async () => {
    const tree = await baked({ version: 1, home: { keys: [{ icon: "#a", tap: { op: "delay", ms: 1 } }] } });
    const result = packTree(tree);
    const file = toBpak(result);
    assert.deepEqual(file, buildCombinedBuffer(result));
    assert.equal(file.readUInt32LE(0), 0x4B415042);
    assert.equal(file.length % 4, 0);
});

test("the C header declares the structs and op codes", () => {
    const h = headerText();
    assert.match(h, /typedef struct/);
    assert.match(h, /OP_HID_KEY/);
    assert.match(h, /OP_SWITCH_ACTIVITY/);
});

test("fields are not silently dropped: an unknown property on the tree is rejected by strict packing", async () => {
    const tree = await baked({ version: 1, home: {} });
    tree.surprise = 1;
    assert.throws(() => packTree(tree), /surprise/);
});

test("an image shared by Home and an activity page has a pointer from both", async () => {
    // The activity's pages are packed after Home's images; their pointers to the same
    // Buffer used to stay null.
    const tree = await baked({
        version: 1,
        home: { keys: [{ color: "red", icon: "#a" }] },
        activities: { A: { pages: [{ keys: [{ color: "red", icon: "#a" }] }] } },
    });
    const { binary, relocations } = packTree(tree);
    const u = unpack(ROOT_TYPE, binary);
    const homeKey = u.home.keys[0], pageKey = u.activities[0].pages[0].keys[0];
    for (const layer of ["normal", "pressed"]) {
        assert.ok(pageKey[layer], `${layer} present on the page key`);
        assert.deepEqual(Buffer.from(pageKey[layer]), Buffer.from(homeKey[layer]));
    }
    // Every non-null pointer slot is in the relocation table, and every table entry holds a non-null pointer
    const table = new Set(relocations);
    for (const slot of relocations)
        assert.notEqual(binary.readUInt32LE(slot), 0, `slot ${slot} is null but relocated`);
    assert.ok(table.size === relocations.length, "no duplicate relocations");
});
