# Lua script runtime (`script` op)

Status: Complete (built and host tested; hardware checks listed below and in CLAUDE.md Open Items)

## Description
Build phase B of `docs/standalone-ops-plan.md`: the `script` op runs embedded Lua 5.4 source from the config blob, with an API that mirrors the ops. Phase A (net ops, executors, `param`, registry, activity switching) is built and host tested; it is not yet verified on the hardware, but the user says that must not block this work. Hardware-only checks are collected at the end for the user to run later.

## Notes

### State of the code (investigated)
- `OP_SCRIPT` exists end to end except the runtime: bake accepts `lua`/`file`, syntax-checks with wasmoon 1.16.0 (`tools/bake/src/lua.js`), dedupes sources; `config_blob.cpp` validates `scriptOp.source` (string array, NUL-terminated); `op_registry.cpp` has `{ OP_SCRIPT, "script", nullptr, nullptr, nullptr }` (so it is Done at once, a no-op).
- Executor protocol (`op_exec.h`): `start/poll/cancel(ExecCtx&, const cfgdata::op*, OpTask&)`. The executors take the **blob's op structs** (`cfgdata::httpGetOp` etc.), NOT the `OpArgs` the original plan proposed (that was superseded). `ExecCtx` already has `queue` ("for ops that push more (script)"), `hid`, `net`, `switch_activity`, `now_ms`.
- `OpTask` is a POD owned by the queue for the one active op: `deadline`, `request`, `generation`, `param` (never null), `error`, `releasing`. A script needs more state (a coroutine, a sub-task) so the script executor will need a per-run slot outside the POD (a single `ScriptRun` owned by the script host, since only one op is active at a time).
- Net executors currently force `req.wait = false` (`exec_net.cpp:17`); a script's `http.get` must wait for the reply, so that executor needs a variant with `wait = true` and a way to fetch the body/status.
- No Lua anywhere in the tree: no submodule, no component in `firmware/components` or the IDF. `.gitmodules` has rle565, node-buttonImage, SimpleLib, binpack.
- Host tests build 32-bit with MSVC via `firmware/test/test.mk.js`; Lua is plain C and should build there (rle565.c already does). Lua is built with default 64-bit numbers (see step 1).

### Design (from the plan, still valid)
- Heap: PSRAM with ~256 KB cap through `lua_newstate` and an injected allocator (D1). Sandbox: base (minus `dofile`, `loadfile`, `load`, `require`), `string`, `table`, `math`, `utf8`; no `io`/`os`/`package`/`debug`/`coroutine`; text chunks only. `print` becomes `log`.
- Limits: instruction-count hook for a no-yield budget (100 ms), 30 s wall timeout per run, HTTP 10 s, response cap 16 KB.
- State: none shared between scripts (D3). Each run is a fresh coroutine; `param` is set as a global before the run. Compile lazily on first run (D2), keyed by source pointer; an activity's scripts are released only after its deactivation lists have finished.
- API (D5): `delay(ms)`, `switchActivity(name)`, `hid.key(keys [,mode])`, `http.get/post`, `udp.send`, `wol.send`; helpers `param`, `log`, `millis`. Calls run immediately in script order; `switchActivity` enqueues at the queue tail. `hid.key` tap yields until the up report and settle are done.

### Open design questions found while investigating
1. **How the binder calls executors.** Executors want blob structs, and the Lua binder has Lua values. Simplest DRY route: the binder builds a scratch `cfgdata::*Op` (strings point into Lua-owned memory that stays alive while the call is Pending) and calls the existing `start/poll/cancel`. Alternative: refactor executors to take a neutral args struct. Assumed the first (no refactor of working code). See Flagged assumptions.
2. **Switch names to indices from Lua.** `switchActivity(name)` needs name-to-index; the blob has activity names (check `config_types.h`), so the host can search them. Needs a small helper in the script host.
3. **Where `ScriptHost` lives and its lock.** It runs only on the op task under the standalone mutex (like the other executors), so no new lock.
4. **Lua version parity.** Bake's wasmoon bundles Lua 5.4.x; vendor the same 5.4 minor. Not yet verified; step 1 checks it.
5. **MSVC build of Lua** (`-DLUA_32BITS`, no `lua.c`/`luac.c`) not tried yet.

## Flagged assumptions
- The binder builds scratch blob-op structs and reuses the existing executors (no executor refactor, no `OpArgs`). Revisit only if the parity test gets awkward.
- Lua is vendored as a git submodule `lua/` (official lua/lua repo, tag v5.4.x matching wasmoon) rather than an IDF registry component. Needs the user to approve adding a submodule (done in step 1).
- `http.get` reply support is added as a `wait` variant of the existing net executor/`NetHost` rather than a new op.
- The docs/CLAUDE.md updates are proposed only at the end (Rule 4), not applied.

## Plan
Each step ends building (host tests green; device build where files change). Tests first.

- [x] 1. **Vendor Lua and build it on the host.** Done: `lua/` submodule at v5.4.7; `firmware/lua_sources.txt` is the one source list, read by both `test.mk.js` and `main/CMakeLists.txt` (Lua compiled into `main`, like rle565; no separate component). `TestLuaBuild.cpp` passes (498 tests); debug device build is 0 errors. Lua sources are not linked into the image yet (nothing references them). Changing a `define` in `test.mk.js` needs `mk --rebuild`.
  - **Decision (user): 64-bit integers and doubles (Lua defaults), not `LUA_32BITS`.** Lua 5.4's `luaconf.h` hard-codes `LUA_32BITS 0`, so it can't be set from the build without patching the submodule. D6 is superseded; this also matches wasmoon's numbers, so no bake parity concern.
  - wasmoon 1.16.0 reports only `_VERSION` = "Lua 5.4"; the minor is unverifiable from JS. v5.4.7 is the assumed match (step 9 should not assert the minor).
- [x] 2. **Allocator and sandbox.** `lua_heap.*` (capped `lua_Alloc` over an injected realloc), `script_host.*` (state, sandbox: base without dofile/loadfile/load, string, table, math, utf8; text chunks only; `print` = `log`; each script's `_G` is its own env). Tests in `TestScriptHost.cpp` (cap with a real runaway script, sandbox, bytecode rejected).
- [x] 3. **Run model.** `param`, per-source env and compile cache (keyed by the source pointer in the blob; a bad script is cached as bad and logged once), fresh coroutine per run, instruction-count hook (100 ms slice), 30 s wall timeout, cancel. Tests as above.
- [x] 4. **Binder, sync ops.** `script_api.cpp`: `delay`, `switchActivity` (name lookup in `ExecCtx::root`), `hid.key` (tap yields; down/up don't; `hold` refused), `log`/`print`, `millis`. Chords use `hid_chord.*` over `hid_keys.gen.h`, generated by `tools/bake/gen-hid-keys.js` from `src/hid.js` (drift test `hid-keys-header.test.js`).
- [x] 5. **Net binder.** `http.get` (waits; body, status), `http.get{reply=false}`, `http.post`, `udp.send` (string or `{hex=}`), `wol.send`. `NetHost` gained `take_reply` and `abandon`; `NetReply` gained `body`; `NetQueue::Complete` takes the body; `exec_net` exports `NetSend(..., wait)`. A request's strings are copied into a ring of 12 slots of 6 KB (`ScriptHost::NetKeep`) because the queued `NetRequest` only holds pointers.
- [x] 6. **Parity test.** `TestScriptParity.cpp`: 13 cases run as a blob op and as Lua with identical HID, net and switch traces and poll counts; a coverage check fails when a new op code has no case.
- [x] 7. **`script` in the queue.** The registry's `script` entry calls `ScriptHost`. `TestScriptQueue.cpp`: holds the queue while waiting, error ends only that op, takeover cancels and abandons the reply, queued switch ops run after the script. `Standalone` owns the `ScriptHost` (`StandaloneHost::script` platform; realloc null = scripts off) and closes it on every re-arm; three tests in `TestStandalone.cpp`.
- [x] 8. **Device wiring.** `standalone_task.cpp`: PSRAM realloc (`heap_caps_realloc`), log to `ESP_LOGI("lua")`, op task stack 4096 -> 12288. `net_worker.cpp`: 16 KB PSRAM response buffer for waited requests, `take_reply`/`abandon`. `lua_include.h` works around this toolchain leaving `LLONG_MAX` undefined in C++. Debug and release device builds: 0 errors (one harmless linker warning that `times` is not implemented, from Lua's seed code).
- [x] 9. **Bake side.** `MAX_SCRIPT_BYTES` = 16384 (bake) = `CONFIG_MAX_SCRIPT` (blob validator, now enforced; tests on both sides). A bake test pins the checker to Lua 5.4 with 64-bit integers. The minor version is not readable from wasmoon.
- [x] 10. **Docs.** Approved by the user and applied 2026-11-07 (CLAUDE.md, standalone.md, config-format.md, standalone-ops-plan.md). The script API table went into config-format.md only; standalone.md links to it (one copy).

## Results
- Host tests: 552 pass (debug and release). Bake: 139 pass. Device debug and release builds: 0 errors. App size 0x114490 (release), 46% of the app partition free.

## Deviations from the plan, and decisions to review
- No separate `components/lua`; Lua is compiled into `main` from `lua_sources.txt` (shared with the tests).
- D6: 64-bit numbers (user decision). Plan text about `LUA_32BITS` is obsolete.
- Scripts are not released per activity: the compiled cache lives until the next re-arm (takeover/release). Scripts are small and the cap is 256 KB, so this was judged simpler and safe (an activity's deactivation scripts are never discarded early). Revisit if a config has many large scripts.
- `switchActivity` called from a script goes through the same `Standalone::SwitchActivity` as the op; with a full queue it returns `nil, "activity switch refused: op queue full"`.
- `hid.key(..., "hold")` returns `nil, err` (hold only means something on press/release bindings).
- The script holds the standalone lock while it runs, up to 100 ms per slice (`SCRIPT_SLICE_MS`), so a key event can wait that long. Fine for button-sized scripts; measure on the hardware.
- A waited reply's body buffer is shared (one 16 KB buffer, valid until the next waited request completes); safe because only the one running script waits.
- The `started` hook (`StandaloneHost::run`) fires for a script's sub-ops too.

## Proposed doc changes (Rule 4, not applied)
- **CLAUDE.md:** add to the Firmware module list: `lua_heap`, `script_host`, `script_api`, `hid_chord`/`hid_keys.gen.h`, `lua_include.h`, the `lua/` submodule (v5.4.7, 64-bit numbers, sources listed in `firmware/lua_sources.txt`, `LLONG_MAX` note), the `NetHost` `take_reply`/`abandon`; add `lua/` to the repo layout and submodule list; add `node gen-hid-keys.js` next to `gen-types.js`; add Open Items for the Lua hardware checks; update the Standalone mode paragraph ("scripts are not built") and `.gitmodules` note.
- **docs/standalone.md:** script API table, lazy compile (compiled on first run, cached until re-arm), limits (slice, wall, heap, response cap), `Next:` line.
- **docs/config-format.md:** the script API (names above, positional or table args, `nil, err` convention), the 16 KB source cap.
- **docs/standalone-ops-plan.md:** status line (phase B built), D6 superseded.

### Hardware checks (for the user, later)
- Late-fill counts and internal RAM with scripts running (new `RUN_STRESS` script-loop case), Lua PSRAM writes vs the bounce fill.
- Button-to-request latency; a script holding the queue during `http.get`; held key released on takeover.
- Net ops verified on the hardware 2026-11-06 and activity switching 2026-11-07, so these can run now.
- Op task stack (12 KB): check `uxTaskGetStackHighWaterMark` with a deeply recursive script.
- `http.get` reply path end to end (PSRAM response buffer, timeout, abandon on takeover).
- Key-event latency while a script is mid-slice (standalone lock held).
