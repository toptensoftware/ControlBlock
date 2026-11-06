# Macro pad and encoder input (serial from a second MCU)

Status: Implement (C3 phase done; S3 phase not started, see Handover)

## Description
A macro pad (key switches, a rotary encoder, the encoder's knob as a button) is read by a second MCU, an ESP32-C3,
which sends messages over a one-way serial line to the S3 device. The S3 has one free GPIO, used as a serial
**input** (RX only). There is no serial output from the S3 to the C3 (if that is ever needed it will be I2C or
RS485). Goals:

1. Design a robust line protocol for press/release of any button (the encoder knob included) and for the encoder's
   direction and velocity. "Robust" has to survive a one-way line: no acknowledgements, no retransmit requests.
2. Add the S3 receiver: a UART RX task that parses the stream and turns it into triggers.
3. Standalone mode: feed the triggers to the existing bindings (`Trigger(control, event, param)`), with `tap`/`long`
   synthesised by the gesture engine for the buttons.
4. Hosted mode: report the pad and encoder to the host instead (new protocol events), and run no actions.
5. Fill in the bake/config side (control and event names) and the host library.

The C3 firmware is a separate project/repo as far as this job goes; this job defines the protocol and builds the S3
side. (Decided: the C3 firmware lives in this repo, in `pad/`.)

## Handover (written 2026-11-06, end of the C3 phase; read this first)
- **Where it stands:** Implement phase, **C3 phase done**: steps 1, 7, 8, 9 ticked. **S3 phase not started**
  (steps 2, 3, 4, 5, 6, then 10, 11, 12, 13): the user is still testing the S3 (net ops, pixel clock) and said not to
  disturb it. **Do not touch `firmware/` for this job until the user says to start the S3 phase.** Then run the steps one
  at a time in order, each by a fresh subagent if wanted (the previous four ran that way; each returned a short report
  and ticked its step).
- **Verified by the coordinator (2026-11-06):** `pad/test` `mk run` = 44 tests pass; `tools/pad-sim` `npm test` = 49
  pass; `pad/build/pad.bin` exists (esp32c3 build, 0 errors). Nothing in `pad/main` has run on a board.
- **Nothing is committed.** Untracked/new: `pad/`, `tools/pad-sim/`, this file. Also uncommitted from earlier in this
  session (not this job): the `PacketEncoder` submodule add (`.gitmodules`), `firmware/main/crc32.h` ->
  `crc32_table.h` rename (+ includes in `config_store.cpp`, `TestConfigStore.cpp`, `TestProtocol.cpp`, comment in
  `host/src/protocol.js`, `docs/config-upload-plan.md`), pixel clock default 20 MHz
  (`waveshare_rgb_lcd_port.h`, `main/CMakeLists.txt`), `wip/complete-wifi-latency.md` (renamed, staged), and doc edits to
  `CLAUDE.md`, `docs/wifi.md`, `docs/screen-buffers.md`. The user decides when to commit.
- **Decisions so far (all in the sections below; the short list):** UART1 RX on S3 **GPIO 6** (`aclock` uses it as TX),
  115200 8N1, <= 100 mm, one-way; PacketEncoder submodule for framing; 32 keys (`key1..key32`, encoder push buttons
  included) + 4 encoders (`enc1..enc4`); STATE packet = `u32 keys` + per encoder `u32 position` (detents, absolute) +
  `u16 interval` (urem's 4-edge average, microseconds, **computed on the C3**); HELLO x3 at boot; repeat every 100 ms,
  encoder-only changes at most every 5 ms, offline after 300 ms; held pad keys dropped on takeover/switch/restart;
  hosted events `0x85` PAD_KEY / `0x86` PAD_TURN; no pad-state request; **button repeat is out of scope**; the encoder
  must **match urem** (`C:\Users\Brad\Projects\urem\frontend\gpio.js` / `volume.js`; do not copy irtx's different
  polled algorithm). Trigger: control `enc1`, event `turn`, param `delta=<signed> interval=<us>`.
- **Test rig:** FTDI 3.3 V module: C3 link TX -> FTDI RX (`tools/pad-sim/dump.js COMx`), or FTDI TX -> S3 GPIO 6
  (`sim.js COMx`) to test the S3 without the C3.
- **User actions pending (hardware):** adjust `pad/main/pad_pins.h` (placeholder map; the link TX is on GPIO 2, a
  strapping pin, because the C3 zero board has only eight safe pins: check boot with the S3 attached); flash and check
  the checklist in step 8's notes and `pad/README.md`; confirm the encoder part (quadrature, counts per detent);
  compare the feel against urem (thresholds 8 ms / 20 ms); the glitch filter follows pigpio's documented, not its
  source, behaviour.
- **Docs not yet updated (Rule 4: propose to the user first, step 12):** `CLAUDE.md` (the `pad/` project, `pad/common`,
  `pad/test`, `tools/pad-sim`, the PacketEncoder submodule in the layout, GPIO 6, the control names, Open items),
  `docs/standalone.md` (Still open items closed; trigger names), `docs/config-format.md` (replace `macro1..6` /
  `volEnc` / `turnLeft`/`turnRight` by `keyN` / `encN` / `turn`), `docs/protocol.md` (events `0x85`/`0x86`), a new
  `docs/macro-pad.md` (wire protocol). Also `.gitmodules` lists `PacketEncoder` with an SSH URL.
- **Other jobs, for context:** `wip/complete-wifi-latency.md` is closed (cause: RGB panel pin drive strength; default
  pixel clock now 20 MHz, may change). `complete-activity-device-management.md` (step 9 = hardware checklist, passed) and
  `wip-lua-scripts.md` (step 10 docs proposal waiting for the user's OK) are built and host tested but not run on the
  hardware; net-op hardware tests are the next thing the user will do.

## Notes

### What the docs already decide (do not re-litigate)
- `docs/standalone.md` (Triggers and bindings, Key gestures, Decided): the pad is **6 keys** and **one rotary
  encoder**. They are bound **per activity**, not per page. Home has no pad/encoder bindings; the pad and encoder
  always use the **current activity's** bindings, even while Home is in view; with no activity selected they do
  nothing.
- The pad delivers only raw `press` and `release`; the device synthesises `tap` and `long` (per control, via
  `GestureBank`, which is already shared by touch keys and "anything else").
- The encoder's velocity reaches the op list as the trigger's `param` string (a script parses it). No coalescing: each
  encoder tick fires the binding (see Rate in `standalone.md`). `http.post` copies into a bounded request queue and
  returns at once.
- Hosted mode: the pad and encoder are **reported to the host** (like `PostKey` for touch keys) and never run activity
  actions. `protocol.md` needs pad-key and encoder events "when this is built". The host detects long/double press.
- `docs/config-format.md` "Bindings": `control` is free-form and matched exactly (`macro1`..`macro6`, `volEnc` are
  *proposed* names); `event` is `press`/`release`/`tap`/`long` or the control's own events (`turnLeft`, `turnRight`:
  *undecided*). A `control` that looks like `touch[...]` is a bake error.
- Open items that this job closes: "the serial line format from the second MCU, and the exact event names for the
  encoder" (`standalone.md` Still open), and "Control and event names for the pad and encoder wait on the second MCU's
  serial format" (`config-format.md`).

### Code that this plugs into
- `TriggerDispatcher::Trigger(control, event, param)` and `HasBinding` (`trigger_dispatch.h`): exact string match over
  Home's and the selected activity's bindings; unmatched ignored.
- `GestureBank` (`key_gesture.h`): `Press/Release/Cancel(control, now_ms, long_ms)` and `Tick`. Max 8 controls held at
  once, control names up to 23 characters, a press of an already-held control is ignored. Has no per-control
  "release all" call except `Reset()` (drops everything silently).
- `Standalone` (`standalone.h`) owns the dispatcher and gestures under the standalone mutex; `host_link.cpp`'s `on_key`
  is "the single switch point for key events" (hosted: `Protocol::PostKey`; standalone: `standalone_task`). The pad
  input needs an equivalent switch point so that the mode decision lives in one place.
- Encoder events are not gestures: they are discrete triggers with a `param`, so they go straight to
  `TriggerDispatcher::Trigger`, not through `GestureBank`. The encoder knob as a button is a normal control and does
  go through `GestureBank`.
- Hosted-mode events: `protocol.md` event types are `0x80` KEY, `0x81` PAGE_CHANGED, `0x82` WAKE, `0x83` HEARTBEAT,
  `0x84` WIFI_STATE. New ones would take `0x85` upwards. Events go through `Protocol`'s event queue (with its `lost`
  overflow flag). Events exist only while hosted for KEY/PAGE; WAKE/HEARTBEAT/WIFI_STATE are both modes.
- Takeover / release / activity switch / USB disconnect all re-arm gestures (`SetHosted`, `Reset`), so held pad keys
  are dropped silently there; the HID keys are released by `HidKeyboard::ReleaseAll`. The pad needs the same
  treatment (a pad key still physically held across a takeover has to be re-synchronised, see below).
- Host tests are in `firmware/test` (pure code; fake platform via injected callbacks). The UART layer would be a thin
  device file like `usb.cpp`; the parser and the pad-to-trigger logic must be pure and tested.
- Lock order is standalone mutex, then model lock. A UART task calling `Standalone` takes the standalone mutex like
  `host_link`/`swipe` do.

### Hardware facts
- S3 pins already used (from `waveshare_*_port.h` and CLAUDE.md): RGB panel (0, 1, 2, 3, 5, 7, 10, 14, 17, 18, 21, 38,
  39, 40, 41, 42, 45, 46, 47, 48), I2C 8/9, touch INT 4, UART0 console (43/44, the CH343P), native USB 19/20.
  **Which GPIO is the free one is not stated anywhere in the repo**; needs the user (see Open questions). Whatever it
  is, the UART RX can be routed to any GPIO through the GPIO matrix, so UART1 or UART2 with RX only works. Avoid the
  strapping pins (0, 3, 45, 46) as the idle level matters at boot, and note that 0/3/45/46 are already RGB pins here.
- Radio interference lesson from the Wi-Fi job: the RGB bus is noisy; a long unshielded serial wire next to it is a
  likely source of corrupted bytes. Another reason the protocol needs a CRC and resynchronisation, not just framing.
- The C3 side has its own constraints: it has no native "one wire, read-only" concerns, it just transmits. Baud rate,
  idle level at boot (the C3 prints boot logs on its UART0 at 115200 unless its console is moved, which would feed
  garbage to the S3 as the C3 boots) and power-up ordering (S3 up before the C3, or after) all matter.
- The C3 can be reset or brown out independently of the S3, and the S3 can restart (config upload restarts it) while
  the C3 keeps sending.

### Protocol design considerations (to be turned into a decision, not yet decided)
These are the robustness requirements a one-way line forces; the design should answer each.
1. **Framing and resync:** a start/sync marker plus length, or a byte-stuffed delimiter (COBS/SLIP), so the receiver
   can find a message boundary after noise or after joining mid-stream.
2. **Integrity:** a checksum (CRC-8 or CRC-16) per message; a bad message is dropped and counted, never acted on.
3. **No ACK, so state must be self-healing.** A lost `release` is the dangerous case (a key stuck down; with HID
   `hold` mode this would stick a key on the PC). Options: the C3 sends a periodic **full state** frame (all buttons'
   up/down) every N ms and on any change, so a lost edge is repaired within N ms; or sends each edge several times with
   a sequence number. State frames also fix the "S3 started after the C3" and "S3 restarted" cases.
4. **Sequence numbers** to detect lost frames and discard duplicates, if edges are repeated.
5. **Encoder:** relative movement can't be repaired the way button state can. Options: send signed deltas (a lost frame
   loses movement), or send a free-running counter/position (the S3 computes the delta, a lost frame loses nothing,
   and the first frame after a restart just sets the baseline). Velocity: either computed on the C3 (it has the
   precise timing) and sent in the frame, or computed on the S3 from timestamps (jittery over a UART). The binding
   `param` is a string, e.g. `velocity=10`, so the format of that string also needs fixing.
6. **Liveness:** a heartbeat from the C3 lets the S3 know the pad is connected (and release any held pad keys when it
   goes silent, so a pulled cable doesn't stick a key). Also a possible future "pad connected" indicator in the status.
7. **Versioning and identity:** a protocol version, and what the C3 reports about itself (number of keys, whether it has
   an encoder), so the S3 doesn't hard-code "6 keys + 1 encoder" beyond the control names.
8. **Throughput:** trivial at 115200 baud (a button edge is a few bytes), but an encoder spun fast can emit hundreds of
   events a second; the S3 side buffer must not drop button edges when the encoder floods (consider a bounded ring and
   accounting for what's dropped).
9. **Debounce** belongs on the C3, not in the protocol. The S3 trusts that edges are clean.
10. **Names:** the wire should carry a small numeric control id; the S3 maps ids to the strings used by bindings
    (`macro1`..`macro6`, `volEnc`, knob). Where that mapping lives (firmware constants vs the config blob) is a
    decision.
11. **Hosted mode:** the same stream, mapped to a protocol event for the host (raw press/release/turn). In hosted mode
    the host owns detection of long/double press.
12. **Test tooling:** the parser must be fuzzable and testable with raw bytes in (as the existing protocol tests do), and
    a host-side script that plays frames into the S3 (e.g. via a USB-serial adapter on the free pin) is needed to
    test without the C3. A C3 reference firmware (or at least a frame encoder library shared between the C3 and the
    tests) would keep the two ends in step.

## Answers from the user (2026-11-06) and what they settle
1. **Pin:** the sister project `aclock` uses **GPIO 6** as the UART1 TX to `audmod` (`aclock/clock/main/audio.c`:
   `AUDIO_UART_PORT UART_NUM_1`, `AUDIO_TX_GPIO 6`, `uart_set_pin(..., tx, UART_PIN_NO_CHANGE, ...)`). Same Waveshare
   board. ControlBlock uses none of GPIO 6 today, and it isn't a strapping pin. So: **UART1 RX on GPIO 6.** (Verified
   against the pin list in Notes. `aclock` installed the driver with a 256-byte RX buffer, the minimum.)
2. **115200 baud, 8N1, at most 100 mm of wire.** Short and slow: corruption should be rare, but the S3's RGB bus is
   noisy (Wi-Fi job), so the CRC stays.
3. **The C3 firmware lives in this repo**, with a reference implementation.
4. **Up to 32 buttons (the encoders' push buttons included) and up to 4 encoders.**
5. **Encoder = an absolute unsigned 32-bit position**; direction comes from the change in the position, and speed is
   calculated from the turn updates (on the S3, from the timing of the position changes).
6. **Turn events carry a signed delta and a speed.** No "turn while pressed".
7. **A pad button held across takeover, activity switch or restart is dropped** (the touch-key rule).

### PacketEncoder (submodule `PacketEncoder/`, read 2026-11-06)
- Gives us the framing, so the design points 1, 2 and 4 above are covered by the library: `AA AA AA 00` start signal
  (unstuffed anywhere else, so the decoder resyncs at any time, even mid-packet), varint `seq`, varint `cmd`, varint
  `length`, payload, big-endian CRC-32 (zlib's), `55` terminator. A packet is delivered only after the CRC and the
  terminator check, so the callback never sees partial or corrupt data. Errors are counted by code
  (`packet_error_*`, 1..6) through an optional `onError`.
- **No ACK and no retransmit**, by design. The library says so: guaranteed delivery would be built on top. We can't
  (one wire), so the pad protocol must be self-healing (design below).
- C: `packenc.c/.h`, `crc32.c/.h`, no allocation; the decoder needs a caller-supplied payload buffer (`pBuf`/`cbBuf`;
  a payload bigger than that is dropped with `packet_error_too_large`). `packet_encode` emits through a per-byte
  callback (a plain function pointer, no context argument: a C3 UART writer will use a file-static buffer, as
  `aclock`'s `audio.c` does). JS twin: `packenc.js` (ES module, `Buffer`) with `packet_encode` and `packet_decoder`,
  same wire format: usable for host-side tooling and tests. Its `test/` is minimal (one sample packet).
- Overhead: about 12 bytes for small packets.
- **Name clash to handle:** `PacketEncoder/crc32.h` (C, `crc32(const void*, int)`, plus `crc32_start/update/finish`)
  has the same file name as `firmware/main/crc32.h` (C++, inline `crc32(uint32_t crc, const void*, size_t)`, namespace
  `crc32_detail`). Whichever include path comes first wins, which would silently break one of them. The two functions
  overload (different signatures) but the C one is `extern "C"` in the same translation unit if both are included.
  Options in Plan: add only `PacketEncoder/packenc.h` to the include path and compile `crc32.c` as `pe_crc32.c`-style
  renamed? (can't rename a submodule file); or add `crc32.c` but make `packenc.c` the only includer; or make our
  `crc32.h` the single source and give PacketEncoder a thin `crc32` shim (touches the submodule); or ask the
  submodule's owner (the user) to rename or namespace it. Needs the user's decision.
- `decode_context` is zero-initialised, with `onPacket`/`onError` plain function pointers and no user context
  pointer: the S3 receiver wraps one decoder instance in a file-static or reaches the receiver through a static.
- Not yet checked: that `packenc.c` and `crc32.c` build clean with the project's flags on the S3 (`-Werror`,
  gnu++26 for C++; these are C files, like `rle565.c`) and under MSVC x86 for the host tests; the 1 KB CRC table is
  built on first use (RAM; not thread-safe if two tasks first-use it at once. The S3 would have it used by one task
  only, but the config upload's own CRC is separate code, so no clash at run time).

### Proposed protocol (a draft for the Plan phase to confirm; nothing here is built)
One `cmd` does nearly everything, so there is a single code path to get right:

- **`cmd 1` STATE** (the C3 -> S3 workhorse), payload `u32 buttons` (little-endian; bit `n` = button `n+1` down) then
  `n` x `u32 position` (little-endian), `n` = 0..4 encoders, **implied by the payload length** (`4 + 4n`). Sent
  **immediately on any change** and **repeated every ~100 ms regardless** (the heartbeat). Max 20 bytes, ~32 bytes on
  the wire (~3 ms at 115200).
- **The S3 never reacts to a packet; it reacts to the difference between a STATE and the previous one.** A button
  bit going 0 -> 1 is `Press`, 1 -> 0 is `Release`; a position change is a `turn` (below). Hence a lost packet costs
  nothing: the next one carries the same edge (a `release` lost to noise is repaired at the next heartbeat, 100 ms
  later, rather than sticking a HID key down). Duplicates change nothing.
- **Encoder coalescing is free:** a position is absolute, so the C3 may send at most one STATE every ~5 ms while an
  encoder spins and lose no movement (the delta just covers more ticks). That bounds the S3's trigger rate to ~200/s,
  and the line to ~60% load at the very worst; a button edge goes out at once anyway.
- **`turn` event:** control `enc1`..`enc4`, event `turn`, `param` = `delta=<signed int> speed=<ticks per second>`
  (e.g. `delta=-3 speed=42`). `delta` = `(int32)(pos - last_pos)` (wraps correctly across 2^32). `speed` is computed on
  the S3 from the delta and the time since the encoder's previous change, an unsigned magnitude (the sign is in
  `delta`). Exact format and whether `speed` is smoothed are Plan decisions. No `turnLeft`/`turnRight`: one event,
  signed delta (the user's choice in answer 6). Encoder push buttons are ordinary buttons.
- **Baseline rules** (so nothing spurious fires): the **first STATE ever seen**, and the first after the C3 has been
  silent for more than the timeout, **sets the baseline and fires nothing** (neither presses for already-held buttons
  nor turns). A held-at-start button is then ignored until released and pressed again (answer 7: dropped).
- **Liveness:** if no valid STATE arrives for ~300 ms (3 heartbeats), the pad is offline: held pad buttons are
  released (a Cancel-style `release`, so a `hold` HID key isn't left down by an unplugged wire) and the baseline is
  cleared, so reconnecting starts fresh. Whether to tell the host (a pad-connected flag) is a Plan item.
- **C3 restart:** it comes back with the position it chose. To avoid a bogus huge delta, the C3 sends a **`cmd 2`
  HELLO** (payload: `u8 protocol_version`, `u8 button_count`, `u8 encoder_count`) a few times at boot; the S3 treats
  HELLO as "clear the baseline". Also reject an implausibly large `|delta|` (e.g. over 2^15) as a baseline reset
  rather than a turn. Unknown `cmd`s are ignored (room for the future: LEDs can't be sent back, but e.g. battery
  status could be sent forward).
- **`seq`:** a free-running u32 per packet from the C3. The S3 uses it only for statistics (gaps = lost or corrupted
  packets, counted in the stats line), never for logic.
- **Control names:** generated on the S3 from the index: buttons `pad1`..`pad32`, encoders `enc1`..`enc4`
  (assumption; replaces the config-format proposals `macro1`..`macro6` / `volEnc`, which would be updated with the
  docs). `GestureBank` gives `press`/`release`/`tap`/`long` for the buttons. (Names are 4-5 characters; the gesture
  limit is 23.) `GESTURE_SLOTS` is 8 held at once: more simultaneously held pad buttons are ignored, which is
  acceptable (a 32-key chord isn't a use case) but worth a log counter.
- **Hosted mode:** new events `0x85` PAD_BUTTON (`u8 index`, `u8 down`) and `0x86` PAD_TURN (`u8 encoder`, `i32 delta`,
  `u16 speed`), sent in hosted mode only, like KEY. The host detects long/double press. (Draft; Plan decides the
  layout, and whether the host also gets a `PAD_STATE` for resync on takeover.)
- **S3 receive path:** UART1 RX, GPIO 6, 115200 8N1, a small task reading the driver's RX queue and feeding
  `packet_decode`; pad logic split pure/device like the rest: `pad_link` (pure: STATE diffing, baseline, timeout,
  speed, names -> `PadHost` callbacks) and `pad_uart.cpp` (device: UART + task + wiring to `Standalone` / `Protocol`
  through one switch point beside `on_key`).
- **Counters** (stats line): packets ok, decode errors by code, seq gaps, dropped triggers (queue/param pool full),
  presses ignored (no gesture slot).

### Pool pressure to check in Plan
`TriggerDispatcher::Trigger` puts the `param` into the 8-slot refcounted pool. Fast encoder turns each take a slot
until their op list finishes; a slow script (HTTP volume change) with ~200 triggers/s will fill the pool and the
40-entry op queue, and further triggers are dropped and counted. `standalone.md` already says "no coalescing,
decided", so this is accepted as is, but the rate cap on the C3 (5 ms) and the drop counters make it visible. Revisit
only if the hardware shows an encoder feeling dead.

### C3 side (to design in Plan)
- A new ESP-IDF project for the ESP32-C3, in `pad/` (name open), with its own `CMakeLists`, scanning a key matrix or
  direct GPIOs, debouncing, reading quadrature encoders (PCNT peripheral), and sending STATE through
  `packet_encode` -> UART TX. Needs the C3 module's pin choices from the user.
- **Use UART1 (or a non-console pin) for the link, and keep the C3's console and boot log off that TX pin:** the ROM
  boot log prints on UART0 at 115200 and would otherwise feed garbage into the S3 (the packet decoder discards it,
  but it would cost errors and CPU).
- The shared protocol code (`pad_protocol.h`: build and parse STATE/HELLO payloads, button/encoder limits, timing
  constants) must be one pure header used by the C3 firmware, the S3 and the host tests, and mirrored for Node (a
  `host/` or tools script that plays STATE packets for testing and reads them, using `packenc.js`).

## Decisions, round 2 (user, 2026-11-06; these answer the questions below)
1. **crc32 clash:** our header renamed to `firmware/main/crc32_table.h` (done: includes in `config_store.cpp`,
   `TestConfigStore.cpp`, `TestProtocol.cpp`, the comment in `host/src/protocol.js`, `docs/config-upload-plan.md` and
   CLAUDE.md updated; host tests: 552 pass). `PacketEncoder/crc32.h` is now free to use as is.
2. **Control names: `keyN` and `encN`** (`key1`..`key32`, `enc1`..`enc4`), replacing the `macro1..6` / `volEnc`
   proposals in `config-format.md` (update when documenting).
3. **Timings accepted as the starting point:** 100 ms STATE repeat, 300 ms offline timeout, 5 ms encoder rate cap;
   tune on the hardware.
4. **Encoders are quadrature** (the user will confirm the part). Speed in ticks per second; whether a tick is a detent
   or a raw edge depends on the part, so the C3 reports **detents** (it does the quadrature decoding) and the part's
   counts-per-detent is a C3 constant. Smoothing: none at first.
5. **C3 board:** the same C3 board as the C3 version of `irtx` (a sister project, `C:\Users\Brad\Projects\irtx`; look
   there for the board and pin conventions). **Direct GPIOs for all buttons**, any pins are fine (so the pin map is a
   header of my choosing; the up-to-32 limit is the protocol's, not the board's).
6. **Hosted mode: no pad-state request.** Only the two events (button, turn).

## Reference behaviour: `urem` (`C:\Users\Brad\Projects\urem\frontend\gpio.js`, `volume.js`, read 2026-11-06)
The user: this is how the same encoder is handled in `urem` and it "works great"; **ControlBlock must match it**.
Facts from the code (Raspberry Pi, `pigpio`):
- **Pins/wiring:** 7 buttons (BCM 17, 27, 22, 23, 24, 25, 16; button 6 is mute in `volume.js`, likely the knob
  button) and encoder A=21, B=20. All inputs **internal pull-up, active low** (level 0 = pressed).
- **Debounce:** pigpio glitch filter, **5 ms on buttons, 1 ms on the encoder pins** (a level must hold that long to be
  seen). On the C3 that is a per-pin debounce (hardware GPIO glitch filter if the C3 has one, else a timer).
- **Decoding:** the encoder pins are watched on **every edge of A and B**; each edge runs a full-quadrature Gray-code
  table on the 2-bit state (CW: 00->01->11->10->00, CCW the reverse); invalid transitions are ignored. A running
  `pos` changes by +/-1 per valid edge and a **tick fires every 4 edges** (`pos % 4 == 0`), so one detent = one tick
  of +/-1, i.e. **detent resolution, 4 quadrature edges per tick**.
- **Velocity:** `velocityInterval` = the **average of the last 4 inter-edge intervals, in ms** (`Date.now()`
  differences, each capped at 1000 ms, initialised to 1000). The interval is taken at **every call, including edges
  that produced no valid transition** (bounce counts), and handed to the listener on each tick. So it is ms *per
  edge* (roughly a quarter of the per-detent time), **smaller = faster**.
- **How `volume.js` uses it:** `step = 1`; if `velocityInterval < 8` -> 5; else if `< 20` -> 3; then
  `adjustVolume(delta, step)`. One call per tick (delta = +/-1 each time), no coalescing.
- **Buttons (`GpioButton`):** `press` on press; `release` on release; optional **`repeat`** (a number = period and
  initial delay, or `{ period, initialDelay }`; defaults period 100 ms, initial delay 500 ms: **the `press` handler is
  called again every period while held**, used for `repeat: 100` on arrow keys); optional **`longPress`** after
  `longPressDelay` (default 500 ms), which also **suppresses the following release** (and ends any repeat).
- Layered activities (`Layer`) pick which button/encoder objects are active, like our per-activity bindings.

### What this changes (differences to resolve in Plan)
1. **Speed.** urem's feel comes from the *per-edge* timing at ~1 ms resolution measured at the pin. The S3 can't
   reproduce that from STATE packets: they arrive at most every 5 ms and UART/RTOS jitter is several ms, while the
   urem thresholds are 8 ms and 20 ms. Decision 5 of the user ("speed calculated from turn updates", absolute
   position) therefore can't match urem unless the **C3 measures the interval** and sends it. Proposed (replacing the
   speed half of decision 5; the absolute position stays): each encoder's STATE carries `position` (u32, in detents,
   the user's absolute value) **and `interval`** (u16: urem's moving-average ms-per-edge, in microseconds, saturating
   at 65535 = 65 ms, which is above the 20 ms top threshold). The `turn` trigger's param is then
   `delta=<signed detents> interval=<value>`; so the Lua volume script is a direct port of `volume.js` (compare
   `interval < 8000` and `< 20000`, or the S3 formats ms). Naming: `interval` (smaller = faster) rather than a
   `speed` where bigger = faster, so scripts match `velocityInterval`. **Needs the user's OK** (see Open question A).
2. **Detent vs edge:** the C3 decodes the quadrature exactly like urem (same 8-entry table, 4 edges per detent) and
   reports detents. Matches decision "C3 reports detents".
3. **Debounce** values: 5 ms buttons, 1 ms encoder pins, as urem. Constants in the C3 firmware.
4. **Coalescing:** with a 5 ms STATE cap one packet can carry several detents (delta of 2..n). `adjustVolume(delta,
   step)` takes a delta, so one event with `delta=3` is equivalent to three events with the same `step`; the step
   uses the latest interval. Acceptable.
5. **Button `repeat` is missing from ControlBlock.** `GestureBank` has press/release/tap/long only. urem's activities
   use `repeat: 100` heavily (the Apple TV arrows). Needs a decision, probably a new gesture event or a per-binding
   repeat on the pad (and on touch keys), in the S3 (not the C3). That is a separate piece of work (gesture engine,
   config-format and bake), maybe its own job; the pad job should not depend on it. See Open question B.
6. **Release after long:** urem suppresses the `release` after a `longPress` fires; ControlBlock still fires
   `release` after `long` (it matters for HID `hold`). Not a conflict for the pad protocol; note for the authoring
   layer. Not changing.

### Open questions (new)
A. **Speed from the C3 (above)**: OK to replace "speed computed on the S3 from update timing" by the C3's
   `interval` (urem's algorithm), with the position still absolute?
B. **Button repeat**: do you want it as part of this job, as a separate job, or later?

## Open questions, as asked (all answered above)
1. **`crc32.h` clash** between the submodule and `firmware/main/crc32.h` (see above): which fix? Recommended: leave the
   submodule alone, add only `PacketEncoder/` to the include path *after* `firmware/main`, and compile
   `packenc.c`/`crc32.c` with a tiny wrapper header? That is fragile. The robust options are renaming in the submodule
   (your repo) or my `crc32.h` moving to a non-colliding name (e.g. `crc32_table.h`, a small change in firmware and the
   tests). Which do you prefer?
2. **Names:** `pad1`..`pad32` and `enc1`..`enc4` on the wire-to-binding side (the config's `control` strings), or do
   you want per-device naming in the config (the config maps `pad3` to your own label)? The device only matches exact
   strings, so the authoring layer can alias.
3. **Heartbeat/timeout numbers:** 100 ms repeat and 300 ms offline timeout, and the 5 ms encoder rate cap. OK to start
   there and tune on the hardware?
4. **Speed unit:** ticks per second, unsmoothed or lightly smoothed? Does the C3 count ticks (detents) or raw
   quadrature edges (4 per detent)? The encoder hardware decides; tell me what the encoder is.
5. **C3 board and pins:** which ESP32-C3 module (pins for the matrix/direct inputs, encoder A/B, the TX pin), and does
   the C3 scan a matrix or use direct GPIOs for up to 32 buttons? (I can leave the pin map as a header you fill in.)
6. **Hosted mode:** is two new events (button, turn) enough, or do you want the host to be able to read the current
   pad state on takeover (a request/response such as `GET_PAD_STATE`)?

## Flagged assumptions
- Mode decision point: a new `pad_input` switch point beside `on_key` in `host_link.cpp`, rather than the UART task
  deciding. (Matches the existing "single switch point" rule.)
- UART layer is a thin device file (`pad_uart.cpp`) and everything else is pure and tested, per the Conventions.
- No S3-to-C3 traffic of any kind, including no flow control; the S3 must always keep up or drop and count.
- The proposed protocol above is a draft; the user has only decided answers 1 to 7.

## Final design (decided 2026-11-06; supersedes the "Proposed protocol" draft where they differ)
Decisions from the user in round 3: **speed is calculated on the C3** (urem's algorithm; positions are not
timestamped); **button repeat is out of scope** for now.

**Wire (PacketEncoder, 115200 8N1, C3 TX -> S3 RX on GPIO 6, one way):**
- `cmd 1` STATE, payload (little-endian): `u32 keys` (bit n = `key{n+1}` down; encoder push buttons are ordinary
  keys), then per encoder `u32 position` (detents, absolute, wraps) and `u16 interval` (urem's `velocityInterval`:
  the average of the last 4 inter-edge intervals in **microseconds**, each capped at 1,000,000 µs as urem caps at
  1000 ms, then saturated to 65535 µs since every threshold of interest is below 20 ms; initial value saturated).
  Encoder count `n` = 0..4 implied by the length: `len = 4 + 6n` (4..28 bytes). Any other length is dropped.
- `cmd 2` HELLO, payload `u8 version (1)`, `u8 key_count`, `u8 encoder_count`: sent three times, 50 ms apart, at C3
  boot. The S3 clears its baseline. Unknown `cmd`s ignored. `seq`: free-running per packet, S3 stats only.
- Send policy on the C3: STATE at once on a key change; for an encoder-only change at most one per 5 ms (positions are
  absolute so nothing is lost); a repeat of the current STATE every 100 ms regardless (heartbeat and repair).

**S3 reaction** (`PadInput`, pure): acts on the difference between consecutive STATEs. First STATE (or first after the
300 ms offline timeout, or after a HELLO) only sets the baseline: nothing fires, and buttons already down are ignored
until released (decision 7). Key bit 0->1 = `Press`, 1->0 = `Release` on `GestureBank` as `key{N}`. Encoder position
change: `delta = (int32)(pos - last)`; `|delta|` over 2^15 is treated as a baseline reset, not a turn; else a trigger
`enc{N}` / `turn` / param `delta=<signed> interval=<µs>` straight to `TriggerDispatcher::Trigger` (not through
gestures; it is a discrete event). Offline (no valid STATE for 300 ms): `Cancel` every held `key*` control (a
`release` fires, so a `hold` HID key never sticks) and clear the baseline. Param is at most about 32 characters
(`PARAM_SLOT_BYTES` is 48).

**Hosted mode:** events `0x85` PAD_KEY (`u8 index`, `u8 down`) and `0x86` PAD_TURN (`u8 encoder`, `i32 delta`,
`u32 interval_us`); only while hosted, like KEY. No pad-state request (decision 6).

**Reference behaviour to reproduce on the C3 (urem):** pull-up inputs, active low; glitch filter 5 ms on buttons and
1 ms on encoder pins (a level counts only after holding that long); the 8-entry Gray-code table, one detent = 4
valid edges; the interval average is updated on **every** edge of A or B, valid or not.

**C3 firmware platform:** ESP-IDF (same toolchain as the S3, v6.0, target `esp32c3`), in `pad/`. `irtx`'s C3 build is
Arduino (`arduino-cli`, `c3_zero` board) and its encoder code is a *polled* 4-tick-timestamp scheme with a 2 ms
debounce, which is **not** the algorithm that works in `urem`; do not copy it. Encoder pin edges come from GPIO
interrupts (both edges) timestamped with `esp_timer_get_time()`, like urem's alerts.

### Step 1 notes (done 2026-11-06)
- Host tests: `pad/test/` (`test.mk.js`, `main.cpp`, `TestPadProtocol.cpp`; `cd pad/test && mk run`, ~3 s). 12 tests,
  same count in debug and release; `FUZZ_SEEDS=5000` release soak passes. Built x86 MSVC; `packenc.c` and `crc32.c`
  compile with no warnings (the `vswhere.exe is not recognized` line printed by `mk` is harmless noise).
- Shared code: `pad/common/pad_protocol.h/.cpp` (pure C++, no STL; `PadState`, `PadHello`, `PadBuildState/ParseState/
  BuildHello/ParseHello`, `PAD_PAYLOAD_MAX_BYTES` = 28 for the decoder buffer, timing and HELLO constants).
  Golden wire bytes (STATE seq 5, HELLO seq 0) are in `pad/common/pad_golden.md` and asserted in the C++ test; step 9
  must assert the same arrays with `packenc.js` (they were generated with it, so the JS and C encoders agree).
- PacketEncoder: delivers an unknown `cmd` and any payload length up to `cbBuf` (the receiver must call
  `PadParseState`/`PadParseHello`, which reject bad lengths); a payload over `cbBuf` gives `packet_error_too_large`.
  `packet_encode` takes a bare function pointer (tests write to a static buffer). A packet cut anywhere is replaced
  cleanly by the next start signal, and with random drops/flips/inserts nothing but bit-exact originals is delivered
  (CRC-32). `write_varlen` in `packenc.c` is non-static (global symbol) and the C file defines non-static consts
  (`signal_byte` etc.): harmless now, but a name clash risk if our code defines the same names.
- `decode_context.crcRecv` is not cleared between packets (it is shifted 4 times so it doesn't matter).
- Nothing under `firmware/` or `PacketEncoder/` was touched.

### Step 7 notes (done 2026-11-06)
- Host tests: 44 pass in debug and release (`cd pad/test && mk run`; `FUZZ_SEEDS=400` release soak passes). New files:
  `pad/common/glitch_filter.h/.cpp`, `quad_decoder.h/.cpp`, `encoder_channel.h/.cpp` (two filters + a decoder = urem's
  encoder path, one class so the scanner and the differential test share it), `pad_scanner.h/.cpp`; tests
  `TestPadScanner.cpp` (hand-written cases + a random-input invariant fuzz), `TestPadUrem.cpp` (reads
  `pad/test/data/urem_vectors.json`, 109 cases, 6734 ticks, ~780 KB, committed); generator
  `pad/test/gen-urem-vectors.mjs` (run `node gen-urem-vectors.mjs`; deterministic seeds; **.mjs**, ES module). The test never
  runs node. Mutation check: changing the filter's `>=` to `>` fails the vectors and 7 hand tests.
- **GlitchFilter semantics (chosen):** a change is accepted once the new level has held >= N us (a pulse of exactly N passes);
  a shorter pulse produces no event; the accepted change carries its ORIGINAL edge time; edge driven, so call it on every raw
  edge and also poll (one accept per call; it assumes the raw level held between calls). pigpio delivers the alert N late and
  urem timestamps with Date.now() at delivery, which shifts all edges of both encoder pins by the same N, so intervals are the
  same. This is my model of pigpio, not pigpio's source: the JS twin in the generator is independent code but the same
  semantics, so the vectors guard the C++ against bugs, not against a wrong reading of pigpio (>= vs >, and the reported tick).
- **QuadDecoder = urem's updateEncoder:** every call (valid or not) updates the 4-ring (capped 1 s, start 4 x 1 s; the very first
  call counts as the cap, as urem's `timeLast = 0`), interval = sum/4 in integer us (urem's ms value x 1000 exactly on a ms
  grid, floored otherwise), 8-entry Gray table, tick when the running edge position (int32, starts 0) is a multiple of 4
  (`% 4 == 0`, negatives included). Consequence kept on purpose: a reversal fires a tick of the opposite sign when the edge
  position returns to a multiple of 4 (out two edges and back two = one -1 tick; detent position is the sum of ticks, not
  edgePos/4). **Cold start:** the first tick after boot carries ~250 ms in its average (the 1 s start values) = saturated 65535
  = "slow", same as urem after boot.
- **Where the C3 and urem deliberately differ:** (1) urem's updateEncoder re-reads both pins raw (digitalRead) when one pin's
  filtered alert arrives; the C3 gives the decoder the FILTERED levels of both pins as of that change, so every call changes
  exactly one bit. Same results on clean and bouncing streams; they can differ only when A and B change within the filter time
  of each other. (2) Edge time (us) instead of delivery time (uniform shift, no effect). (3) Time is uint64 us (no wrap).
  (4) The interval in STATE is the one handed over with the latest tick (urem hands it to the listener per tick), saturated to
  u16; urem's value is a fractional ms. (5) pigpio's own filter code isn't ported, only its documented semantics.
- **PadScanner:** `Init(config, sink, now, key_levels, enc_pins)`, `SetKeyLevels`, `SetEncoderPins`, `Poll` (call every ~1 ms and
  on every pin edge). Keys use one GlitchFilter each (5 ms), active low (a 0 bit = pressed; a key held at start counts as down).
  The sink is `void(*)(uint8_t)` straight into `packet_encode` (which has no context argument): the C3 layer must give it a
  file-static TX buffer (one scanner per process). Policy (all tested on a fake clock): HELLO x3 at 0/50/100 ms; **no STATE until
  the last HELLO is out** (a HELLO after a STATE would clear the S3's baseline again; my addition), then STATE at once; key change
  = immediate, not capped; encoder-only = at most one per 5 ms measured from the last STATE of any kind (the held-back one
  goes at the first poll that is 5 ms after); repeat 100 ms after the last STATE of any kind; seq free running from 0 (HELLOs
  included). An encoder change that nets to zero within a held-back 5 ms still sends (harmless).
- For the step 8 C3 layer: poll `PadScanner::Poll` at 1 kHz or faster; encoder ISR stamps (time, a, b) and the task calls
  `SetEncoderPins(stamp_time, ...)` in time order (the filters need the call sequence in time order, with the time of the edge).
- Test suite caveat remembered: keep `mk run` debug and release counts equal (both 44).

### Step 9 notes (done 2026-11-06)
- `tools/pad-sim/` (ES modules, `npm test` = 49 tests, ~1.5 s, no hardware; `serialport` 13 installed fine). Files:
  `src/pad_wire.js` (the JS twin of `pad_protocol`; imports `../../../PacketEncoder/packenc.js`, no copy), `src/pad_dump.js`
  (`PadDump`: pure, bytes in, events out, injected clock), `src/pad_sim.js` (`parseScript`, `PadSim`, `corrupt`, `makeRng`),
  thin wrappers `dump.js` / `sim.js`, `README.md` (wiring, usage, script format).
- Golden bytes asserted in `test/pad_wire.test.js` (same arrays as `pad_golden.md`); if the wire format changes, change
  the C++ test, `pad_golden.md` and this test together.
- `packet_decoder` reports errors as text only; `classifyError` maps them to names (`crc`, `newSignal`, `tooLarge`,
  `terminator`, `separator`, `stuff`). Its callback payload is a view into a reused buffer: copy it (`Buffer.from`).
- `PadDump` mirrors the S3 rules (baseline on first STATE / HELLO / after 300 ms silence, `|delta|` over 2^15 = reset, changed
  encoder count rebaselines), so it also serves as a readable model for step 2. Repeats and seq gaps/resets are counted.
- `PadSim` mirrors `pad_scanner.cpp` policy: HELLO x3 at 0/50/100 ms, no STATE until the last HELLO, key change at
  once, encoder-only at most every 5 ms, repeat 100 ms. The script starts after a 300 ms lead-in (S3 baseline first;
  `--lead` overrides). `spin` = n detents, `<ms>` per edge, a detent every 4 x ms, interval = ms x 1000 us. Noise is
  per byte: drop / bit flip / random byte inserted, seedable.
- `sim.js -` writes to stdout so `sim.js - ... | dump.js --file -` checks both with no hardware (a CLI test does it).
- Unverified (no hardware): the actual serial open/read/write on a COM port (only the open-failure message was seen,
  with COM99), real FTDI timing (setInterval 1 ms jitter), and the S3 receiving the sim's output.
- Nothing under `firmware/` or `PacketEncoder/` was touched.

### Step 8 notes (done 2026-11-06, built, NOT run on a board)
- The esp32c3 toolchain was already installed (`riscv32-esp-elf` esp-15.2.0 under `C:\Espressif\tools`). Build from `pad/`:
  `idf.py set-target esp32c3` then `idf.py build`: 0 errors, no warnings, `pad.bin` 0x2adc0 bytes. Files: `pad/CMakeLists.txt`,
  `pad/main/CMakeLists.txt` (compiles `../common/*.cpp` and `PacketEncoder/packenc.c`, `crc32.c`), `sdkconfig.defaults`
  (HZ 1000, 160 MHz, GPIO ctrl in IRAM), `.gitignore` (build, sdkconfig), `main/pad_pins.h`, `main/main.cpp`, `README.md`.
  No change to `pad/common` was needed (tests untouched: 44 + 44).
- Design of `main.cpp`: GPIO ISR (IRAM, `ESP_INTR_FLAG_IRAM`) on both edges of each encoder pin reads both pins and pushes
  `{esp_timer us, encoder, a, b}` into a 256-entry SPSC ring (GCC `__atomic` builtins, no STL; full = drop and count in
  `overruns`). A high-priority task every tick (1 ms): drains the ring in order into `SetEncoderPins(stamp, ...)`, then reads
  the keys, `SetKeyLevels(now)`, `Poll(now)`, and after every scanner call flushes the file-static TX buffer with
  `uart_write_bytes` (UART1, TX only, driver RX buffer 256 because the driver insists on > 128). A separate priority-1
  stats task logs once a second (a log line takes ~7 ms at 115200, which would stall the 1 kHz task).
  Times given to the scanner never go backwards (single core: the ISR stamps and publishes atomically w.r.t. the task).
- Pin map: safe C3 pins are only 0, 1, 3, 4, 5, 6, 7, 10 (eight). 6 keys + 1 encoder + link TX need nine, so the **link TX is
  on GPIO 2** (strapping, but it idles high and drives nothing before the UART starts). Documented in `pad_pins.h`; the user
  adjusts it.
- Unverified without a board: ISR latency/jitter at a fast spin, 1 ms tick accuracy (FreeRTOS tick, not an `esp_timer`),
  that GPIO 2 as TX does not disturb boot with the S3 attached, the real pin map, UART TX timing, the `maxdrain` figure.
- Key edges are timestamped at the poll (1 ms resolution against a 5 ms filter), unlike the encoder edges (ISR stamps).

## Plan
Pure code first, device layers last, a hardware step at the end. Every step: tests first (Rule 3), host tests green
(`mk run` in `firmware/test`, debug and release counts equal) and, where files are touched, the S3 device builds
(`idf.py --preset debug build`) with 0 errors. **Steps 6, 8 and 13 cannot be covered by host tests: flag it.**
Shared pure code lives in `pad/common/` (used by the S3 firmware, the C3 firmware and the host tests).

**Order of work (changed 2026-11-06, the user is still testing the S3 and does not want it disturbed):**
**Phase C3 first: steps 1, 7, 8, 9, in that order. Nothing under `firmware/` (S3 firmware, its tests, its sdkconfig
or CMake) may be touched in this phase.** The C3 side gets its own host test project, **`pad/test/`** (same `mk` +
`UnitTesting.h` method as `firmware/test`, its own `test.mk.js`), not `firmware/test`, so the S3 tree is untouched.
(Steps 1 and 7 say `firmware/test` below: read that as `pad/test/`.) Phase S3 afterwards, when the user says so:
steps 2, 3, 4, 5, 6, then 10, 11, 12, 13; the S3 tests will then also build `pad/common/pad_protocol.cpp`.
The C3 target toolchain (riscv, `esp32c3`) may not be installed: step 8 must check and report, not install
anything without telling the user.

- [x] 1. **Wire format + PacketEncoder in the host tests.** Add `PacketEncoder/packenc.c`, `crc32.c` and the include
  path to `test.mk.js` (and check they compile under MSVC x86). Tests first (`TestPadProtocol.cpp`): build/parse
  STATE for 0..4 encoders and HELLO; reject bad lengths, 5 encoders, unknown cmd; round trips through the real
  `packet_encode`/`packet_decode` including stuffing (`keys = 0xAAAAAAAA`, `position = 0xAAAAAAAA`), a junk prefix,
  dropped/flipped bytes (fuzz with `FUZZ_SEEDS`, nothing corrupt is ever delivered), and a packet cut by a new start
  signal. Then `pad/common/pad_protocol.h/.cpp` (constants: `PAD_MAX_KEYS` 32, `PAD_MAX_ENCODERS` 4, cmds, version,
  timing constants 100/300/5 ms). Golden bytes also written down for the Node side (step 9).
- [ ] 2. **`PadInput` (S3, pure).** Tests first (`TestPadInput.cpp`): baseline on the first STATE, key edges, held-at-
  baseline ignored until released, wrapping position delta, big-delta reset, multiple changes in one STATE, HELLO
  resets the baseline, offline timeout releases held keys and clears the baseline, duplicates change nothing, counters
  (packets, decode errors, seq gaps). Injected `PadHost` callbacks (`key(index, down)`, `turn(encoder, delta,
  interval_us)`, `offline()`) and an injected clock. Then `pad_input.cpp/.h`.
- [ ] 3. **Standalone wiring.** Tests first (`TestPadStandalone.cpp`, with the sample blob extended by `key1`/`enc1`
  bindings and re-baked; also bump `sample_config.bin`): `key{N}` press/release/tap/long reach the activity's
  bindings (and not before an activity is selected, and not Home's), `enc{N}` `turn` pushes the list with the param
  `delta=-3 interval=6500`, an activity switch/takeover drops held pad keys silently (decision 7) and rebaselines,
  offline cancels held keys (release fires), a full queue/param pool drops and counts. Then: `PadControlName`,
  `FormatTurnParam` (one place), `GestureBank::CancelMatching(prefix)` (or equivalent), `Standalone::PadKey/PadTurn/
  PadOffline`. `GESTURE_SLOTS` stays 8; a press with no free slot is counted as ignored.
- [ ] 4. **Hosted events.** Tests first (`TestProtocol.cpp`: byte-exact `0x85`/`0x86`, only while hosted, `lost` flag,
  dropped on release; `host/` tests with `FakeDevice.js`: `padKey` and `padTurn` events). Then `Protocol::PostPadKey /
  PostPadTurn`, `host/src` events, and `docs/protocol.md` (keep firmware, `host/` and the spec in sync).
- [ ] 5. **One switch point.** `host_link.cpp` gets `on_pad_key / on_pad_turn / on_pad_offline` beside `on_key`:
  hosted -> `Protocol::PostPad*`, standalone -> `Standalone::Pad*`; a mode change calls `PadInput::Rebaseline()`
  (tested in a pure `PadRouter` or in `TestPadStandalone` with an injected mode; the `host_link.cpp` glue itself is
  hardware only). Device build.
- [ ] 6. **S3 UART device layer (hardware only).** `pad_uart.cpp/.h`: UART1, RX GPIO 6 (`PAD_RX_GPIO` in the header),
  115200 8N1, RX buffer 256, a task (core 0, low priority) feeding `packet_decode` (payload buffer 32 bytes; plain-
  function callbacks, so one file-static decoder), `onPacket` -> `PadInput` (taking the standalone mutex through
  the same path as `on_key`), `Tick` from the task every 20 ms for the timeout, counters in the stats line
  (`pad: ok/err/gaps/ign`). `main/CMakeLists.txt` adds the PacketEncoder sources and include path (renamed
  `crc32_table.h` already avoids the clash) and `pad/common/`. Init after the host link so a pad fault can never stop
  the screen. Warn: untested on a device; first check with the PC simulator (step 9) through a USB-serial adapter.
- [x] 7. **C3 pure modules + tests.** Tests first (`TestPadScanner.cpp`, host): `GlitchFilter` (stable-for-N-µs),
  `QuadDecoder` (the urem table, 4 edges per detent, invalid transitions ignored, interval = 4-edge average updated
  on every edge incl. invalid, 1 s cap, initial 1 s, saturation to u16 µs), `PadScanner` (key debounce 5 ms, encoder
  filter 1 ms, send policy: immediately on key change, 5 ms cap for encoder changes, 100 ms repeat, HELLO x3 at boot,
  builds STATE through `pad_protocol`). **Differential test against urem:** a Node port of urem's `updateEncoder`
  (`firmware/test/data/` generated vectors, same method as `op-vectors.json`) over random and real-looking edge
  streams (with bounces) must give the same detent ticks and the same intervals (ms). Then `pad/common/` code.
- [x] 8. **C3 firmware project (`pad/`, hardware only).** ESP-IDF `esp32c3` project: `CMakeLists`, `sdkconfig.defaults`
  (console on UART0, link on UART1 on a non-boot-log pin), `pad_pins.h` (direct GPIOs for up to 32 keys and 4 encoders,
  A/B pairs; **pin map is a placeholder header the user adjusts**), GPIO ISRs on both edges of each encoder pin
  (`esp_timer_get_time()` stamps into a small queue), key polling at 1 kHz, a task running `PadScanner` and writing
  packets with `packet_encode` into a TX buffer for `uart_write_bytes`. Builds with 0 errors for `esp32c3` (needs
  the user to have the C3 target installed; check). Warn: not runnable without the board.
- [x] 9. **PC tooling** (brought forward by the user, runs before step 8; **test rig: an FTDI USB-UART module at 3.3 V,
  its RX on the C3's link TX pin, common ground; for S3 tests the FTDI's TX goes to S3 GPIO 6**; so `dump` and `sim` take
  a COM port argument, 115200 8N1; no USB-mirror build option is needed on the C3). `tools/pad-sim/` (Node, using `PacketEncoder/packenc.js` and the same constants, kept in step
  by a golden-bytes test shared with step 1): `sim` (play scripted key presses/spins, with timing and optional line
  noise, onto a serial port, for testing the S3 without the C3) and `dump` (decode a serial stream and print it, for
  debugging the C3). `npm test` with no hardware.
- [ ] 10. **Bake/config side.** Tests first (`tools/bake/test`): a binding `control` must be `key1..key32` or
  `enc1..enc4` (touch keys stay an error); events: `key*` accept `press/release/tap/long`, `enc*` accept `turn`;
  anything else is an error with a path. Then `resolve.js` and `docs/config-format.md` (replaces `macro1..6` /
  `volEnc`/`turnLeft`/`turnRight`). Rebake the sample config if the schema is unchanged only the strings.
- [ ] 11. **Example script.** `tools/bake/examples/` Lua port of `volume.js` (parse `delta` and `interval` from
  `param`, `interval < 8000` -> step 5, `< 20000` -> 3, else 1; `http.post`), checked by the bake's Lua checker and
  by a `TestScript*` case that feeds it a param through the script host and checks the op it issues.
- [ ] 12. **Docs** (propose the edits to the user first, Rule 4): new `docs/macro-pad.md` (the wire protocol, the C3
  behaviour, reference to urem), `standalone.md` (close the "Still open" items, trigger names, the new `turn`
  event), `config-format.md` (done in 10), `protocol.md` (done in 4), CLAUDE.md (the modules, GPIO 6, the
  PacketEncoder submodule, `crc32_table.h`, the `pad/` project and how to build it, Open items for the hardware
  checks).
- [ ] 13. **Hardware checklist for the user (not automatable).** C3 flashed and wired to GPIO 6 (3.3 V, 100 mm); S3 stats
  line counts packets with no errors; every key press/release reaches an activity binding; `enc1` `turn` events
  feel the same as urem (compare the step 1/3/5 thresholds), fast spin loses nothing, direction correct; unplug
  the C3 mid-hold: HID key released within ~300 ms; reset the C3 while the S3 runs: no phantom turn; reset the S3
  while the C3 runs: baseline, no phantom press; hosted mode shows `padKey`/`padTurn` events; Wi-Fi latency and
  late fills unchanged with the pad running (the S3 UART RX and the RGB bus share a board).

