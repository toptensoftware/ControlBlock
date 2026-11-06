// The package's entry points: browser.js is the device API only (nothing Node only in its static
// import graph); index.js is everything (device, images, bake).
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import * as browser from "../browser.js";
import * as node from "../index.js";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");

// Every file reached by static imports/re-exports from `entry`, and every non-relative specifier
// seen (dynamic import() is not followed: ControlBlock.open loads "usb" that way, Node only)
function importGraph(entry)
{
    const files = new Set(), bare = new Set(), todo = [path.resolve(root, entry)];
    const re = /^\s*(?:import|export)\b[^;]*?\bfrom\s*["']([^"']+)["']|^\s*import\s*["']([^"']+)["']/gm;
    while (todo.length) {
        const file = todo.pop();
        if (files.has(file))
            continue;
        files.add(file);
        for (const m of fs.readFileSync(file, "utf8").matchAll(re)) {
            const spec = m[1] ?? m[2];
            if (spec.startsWith("."))
                todo.push(path.resolve(path.dirname(file), spec));
            else
                bare.add(spec);
        }
    }
    return { files: [...files].map(f => path.relative(root, f).replaceAll("\\", "/")).sort(), bare: [...bare] };
}

test("browser.js exports the device API and imports nothing Node only", () => {
    for (const name of ["ControlBlock", "ControlBlockError", "Emitter", "Msg", "Status", "WifiState", "KeyAction", "crc32", "MessageParser", "decodeInfo"])
        assert.ok(name in browser, `browser.js lacks ${name}`);
    for (const name of ["keyImage", "bake", "bakeFile", "uploadBlob"])
        assert.ok(!(name in browser), `browser.js exports ${name}`);
    const g = importGraph("browser.js");
    assert.deepEqual(g.bare, [], `bare imports: ${g.bare.join(", ")}`);
    assert.deepEqual(g.files, ["browser.js", "src/ControlBlock.js", "src/Emitter.js", "src/protocol.js"]);
});

test("index.js exports the device API, the image helpers and the bake API", () => {
    for (const name of Object.keys(browser))
        assert.equal(node[name], browser[name], `index.js: ${name}`);
    for (const name of ["imageId", "encodeImage", "keyImage", "keyImageFromSvg", "renderButtonImage", "buttonKeyImage",
                        "bake", "BakeError", "resolve", "renderImages", "packTree", "toBpak", "ROOT_TYPE", "formatEntries",
                        "createNodeHost", "createRenderer", "createLuaChecker", "loadConfigFile", "bakeFile", "uploadBlob",
                        "parseChord", "KEYS", "MODIFIERS", "KEY_SIZE", "BLOB_VERSION", "OP"])
        assert.ok(node[name] !== undefined, `index.js lacks ${name}`);
});

test("package.json points browsers at browser.js and Node at index.js", () => {
    const pkg = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));
    assert.equal(pkg.main, "index.js");
    assert.deepEqual(pkg.exports["."], { browser: "./browser.js", default: "./index.js" });
    assert.equal(pkg.exports["./package.json"], "./package.json");
});
