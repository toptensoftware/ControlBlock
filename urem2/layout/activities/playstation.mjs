// PlayStation (urem activity-playstation.js: devices and input only, no keys). urem's sound keys were global;
// here the activity gets one page with the shared 7ch Stereo and mute keys, where atv has them.

import { defineActivity } from "./activity.mjs";
import { sevenChKey, muteKey } from "../keys.mjs";

export const playstation = ctx => defineActivity(ctx, "playstation", {
    pages: [{ 10: sevenChKey(ctx), 14: muteKey(ctx) }],
});
