// Walking the resolved tree: where the keys (and so the images) are.

import { childPath } from "./diag.js";

export const LAYERS = ["normalBg", "normalFg", "pressedBg", "pressedFg"];

// Every key of the tree with its path: [key, "home.keys[3]"]
export function* allKeys(tree)
{
    yield* tree.home.keys.map((key, i) => [key, childPath("home.keys", i)]);
    for (const activity of tree.activities)
        for (const [p, page] of activity.pages.entries())
        {
            const keys = childPath(childPath(childPath("activities", activity.name), "pages"), p);
            yield* page.keys.map((key, i) => [key, childPath(childPath(keys, "keys"), i)]);
        }
}
