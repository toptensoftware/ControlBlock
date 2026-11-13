// PlayStation (urem activity-playstation.js: devices and input only, no keys). urem's sound keys were global;
// here the activity gets one page with the shared 7ch Stereo and mute keys, where atv has them.

import { defineActivity } from "./activity.mjs";
import { sevenChKey, volUpKey, volDownKey, muteKey } from "../keys.mjs";

export const playstation = ctx => defineActivity(ctx, "playstation", {
    pages: [
        [
            { ...sevenChKey(ctx), x: 0, y: 1 },

            { ...volUpKey(ctx), x: 4, y: 0 },
            { ...volDownKey(ctx), x: 4, y: 1 },
            { ...muteKey(ctx), x: 4, y: 2 },
        ],
    ],
});
