// Runs urem2's Lua scripts in Node for the tests: wasmoon (Lua 5.4, the bake's compile checker, reached through the
// host library's own dependency) plus the part of the device's script API the scripts use
// (docs/layout-format.md "Script API"), over a synchronous JS fake:
//   http.get(url [, headers])            -> body, status    (or nil, "timeout" when the fake gives no reply)
//   http.get{ url=, headers=, reply= }   reply = false: fire and forget -> true (the fake still sees it)
//   log(...), print(...)                 joined with tabs
//   millis()                             a fake clock
//   param                                the trigger's param string
// The device's missing libraries (load, dofile, loadfile, require, io, os, package, debug, coroutine) are removed.

import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";

const require = createRequire(new URL("../../host/package.json", import.meta.url));
let factoryPromise;

async function engine()
{
    factoryPromise ??= import(pathToFileURL(require.resolve("wasmoon")).href).then(m => new m.LuaFactory());
    return (await factoryPromise).createEngine();
}

// The Lua side: multiple returns from one JS call (__http_get returns { sent } | { err } | { body, status })
const API = `
http = {}
function http.get(a, headers)
    local r = __http_get(a, headers)
    if r.sent then return true end
    if r.err then return nil, r.err end
    return r.body, r.status
end
function print(...)
    local t = table.pack(...)
    for i = 1, t.n do t[i] = tostring(t[i]) end
    __log(table.concat(t, "\\t", 1, t.n))
end
log = print
load, dofile, loadfile, require, io, os, package, debug, coroutine = nil, nil, nil, nil, nil, nil, nil, nil, nil
`;

// Runs `source` and resolves its first return value (no API; for literal round trips)
export async function evalLua(source)
{
    const lua = await engine();
    try
    {
        return await lua.doString(source);
    }
    finally
    {
        lua.global.close();
    }
}

// get(url, headers) -> { status, body } | null (no reply). Resolves { error (string | null), logs, requests }:
// requests are { url, reply } in call order (reply false = fire and forget).
export async function runLua(source, { param = "", get = () => null, now = () => 0 } = {})
{
    const lua = await engine();
    const logs = [];
    const requests = [];

    lua.global.set("__log", text => { logs.push(text); });
    lua.global.set("millis", () => now());
    lua.global.set("param", param);
    lua.global.set("__http_get", (a, headers) => {
        const args = typeof a === "object" && a !== null ? a : { url: a, headers };
        const reply = args.reply !== false;
        if (typeof args.url !== "string" || !args.url.startsWith("http://"))
            return { err: "http.get: url must start with http://" };
        requests.push({ url: args.url, reply });
        const r = get(args.url, args.headers ?? null);
        if (!reply)
            return { sent: true };
        return r ? { body: r.body, status: r.status } : { err: "timeout" };
    });

    try
    {
        await lua.doString(API);
        await lua.doString(source);
        return { error: null, logs, requests };
    }
    catch (e)
    {
        return { error: e.message, logs, requests };
    }
    finally
    {
        lua.global.close();
    }
}
