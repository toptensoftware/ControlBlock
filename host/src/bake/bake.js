// The pipeline: resolve (stage 1) -> render (stage 2) -> pack (stage 3). Browser-safe: the host
// (files, Lua checker) and the renderer are injected.

import { resolve } from "./resolve.js";
import { renderImages } from "./render.js";
import { packTree, toBpak } from "./pack.js";
import { formatEntries } from "./diag.js";

// Thrown when the layout has errors; `errors` is the full list of { path, message }
export class BakeError extends Error
{
    constructor(errors)
    {
        super(`${errors.length} error${errors.length === 1 ? "" : "s"} in the layout:\n${formatEntries(errors)}`);
        this.errors = errors;
    }
}

// -> { blob (the BPAK file), tree, warnings }
export async function bake(layout, host, render)
{
    const { tree, errors, warnings } = resolve(layout, host);
    if (errors.length)
        throw new BakeError(errors);
    await renderImages(tree, render);
    return { blob: toBpak(packTree(tree)), tree, warnings };
}
