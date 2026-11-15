// The binpack library (the `binpack` submodule), reached through the "#binpack" import in package.json.
// binpack isn't an npm workspace: npm would hoist its dependency and delete the node_modules the binpack
// repo has checked in. The CLI package build points "#binpack" at a vendored copy instead of this file.
export * from "../../../binpack/index.js";
