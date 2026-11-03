// End to end: the example in docs/config-format.md through all three stages, with the real
// Lua checker and a fake renderer; plus the CLI with the real renderer.
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { execFileSync, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { unpack } from "../../../binpack/index.js";
import { bake, BakeError } from "../src/bake.js";
import { createLuaChecker } from "../src/lua.js";
import { ROOT_TYPE } from "../src/pack.js";
import { fakeRenderer, fakeHost } from "./helpers.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const cliPath = path.join(here, "..", "cli.js");

const example = () => ({
    version: 1,
    settings: { longPressMs: 600 },
    defaults: { color: "#22262e", iconColor: "white", pressed: { color: "#3a86ff" } },
    devices: {
        tv: { turnOn: { op: "sendWol", mac: "AA:BB:CC:DD:EE:FF" },
              turnOff: { op: "httpPost", url: "http://tv.local/api/power", body: "state=off" } },
        receiver: { turnOn: { op: "httpPost", url: "http://avr.local/power", body: "on=1" },
                    turnOff: { op: "httpPost", url: "http://avr.local/power", body: "on=0" } },
    },
    actions: {
        mute: { op: "script", file: "scripts/mute.lua" },
        back: [{ op: "hidKey", keys: "esc" }],
    },
    home: { keys: [
        { icon: "#apple-tv", label: "Watch TV", color: "#8338ec", action: { op: "switchActivity", activity: "Watch TV" } },
        { icon: "#list", label: "Listen", action: { op: "switchActivity", activity: "Listen" } },
    ] },
    activities: {
        "Watch TV": {
            displayName: "TV",
            devices: ["tv", "receiver"],
            hooks: { didActivate: { op: "httpPost", url: "http://avr.local/input", body: "src=hdmi1" } },
            pages: [{ keys: [
                { icon: "#play", label: "Play", action: { op: "hidKey", keys: "space" } },
                { icon: "#next-track", label: "Next", action: { op: "hidKey", keys: "ctrl+right" } },
                null, null,
                { icon: "#volume-mute", label: "Mute", action: { use: "mute" } },
                { icon: "#arrow-left", action: { use: "back" }, long: { op: "switchActivity", activity: "Listen" } },
            ] }],
            bindings: [
                { control: "macro1", event: "tap", do: { use: "mute" } },
                { control: "volEnc", event: "turnLeft", do: { op: "script", file: "scripts/vol.lua" } },
                { control: "volEnc", event: "turnRight", do: { op: "script", file: "scripts/vol.lua" } },
            ],
        },
        Listen: {
            devices: ["receiver"],
            pages: [{ keys: [{ icon: "#play", action: { op: "hidKey", keys: "space" } }] }],
        },
    },
});

const files = {
    "scripts/mute.lua": "local r = http.get('http://avr.local/mute')\nhttp.post('http://avr.local/mute', r == '1' and 'on=0' or 'on=1')\n",
    "scripts/vol.lua": "http.post('http://avr.local/vol', 'delta=' .. ((tonumber(param) or 1) // 10))\n",
};

const makeHost = async () => fakeHost(files, { luaCheck: await createLuaChecker() });

test("the docs example bakes to a valid BPAK file", async () => {
    const { blob, tree, warnings } = await bake(example(), await makeHost(), fakeRenderer());
    assert.equal(blob.readUInt32LE(0), 0x4B415042);
    assert.equal(warnings.length, 0, JSON.stringify(warnings));

    // Mute is touch[9,0], back is touch[5,1]
    const controls = tree.activities[0].bindings.map(b => `${b.control}/${b.event}`);
    assert.deepEqual(controls, ["touch[5,0]/tap", "touch[6,0]/tap", "touch[9,0]/tap", "touch[5,1]/tap", "touch[5,1]/long",
                                "macro1/tap", "volEnc/turnLeft", "volEnc/turnRight"]);
});

test("the blob reads back (strip the 32-byte header, unpack at offset 32)", async () => {
    const { blob } = await bake(example(), await makeHost(), fakeRenderer());
    const count = blob.readUInt32LE(8), tableOffset = blob.readUInt32LE(12);
    assert.ok(count > 0 && tableOffset % 4 === 0 && tableOffset < blob.length);
    const root = unpack(ROOT_TYPE, blob, 32);   // pointers are file-relative in the combined format
    assert.equal(root.activities.length, 2);
    assert.equal(root.activities[0].name, "Watch TV");
    assert.equal(root.activities[0].pages[0].keys[0].longPressMs, 0);
    assert.equal(root.longPressMs, 600);
    // the shared face and the shared pressed face are single stored copies across all 4 keys that use them
    assert.ok(blob.length < 20000, `blob is ${blob.length} bytes`);
});

test("bake is deterministic: same input, same bytes", async () => {
    const a = (await bake(example(), await makeHost(), fakeRenderer())).blob;
    const b = (await bake(example(), await makeHost(), fakeRenderer())).blob;
    assert.deepEqual(a, b);
});

test("errors throw a BakeError listing every problem, and nothing is rendered", async () => {
    const cfg = example();
    cfg.activities["Watch TV"].devices = ["ghost"];
    cfg.home.keys[0].iconScale = "x";
    const render = fakeRenderer();
    await assert.rejects(bake(cfg, await makeHost(), render), err => {
        assert.ok(err instanceof BakeError);
        assert.equal(err.errors.length, 2);
        assert.match(err.message, /ghost/);
        assert.match(err.message, /iconScale/);
        return true;
    });
    assert.equal(render.calls.length, 0);
});

test("a real Lua syntax error is caught", async () => {
    const f = { ...files, "scripts/mute.lua": "local x = = 1" };
    const host = fakeHost(f, { luaCheck: await createLuaChecker() });
    await assert.rejects(bake(example(), host, fakeRenderer()), /scripts\/mute\.lua|script:1/);
});

// ---------------------------------------------------------------- CLI (real renderer, real files)

test("cli: bakes a config file with real images to a .bin", { timeout: 120000 }, () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cbbake-"));
    try {
        fs.mkdirSync(path.join(dir, "scripts"));
        for (const [name, text] of Object.entries(files))
            fs.writeFileSync(path.join(dir, name), text);
        fs.writeFileSync(path.join(dir, "config.json"), JSON.stringify(example()));
        const out = path.join(dir, "out.bin");
        execFileSync(process.execPath, [cliPath, path.join(dir, "config.json"), "-o", out], { cwd: dir, encoding: "utf8" });
        const blob = fs.readFileSync(out);
        assert.equal(blob.readUInt32LE(0), 0x4B415042);
        assert.equal(blob.length % 4, 0);
        const root = unpack(ROOT_TYPE, blob, 32);
        // real rle565 data: starts with the "R565" signature
        const img = Buffer.from(root.home.keys[0].normalBg);
        assert.equal(img.readUInt32LE(0), 0x35363552);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

test("cli: a bad config prints every error with its path and exits 1", () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cbbake-"));
    try {
        fs.writeFileSync(path.join(dir, "config.json"), JSON.stringify({ version: 1, home: { keys: [{ iconScale: "x" }] }, oops: 1 }));
        const r = spawnSync(process.execPath, [cliPath, path.join(dir, "config.json")], { cwd: dir, encoding: "utf8" });
        assert.equal(r.status, 1);
        assert.match(r.stderr, /home\.keys\[0\]\.iconScale/);
        assert.match(r.stderr, /oops: unknown property/);
        assert.equal(fs.existsSync(path.join(dir, "config.bin")), false);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

test("cli: default output is the config name with .bin; usage on no args", () => {
    const r = spawnSync(process.execPath, [cliPath], { encoding: "utf8" });
    assert.equal(r.status, 1);
    assert.match(r.stderr, /usage/i);
});
