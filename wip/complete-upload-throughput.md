# Hosted image upload throughput

Status: Complete

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

## Run Results

1. Normal:

```
I (1481) controlblock: heap after init: internal 150727 free (largest 110592, min ever 150727), PSRAM 4757648 free
```

```
/c/Users/Brad/Projects/ControlBlock (main)$ node examples/bench.js
ping: 0.61 ms round trip
ping 8192 B x 50: 702 KB/s (both ways)
sink 8192 B x 50: 250 KB/s (one way, nothing stored)
upload 40 x 21.8 KB: 250 KB/s
a page of keys: 30 images, 142.5 KB in 561 ms (254 KB/s; 4.7 KB per image)
image memory: 1014 KB -> 0 KB after freeing
```

```
I (32631) controlblock: 49 fps, fill max 139 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (33631) controlblock: 49 fps, fill max 139 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (34161) host: host took control
I (34631) controlblock: 49 fps, fill max 143 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 36873 us), 1 swaps, 0 held | img 0 (0 KB) | usb rx: 453 xfers, 121 KB, 357 wakes, 453 reads, fifo 12/716/4096, receive 165566 us, 0 checks, 0 us
I (35641) controlblock: 49 fps, fill max 139 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 708 xfers, 333 KB, 463 wakes, 708 reads, fifo 12/1191/4096, receive 401850 us, 0 checks, 0 us
I (36651) controlblock: 49 fps, fill max 170 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 539 xfers, 253 KB, 539 wakes, 539 reads, fifo 12/482/512, receive 4139 us, 0 checks, 0 us
I (37661) controlblock: 49 fps, fill max 145 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 7 (174 KB) | usb rx: 527 xfers, 249 KB, 513 wakes, 527 reads, fifo 12/505/1536, receive 53422 us, 7 checks, 46048 us
I (38671) controlblock: 50 fps, fill max 145 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 18 (413 KB) | usb rx: 538 xfers, 255 KB, 515 wakes, 538 reads, fifo 12/517/1536, receive 83161 us, 11 checks, 72937 us
I (39681) controlblock: 49 fps, fill max 145 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 30 (675 KB) | usb rx: 534 xfers, 253 KB, 510 wakes, 534 reads, fifo 12/520/1536, receive 89336 us, 12 checks, 78638 us
I (40691) controlblock: 49 fps, fill max 145 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 40 (871 KB) | usb rx: 443 xfers, 210 KB, 425 wakes, 443 reads, fifo 12/517/1536, receive 73631 us, 10 checks, 64955 us
I (41701) controlblock: 49 fps, fill max 139 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 55 (946 KB) | usb rx: 157 xfers, 74 KB, 157 wakes, 157 reads, fifo 74/486/512, receive 44137 us, 15 checks, 39980 us
I (42241) host: standalone
I (42711) controlblock: 50 fps, fill max 143 us, 0 over 615 us, page 0 | comp: 24 keys (max 9606 us, busy 70495 us), 1 swaps, 7 held | img 24 (0 KB) | usb rx: 148 xfers, 68 KB, 147 wakes, 148 reads, fifo 10/476/512, receive 108040 us, 15 checks, 41334 us
I (43721) controlblock: 49 fps, fill max 139 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (44721) controlblock: 49 fps, fill max 139 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
```

2. Idle Fill

```
I (1470) controlblock: heap after init: internal 153303 free (largest 112640, min ever 153303), PSRAM 4757648 free
```

```
ping: 0.48 ms round trip
ping 8192 B x 50: 711 KB/s (both ways)
sink 8192 B x 50: 250 KB/s (one way, nothing stored)
upload 40 x 21.8 KB: 251 KB/s
a page of keys: 30 images, 142.5 KB in 554 ms (257 KB/s; 4.7 KB per image)
image memory: 1014 KB -> 0 KB after freeing
```

```
I (25620) controlblock: 49 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (26620) controlblock: 49 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (27100) host: host took control
I (27620) controlblock: 49 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 516 xfers, 150 KB, 391 wakes, 516 reads, fifo 12/798/4096, receive 205811 us, 0 checks, 0 us
I (28630) controlblock: 49 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 696 xfers, 327 KB, 479 wakes, 696 reads, fifo 12/1121/4096, receive 344310 us, 0 checks, 0 us
I (29640) controlblock: 49 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 538 xfers, 253 KB, 538 wakes, 538 reads, fifo 12/482/512, receive 2057 us, 0 checks, 0 us
I (30650) controlblock: 49 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 8 (196 KB) | usb rx: 527 xfers, 249 KB, 518 wakes, 527 reads, fifo 12/493/1024, receive 48191 us, 8 checks, 42165 us
I (31660) controlblock: 50 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 19 (435 KB) | usb rx: 537 xfers, 254 KB, 525 wakes, 537 reads, fifo 12/498/1536, receive 67057 us, 11 checks, 58350 us
I (32670) controlblock: 49 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 31 (697 KB) | usb rx: 537 xfers, 255 KB, 522 wakes, 537 reads, fifo 12/497/1024, receive 72427 us, 12 checks, 63787 us
I (33680) controlblock: 49 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 40 (871 KB) | usb rx: 391 xfers, 185 KB, 383 wakes, 391 reads, fifo 12/496/1024, receive 53625 us, 9 checks, 47593 us
I (34690) controlblock: 49 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 303 xfers, 143 KB, 303 wakes, 303 reads, fifo 10/484/512, receive 75481 us, 30 checks, 68422 us
I (34790) host: standalone
I (35700) controlblock: 50 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB) | usb rx: 2 xfers, 0 KB, 2 wakes, 2 reads, fifo 12/12/12, receive 53424 us, 0 checks, 0 us
I (36700) controlblock: 49 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (37700) controlblock: 48 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
```



3. No display

```
I (1453) controlblock: heap after init: internal 204291 free (largest 163840, min ever 204291), PSRAM 4761924 free
```

```
ping: 0.49 ms round trip
ping 8192 B x 50: 712 KB/s (both ways)
sink 8192 B x 50: 251 KB/s (one way, nothing stored)
upload 40 x 21.8 KB: 251 KB/s
a page of keys: 30 images, 142.5 KB in 553 ms (258 KB/s; 4.7 KB per image)
image memory: 1014 KB -> 0 KB after freeing
```

```
I (17603) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (18603) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (19153) host: host took control
I (19603) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 461 xfers, 124 KB, 360 wakes, 457 reads, fifo 12/713/4096, receive 155791 us, 0 checks, 0 us
I (20613) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 713 xfers, 335 KB, 472 wakes, 717 reads, fifo 12/1182/4096, receive 388316 us, 0 checks, 0 us
I (21623) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 539 xfers, 254 KB, 539 wakes, 539 reads, fifo 12/483/512, receive 2207 us, 0 checks, 0 us
I (22633) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 7 (174 KB) | usb rx: 529 xfers, 250 KB, 520 wakes, 529 reads, fifo 12/492/1536, receive 43094 us, 7 checks, 37125 us
I (23643) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 19 (435 KB) | usb rx: 536 xfers, 254 KB, 522 wakes, 536 reads, fifo 12/499/1024, receive 71945 us, 12 checks, 63405 us
I (24653) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 30 (675 KB) | usb rx: 536 xfers, 254 KB, 524 wakes, 536 reads, fifo 12/498/1024, receive 67055 us, 11 checks, 58606 us
I (25663) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 40 (871 KB) | usb rx: 428 xfers, 202 KB, 417 wakes, 428 reads, fifo 12/497/1024, receive 59593 us, 10 checks, 52699 us
I (26673) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 303 xfers, 143 KB, 303 wakes, 303 reads, fifo 10/484/512, receive 74555 us, 30 checks, 67383 us
I (26853) host: standalone
I (27683) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB) | usb rx: 2 xfers, 0 KB, 2 wakes, 2 reads, fifo 12/12/12, receive 52954 us, 0 checks, 0 us
I (28683) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (29683) controlblock: 0 fps, fill max   0 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)

```


4. Big transfers

```
I (1449) controlblock: heap after the host link (USB): internal 151759 free (largest 110592, min ever 151759), PSRAM 4757676 free
```

```
ping: 0.62 ms round trip
ping 8192 B x 50: 1081 KB/s (both ways)
sink 8192 B x 50: 250 KB/s (one way, nothing stored)
upload 40 x 21.8 KB: 251 KB/s
a page of keys: 30 images, 142.5 KB in 554 ms (257 KB/s; 4.7 KB per image)
image memory: 1014 KB -> 0 KB after freeing
```

```
I (16629) controlblock: 49 fps, fill max 138 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (17629) controlblock: 48 fps, fill max 138 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (18629) controlblock: 49 fps, fill max 138 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (18829) host: host took control
I (19629) controlblock: 49 fps, fill max 141 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 36710 us), 1 swaps, 0 held | img 0 (0 KB) | usb rx: 338 xfers, 366 KB, 321 wakes, 337 reads, fifo 12/1102/4108, receive 505556 us, 0 checks, 0 us
I (20639) controlblock: 49 fps, fill max 138 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 0 (0 KB) | usb rx: 101 xfers, 268 KB, 100 wakes, 102 reads, fifo 12/2734/4108, receive 66195 us, 0 checks, 0 us
I (21649) controlblock: 49 fps, fill max 138 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 3 (87 KB) | usb rx: 93 xfers, 249 KB, 93 wakes, 93 reads, fifo 12/2750/4096, receive 23842 us, 3 checks, 19100 us
I (22659) controlblock: 50 fps, fill max 151 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 15 (348 KB) | usb rx: 93 xfers, 254 KB, 93 wakes, 93 reads, fifo 12/2796/4096, receive 85852 us, 12 checks, 76227 us
I (23669) controlblock: 49 fps, fill max 174 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 27 (588 KB) | usb rx: 93 xfers, 254 KB, 93 wakes, 93 reads, fifo 12/2796/4096, receive 85502 us, 12 checks, 75912 us
I (24679) controlblock: 49 fps, fill max 138 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 38 (849 KB) | usb rx: 94 xfers, 256 KB, 94 wakes, 94 reads, fifo 12/2791/4096, receive 79009 us, 11 checks, 69517 us
I (25689) controlblock: 49 fps, fill max 138 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 57 (956 KB) | usb rx: 45 xfers, 112 KB, 45 wakes, 45 reads, fifo 12/2554/4096, receive 62515 us, 19 checks, 56979 us
I (26189) host: standalone
I (26699) controlblock: 50 fps, fill max 141 us, 0 over 615 us, page 0 | comp: 24 keys (max 9427 us, busy 69601 us), 1 swaps, 7 held | img 24 (0 KB) | usb rx: 29 xfers, 58 KB, 29 wakes, 29 reads, fifo 12/2080/4096, receive 101786 us, 13 checks, 34645 us
I (27709) controlblock: 49 fps, fill max 138 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (28709) controlblock: 49 fps, fill max 138 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
I (29709) controlblock: 49 fps, fill max 138 us, 0 over 615 us, page 0 | comp: 0 keys (max 0 us, busy 0 us), 0 swaps, 0 held | img 24 (0 KB)
```



### Step 4 findings (2026-11-12, from the Run Results above)
| Build | ping 8 KB (both ways) | sink | upload 21.8 KB | page of keys |
|---|---|---|---|---|
| Normal | 702 KB/s | 250 KB/s | 250 KB/s | 561 ms |
| Idle fill | 711 | 250 | 251 | 554 ms |
| No display | 712 | 251 | 251 | 553 ms |
| usbbig (4 KB transfers) | **1081** | 250 | 251 | 554 ms |

- **The sink runs exactly as fast as the real uploads, in every build.** It stores nothing and never runs
  `check_image`, so the image path (PSRAM copy, check, allocation, locks) costs nothing visible. Ruled out.
- **The display changes nothing** (idle fill, no display): the fill ISR, PSRAM contention on core 0 and the RGB
  bus are ruled out for uploads.
- **Bigger OUT transfers change nothing for uploads** (usbbig: 93 transfers/s of ~2.7 KB instead of ~539 of
  512 B, same 250 KB/s), though they lift ping from 702 to 1081 KB/s. So the device's 512 B re-arm rhythm is not
  the upload limit. Fix candidates 1, 2 and 4 are ruled out for uploads.
- **The device is starved, not slow.** During the sink (normal build) every wake finds one transfer: FIFO avg 482,
  max 512, `receive` ~2-4 ms of CPU per second. During uploads the protocol task uses ~5-9% of a second (`receive`
  45-90 ms, of which `check_image` ~40-80 ms, ~6.5 ms per 21.8 KB image).
- **The limit is ~32 ms per 8 KB message** (~31 messages/s) in the sink and uploads, against ~15-23 ms per 8 KB
  message each way in ping, which is pipelined the same way (`Promise.all`) with the same 8204 B writes. What
  differs: the reply (ping: 8 KB at the end of the message; sink: 16 B at the header; PUT_IMAGE: 16 B at the end)
  and the message type. The host sends one `transferOut` at a time (`ControlBlock._write`); node usb's
  `transferOut` is a plain native transfer (1000 ms timeout), nothing in between.
- Not explained yet: why a stream of 8 KB messages with small replies moves at 250 KB/s while ping's moves at
  352-540 KB/s each way. The next measurement is host-side (timing each `transferOut` and the gaps between them,
  candidate measurement 5), which needs no firmware change.
- usbbig costs 23 KB of internal RAM and only helps ping: not worth keeping for uploads (decide with the fix).
- Internal RAM at boot (release, Wi-Fi not yet up): 150,727 B free normal, 153,303 idle fill, 204,291 no display
  (the panel's bounce buffers etc.), 151,759 usbbig (that line is taken after the host link only, so not
  comparable).

### Trace results (2026-11-12, `examples/trace.js`, normal release build; records in `scratch/trace.json`,
summarised by `scratch/analyze-trace.mjs`)
| Case | Out KB/s | Each 8204 B write (med) | Gaps | Reads |
|---|---|---|---|---|
| ping pipelined | 356 | 22.4 ms | 0 | 149 (397 KB) |
| ping one at a time | 171 | 32.2 ms | 14.9 ms (the reply) | 152 |
| sink pipelined | 252 | 32.0 ms | 0 | 52 x 16 B |
| sink one at a time | 245 | 32.3 ms | 0.26 ms | 51 |
| raw sink, 1 / 2 / 3 writes in flight | 251 / 252 / 252 | 32 / 64 / 96 ms | 0 | ~50 |

- **The time is inside each transfer**: an 8204 B write takes ~32 ms on the bus (~4 packets per 1 ms frame), with
  no gaps between writes. More writes in flight only queue behind each other (64, 96 ms each) at the same rate:
  **fix candidate 3 (several transfers outstanding) is ruled out**, as are round trips and the host's send path.
- **The same write is faster (22 ms) only in pipelined ping**, where the device is sending data back so the
  host's reads complete. In every other case a read sits pending with nothing to return, and ping one at a time
  (reads idle again) is back at 32 ms. Hypothesis: **a pending bulk IN read that the device NAKs is polled by the
  host controller so often that it takes most of each full-speed frame, starving the OUT pipe.** It also fits
  usbbig ping at 540 KB/s each way.
- **Test (built, waiting on a hardware run):** case "raw sink, reads held": `gateReads(device)`
  (`bench-trace.mjs`, tested) holds the read loop's next `transferIn` while the writes go out, so no read is
  pending (the one already submitted ends at the first reply; ~50 x 16 B replies wait in the device's 2 KB TX FIFO
  until the gate opens). Writes much faster than 32 ms (towards ~8 ms at ~1 MB/s) confirm it.
- **Confirmed (hardware run 2026-11-12, normal release build): "raw sink, reads held" = 493 KB/s.** The first
  write (the old read still pending) took 32.8 ms; every later one 16.0 ms (15.5-16.0). So: **read pending = one
  512 B device transfer every 2 frames (4 packets/frame); no read pending = exactly one 512 B transfer per 1 ms
  frame (8 packets/frame).** The second limit looks like the device's 512 B OUT re-arm costing a frame each time
  (the host retries a NAKed OUT in the next frame), which the pending read masked: that's why usbbig didn't help
  uploads in step 4. Next run: the same trace on `release-usbbig` (4 KB transfers), to see if both together reach
  ~1 MB/s and whether the 23 KB of internal RAM is worth it (an EPSIZE of 1024 or 2048 may get most of it for
  less).
- Other cases in that run matched the first trace to within 1%.
- **usbbig run (2026-11-12, `release-usbbig`, 4 KB OUT transfers): "raw sink, reads held" = 845 KB/s** (9.0 ms
  per 8204 B write, ~14 packets per frame; the first write, with the old read pending, 32.3 ms). With a read
  pending it is still 251 KB/s (sink, raw 1-3 in flight), so **the pending read caps uploads at ~4 packets/frame
  whatever the transfer size**, and the 512 B re-arm is the second limit (493 -> 845 KB/s). Ping pipelined rose
  to 547 KB/s out (14.2 ms writes). Both fixes together: ~3.4x; a page of keys (142.5 KB) ~170 ms instead of 556.
- Fix options, now that it's confirmed: the device answers an IN poll with data more often (no: nothing to send);
  the host holds back reads during bulk uploads (a pending read can't be cancelled, so the library would stop
  resubmitting reads while writes are queued and resume after; the replies wait in the TX FIFO, which must not
  fill: at most ~128 x 16 B replies, and the heartbeat already skips a full FIFO); or a device NAK/poll setting
  (bInterval is ignored for full-speed bulk). Deflate still helps either way.

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
- [x] 4. **STOP: user measures on the hardware.** (Done 2026-11-12: Run Results and "Step 4 findings". The cause
  is not on the device; step 5 is blocked on the user's choice of next measurement or fix, 1-3-1 below.) Normal release build with `-DCB_STATS_LOG=ON`: `node
  examples/bench.js` (all cases incl. sink) and the stats lines during it; then the same on `-DCB_IDLE_FILL=ON`
  and `-DCB_NO_DISPLAY=ON`; then the `release-usbbig` preset. Record the numbers in Notes and decide the fix.
- [x] 5. **The fix (user chose: both, 4 KB transfers / 8 KB FIFO, 2026-11-12).**
  - Library (`host/src/ControlBlock.js`): `_write` counts queued writes (`_queuedWrites`); the read loop calls
    `_holdRead()` before each `transferIn`, which waits while writes are queued, up to `readHoldMs` (option,
    default `READ_HOLD_MS` 20 ms: the device drops a reply it can't send within 1 s, `WRITE_TIMEOUT_US`, so the cap
    keeps its 2 KB TX FIFO draining even with big replies like 8 KB pings). The last write finishing, or
    `close()`, releases the hold. Only the submission waits: no read is ever cancelled (the deaf-pipe rule holds).
    Tests (`host/test/ControlBlock.test.js`): no read submitted while writes are queued and replies read after;
    a held read goes out after `readHoldMs` while a write stays stuck; `close()` releases a held read.
  - Firmware (`sdkconfig.defaults`): `CONFIG_TINYUSB_VENDOR_EPSIZE` 4096, `RX_BUFSIZE` 8192 (TinyUSB arms a
    transfer only for the FIFO's free room, rounded to 64 B, so 8 KB holds two full transfers and never loses
    data: `tusb.c` `tu_edpt_stream_read_xfer`). Static DIRAM (release `idf.py size`) 139,271 B: +14,752 B over
    the old 512/4096 (124,519), vs +23,040 for usbbig. Generated sdkconfigs moved aside to
    `build/{debug,release}/sdkconfig.pre-usb4k` (the regenerated ones differ only in those two lines). Debug and
    release build, 0 errors.
  - Removed: `sdkconfig.usbbig` and the `debug-usbbig`/`release-usbbig` presets (`build/release-usbbig` is left
    on disk, delete at will). `FakeDevice`'s `USB_RX_TRANSFER` is 4096 (it mirrors the EPSIZE); the ZLP test gained
    4096 and 4160-byte messages.
  - Docs: `protocol.md` (the ZLP reason now says 4096; new "Upload speed: hold reads during bulk writes"),
    `image-assets.md` (4 KB per read). CLAUDE.md changes proposed to the user (Rule 4), not made.
  - Tests: host 276, cli 31, examples 34 pass. Not covered by tests: the device side (hardware run, step 7).
  - **Bounce fill risk to check in step 7:** uploads now write image data into PSRAM ~3.4x faster
    (`ImageStore::PutData` memcpy on the protocol task, core 0). Watch `fill max` / `over` on the stats line
    during `bench.js`, and run a release `RUN_STRESS` (its image-replace case runs at the old USB speed).
- **Hardware run of the fix (2026-11-12, release):** ping 0.50 ms; sink 842 KB/s; upload 40 x 21.8 KB 858 KB/s;
  a page of keys 142.5 KB in **175 ms** (815 KB/s; was 556 ms): 3.4x. But **ping 8 KB x 50 fell to 259 KB/s**
  (was 702): every request held reads, so the 8 KB echoes filled the device's 2 KB TX FIFO, its writes blocked and
  it stopped reading until each 20 ms hold ended. Fixed: only `PUT_IMAGE` and `LAYOUT_DATA` (16-byte replies) hold
  reads (`SMALL_REPLY` in `ControlBlock.js`); any other request queued releases a held read and keeps reads going
  (`_bigReplyWrites`). New test: "reads aren't held while any other request is queued" (failed before the change).
  host 277, cli 31, examples 34 pass. `protocol.md` says so. The change broke `cli` "upload bakes a layout...":
  a gap in `FakeDevice`, not the library. While "restarting" after a commit, the fake still answered writes
  (`reconnect()`'s retries' GET_INFOs) and kept the replies queued across the restart, so a later connection
  (seq restarts at 1) read a stale GET_INFO as its own. Fixed in the fake: `transferOut` throws while `gone`,
  and the restart clears its queued output, held bytes and parser, as a real restart does. (Possible library
  hardening, not done: start each connection's seq at a random value, so a reply left over from an earlier
  connection can't match.) Re-run bench.js to confirm ping is back (~700 KB/s)
  with uploads unchanged; the stats lines (`fill max`, `over`) from that run are still wanted.
- Earlier text of step 5: **User chose (a), 2026-11-12. Trace built; waiting on the hardware run.** `examples/bench-trace.mjs`
  (`traceDevice`: wraps the device's `transferOut`/`transferIn` and records `{ dir, start, end, bytes }`; `summarize`:
  KB/s, write durations, gaps, share of the span with a write in flight; `sendRaw(device, messages, depth)`: up to
  `depth` writes in flight, in order, refusing a message that needs a ZLP above depth 1), tested in
  `examples/bench-trace.test.mjs` (8 pass, no device). Runner `examples/trace.js` (not unit testable: hardware only):
  50 messages of the max payload per case: ping pipelined / one at a time, sink pipelined / one at a time, raw
  sink with 1, 2, 3 writes in flight; `--json file` dumps every record. Run: release build (stats on is fine), board
  as for step 4, `node examples/trace.js --json scratch/trace.json` from the repo root; paste the output under Run
  Results. Then decide the fix. Earlier text: **BLOCKED (1-3-1, waiting on the user).** Problem: step 4 rules out every device-side fix; the ~32 ms per
  8 KB message happens somewhere between the host's writes and the device's endpoint, and ping's faster rate isn't
  explained. Options: (a) host-only trace first: a bench script that times every `transferOut` (start, end, gap)
  and `transferIn` completion, for ping and sink, pipelined and one at a time, plus a sink variant with 2-3
  `transferOut`s outstanding (no firmware change; one hardware run) (recommended); (b) build fix candidate 3 (2-3
  transfers outstanding in `ControlBlock._write`) blind and measure; (c) skip the cause and add deflate (2.4x fewer
  bytes whatever the limit). Original text: **The fix** (planned from step 4's results: likely bigger OUT transfers and draining the FIFO per wake;
  maybe several host transfers outstanding; deflate only if the pipe still limits).
- [x] 6. **Docs and CLAUDE.md** (done 2026-11-12: protocol.md, image-assets.md; CLAUDE.md with the user's OK:
  Components line (4 KB transfers), Communication (ZLP 4 KB, new read-hold bullet), stale iram presets removed,
  bench.js / deflate Open Items, trace.js in the layout) (protocol.md / image-assets.md throughput notes; propose CLAUDE.md changes, Rule 4).
- [x] 7. **STOP: user re-runs bench.js on the hardware** (expect upload ~800 KB/s, a page of keys ~170-200 ms;
  `trace.js`'s sink cases now go through the library's hold, its raw cases don't: they keep showing the
  unheld baseline) and confirms the gain; also a release `RUN_STRESS`
  if the fix touches the fill ISR's core or PSRAM writes.
  - **Hardware run 2026-11-13 (release, with the `SMALL_REPLY` change):**

    | Case | Before the job | After |
    |---|---|---|
    | ping round trip | 0.63 ms | 0.69 ms |
    | ping 8192 B x 50 (both ways) | 705 KB/s | **1082 KB/s** |
    | sink 8192 B x 50 | 250 KB/s | **886 KB/s** |
    | upload 40 x 21.8 KB | 251 KB/s | **857 KB/s** |
    | a page of keys (30 images, 142.5 KB) | 556 ms | **180 ms** (791 KB/s) |

    Ping is back above its old rate (the 4 KB transfers help it too) and uploads keep the 3.4x gain. Image memory
    freed back to 0 KB.
  - **Not checked (left open):** the stats line's `fill max` / `over` during the bench, and a release `RUN_STRESS`
    since the fix. The fix doesn't touch the fill ISR or its core; the only new load is the protocol task's
    `PutData` memcpy into PSRAM at ~850 KB/s (was ~250), far below what the compositor writes in the stress run,
    so the risk is low. Worth a look the next time a stress run is done for another reason.
