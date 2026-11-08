// The context every layout module builds from: the chosen target (urem2/targets.mjs) and its op builders.

import { makeOps } from "./ops.mjs";

export const makeContext = target => ({ target, ops: makeOps(target) });
