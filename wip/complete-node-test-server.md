# Port the test server to Node

Status: Complete

## Description
Replace `examples/test-server/net_test_server.py` (Python 3, standard library) with a Node.js version that
behaves the same for the device, so the hardware test kit (`examples/`) needs only Node. Add host-side tests
for it (the Python server had none), then update the references and remove the Python file.

## Notes

### What the Python server does (96 lines)
- **HTTP on :8000**, `ThreadingHTTPServer`, HTTP/1.1 keep-alive (the device reuses one connection per host;
  the server's per-request line prints the client's source port, so a repeated port = a reused connection).
  Python keeps an idle connection open until the client closes it (no keep-alive timeout).
- Per request it prints `HH:MM:SS.mmm [<client port>] <METHOD> <path>`, then each header except `Host` and
  `Accept` as `            Name: value` (original case and order). Millisecond timestamps are used by the
  latency checks (`script-test.mjs` "Latency op+script").
- GET routes (prefix match on the path, query included, so `/slow?n=3` matches `/slow`):
  - `/redirect` → 302, empty body, `Location: /get?redirected=1`
  - `/hang` → accepts, never answers (sleeps 60 s, then the handler returns with no response)
  - `/slow` → 200 `ok` after 3 s
  - `/big` → 200, `Content-Length` 5 MB, written in 64 KB chunks of `x`; prints `/big fully sent` or
    `/big client went away`
  - `/get` → 200 `ok`
  - `/mute` → 200 `mute=<state>` (state starts `"0"`)
  - anything else → 404 `not found`
- POST (any path) → reads `Content-Length` bytes, prints the request lines plus
  `            body (<n> bytes): b'...'` (Python repr), replies 200 `ok`. `/mute` with a body starting
  `mute=` sets the state to the next character.
- Every reply sets `Content-Length`. Logging of Python's own access lines is off.
- **UDP on :9 (WoL) and :9999**, `SO_BROADCAST`, bound to all interfaces. Per datagram:
  `HH:MM:SS.mmm UDP :<port> from <ip>:<port> <len> bytes <repr of first 32 bytes>`; a 102-byte packet starting
  with six `0xFF` adds `  WoL magic, mac=aa:bb:..., repeats ok=True|False` (the MAC repeated 16 times).
- Startup line: `HTTP :8000, UDP :9 and :9999 - Ctrl+C to stop`.

### Who uses it
- Layouts: `examples/net-test.json` (all GET routes except `/mute`, POST `/post`, UDP :9999 by IP/name/hex,
  WoL to :9), `examples/activity-test.mjs` and `examples/script-test.mjs` via `examples/test-common.mjs`
  (`SERVER`, `HTTP`, `udp()`, `get()`; script-test also uses `/hang`, `/big`, `/mute` GET and POST).
- Mentions of `net_test_server.py`: `examples/README.md` (table row and the hard-coded address note),
  comments in `examples/test-common.mjs`, `examples/activity-test.mjs`, `examples/script-test.mjs`,
  `CLAUDE.md` (layout line 67, Net ops and Activity switching items), `wip/wip-lua-hardware-checks.md`
  (run instructions and notes). `complete-*.md` mention it too but must not be touched.
- `scratch/net_test_replay.py` (gitignored throwaway) replays requests against it; leave it.
- `examples/test-server/takeover.mjs` is already Node; unchanged.

### Node design
- Node v24 is installed; `cli/` and `host/` already require Node >= 22 and are ES modules. `examples/` has no
  `package.json` and no dependencies: use `.mjs` and only Node built-ins (`node:http`, `node:dgram`).
- New file `examples/test-server/test-server.mjs`: exports `startTestServer(options)` returning
  `{ httpServer, httpPort, udpPorts, close() }`; run directly (`node examples/test-server/test-server.mjs`) it starts on
  the real ports. Options (defaults = the Python behaviour): `httpPort` 8000, `udpPorts` [9, 9999], `host`,
  `log` (default `console.log`), `hangMs` 60000, `slowMs` 3000, `bigBytes` 5 MB, `now` (for timestamps).
  Port 0 and short timings let tests run fast on free ports.
- One `repr(bytes)` helper shared by the POST body and UDP lines (DRY): printable ASCII as is, `\xNN`
  otherwise, in quotes (`b'...'` like Python, so log lines look the same).
- Node specifics to get right:
  - **Keep-alive:** Node's default `keepAliveTimeout` is 5 s, which would close the device's idle connection
    and change the "connection reused" check. Set `server.keepAliveTimeout = 0` (no timeout, like Python).
    `requestTimeout` (300 s) and `headersTimeout` (60 s) are fine for `/hang` (60 s).
  - **Header case:** `req.headers` is lowercased; print from `req.rawHeaders` to keep case and order.
  - **No blocking:** `/slow` and `/hang` use timers (Python used one thread per connection; Node is async).
  - **`/big`:** write with backpressure (`write()` false → wait for `drain`), stop if the socket closes;
    `client went away` when `res` closes before `finish`.
  - **UDP:** `dgram.createSocket("udp4")` bound to `0.0.0.0`, `setBroadcast(true)` after bind. Binding
    port 9 needs no admin on Windows.
  - **HTTP bind:** listen on `0.0.0.0` (IPv4, like Python's `""`), so remote addresses aren't `::ffff:`.
  - Node adds `Date` / `Connection` headers to replies; harmless for the device.
- **Firewall:** the Windows firewall rule was for `python.exe`; `node.exe` needs its own (Windows prompts on
  the first listen). Tell the user in the hardware check.

### Tests
- `examples/test-server/test-server.test.mjs`, Node's test runner, run with
  `node --test examples/test-server/test-server.test.mjs` from the repo root (Node 24 doesn't accept a bare directory: it tries to load it as a module). Starts the server on port 0 (HTTP and UDP) with a
  captured `log`, short `hangMs`/`slowMs`, small `bigBytes`, and checks responses and log lines.
- Can't be automated: the device against the real server (Wi-Fi, firewall, port 9). That is the last step,
  done by the user on the hardware.

## Flagged assumptions
- Remove `net_test_server.py` once the Node server is verified on the hardware (step 6), rather than keep both.
- `/hang`: after `hangMs` the Node server destroys the socket (Python just left the connection open with no
  reply). The device gives up after 10 s anyway, so either is fine for the tests.
- New name `test-server.mjs` (the folder already says what it is) instead of `net_test_server.mjs`.
- Tests live beside the server and are run with `node --test examples/test-server/test-server.test.mjs` (no `package.json`
  added to `examples/`).

## Plan
- [x] 1. Tests first: write `examples/test-server/test-server.test.mjs` covering every route and log line in
  *Notes* (GET `/get` with header lines, Host/Accept skipped and case kept; `/redirect` 302 + Location;
  404; `/slow` delayed; `/hang` no reply within the window; `/big` full length with `fully sent`, and an
  aborted download logging `client went away`; POST body line and `/mute` GET/POST round trip; two requests
  on one keep-alive agent share a source port (and the returned handle reports `keepAliveTimeout` 0, so
  idle connections aren't closed; a real 5 s wait would slow the suite); UDP line format, repr of non-printable bytes, the 32-byte
  cut; WoL magic with good and bad repeats; timestamp format `HH:MM:SS.mmm`). They fail (no module yet).
- [x] 2. Write `examples/test-server/test-server.mjs` as designed in *Notes* until the tests pass. Run it
  directly once to check the startup line and that ports 8000, 9 and 9999 bind.
- [x] 3. Point the users at it: comments in `examples/test-common.mjs`, `examples/activity-test.mjs`,
  `examples/script-test.mjs`; `examples/README.md` (table row: `node examples/test-server/test-server.mjs`,
  Node only, and the address note); `wip/wip-lua-hardware-checks.md` run instructions.
- [x] 4. Propose the `CLAUDE.md` edits (layout line, Net ops and Activity switching mentions, a line under
  the test sections for `node --test examples/test-server/test-server.test.mjs`) and apply them once the user confirms (Rule 4).
- [x] 5. Hardware check (user): allow `node.exe` through the firewall, run the Node server, and tap through
  `examples/net-test.json` (GET headers, POSTs, redirect, 404, hang timeout, 5 MB, 2nd host, slow, burst,
  UDP IP/name/hex, WoL) and the `/mute` and latency keys of `script-test.mjs`; compare with the Python
  server's output (connection reuse shows as a repeated source port).
- [x] 6. Once step 5 passes: delete `examples/test-server/net_test_server.py`, grep for leftover mentions
  (outside `complete-*.md`), and rename this file to `complete-node-test-server.md`.

## Implementation notes
- Steps 1-3 (2026-11-07): 14 tests, all pass first time against the new server (~0.75 s); before it existed
  they failed on the missing module. Run directly, the server binds :8000, :9 and :9999 and prints the startup line.
- `repr` always uses single quotes and escapes `'` (Python switches to double quotes when the bytes hold a `'`
  but no `"`). Only affects how such a body looks in the log.
- `wip-lua-hardware-checks.md`: only the "Before you start" line was changed; its history notes still name the
  Python file as written.
- Step 5 (2026-11-12): the user's hardware run passed.
- Steps 4 and 6 (2026-11-12): CLAUDE.md edits approved and applied; the Python server deleted.
