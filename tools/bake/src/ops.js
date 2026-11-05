// Op lists: validation and the packed form of each op, in one table. An op list is one op
// object, an array of them, or null; `{ use: "name" }` expands a named action inline.
// See docs/config-format.md "Op lists" and docs/standalone.md "Actions".

import { OP, HID_MODE, ACTIVITY_NONE, MAX_SCRIPT_BYTES } from "./consts.js";
import { childPath } from "./diag.js";
import { t, required, checkObject } from "./check.js";
import { parseChord } from "./hid.js";

const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);

// ------------------------------------------------------------ property checkers

const httpUrl = (v, p, d) =>
    !t.string({ nonEmpty: true })(v, p, d) ? false
    : !v.startsWith("http://") ? (d.error(p, "url must start with http:// (TLS is not supported yet)"), false)
    : true;

const headers = t.nullable(t.map(t.string()));
const formatHeaders = h => h && Object.keys(h).length ? Object.entries(h).map(([k, v]) => `${k}: ${v}`).join("\r\n") : null;

const HEX = /^([0-9a-fA-F]{2})*$/;
const payload = (v, p, d) =>
    typeof v === "string" ? true
    : isObject(v) ? checkObject(v, { hex: required(t.string()) }, p, d) && (HEX.test(v.hex.replace(/\s+/g, "")) || (d.error(childPath(p, "hex"), "hex must be an even number of hex digits"), false))
    : (d.error(p, 'expected a string or { "hex": "..." }'), false);
const payloadBytes = v => typeof v === "string" ? Buffer.from(v) : Buffer.from(v.hex.replace(/\s+/g, ""), "hex");

const MAC = /^[0-9a-f]{2}([:-][0-9a-f]{2}){5}$/i;
const macAddress = (v, p, d) =>
    !t.string()(v, p, d) ? false
    : !MAC.test(v) ? (d.error(p, "mac must look like AA:BB:CC:DD:EE:FF"), false)
    : true;

// ------------------------------------------------------------ the op table
// props: checkers for the op's own properties. build(o, path, rc, gesture) -> packed op, or
// undefined after reporting an error.

const OP_TABLE = {
    delay: {
        props: { ms: required(t.integer(1, 60000)) },
        build: o => ({ op: OP.delay, duration: o.ms }),
    },

    switchActivity: {
        props: { activity: required(t.nullable(t.string({ nonEmpty: true }))) },
        build(o, p, rc, gesture)
        {
            // Hooks and device lists run as part of a switch: switching from there would nest
            if (gesture === "hook")
                return void rc.diag.error(p, "switchActivity is not allowed in an activity hook or a device's turnOn/turnOff");
            if (o.activity === null)
                return { op: OP.switch_activity, index: ACTIVITY_NONE };
            if (!rc.activityIndex.has(o.activity))
                return void rc.diag.error(childPath(p, "activity"), `unknown activity "${o.activity}"`);
            return { op: OP.switch_activity, index: rc.activityIndex.get(o.activity) };
        },
    },

    hidKey: {
        props: { keys: required(t.string()), mode: t.oneOf(...Object.keys(HID_MODE)) },
        build(o, p, rc, gesture)
        {
            const mode = o.mode ?? "tap";
            const chord = parseChord(o.keys);
            if (chord.error)
                return void rc.diag.error(childPath(p, "keys"), chord.error);
            // hold = key down on press and key up on release; it needs those gestures
            if (mode === "hold" && gesture !== undefined && gesture !== "press" && gesture !== "release")
                return void rc.diag.error(childPath(p, "mode"), `mode "hold" only works under press or release (this is ${gesture})`);
            return { op: OP.hid_key, mode: HID_MODE[mode], modifiers: chord.modifiers, key: chord.key };
        },
    },

    httpGet: {
        props: { url: required(httpUrl), headers },
        build: o => ({ op: OP.http_get, url: o.url, headers: formatHeaders(o.headers) }),
    },

    httpPost: {
        props: { url: required(httpUrl), body: t.string(), contentType: t.string({ nonEmpty: true }), headers },
        build: o => ({
            op: OP.http_post, url: o.url, data: Buffer.from(o.body ?? ""),
            contentType: o.contentType ?? "application/x-www-form-urlencoded", headers: formatHeaders(o.headers),
        }),
    },

    udpPacket: {
        props: { host: required(t.string({ nonEmpty: true })), port: required(t.integer(1, 65535)), data: required(payload) },
        build: o => ({ op: OP.udp_packet, host: o.host, port: o.port, data: payloadBytes(o.data) }),
    },

    sendWol: {
        props: { mac: required(macAddress), broadcast: t.string({ nonEmpty: true }) },
        build: o => ({ op: OP.send_wol, mac: o.mac.split(/[:-]/).map(h => parseInt(h, 16)), broadcast: o.broadcast ?? null }),
    },

    script: {
        props: { lua: t.string(), file: t.string({ nonEmpty: true }) },
        build(o, p, rc)
        {
            const { diag, host } = rc;
            if ((o.lua === undefined) === (o.file === undefined))
                return void diag.error(p, "a script needs exactly one of lua or file");
            let source = o.lua, where = childPath(p, "lua");
            if (o.file !== undefined)
            {
                where = childPath(p, "file");
                const file = host.resolvePath(o.file);
                if (!host.exists(file))
                    return void diag.error(where, `file not found: ${o.file}`);
                source = host.readText(file);
            }
            if (Buffer.byteLength(source) > MAX_SCRIPT_BYTES)
                return void diag.error(where, `script is longer than ${MAX_SCRIPT_BYTES} bytes`);
            const problem = host.luaCheck(source);
            if (problem)
                return void diag.error(where, problem);
            return { op: OP.script, source };
        },
    },
};

export const OP_NAMES = Object.keys(OP_TABLE);

// ------------------------------------------------------------ resolving

// -> array of packed ops ([] for null or empty). `gesture` is what triggers the list (undefined
// when checking an action's definition on its own). `stack` is the chain of `use`s being expanded.
export function resolveOpList(value, path, rc, gesture, stack = [])
{
    if (value === null || value === undefined)
        return [];
    if (Array.isArray(value))
        return value.flatMap((v, i) => resolveOp(v, childPath(path, i), rc, gesture, stack));
    return resolveOp(value, path, rc, gesture, stack);
}

function resolveOp(value, path, rc, gesture, stack)
{
    const { diag } = rc;

    if (!isObject(value))
    {
        diag.error(path, "expected an op object, an array of ops, or null");
        return [];
    }

    if (Object.hasOwn(value, "use"))
        return expandUse(value, path, rc, gesture, stack);

    const name = value.op;
    if (name === undefined)
    {
        diag.error(childPath(path, "op"), "required");
        return [];
    }
    if (typeof name !== "string" || !Object.hasOwn(OP_TABLE, name))
    {
        diag.error(childPath(path, "op"), `unknown op ${JSON.stringify(name)} (known: ${OP_NAMES.join(", ")})`);
        return [];
    }

    const entry = OP_TABLE[name];
    if (!checkObject(value, { op: t.any, ...entry.props }, path, diag))
        return [];
    const packed = entry.build(value, path, rc, gesture);
    return packed ? [packed] : [];
}

function expandUse(value, path, rc, gesture, stack)
{
    const { diag } = rc;
    if (!checkObject(value, { use: required(t.string({ nonEmpty: true })) }, path, diag))
        return [];
    const name = value.use;
    if (!Object.hasOwn(rc.actions, name))
    {
        diag.error(childPath(path, "use"), `unknown action "${name}"`);
        return [];
    }
    if (stack.includes(name))
    {
        diag.error(childPath(path, "use"), `action cycle: ${[...stack, name].join(" -> ")}`);
        return [];
    }
    rc.usedActions.add(name);
    return resolveOpList(rc.actions[name], childPath("actions", name), rc, gesture, [...stack, name]);
}
