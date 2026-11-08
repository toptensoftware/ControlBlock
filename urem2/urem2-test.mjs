// urem2 against the test server (targets.test in urem2/targets.mjs: every device is urem2/test-server on the
// SERVER PC from examples/test-common.mjs). From the repo root:
//   node cli/cli.js upload urem2/urem2-test.mjs       (bake and upload)
//   node cli/cli.js bake urem2/urem2-test.mjs -o scratch/urem2-test.bin
// The house's version is urem2/urem2.mjs. See urem2/README.md.

import { buildLayout } from "./layout/build.mjs";
import { targets } from "./targets.mjs";

export default buildLayout(targets.test);
