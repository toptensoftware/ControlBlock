// The Node host for stage 1: files relative to the layout file's directory, and the Lua checker.

import fs from "node:fs";
import path from "node:path";
import { icons } from "@toptensoftware/button-face/node";
import { createLuaChecker } from "./lua.js";

export async function createNodeHost(baseDir)
{
    return {
        resolvePath: p => path.resolve(baseDir, p),
        exists: p => fs.existsSync(p),
        hasIcon: name => Object.hasOwn(icons, name),    // button-face's built-in icons ("#name")
        readText: p => fs.readFileSync(p, "utf8"),
        luaCheck: await createLuaChecker(),
    };
}
