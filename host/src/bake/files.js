// Node helpers for layout files on disk: load a layout (.json, or a .js/.mjs module's default
// export) and bake it with its assets resolved relative to the file's directory.

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { bake, resolveLayout } from "./bake.js";
import { createNodeHost } from "./host-node.js";
import { createRenderer } from "./render-node.js";
import { renderPages } from "./preview.js";

export async function loadLayoutFile(file)
{
    if (path.extname(file).toLowerCase() === ".json")
        return JSON.parse(fs.readFileSync(file, "utf8"));
    return (await import(pathToFileURL(path.resolve(file)).href)).default;
}

// -> { blob, tree, warnings }; throws a BakeError listing every problem.
// `renderer` defaults to the Node renderer (button-face + rle565).
export async function bakeFile(file, { renderer = createRenderer() } = {})
{
    return bake(await loadLayoutFile(file), await hostFor(file), renderer);
}

// The layout file's pages as screen images (preview.js renderPages; `options` are its):
// -> { pages, warnings }; throws a BakeError listing every problem.
export async function previewFile(file, options)
{
    const { tree, warnings } = resolveLayout(await loadLayoutFile(file), await hostFor(file));
    return { pages: await renderPages(tree, options), warnings };
}

const hostFor = file => createNodeHost(path.dirname(path.resolve(file)));
