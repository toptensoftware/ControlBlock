// Node helpers for config files on disk: load a config (.json, or a .js/.mjs module's default
// export) and bake it with its assets resolved relative to the file's directory.

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { bake } from "./bake.js";
import { createNodeHost } from "./host-node.js";
import { createRenderer } from "./render-node.js";

export async function loadConfigFile(file)
{
    if (path.extname(file).toLowerCase() === ".json")
        return JSON.parse(fs.readFileSync(file, "utf8"));
    return (await import(pathToFileURL(path.resolve(file)).href)).default;
}

// -> { blob, tree, warnings }; throws a BakeError listing every problem.
// `renderer` defaults to the Node renderer (node-buttonImage + rle565).
export async function bakeFile(file, { renderer = createRenderer() } = {})
{
    const host = await createNodeHost(path.dirname(path.resolve(file)));
    return bake(await loadConfigFile(file), host, renderer);
}
