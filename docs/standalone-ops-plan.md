# Plan: remaining standalone ops and the Lua runtime

Written by a planning sub-agent (read-only; it did not read the binpack source or irtx's `activities.cpp`). Assumes the first milestone (take/release control, `hidKey` with a HID sink, `delay`, `switchActivity`) lands first. Nothing here is implemented or approved yet; the decisions in section 5 are open.

## 0. What the docs define

Op codes today, in `consts.js` OP and `config_layout.h`: 1 delay, 2 switchActivity, 3 hidKey, 4 httpGet, 5 httpPost, 6 udpPacket, 7 sendWol, 8 script. The milestone covers 1 to 3.

**Scope:**
- httpGet, httpPost, udpPacket and sendWol, with their executors and the net worker.
- The `script` op and the Lua runtime.
- The Lua API for all of them.
- Carrying `param` through the queue.
- The shared op spec and codegen.
- Baking scripts.

**Not ops, but they matter to the design:**
- Device on/off tracking and the switch order (from standalone.md's "Next" list).
- The hooks (`willActivate` etc.) and the device `turnOn`/`turnOff` lists. These are op lists and use the same executors.

**Ambiguous or missing in the docs:**
1. **MIDI** is in CLAUDE.md ("HID first, then MIDI, HTTP and UDP") but has no op, code, schema or API anywhere. Out of scope. The design makes it one spec entry plus an executor.
2. **HID gaps** the milestone will hit: consumer/media keys are unnamed; a chord is modifiers plus one key; `hold` is only legal under `press`/`release`, so it makes no sense from a script.
3. **Script API names:** config-format.md lists `http.get/post, udp, hid, delay, switchActivity` and omits WoL. "A script can do everything an op can" requires a `wol` function. standalone.md also says "script API surface: built as we go".
4. **httpGet as an op** is "fire and forget, response discarded", but the docs don't say whether it holds the queue. They also don't set the HTTP timeout, a response size cap, redirect handling, or header and body size limits (`CONFIG_MAX_OP_DATA` is 4096 for bodies and UDP data only).
5. **sendWol:** no port field (standard WoL uses UDP 9, some use 7). A null `broadcast` has no stated default (255.255.255.255, or a subnet broadcast derived from the netif).
6. **udpPacket** `host` may be a hostname, so a DNS lookup must happen in the net worker, never in the op task.
7. **`param`:** a string, but the docs don't say whether it is empty or `nil` when absent. config-format.md says it is a global; standalone.md's earlier text is looser.
8. **Script lifetime and state:** standalone.md says compile when the activity activates. Whether identical sources share state is not stated. Scripts in a device `turnOn`/`turnOff` list or a hook have no gesture, so gesture-dependent behaviour is undefined for them.
9. **Docs inconsistency:** "fire-and-forget calls (`http.post`, `udp`) don't yield", but a HID `tap` is down, gap, up, so `hid()` must be able to yield.
10. **Lua details:** the Lua version must match bake's wasmoon check (wasmoon 1.16.0 bundles Lua 5.4.x; believed 5.4.7, not verified). Integer and float width on the S3 is undecided: the S3 has a single-precision FPU only, so 64-bit integers and doubles use software floating point. Fine at button rates.
11. **`script` has no Lua equivalent** (nothing to call). Declare that exemption explicitly in the spec.

## 1. Architecture (DRY)

Three things need the same facts about each op: the bake validator (JS), the C++ blob validator and executors, and the Lua binder. The JS and C++ divide cannot be removed, so: one **op spec** that generates everything mechanical, plus **shared conformance vectors** for logic that can't be declarative.

### A. One op spec, `tools/bake/src/op-spec.js`

Per op: name and code; Lua name and signature; an ordered field list (name, kind, range and limits, optional, byte offset in the packed op); flags (`holdAllowed`, `replyCapable`).

Field kinds: `int` (range), `str`, `bytes`, `enum`, `httpUrl`, `mac`, `hexOrStr`, `headers`, `chord`, `activityName`.

Consumers:
- **`ops.js`** builds its OP_TABLE (props, checkers, packed form) from the spec. Custom `build` hooks stay for kinds that need context (`switchActivity` name to index, `script` file read and Lua check). `consts.js` OP and `schema.js` op types derive from it. Behaviour-preserving: the existing bake tests must pass unchanged.
- **A codegen script** (`tools/bake/gen-op-spec.js`) writes the checked-in `firmware/main/op_spec.gen.h`: op codes, field enums, offsets, limits and Lua names as `constexpr` data. A bake test fails if the committed header differs from the generator's output (same safety net as `sample_config.bin`).
- **The HID key table** (`hid.js`) is also generated into `hid_keys.gen.h`, so the device's `hid()` chord parser uses the same table as the bake.

### B. Offsets

Offsets in the spec come from binpack's own layout if it exposes one (step 1 starts by checking). If not, they are written explicitly in the spec and a bake **sentinel test** packs each op with unique field values and asserts the bytes sit at the spec's offsets.

### C. Conformance vectors, `tools/bake/test/op-vectors.json`

Valid and invalid values per kind (URLs, MACs, hex payloads, chords, ports, delays). Both the bake's JS tests and a new C++ host test (`TestOpVectors.cpp`) read it, so adding a vector fails whichever side is wrong.

### D. Device-side descriptor registry, `op_registry.h/.cpp`

One `OpDesc` per op, built from the generated data plus function pointers:
```
OpDesc { code, name, luaName, fields[], start, poll, cancel }
OpArgs { value[n] }     // decoded field values: ints, string pointers, byte pointer + length
```
Executors read values by generated field enums, e.g. `args.str(HttpGet::URL)`.

Three front ends produce `OpArgs` and run the same `start`:
1. The blob decoder reads an op record by the spec's offsets.
2. The Lua binder reads Lua arguments by the same field list: positional, or a single named-field table.
3. The blob validator in `config_blob.cpp` replaces its hand-written `sizes[]` and `switch` with a walk over the descriptors, keeping the structural checks (pointers, NULs, relocations).

`ValidateKind` exists once in C++ and once in JS, tied by the vectors. Limits on both sides come from the spec.

### E. Executor protocol, shared by the queue and scripts

```
OpStatus start(ExecCtx&, const OpArgs&, OpTask&)   // Done | Pending | Error(msg)
OpStatus poll(ExecCtx&, OpTask&)                   // for Pending
void     cancel(ExecCtx&, OpTask&)
```
`OpTask` is a small POD the caller owns (deadline, request id, generation). `ExecCtx` holds the injected hosts: clock, `HidSink`, `NetHost`, `ActivityHost`, and the queue for `switchActivity`.

- **`delay` becomes a normal executor** (Pending until the deadline). Today `OpQueue::Poll` special-cases it; a script's `delay()` then runs the same executor. A HID `tap` gap is Pending in both places.
- **`script` is also a descriptor.** Its `start` begins a coroutine; its `poll` polls the sub-task the script is waiting on and resumes when that is Done.
- One active slot in the queue; a script has one sub-task slot inside its own `OpTask`.

### F. Ordering of script-initiated ops

- The script op is the queue's active op, so the queue is paused while the script runs.
- Calls made by the script run **immediately and in the script's own order**; nothing else in the queue can interleave. The guarantee is the queue's single active slot.
- `switchActivity` from a script (and any hook ops it pushes) **enqueues** at the queue tail, so those ops run after the script finishes.
- If the queue is full, the Lua call returns `nil, "queue full"`.

### G. Param through the queue

- `OpQueue` entries become `{op pointer, param slot}`.
- A small pure `ParamPool` has 8 slots of 48 bytes, refcounted by the number of ops pushed for one trigger (all-or-nothing push, as now); a slot is freed when the last op of that list has started.
- Pool full: the trigger is dropped and counted (same policy as a full queue, per standalone.md).
- A param longer than the slot is truncated and counted, not rejected.
- `TriggerDispatcher::Trigger` already accepts `param`; it now passes it to `PushList`.
- The `OpHost::run` callback in `op_queue.h` is replaced by the registry and `ExecCtx` (step 3).

## 2. Lua hosting

### Library
Vendor the official Lua 5.4.x source as a git submodule (`lua/`), matching bake's wasmoon, and pin the version. Not verified whether an ESP-IDF registry component exists; don't rely on one.
- Device: `firmware/components/lua/CMakeLists.txt` wraps the submodule (compile `lua.c`/`luac.c` out).
- Host tests: add the C files to `test.mk.js` (plain C; rle565.c already builds this way under MSVC).

### Memory (see D1)
- State and heap are custom, via `lua_newstate` with an injected allocator `{alloc, free, cap}`.
- Recommended: a PSRAM heap (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`) with a hard cap of about 256 KB. A small internal heap would cost the display, USB and Wi-Fi: only ~119 KB internal is free once Wi-Fi is connected, largest block 76 KB.
- PSRAM writes from the Lua task: CLAUDE.md warns heavy PSRAM writes starve the bounce fill. Lua writes are small and bursty, but this needs a `RUN_STRESS` case with a script loop. **Hardware only.**

### Task and core placement
- A new **ops task** on core 0 (next to Wi-Fi and lwIP), priority below them. Core 1 stays with the compositor and fill.
- A separate **net worker task** for HTTP, UDP and WoL (core 0).
- The Lua state is touched only from the ops task. The net worker posts completions to a FreeRTOS queue and notifies the ops task.
- The ops task wakes on a task notification (trigger pushed, completion arrived) or at the next deadline; while busy it polls on a short tick of a couple of ms.
- Stack in internal RAM (about 10 to 12 KB, for Lua's C recursion limit); not PSRAM (flash and NVS calls from a PSRAM-stack task are restricted).

### Sandbox (against accidents, not attackers)
- Load only `base` (minus `dofile`, `loadfile`, `load`, `require`), `string`, `table`, `math`, `utf8`. No `io`, `os`, `package`, `debug` or `coroutine` (a user coroutine would conflict with our yields).
- `print` is replaced by `log`.
- Load chunks with `luaL_loadbufferx` in text mode only, so bytecode is rejected.
- **Instruction-count hook** (about every 10k instructions) checks the injected clock against a no-yield budget (first guess 100 ms) and raises an error when exceeded. Pathological `string` patterns can't be interrupted; the allocator cap bounds memory but not CPU. Accepted limit.
- **Per-run wall timeout** (first guess 10 s) cancels a script that waits too long. A late reply is dropped using a generation number.
- **HTTP timeout** (first guess 3 s) is a firmware constant.
- Allocation over the cap returns NULL, so the script errors and is logged; the queue carries on.

### Per-script state and lifetime
- A `ScriptHost` keeps a compile cache keyed by the script's source pointer.
- Each cached script has its own environment table chained to the shared API table. Globals persist across runs (a mute toggle can remember its state).
- Each run creates a fresh coroutine and sets `param` in the environment first, so locals never leak (safe: only one script runs at a time).
- Home and device scripts are resident. An activity's scripts are released once no queued op references them, followed by a full `lua_gc`.
- Compilation is **lazy** (first run), not at activation as standalone.md says: avoids glide stutter, and a compile failure becomes a logged no-op at first use (see D2).

## 3. Lua API (generated from the spec)

Each op's `lua` entry produces one function. One generic binder (a C function with a descriptor as upvalue) reads positional arguments, or a single table of named fields, validates with the shared `ValidateKind`, builds `OpArgs`, and calls the same `start`.

| Lua | Op | Args | Returns |
|---|---|---|---|
| `delay(ms)` | delay | ms 1..60000 | yields; `true` |
| `switchActivity(name)` | switchActivity | activity name | `true`, or `nil, "unknown activity"` |
| `hid(keys [, mode])` | hidKey | chord, mode `tap` (default), `down`, `up` | yields for the tap gap; `true`. `hold` is an error here |
| `http.get(url [, headers])` | httpGet | url, headers table | yields; `body, status`, or `nil, err` |
| `http.post(url, body [, contentType [, headers]])` | httpPost | | queued, no yield; `true`, or `nil, err` |
| `udp(host, port, data)` | udpPacket | data is a string or `{hex=...}` | `true`, or `nil, err` |
| `wol(mac [, broadcast])` | sendWol | | `true`, or `nil, err` |

Named-table forms are also accepted, e.g. `http.post{url=..., body=...}`. `http.get{url=..., reply=false}` is the fire-and-forget form of the op, so "anything an op can do" holds literally.

**Script-only helpers** (not ops, outside the parity rule): global `param` (string, empty when absent), `log(...)`, `millis()`. A `trigger(control, event, param)` helper is possible but deferred (feedback loops).

**Errors:** calls return `nil, err` on failure or timeout, and don't throw. Argument validation failures use the same message strings as the other front ends.

**HTTP worker rules** (from standalone.md "Rate"):
- A bounded request queue in front of one persistent worker with keep-alive.
- Overflow drops the oldest fire-and-forget request. A queued `http.get` that a script waits on is never dropped.
- A `get` waits behind queued posts.

## 4. Steps (test first; **HW** = hardware only, listed for you to run)

**Step 1. Op spec and codegen (bake side; refactor only)**
- Tests first: the spec round-trips to the current OP codes, field offsets (sentinel test) and packed bytes; `op-vectors.json` passes the JS validators; the generated-header drift test.
- Files: new `tools/bake/src/op-spec.js`, `tools/bake/gen-op-spec.js`, `tools/bake/test/op-vectors.json`, `firmware/main/op_spec.gen.h`, `hid_keys.gen.h`; edit `ops.js`, `consts.js`, `schema.js`, `hid.js`.
- Risks: binpack layout access. Keep existing bake tests green throughout.

**Step 2. C++ descriptor table, decoder and validator**
- Tests first: `TestOpVectors.cpp`; decoder tests with the sample blob plus a new all-ops sample; existing `TestConfigBlob` (corrupt and truncated blobs) passes unchanged.
- Files: new `op_registry.h/.cpp`, `op_decode.*`; edit `config_blob.cpp` (replace `sizes[]` and the switch), `config_layout.h`, `test.mk.js`; re-bake `sample_config.json`/`.bin`.
- Risk: refactoring a validator the loader depends on. Land it behind the existing tests.

**Step 3. Executor protocol and the op queue**
- Tests first, with a fake `ExecCtx`: Pending/poll/cancel; `delay` as an executor; param delivery; ParamPool exhaustion and truncation; cancel on takeover (clears the queue, calls `cancel` on the active op).
- Files: edit `op_queue.*`, `trigger_dispatch.*`, `TestTriggerDispatch.cpp`; new `param_pool.h`.
- Changes `OpHost::run`, so do it after the milestone's step 3, and port its `hidKey`/`switchActivity` executors to the descriptors.

**Step 4. Network executors (pure)**
- Tests first: a fake `NetHost` records requests and the test completes them explicitly. Cases: httpGet and httpPost request building (headers, content type, form default); oldest-drop for fire-and-forget; `get` never dropped; timeout; late reply dropped by generation; WoL packet (6x FF, then the MAC 16 times) and its broadcast default; UDP payload from string and from hex.
- Files: new `net_queue.*`, `exec_net.cpp`, `wol_packet.h`; `TestNet*.cpp`.

**Step 5. Net worker, device layer (HW)**
- `esp_http_client` plus lwIP sockets, DNS in the worker, keep-alive. Add `esp_http_client` and `lwip` to REQUIRES.
- Hardware checks: a real GET/POST, UDP and WoL (packet sniffer or WoL tester), a timeout, an offline receiver, and internal RAM and late fills with traffic running.

**Step 6. Lua runtime, pure (no bindings yet)**
- Tests first: `param` and its absence; persistence across runs; fresh locals; syntax error becomes a no-op; instruction budget and wall timeout using a fake clock; memory cap with a fault-injecting allocator; sandbox (no `io`/`os`/`load`/`require`, bytecode rejected); release plus GC.
- Files: Lua submodule and component; new `script_host.*`, `lua_alloc.*`; `TestScriptHost.cpp`; `test.mk.js`.
- Risk: MSVC build of Lua and host test build time.

**Step 7. The generic Lua binder**
- Tests first: per-op argument errors and returns; **parity test**: for every op in the spec, running it from the blob and from Lua produce identical recorded executor calls. This is the main DRY test and proves "anything a non-script can do, a script can".
- Also: an op missing a `lua` entry fails the build, unless explicitly exempt (`script` only).
- Files: new `lua_bind.*`, `TestLuaBind.cpp`.

**Step 8. The `script` op in the queue**
- Tests first: yield and resume with `http.get`, `delay` and `hid` tap; a fire-and-forget script finishes in one resume without pausing the queue; ordering (script calls run before the next queued op; `switchActivity` pushes land after the script); the script error path; cancel on takeover; a late reply after cancel; the encoder-rate case (many triggers, pool and queue limits).
- Files: new `exec_script.cpp`; edit `op_registry.cpp`.

**Step 9. Bake for scripts**
- Tests first: Lua version pinned to the device's (assert wasmoon's reported version); a script size cap (the blob validator currently allows any length; add a cap constant in the spec); identical sources stored once (already true).
- Optional later: a `name` field on `scriptOp` so error logs can say which script (schema change, needs a re-bake).

**Step 10. Device layer for the ops task (HW)**
- Ops task on core 0 with the owner module and lock from the milestone's step 2, the heap choice, the memory log at boot, and a `RUN_STRESS` extension (script loop plus Wi-Fi traffic).
- Hardware checks: late-fill counts with scripts running, internal RAM, button-to-request latency, a script holding the queue during `http.get`, and a held key releasing on takeover.

**Step 11. Docs** (propose, don't apply without OK)
- standalone.md: the executor protocol, the script API table, lazy compile.
- config-format.md: the script API and the generated op table.
- CLAUDE.md: the new modules, `lua/`, `tools/bake` spec and codegen workflow, new Open Items.

## 5. Decisions

**Decided by the user (supersede the options below where they differ):**
- **D2 compile time:** preferably compile when the activity is selected, but scripts an activity or device needs for deactivation or turn-off must not be discarded before those ops have run. If that is complicated, whatever is easiest: lazy compile on first run, with an activity's scripts released only after its deactivation lists have finished.
- **D3 state:** no state shared between scripts. A global table scripts can store arbitrary data in is a possible later addition, not needed at the start.
- **D4 `httpGet`:** both are needed. A script's `http.get` returns the response (body and status) so it can be scanned; the op also has a fire-and-forget form. Timeout 10 s; response size cap 16 KB (a larger response is truncated or errors; decide when implementing and test it).
- **D5 API names:** follow Lua convention, i.e. library tables (`http.get`, `http.post`, `udp.send`, `wol.send`, `hid.key`), with `delay` and `switchActivity` as plain functions. (The agent's table used flat `hid`, `udp`, `wol`; the Lua name is one field in the op spec, so this is a cheap change.)
- **D6 number width:** 32-bit (`LUA_32BITS`) is fine. Check that bake's wasmoon check doesn't depend on 64-bit numbers.
- **D7 WoL:** the defaults are good enough: UDP port 9, broadcast = subnet-directed from the netif, falling back to 255.255.255.255. No port field for now.
- **D8 MIDI:** later.

- **D1 Lua heap:** PSRAM with a cap (about 256 KB), plus a `RUN_STRESS` case with a script loop. Fall back to a small internal heap only if late fills appear.
- **Consecutive `hid.key` calls:** each tap must complete (down, gap, up) before the next call starts, so a script that calls `hid.key` several times in a row types the keys in order and never overlaps two presses. This is done by yielding: `hid.key(..., "tap")` yields until its up report has been sent (the executor is Pending during the gap), so the next call can't begin until it returns. The same executor serves a `hidKey` op in the queue, which holds the active slot until the up is sent. Add a short settle gap after the up before the next down is allowed (a firmware constant, to be tuned on the hardware), so two taps of the same key are not merged by the PC. Modes `down` and `up` send one report and return without yielding, so a script can build a chord by hand. Tests: a script with three `hid.key` calls produces down/up/down/up/down/up in order with the gaps respected (fake clock and fake `HidSink`).

## 5a. Original options (1-3-1)

**D1. Where does the Lua heap live?**
- Problem: internal RAM is tight (~119 KB free with Wi-Fi), and PSRAM writes can starve the display fill.
- Options: (a) PSRAM heap with a cap (256 KB); (b) a small internal heap (about 40 KB cap) with a hard limit on script size; (c) PSRAM, with a pre-allocated single contiguous arena to limit fragmentation.
- Recommendation: (a), with a `RUN_STRESS` case. Fall back to (b) if late fills appear.

**D2. When are scripts compiled?**
- Problem: standalone.md says on activation, which could stutter the glide.
- Options: (a) at activation, as written; (b) lazily on first run; (c) at activation but on the ops task after the glide starts.
- Recommendation: (b). Simplest, avoids the stutter. Deviates from the doc, so the doc needs updating.

**D3. Scope of per-script state.**
- Problem: the same script source may be used by several keys or the macro pad; the doc doesn't say whether they share globals.
- Options: (a) one state per distinct source (a mute toggle is shared between key and pad); (b) one per op (isolated); (c) (a) plus an explicit shared `store` table across scripts.
- Recommendation: (a). Matches how the bake already dedupes sources, and is the least surprising for the mute example.

**D4. Should the `httpGet` op hold the queue until the reply?**
- Problem: the doc says fire and forget, but ordering (a GET followed by another op) may matter to some users.
- Options: (a) fire and forget (doc); (b) hold until completion or timeout; (c) fire and forget by default with an optional `wait` flag on the op.
- Recommendation: (a). Matches the doc and avoids queue stalls; users who need ordering use a script. (c) adds a schema field.

**D5. Lua API names and the `wol` addition.**
- Problem: the doc names `hid`, `udp`, `http.get`, `http.post`, `delay`, `switchActivity` but omits WoL.
- Options: (a) functions as in the table (`hid`, `udp`, `wol`); (b) all under namespaces (`hid.key`, `udp.send`, `wol.send`); (c) keep the doc's names and add `wol` only.
- Recommendation: (a). The Lua name is one field in the spec, so cheap to change later.

**D6. Lua number width.**
- Problem: default 64-bit integers and doubles cost memory and use software floating point on the S3.
- Options: (a) default; (b) `LUA_32BITS`; (c) 64-bit integers, 32-bit floats.
- Recommendation: (a). Scripts are tiny, and (b) changes semantics from the bake's wasmoon check.

**D7. Default WoL broadcast address?**
- Problem: a null `broadcast` has no default.
- Options: (a) 255.255.255.255, UDP port 9; (b) the subnet-directed broadcast derived from the netif; (c) require `broadcast` in the config.
- Recommendation: (b), falling back to (a). Some routers drop limited broadcasts.

**D8. How should MIDI be handled?**
- Problem: it appears in CLAUDE.md but has no op in the specs.
- Options: (a) leave it out of this plan; (b) reserve code 9 and design it with the HID work; (c) design it now as a spec entry with a `MidiSink`.
- Recommendation: (a). The registry makes it one spec entry later; do it with the USB descriptor work, since it needs a second USB interface.

## 6. Risks

- The bake refactor (step 1) and validator refactor (step 2) touch working, tested code. Both are gated on the existing suites staying green.
- Step 3 changes the queue API the milestone agent has just written against; sequence it after their step 3.
- Lua in PSRAM and Wi-Fi traffic on core 0 are untested against the bounce fill; both need a `RUN_STRESS` pass on the hardware.
- Lua string-pattern CPU abuse can't be interrupted by the instruction hook. Accepted, since the config is the device owner's own.
- Host build time grows with Lua (about 30 C files).

## Critical files

- `firmware/main/op_queue.h` (and `.cpp`)
- `firmware/main/config_blob.cpp`
- `tools/bake/src/ops.js`
- `tools/bake/src/schema.js`
- `firmware/main/trigger_dispatch.cpp`
