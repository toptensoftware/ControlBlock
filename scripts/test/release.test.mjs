// The release workflow's helpers (scripts/release.mjs, .github/workflows/release.yml): the tag check
// against the product version, the Release assets staged from a firmware build, and the release notes
// rendered from .github/release-notes.md. The workflow itself can't run here: only these pieces.

import { test, after } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import {
    tagVersion, pagesUrl, renderTemplate, releaseNotes, releaseAssets, flashCommandParts,
    FULL_IMAGE, NOTES_TEMPLATE,
} from "../release.mjs";
import { productVersion } from "../bump-version.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "../..");
const fixture = path.join(here, "fixtures/flasher");
const script = path.join(here, "../release.mjs");

const temps = [];
function tempDir() {
    const d = fs.mkdtempSync(path.join(os.tmpdir(), "cb-release-"));
    temps.push(d);
    return d;
}
after(() => temps.forEach(d => fs.rmSync(d, { recursive: true, force: true })));

// A copy of the fixture build folder plus the merged image the firmware workflow writes
function buildWithFullImage() {
    const dir = tempDir();
    fs.cpSync(fixture, dir, { recursive: true });
    fs.writeFileSync(path.join(dir, FULL_IMAGE), "full image");
    return dir;
}

// A repo root with only the three versioned package.json files
function fakeRoot(version) {
    const root = tempDir();
    for (const f of ["package.json", "host/package.json", "cli/package.json"]) {
        fs.mkdirSync(path.dirname(path.join(root, f)), { recursive: true });
        fs.writeFileSync(path.join(root, f), JSON.stringify({ name: "x", version }, null, 2) + "\n");
    }
    return root;
}

const run = (...args) => spawnSync(process.execPath, [script, ...args], { encoding: "utf8" });

test("tagVersion accepts v<product version> and returns the version", () => {
    assert.equal(tagVersion("v1.2.3", fakeRoot("1.2.3")), "1.2.3");
    assert.equal(tagVersion("refs/tags/v1.2.3", fakeRoot("1.2.3")), "1.2.3");
    assert.equal(tagVersion(`v${productVersion(repoRoot)}`, repoRoot), productVersion(repoRoot));
});

test("tagVersion refuses another version or a tag without the v", () => {
    const root = fakeRoot("1.2.3");
    assert.throws(() => tagVersion("v1.2.4", root), /tag v1\.2\.4 doesn't match the product version 1\.2\.3/);
    assert.throws(() => tagVersion("1.2.3", root), /isn't a release tag/);
    assert.throws(() => tagVersion("cli-v1.2.3", root), /isn't a release tag/);
    assert.throws(() => tagVersion("", root), /isn't a release tag/);
});

test("tagVersion refuses when the package versions disagree", () => {
    const root = fakeRoot("1.2.3");
    fs.writeFileSync(path.join(root, "cli/package.json"), JSON.stringify({ name: "x", version: "1.2.2" }, null, 2) + "\n");
    assert.throws(() => tagVersion("v1.2.3", root), /disagree/);
});

test("pagesUrl is the repo's project site", () => {
    assert.equal(pagesUrl("toptensoftware/ControlBlock"), "https://toptensoftware.github.io/ControlBlock/");
    assert.equal(pagesUrl("Someone/repo"), "https://someone.github.io/repo/");
    for (const bad of ["", "noslash", "a/b/c", undefined])
        assert.throws(() => pagesUrl(bad), /owner\/name/);
});

test("renderTemplate fills every placeholder and refuses unknown or unused ones", () => {
    assert.equal(renderTemplate("a {{x}} b {{ y }} {{x}}", { x: "1", y: "2" }), "a 1 b 2 1");
    assert.throws(() => renderTemplate("{{x}} {{z}}", { x: "1" }), /unknown placeholder \{\{z\}\}/);
    assert.throws(() => renderTemplate("{{x}}", { x: "1", y: "2" }), /never uses \{\{y\}\}/);
});

test("flashCommandParts lists the three parts at their offsets, under the Release names", () => {
    assert.equal(flashCommandParts("0.1.0", path.join(fixture, "flasher_args.json")),
        "0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 controlblock-0.1.0.bin");
});

test("the release notes say how to flash, what the full image wipes, and the pinned npx form", () => {
    const notes = releaseNotes("0.1.0", "toptensoftware/ControlBlock", path.join(fixture, "flasher_args.json"));
    assert.doesNotMatch(notes, /\{\{|\}\}/);
    assert.match(notes, /https:\/\/toptensoftware\.github\.io\/ControlBlock\//);
    assert.match(notes, /write-flash 0x0 bootloader\.bin 0x8000 partition-table\.bin 0x10000 controlblock-0\.1\.0\.bin/);
    assert.match(notes, /controlblock-0\.1\.0-full\.bin/);
    assert.match(notes, /keeps/i);
    assert.match(notes, /Wi-Fi/);
    assert.match(notes, /stored layout/);
    assert.match(notes, /npx github:toptensoftware\/ControlBlock#cli-v0\.1\.0 /);
    assert.match(notes, /controlblock-0\.1\.0-elf\.zip/);
    assert.ok(fs.existsSync(NOTES_TEMPLATE));
});

test("releaseAssets stages the three parts, flasher_args.json and the full image", () => {
    const build = buildWithFullImage();
    const out = tempDir();
    const files = releaseAssets("0.1.0", path.join(build, "flasher_args.json"), out);
    const expected = ["bootloader.bin", "partition-table.bin", "controlblock-0.1.0.bin", "flasher_args.json", "controlblock-0.1.0-full.bin"];
    assert.deepEqual(files, expected);
    assert.deepEqual(fs.readdirSync(out).sort(), [...expected].sort());
    const same = (name, src) => assert.deepEqual(fs.readFileSync(path.join(out, name)), fs.readFileSync(path.join(build, src)));
    same("bootloader.bin", "bootloader/bootloader.bin");
    same("partition-table.bin", "partition_table/partition-table.bin");
    same("controlblock-0.1.0.bin", "controlblock.bin");
    same("flasher_args.json", "flasher_args.json");
    same("controlblock-0.1.0-full.bin", FULL_IMAGE);
});

test("releaseAssets refuses a build without the full image, and writes nothing", () => {
    const out = path.join(tempDir(), "assets");
    assert.throws(() => releaseAssets("0.1.0", path.join(fixture, "flasher_args.json"), out), /missing full image/);
    assert.equal(fs.existsSync(out), false);
});

test("the CLI: check-tag prints the version or fails", () => {
    const ok = run("check-tag", "v1.2.3", "--root", fakeRoot("1.2.3"));
    assert.equal(ok.status, 0, ok.stderr);
    assert.equal(ok.stdout, "1.2.3\n");
    const bad = run("check-tag", "v9.9.9", "--root", fakeRoot("1.2.3"));
    assert.equal(bad.status, 1);
    assert.match(bad.stderr, /doesn't match/);
});

test("the CLI: notes and assets write their output; bad commands fail with usage", () => {
    const out = tempDir();
    const notesFile = path.join(out, "notes.md");
    const n = run("notes", "0.1.0", "toptensoftware/ControlBlock", path.join(fixture, "flasher_args.json"), notesFile);
    assert.equal(n.status, 0, n.stderr);
    assert.equal(fs.readFileSync(notesFile, "utf8"),
        releaseNotes("0.1.0", "toptensoftware/ControlBlock", path.join(fixture, "flasher_args.json")));

    const build = buildWithFullImage();
    const assets = path.join(out, "assets");
    const a = run("assets", "0.1.0", path.join(build, "flasher_args.json"), assets);
    assert.equal(a.status, 0, a.stderr);
    assert.ok(fs.existsSync(path.join(assets, "controlblock-0.1.0-full.bin")));

    for (const args of [[], ["bogus"], ["notes", "0.1.0"], ["check-tag"]]) {
        const r = run(...args);
        assert.equal(r.status, 1, args.join(" "));
        assert.match(r.stderr, /usage/);
    }
});
