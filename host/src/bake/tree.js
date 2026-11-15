// Walking the resolved tree: where the keys (and so the images) are.

import { childPath } from "./diag.js";

// A key's images, one per state
export const IMAGES = ["normal", "pressed"];

// Each key's path in the layout file ("home.keys[3]", "home.keys[4].keys[1]"), set by stage 1. The
// tree's own lists skip empty slots and flatten groups, so their indices aren't the file's; a
// WeakMap keeps the path off the packed tree.
const sourcePaths = new WeakMap();
export const setKeyPath = (key, path) => void sourcePaths.set(key, path);

// Every key of the tree with its path: [key, "home.keys[3]"]
export function* allKeys(tree)
{
    const withPath = (keys, base) => keys.map((key, i) => [key, sourcePaths.get(key) ?? childPath(base, i)]);
    yield* withPath(tree.home.keys, "home.keys");
    for (const activity of tree.activities)
        for (const [p, page] of activity.pages.entries())
            yield* withPath(page.keys, childPath(childPath(childPath(childPath("activities", activity.name), "pages"), p), "keys"));
}
