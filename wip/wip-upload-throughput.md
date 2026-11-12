# Hosted image upload throughput

Status: Implement

## Description
Image uploads in hosted mode run at ~250 KB/s, about a third of what the USB link carries (~705 KB/s both ways
with 8 KB pings). A full page of keys (30 opaque images, 142.5 KB) takes 556 ms. Find where the time goes and
how to speed uploads up, before deciding whether deflate on transmission is worth adding.

## Notes
- `examples/bench.js` on the hardware (2026-11-12, after the key render redesign, release build): ping 0.63 ms
  round trip; ping 8192 B x 50: 705 KB/s (both ways); upload 40 x 21.8 KB: 251 KB/s; a page of keys: 30 images,
  142.5 KB in 556 ms (256 KB/s; 4.7 KB per image).
- Deflate measured on urem2's opaque images: 2.4x smaller per image (`wip/complete-key-render-redesign.md`,
  Measured). Only worth it if the link, not the device, is the limit.
- Suspects: a reply per chunk (no pipelining), the opacity check (`check_image` decodes every image in full),
  the PSRAM copy, the protocol task's read loop and chunk size, host-side encoding or waiting in `ControlBlock.js`.
- Keep the USB rules in CLAUDE.md: never let a bulk IN read time out (heartbeat), ZLP after a write that is a
  multiple of 64 bytes, and nothing may write PSRAM heavily while fills run.

### Investigation (2026-11-12)

**The path, end to end**
- Bench (`examples/bench.js:37`, `:55`): every `putImage` is started at once (`Promise.all`); the timer covers sending
  only (images are encoded before it starts). So uploads are already fully pipelined: no per-image or per-chunk wait.
- Host (`host/src/ControlBlock.js:335-344`): `putImage` splits into chunks of `maxPayload - 12` = 8180 B
  (`PUT_IMAGE_PREFIX`, `protocol.js:17`) and sends them all without waiting. `request()` (`:151-161`) writes at once;
  `_write()` (`:518-527`) chains writes so **one `transferOut` is outstanding at a time**, plus a ZLP when the
  length is a multiple of 64. A 4.7 KB key = one ~4.7 KB transfer; the 21.8 KB image = 3 transfers of 8204 B.
  Ping 8 KB messages are also 8204 B, so the two benches use the same write sizes.
- Device max payload 8192 (`firmware/main/protocol.h:31`). USB is full speed, 64-byte bulk packets (`usb.cpp:55`).
- TinyUSB (`sdkconfig.defaults:41-44`): RX FIFO 4096, TX FIFO 2048, `VENDOR_EPSIZE` 512; DMA mode
  (`CONFIG_TINYUSB_MODE_DMA=y` in `build/release/sdkconfig`). The OUT endpoint is armed for **one 512 B transfer at a
  time** (`tusb.c:476-509`): after each 512 B the DMA completes, the DWC2 ISR posts to the TinyUSB task (prio 5,
  core 0, `usb.cpp:307-309`), `vendord_xfer_cb` copies it into the FIFO, calls `tud_vendor_rx_cb` (wakes the
  protocol task) and re-arms (`vendor_device.c:271-278`), only if the FIFO has >= 64 B free. The endpoint NAKs
  in between.
- Protocol task (`host_link.cpp:160-190`, core 0, prio 4): woken by the rx notify, reads **512 B at a time**
  (`buf[512]`, `:162`) via `usb_vendor_read` (`usb.cpp:325-332`, a mutex per read; each read also re-arms), and
  `Protocol::Receive` (`protocol.cpp:192-263`): PUT_IMAGE data goes **straight into the image's PSRAM allocation**
  (`ImageData` state, `:218-230`; `ImageStore::PutData` memcpy, `image_store.cpp:136-145`), no staging copy.
  `flush` after every 512 B read (`:256`, cheap when nothing to send). No `vTaskDelay`, no throttle anywhere on
  this path (docs/image-assets.md:189 only notes it should be measured).
- End of image: `PutEnd` under the model lock (`protocol.cpp:224-226`) runs `Check` -> `check_image`
  (`image_store.cpp:157`, `rle565.c:697-809`: walks every pixel of the 144x144 image, reading from PSRAM), then
  `Install` and a 16-byte response. `EndMessage` takes the lock again for `Collect` (`:334-336`). Allocation is
  `heap_caps_aligned_alloc` in PSRAM at the first chunk (`model.cpp:18`). Lock contention: none during the bench
  (taking control cleared the keys; the compositor only takes the lock briefly in `get_page`).
- Response: 16 bytes per message; IN carries next to nothing during an upload.

**What the numbers say**
- Both upload benches run at the same rate per byte (251 KB/s for 21.8 KB images in 8 KB messages, 256 KB/s for
  4.7 KB images, one message each). Fitting t = bytes/R + n x c to the two gives c ~ 0 (slightly negative, i.e.
  noise): **no visible per-image or per-message cost**. The limit is per byte (or per 512 B transfer), not round
  trips, `check_image`, allocation or message count.
- 251 KB/s is ~4 x 64 B packets per 1 ms frame, i.e. one 512 B device transfer every ~2 ms. Full-speed bulk can
  carry ~19 packets/frame (~1.1-1.2 MB/s in practice ~1 MB/s); the ping bench moved ~11 packets/frame
  (705 KB/s both ways = 352 KB/s each way), so the bus isn't the limit.

**Per-image breakdown (4.7 KB key, 18.5 ms each in the page bench)**
| Component | Estimate | Share |
|---|---|---|
| Bytes on the wire at ~1 MB/s | ~4.5 ms | ~25% |
| Unexplained per-byte stall (endpoint NAKing / transfer gaps) | ~13-14 ms | ~70-75% |
| Round trips | ~0 (pipelined; one 0.63 ms RTT per batch, ~0.02 ms per image) | ~0% |
| PSRAM memcpy (4.7 KB) | <0.1 ms (maybe 0.5 ms with fill contention) | <3% |
| `check_image` (20.7 k px x ~5-15 cycles at 240 MHz) | ~0.4-1.3 ms, overlapped with reception (4 KB FIFO) | hidden |
| Alloc, install, locks, response | tens of µs | ~0% |

**Likely bottleneck:** the OUT pipe's refill rhythm, not device processing: the endpoint is armed 512 B at a time
and the re-arm (DWC2 ISR -> TinyUSB task -> FIFO copy -> re-arm, on core 0) plus the host controller's NAK retry
policy for a full-speed device (xHCI/TT may not retry until a later (micro)frame) leaves the pipe idle more than it
runs. Contributors on core 0: the bounce-fill ISR (~120 µs every ~615 µs at rest; allocated from `app_main`, so
probably core 0, see Flagged assumptions) delaying the USB ISR/task, and code fetched from PSRAM. Second suspect:
the host's single outstanding `transferOut` (a gap per transfer: small for 8 KB transfers, so probably minor). Why
ping's OUT side does better (352 vs 251 KB/s) is unexplained: ping data goes to internal RAM (`m_buf`) and image
data to PSRAM, so PSRAM write stalls against the fill's reads remain a suspect until measured.

**Candidate fixes**
1. Bigger OUT transfers: `CONFIG_TINYUSB_VENDOR_EPSIZE` 2048-4096 with RX FIFO 8-16 KB, and a protocol read buffer
   to match. Fewer re-arms per KB. Expected: up to 2-3x if the re-arm gap is the cause. Risk: internal RAM (epout +
   epin buffers are both EPSIZE, plus the FIFO: ~10-20 KB more, against ~106 KB free with Wi-Fi), and the ZLP rule
   matters more (a transfer ends only when full or short; already handled by the host).
2. Re-arm sooner: drain in the TinyUSB callback or read the whole FIFO per wake (bigger `buf`), so the FIFO never
   blocks a re-arm. Small gain alone; pairs with 1.
3. Host: keep 2-3 `transferOut`s outstanding (order is kept per endpoint; the ZLP must stay right after its
   transfer). Expected: small (removes the per-transfer Node/libusb gap). Risk: WebUSB/browser behaviour, error
   handling.
4. Move the USB ISR/TinyUSB task off the fill ISR's core (or the fill ISR to core 1). Gain unknown; risk to the
   bounce fill rules (needs a stress run).
5. Deflate (2.4x fewer bytes): helps whatever the per-byte limit is (fewer transfers through the same pipe), so
   ~2.4x on its own; ROM `tinfl` could inflate it. Costs: inflate CPU and a staging buffer, protocol/host change.
   Better decided after 1-2: if the pipe reaches ~1 MB/s, a page is ~140 ms and deflate is less needed.
- Not worth doing: windowing/pipelining (already fully pipelined), bigger messages or batching images per message
  (no per-message cost shows), speeding up `check_image` (hidden today; ~30-40 ms per page only if the link gets
  ~4x faster).

**Measurements that would confirm it**
1. Host only, no firmware change: a bench "sink" variant sending 8 KB `PUT_IMAGE`s with id 0 (the device answers
   BAD_ARG and skips the data in the Skip state: no PSRAM write, no check; use `request()`, not `call()`). ~250
   KB/s -> USB pipe; much faster -> the image path (PSRAM copy).
2. The same benches on `-DCB_NO_DISPLAY=ON` and `-DCB_IDLE_FILL=ON` builds: a big gain points at the fill ISR /
   PSRAM contention on core 0.
3. Device counters on the `CB_STATS_LOG` line: OUT transfers completed and bytes, FIFO level seen at each
   protocol read (often full = device drains too slowly; near empty = bytes aren't arriving), µs in `Receive` and
   in `check_image` (cycle counter), protocol task wakes.
4. A build with `VENDOR_EPSIZE` 2048/4096 (delete the generated sdkconfig) and the sink bench: the direct test of
   fix 1.
5. Host: timestamps around each `_transferOut` in bench to see the gap between transfers.

### Implement notes (steps 1-3, 2026-11-12)
- **Sink bench:** `examples/bench-sink.mjs` (`sinkSend(cb, count)`: `count` x 8192 B `PUT_IMAGE`s for id 0 via
  `request()`, all at once; throws unless every answer is BAD_ARG), tested in `examples/bench-sink.test.mjs`;
  `host/test/ControlBlock.test.js` checks the FakeDevice refuses id 0, stores nothing and stays in step. `bench.js`
  prints `sink 8192 B x 50: N KB/s (one way, nothing stored)` between the ping and upload cases.
- **Counters:** `firmware/main/link_stats.h/.cpp` (pure, tested in `test/TestLinkStats.cpp`): `LinkCounters`
  (lock-free atomics; always counting, only logged with `CB_STATS_LOG`) and `FormatLinkSample`. Fed by
  `host_link.cpp` (`on_rx` counts each OUT transfer from TinyUSB's `tud_vendor_rx_cb` bufsize; the protocol task
  counts wakes, the FIFO level before each read (`usb_vendor_read` gained a `waiting` out-param) and cycles in
  `Receive`). `check_image` time: `ImageStore::PutEnd` times `Check` (`ImageStoreStats.checks`/`check_cycles`,
  cumulative; main diffs them; 0 cycles on the host; tested in `TestImageStore.cpp`). The stats line gains, after
  `img ...`: ` | usb rx: X xfers, K KB, W wakes, R reads, fifo min/avg/max, receive U us, C checks, U us` (absent
  in a second with no transfers, reads or checks). The device wiring (usb.cpp, host_link.cpp, main.cpp) is
  **not unit testable**: only the hardware run shows it works.
- **Reading the counters:** KB/xfers = bytes per OUT transfer (512 normally, 4096 in usbbig when full; small =
  short messages). fifo avg near the FIFO size = the device drains too slowly; near one transfer = bytes arrive
  slowly. `receive` + `checks` us out of 1,000,000 = protocol task CPU share. xfers/s x 512 B vs KB/s = pipe rate.
- **usbbig variant:** `firmware/sdkconfig.usbbig` (EPSIZE 4096, RX FIFO 16384; TX FIFO unchanged 2048) and presets
  `debug-usbbig` / `release-usbbig` (`build/debug-usbbig`, `build/release-usbbig`). The protocol read buffer is
  `CONFIG_TINYUSB_VENDOR_EPSIZE`. **Internal RAM cost: +23,040 B static** (`idf.py size`, release vs
  release-usbbig: DIRAM 124,519 -> 147,559 B; .bss +15,872, .data +7,168), i.e. ~22.5 KB less free heap at boot
  (the boot `heap after init` line will confirm; ~106 KB free with Wi-Fi connected today -> ~84 KB).
- `CLAUDE.md` mentions presets `debug-iram`/`release-iram` (`sdkconfig.wifi_iram`), but they are no longer in
  `CMakePresets.json` (removed in 1f7b7fd; only a stale `build/debug-iram` folder remains): propose fixing that line
  and adding the usbbig presets when the job's docs step runs.
- Pre-existing host failure, not from this job: `host` `npm test` fails 1 of 265, "FakeDevice's geometry and wire
  sizes match the firmware's headers": it expects `#define SCREEN_PAGER_H` in `screen_layout.h`, which 6d23afc
  ("Drop the pager bar") removed. Fix: drop that assertion (the test's gridY check already covers it).

**Build folders as left (all build clean, 0 errors):**
- `build/debug`: debug, every `CB_` switch off (unchanged).
- `build/release`: release, `CB_RUN_STRESS=OFF`, `CB_STATS_LOG=ON` (no other switches set): ready for step 4's first run.
- `build/release-usbbig`: new, `CB_STATS_LOG=ON`, 4 KB transfers.
- `build/debug-usbbig`: not created (preset only).

### Step 4 run instructions
From PowerShell in `firmware/` (once per window: `Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue;
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1`). The board on native USB + UART (COM4). For each run:
flash and monitor in one window, wait for `heap after init`, then in a second window from the repo root run
`node examples/bench.js` (about 15-30 s: ping, ping 8 KB, sink, upload, page of keys). Note whether Wi-Fi is
connected and whether the board is on a hub or a root port (the same for every run).

1. Normal: `idf.py --preset release -p COM4 flash monitor` (already configured with stats on).
2. Idle fill: `idf.py --preset release -DCB_IDLE_FILL=ON -p COM4 flash monitor`.
3. No display: `idf.py --preset release -DCB_IDLE_FILL=OFF -DCB_NO_DISPLAY=ON -p COM4 flash monitor`.
4. Big transfers: `idf.py --preset release-usbbig -p COM4 flash monitor`.
5. Afterwards put release back: `idf.py --preset release -DCB_NO_DISPLAY=OFF build` (switches stick in the cache).

Copy back for each run: bench.js's whole output; the monitor's `heap after init` line; and the `controlblock:`
stats lines (one per second) from just before the bench starts to just after it ends (~20-30 lines). Check each
flash shows `Hash of data verified.`

## Questions for the user
- Was Wi-Fi connected, and the board on a hub or a root port, when the bench ran? (Both affect core 0 load and
  NAK retry timing.)
- How much internal RAM can go to bigger USB buffers (~10-20 KB)?
- What throughput would be good enough (e.g. a page of keys under ~150 ms)? That decides whether deflate is needed
  after fixing the pipe.
- OK to add a sink mode to `bench.js` and temporary counters to the firmware for the measurements?

## Flagged assumptions
- The bounce-fill (LCD) ISR runs on core 0: `screen_init` is called from `app_main` (`main.cpp:682`), which runs
  on core 0 by default, and esp_lcd allocates its interrupt on the calling core. Not checked on the device.
- `check_image` cost is estimated (~5-15 cycles per pixel, release build), not measured.
- The bench numbers are from the release build (as the Notes say) and the RX path is the same in both benches.
- The per-byte conclusion relies on two data points; bench noise could hide a per-image cost of up to ~1 ms.
- usbbig: assumed the S3's DWC2 (DMA mode) takes a 4096 B OUT transfer in one go (esp_tinyusb's Kconfig allows up
  to 32768 and recommends 8192); it builds, but enumeration and uploads on it are only proven by the step 4 run.
  `CB_STATS_LOG` was also turned on for `release-usbbig` so its run gives the same stats line.

## Plan
Measure first, then fix (user, 2026-11-12). Suites: **host** = `npm test` in `host/`; **fw-test** = `mk run` and
`mk config=release run` in `firmware/test` (counts must match); **fw-build** = debug build (CLAUDE.md Building).

- [x] 1. **Sink bench (host only).** Tests first in `host/test/` against `FakeDevice` (a `PUT_IMAGE` with id 0 is
  refused BAD_ARG and its data skipped, as the firmware does). `examples/bench.js` gains a "sink" case: 8 KB
  `PUT_IMAGE` messages with id 0 sent pipelined like the upload case (via `request()`), reporting KB/s. host,
  examples.
- [x] 2. **Device counters (device only, not unit testable: warn).** On the `CB_STATS_LOG` line: OUT transfers and
  bytes received, the vendor RX FIFO level seen at each protocol read (min/avg/max), protocol task wakes, µs spent
  in `Protocol::Receive` and in `check_image` (cycle counter, `port.h`), per second. Keep the counting cheap and
  out of the fill ISR. fw-test, fw-build.
- [x] 3. **Bigger-transfer build variant.** A sdkconfig fragment and presets (e.g. `release-usbbig`, following
  `release-iram`): `CONFIG_TINYUSB_VENDOR_EPSIZE` 4096, RX FIFO 16384; the protocol task's read buffer follows
  the configured size. Report the internal RAM cost (free heap at boot vs the normal release build). fw-build
  (release and release-usbbig).
- [ ] 4. **STOP: user measures on the hardware.** Normal release build with `-DCB_STATS_LOG=ON`: `node
  examples/bench.js` (all cases incl. sink) and the stats lines during it; then the same on `-DCB_IDLE_FILL=ON`
  and `-DCB_NO_DISPLAY=ON`; then the `release-usbbig` preset. Record the numbers in Notes and decide the fix.
- [ ] 5. **The fix** (planned from step 4's results: likely bigger OUT transfers and draining the FIFO per wake;
  maybe several host transfers outstanding; deflate only if the pipe still limits).
- [ ] 6. **Docs and CLAUDE.md** (protocol.md / image-assets.md throughput notes; propose CLAUDE.md changes, Rule 4).
- [ ] 7. **STOP: user re-runs bench.js on the hardware** and confirms the gain; also a release `RUN_STRESS`
  if the fix touches the fill ISR's core or PSRAM writes.
