// Take control of the ControlBlock straight away, hold it, then release it.
// For the takeover-in-the-middle-of-a-switch test (wip-activity-device-management.md step 9 h).
//   node tools/test-server/takeover.mjs [seconds]      (default 3)

import { ControlBlock } from "../../host/index.js";

const seconds = Number(process.argv[2] ?? 3);
const cb = await ControlBlock.open();
await cb.takeControl();
console.log(`took control; releasing in ${seconds} s`);
await new Promise(r => setTimeout(r, seconds * 1000));
await cb.releaseControl();
console.log("released");
await cb.close();
