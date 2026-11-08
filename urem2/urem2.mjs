// urem2 for the house: the real devices (targets.real in urem2/targets.mjs). From the repo root:
//   node cli/cli.js upload urem2/urem2.mjs            (bake and upload)
//   node cli/cli.js bake urem2/urem2.mjs -o scratch/urem2.bin
// The test server's version is urem2/urem2-test.mjs. See urem2/README.md.

import { buildLayout } from "./layout/build.mjs";
import { targets } from "./targets.mjs";

export default buildLayout(targets.real);
