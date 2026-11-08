// Lua scripts with constants injected from JS: a generated prelude (`local NAME = <literal>` per constant) in front
// of a .lua file from urem2/scripts/. Addresses, URLs and tuning numbers stay in the JS tables and targets, so
// nothing is written twice. Emitted as `lua:` source, so the bake still compile-checks and dedupes it.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const SCRIPTS_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "scripts");

const LUA_KEYWORDS = new Set(("and break do else elseif end false for function goto if in local nil not or " +
                              "repeat return then true until while").split(" "));
const isLuaName = s => /^[A-Za-z_][A-Za-z0-9_]*$/.test(s) && !LUA_KEYWORDS.has(s);

const ESCAPES = { '"': '\\"', "\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t" };

// A Lua string literal: printable bytes as is, the rest escaped (\ddd: three digits, so a digit after it is safe)
function luaString(s)
{
    return '"' + s.replace(/["\\\n\r\t]|[\0-\x1f\x7f]/g, c => ESCAPES[c] ??
                           "\\" + String(c.charCodeAt(0)).padStart(3, "0")) + '"';
}

const isPlainObject = v => v !== null && typeof v === "object" && Object.getPrototypeOf(v) === Object.prototype;

// A JS value as Lua source: strings, integers, booleans, arrays (sequences) and string-keyed objects (tables)
export function luaLiteral(v)
{
    if (typeof v === "string")
        return luaString(v);
    if (typeof v === "boolean")
        return String(v);
    if (Number.isSafeInteger(v))
        return String(v);
    if (Array.isArray(v))
        return `{${v.map(luaLiteral).join(", ")}}`;
    if (isPlainObject(v))
        return `{${Object.entries(v).map(([k, x]) => `[${luaString(k)}] = ${luaLiteral(x)}`).join(", ")}}`;
    throw new Error(`luaLiteral: can't write ${String(v)} (${typeof v}) as Lua`);
}

// The prelude (one local per constant, in the object's order) and the body
export function luaSource(body, consts)
{
    const lines = Object.entries(consts).map(([name, v]) => {
        if (!isLuaName(name))
            throw new Error(`luaSource: ${JSON.stringify(name)} is not a Lua name`);
        return `local ${name} = ${luaLiteral(v)}`;
    });
    return [...lines, body].join("\n");
}

// -> a script op: `file` (relative to urem2/scripts/) with the constants in front
export function luaScript(file, consts)
{
    return { op: "script", lua: luaSource(fs.readFileSync(path.resolve(SCRIPTS_DIR, file), "utf8"), consts) };
}
