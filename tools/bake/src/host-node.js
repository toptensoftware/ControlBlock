// The Node host for stage 1: files relative to the config file's directory, and the Lua checker.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createLuaChecker } from "./lua.js";

// node-buttonImage's built-in icons ("#name" -> images/name.svg)
const iconDir = path.join(path.dirname(fileURLToPath(import.meta.resolve("@toptensoftware/node-buttonImage"))), "images");

export async function createNodeHost(baseDir)
{
    return {
        resolvePath: p => path.resolve(baseDir, p),
        exists: p => fs.existsSync(p),
        hasIcon: name => fs.existsSync(path.join(iconDir, name + ".svg")),
        readText: p => fs.readFileSync(p, "utf8"),
        luaCheck: await createLuaChecker(),
    };
}
