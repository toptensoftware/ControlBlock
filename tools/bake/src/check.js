// Declarative property checking. A checker is (value, path, diag) => boolean (true = ok) and
// reports its own errors. `checkObject` runs a spec (name -> checker) over an object, reporting
// unknown, missing and invalid properties together.

import { childPath } from "./diag.js";

const isPlainObject = v => v !== null && typeof v === "object" && !Array.isArray(v) && !Buffer.isBuffer(v);
const describe = v => v === null ? "null" : Array.isArray(v) ? "an array" : typeof v === "object" ? "an object" : `${typeof v} ${JSON.stringify(v)}`;

const bad = (diag, path, expected, v) => { diag.error(path, `expected ${expected}, got ${describe(v)}`); return false; };

export const required = checker => Object.assign((v, p, d) => checker(v, p, d), { required: true });

export const t = {
    any: () => true,

    string: ({ nonEmpty = false } = {}) => (v, p, d) =>
        typeof v !== "string" ? bad(d, p, "a string", v)
        : nonEmpty && v === "" ? (d.error(p, "must not be empty"), false)
        : true,

    number: ({ min = -Infinity, max = Infinity } = {}) => (v, p, d) =>
        typeof v !== "number" || !Number.isFinite(v) ? bad(d, p, "a number", v)
        : v < min || v > max ? (d.error(p, `must be between ${min} and ${max}`), false)
        : true,

    integer: (min = -Infinity, max = Infinity) => (v, p, d) =>
        !Number.isInteger(v) ? bad(d, p, "an integer", v)
        : v < min || v > max ? (d.error(p, `must be between ${min} and ${max}`), false)
        : true,

    oneOf: (...values) => (v, p, d) =>
        values.includes(v) ? true : (d.error(p, `must be one of: ${values.join(", ")}`), false),

    nullable: checker => (v, p, d) => v === null || checker(v, p, d),

    // Either checker may match (errors from a failed attempt are discarded; `expected` describes both)
    either: (expected, ...checkers) => (v, p, d) => {
        for (const c of checkers)
        {
            const trial = new d.constructor();
            if (c(v, p, trial))
                return true;
        }
        return bad(d, p, expected, v);
    },

    object: spec => (v, p, d) => checkObject(v, spec, p, d),

    array: (item, { max = Infinity, min = 0 } = {}) => (v, p, d) => {
        if (!Array.isArray(v))
            return bad(d, p, "an array", v);
        let good = true;
        if (v.length > max) { d.error(p, `at most ${max} entries allowed, got ${v.length}`); good = false; }
        if (v.length < min) { d.error(p, `at least ${min} entr${min === 1 ? "y" : "ies"} required`); good = false; }
        v.forEach((x, i) => { good = item(x, childPath(p, i), d) && good; });
        return good;
    },

    // A map with arbitrary keys whose values satisfy `item`
    map: item => (v, p, d) => {
        if (!isPlainObject(v))
            return bad(d, p, "an object", v);
        let good = true;
        for (const [k, x] of Object.entries(v))
            if (!ignored(k))
                good = item(x, childPath(p, k), d) && good;
        return good;
    },
};

// Properties starting with _ or $ are for notes and tooling
const ignored = key => key.startsWith("_") || key.startsWith("$");

// -> { ok, value }: `value` has only the properties that passed (so later stages can carry on and
// report more problems), or null if `value` isn't an object at all
export function checkProps(value, spec, path, diag)
{
    if (!isPlainObject(value))
        return { ok: bad(diag, path, "an object", value), value: null };

    let ok = true;
    const kept = {};
    for (const key of Object.keys(value))
    {
        if (ignored(key))
            continue;
        if (!Object.hasOwn(spec, key))
        {
            diag.error(childPath(path, key), "unknown property");
            ok = false;
        }
        else if (spec[key](value[key], childPath(path, key), diag))
            kept[key] = value[key];
        else
            ok = false;
    }
    for (const [key, checker] of Object.entries(spec))
        if (checker.required && !Object.hasOwn(value, key))
        {
            diag.error(childPath(path, key), "required");
            ok = false;
        }
    return { ok, value: kept };
}

export const checkObject = (value, spec, path, diag) => checkProps(value, spec, path, diag).ok;
