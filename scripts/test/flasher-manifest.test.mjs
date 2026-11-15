// The web flasher's site (scripts/make-flasher-manifest.mjs): manifest.json for ESP Web Tools with the
// three parts at their offsets from flasher_args.json, the bins copied under flat names, the page, and
// the refusals. Built from a fixture build folder (fixtures/flasher: small fake bins) into temp dirs.
// The page itself can't be tested in a browser here: only what it references is checked.

import { test, after } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { makeFlasherSite, flashParts, parseOffset, manifest, PAGE } from "../make-flasher-manifest.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const fixture = path.join(here, "fixtures/flasher");
const fixtureArgs = path.join(fixture, "flasher_args.json");
const script = path.join(here, "../make-flasher-manifest.mjs");
const readJson = p => JSON.parse(fs.readFileSync(p, "utf8"));

const temps = [];
function tempDir() {
    const d = fs.mkdtempSync(path.join(os.tmpdir(), "cb-flasher-"));
    temps.push(d);
    return d;
}
after(() => temps.forEach(d => fs.rmSync(d, { recursive: true, force: true })));

// A copy of the fixture build folder with flasher_args.json changed by `edit(args)`
function editedBuild(edit) {
    const dir = tempDir();
    fs.cpSync(fixture, dir, { recursive: true });
    const file = path.join(dir, "flasher_args.json");
    const args = readJson(file);
    edit(args);
    fs.writeFileSync(file, JSON.stringify(args));
    return file;
}

test("parseOffset reads hex strings and refuses anything else", () => {
    assert.equal(parseOffset("0x0"), 0);
    assert.equal(parseOffset("0x8000"), 32768);
    assert.equal(parseOffset("0x10000"), 65536);
    assert.equal(parseOffset("0X1aB"), 0x1ab);
    for (const bad of ["", "8000", "0x", "0xZZ", 32768, undefined])
        assert.throws(() => parseOffset(bad), /bad offset/);
});

test("the site has the manifest, the three parts and the page", () => {
    const out = tempDir();
    const { manifest: m, files } = makeFlasherSite("0.1.0", fixtureArgs, out);
    assert.deepEqual(files, ["bootloader.bin", "partition-table.bin", "controlblock-0.1.0.bin", "index.html", "manifest.json"]);
    assert.deepEqual(fs.readdirSync(out).sort(), [...files].sort());
    assert.deepEqual(readJson(path.join(out, "manifest.json")), m);
    assert.deepEqual(m, {
        name: "ControlBlock",
        version: "0.1.0",
        new_install_prompt_erase: true,
        new_install_improv_wait_time: 0,
        builds: [{
            chipFamily: "ESP32-S3",
            parts: [
                { path: "bootloader.bin", offset: 0 },
                { path: "partition-table.bin", offset: 0x8000 },
                { path: "controlblock-0.1.0.bin", offset: 0x10000 },
            ],
        }],
    });
    const same = (name, src) => assert.deepEqual(fs.readFileSync(path.join(out, name)), fs.readFileSync(path.join(fixture, src)));
    same("bootloader.bin", "bootloader/bootloader.bin");
    same("partition-table.bin", "partition_table/partition-table.bin");
    same("controlblock-0.1.0.bin", "controlblock.bin");
    assert.deepEqual(fs.readFileSync(path.join(out, "index.html")), fs.readFileSync(PAGE));
});

test("offsets come from flash_files, matched to each role's file", () => {
    const args = readJson(fixtureArgs);
    args.flash_files = { "0x1000": "bootloader/bootloader.bin", "0xA000": "partition_table/partition-table.bin", "0x20000": "controlblock.bin" };
    assert.deepEqual(flashParts(args).map(p => [p.role, p.offset]),
        [["bootloader", 0x1000], ["partition-table", 0xa000], ["app", 0x20000]]);
});

test("a dev version names the app after it", () => {
    const out = tempDir();
    const { manifest: m } = makeFlasherSite("0.2.0-dev+abc1234", fixtureArgs, out);
    assert.equal(m.version, "0.2.0-dev+abc1234");
    assert.ok(fs.existsSync(path.join(out, "controlblock-0.2.0-dev+abc1234.bin")));
});

test("manifest lists only path and offset per part", () => {
    const m = manifest("1.2.3", [{ role: "app", file: "x.bin", src: "/x", path: "a.bin", offset: 1 }]);
    assert.deepEqual(m.builds[0].parts, [{ path: "a.bin", offset: 1 }]);
});

test("refuses another chip", () => {
    const file = editedBuild(a => { a.extra_esptool_args.chip = "esp32"; });
    assert.throws(() => makeFlasherSite("0.1.0", file, tempDir()), /chip "esp32", not esp32s3/);
});

test("refuses a missing role or a role not in flash_files", () => {
    let file = editedBuild(a => { delete a["partition-table"]; });
    assert.throws(() => makeFlasherSite("0.1.0", file, tempDir()), /no partition-table/);
    file = editedBuild(a => { delete a.flash_files["0x10000"]; });
    assert.throws(() => makeFlasherSite("0.1.0", file, tempDir()), /flash_files has no controlblock.bin \(app\)/);
});

test("refuses flash_files with more than the three parts", () => {
    const file = editedBuild(a => { a.flash_files["0xd000"] = "ota_data_initial.bin"; });
    assert.throws(() => makeFlasherSite("0.1.0", file, tempDir()), /more than the three parts: ota_data_initial.bin/);
});

test("refuses a missing bin, and writes nothing", () => {
    const file = editedBuild(() => {});
    fs.rmSync(path.join(path.dirname(file), "bootloader/bootloader.bin"));
    const out = path.join(tempDir(), "site");
    assert.throws(() => makeFlasherSite("0.1.0", file, out), /missing bootloader/);
    assert.equal(fs.existsSync(out), false);
});

test("refuses a bad version", () => {
    for (const bad of ["", "v0.1.0", "0.1", "0.1.0/../x", undefined])
        assert.throws(() => makeFlasherSite(bad, fixtureArgs, tempDir()), /is not a version/);
});

test("the CLI builds the site and fails with usage on bad arguments", () => {
    const out = tempDir();
    let r = spawnSync(process.execPath, [script, "0.1.0", fixtureArgs, out], { encoding: "utf8", timeout: 30_000 });
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /flasher 0\.1\.0: bootloader\.bin, partition-table\.bin, controlblock-0\.1\.0\.bin, index\.html, manifest\.json/);
    r = spawnSync(process.execPath, [script, "0.1.0"], { encoding: "utf8", timeout: 30_000 });
    assert.equal(r.status, 1);
    assert.match(r.stderr, /expected three arguments/);
    assert.match(r.stderr, /usage:/);
});

test("the page loads a pinned ESP Web Tools and installs from manifest.json", () => {
    const html = fs.readFileSync(PAGE, "utf8");
    assert.match(html, /<title>ControlBlock Flasher<\/title>/);
    assert.match(html, /<script type="module" src="https:\/\/unpkg\.com\/esp-web-tools@\d+\.\d+\.\d+\/dist\/web\/install-button\.js\?module"><\/script>/);
    assert.match(html, /<esp-web-install-button[^>]*\smanifest="manifest\.json"/);
    assert.match(html, /fetch\("manifest\.json"/);
    assert.match(html, /slot="unsupported"/);
    assert.match(html, /slot="not-allowed"/);
    assert.match(html, /npx github:toptensoftware\/ControlBlock#cli info/);
});
