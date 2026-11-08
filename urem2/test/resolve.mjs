// Stage-1 helpers for the urem2 layout tests: resolve a layout the way the bake does (the real Node host: icons,
// files relative to urem2/, the Lua compile check), and assert it is clean.

import assert from "node:assert/strict";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { resolve, createNodeHost, formatEntries } from "../../host/index.js";

// urem2/, where the entry files live (the bake resolves icon and script paths against it)
export const UREM2_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

let hostPromise;
const host = () => hostPromise ??= createNodeHost(UREM2_DIR);

// -> { tree, errors, warnings }
export async function resolveLayout(layout)
{
    return resolve(layout, await host());
}

// -> the resolved tree; fails the test listing every error and warning
export async function cleanLayout(layout)
{
    const { tree, errors, warnings } = await resolveLayout(layout);
    assert.equal(errors.length + warnings.length, 0,
                 `errors:\n${formatEntries(errors)}\nwarnings:\n${formatEntries(warnings)}`);
    return tree;
}

// The smallest layout around a Home key list (version 1, nothing else)
export const homeLayout = keys => ({ version: 1, home: { keys } });
