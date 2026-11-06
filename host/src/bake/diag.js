// Error and warning collector. Every entry has a JSON path into the user's config.

// "a" + "b" -> "a.b"; "a" + 3 -> "a[3]"; names that aren't plain identifiers use ["..."]
export function childPath(base, key)
{
    if (typeof key === "number")
        return `${base}[${key}]`;
    if (/^[A-Za-z_$][\w$]*$/.test(key))
        return base ? `${base}.${key}` : key;
    return `${base}[${JSON.stringify(key)}]`;
}

export class Diagnostics
{
    errors = [];
    warnings = [];
    #seen = new Set();

    error(path, message) { this.#add(this.errors, "e", path, message); }
    warn(path, message) { this.#add(this.warnings, "w", path, message); }

    // The same problem reached by several routes (an action used in many places) is reported once
    #add(list, kind, path, message)
    {
        const key = `${kind}\0${path}\0${message}`;
        if (this.#seen.has(key))
            return;
        this.#seen.add(key);
        list.push({ path, message });
    }
}

export const formatEntries = list => list.map(e => `${e.path || "(root)"}: ${e.message}`).join("\n");
