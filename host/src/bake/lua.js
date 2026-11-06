// Lua 5.4 syntax check with wasmoon (Lua compiled to WebAssembly: runs in Node and browsers).
// Async to create, synchronous to use, so stage 1 can stay synchronous.

import { LuaFactory } from "wasmoon";

// -> check(source) -> null | error message. The code is compiled with load(), never run.
export async function createLuaChecker()
{
    const lua = await new LuaFactory().createEngine();
    await lua.doString(`function __check(s) local f, e = load(s, "=script") if f then return "" else return e end end`);
    const check = lua.global.get("__check");
    return source => check(source) || null;
}
