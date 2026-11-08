# Backlight Control

The host switches the backlight on and off (`SET_BACKLIGHT`). While it is off the device is dark, so a touch must not act on keys or pages the user can't see. Instead the device tells the host the screen was touched (`WAKE`) and the host decides what to do, usually turning the backlight back on. Protocol encoding is in [`protocol.md`](protocol.md) (SET_BACKLIGHT, WAKE); touch handling is in [`interactions.md`](interactions.md).

## Goals

- The host owns the policy. It decides when to sleep (idle timer, screensaver, PC locked) and when to wake. The device has no idle timer and never changes the backlight on its own.
- A touch while dark must never press a key, start a swipe or show a pressed look. The first touch only wakes.
- The touch that wakes the screen must not then act on whatever appears under the finger.
- Content keeps updating while dark, so the screen is correct the moment the backlight comes on.

## Hardware

The backlight is a CH422G output (`CH422G_BACKLIGHT`, EXIO2, "DISP"): on or off, no dimming. `display_enable_backlight(bool)` drives it. The display and touch controller keep running while it is off; only the light goes.

### Switch-on glitch: the backlight is locked on for now

Switching the backlight on causes a supply transient that **corrupts PSRAM transfers in progress** (sparse bit errors over ~1 KB, shortly after the switch-on) and makes the CH343P UART USB port disconnect and reconnect. Found 2026-11-07: the stress run's PSRAM copy of `test_keys.bin`, made just after the light came on, differed from the original in 16-448 bytes (`wip/wip-lua-hardware-checks.md`). Code, rodata, page buffers, the layout blob and the Lua heap all live in PSRAM, so this isn't safe to work around in software: quieting the PSRAM writers around a switch-on can't protect code fetches. Waveshare says there is no link in the schematic between the backlight and the CH343P ([`backlight-waveshare-support.md`](backlight-waveshare-support.md)), which points at a 3.3 V/5 V supply dip.

**Until that's solved, firmware never switches the backlight.** It is on at power-up (per Waveshare) and `ch422g_init` keeps it on (`INITIAL_OUT`). `display_enable_backlight()` does nothing unless the build has `-DCB_BACKLIGHT_SWITCH=ON` (or `CB_BACKLIGHT_CYCLE_S`, which turns it on too). Everything above it still works as specified: `SET_BACKLIGHT` sets the state, `GET_INFO` reports it and the wake gate follows it. So with the lock, a host that turns the light "off" leaves the screen lit, but touches still only send `WAKE`. Also locked: the layout upload's dark screen (the screen stays lit, so display glitches during flash erases are visible) and `CB_NO_DISPLAY`'s light off.

**Experiments to try** (with `CB_BACKLIGHT_CYCLE_S`, watching for the UART dropout and running `RUN_STRESS`, whose load report checks the PSRAM copy):
- A 2 A charger instead of the PC's USB port.
- A 47-220 µF capacitor on 3.3 V (and/or 5 V).
- Scope the 3.3 V and 5 V rails at switch-on.
- Waveshare's `disable_ack_check = true` on both CH422G I2C handles; measure DISP/EXIO2.

**Related supply evidence (2026-11-08):** powered from the CH343P UART port alone, Wi-Fi associated but lost nearly all traffic (the first authentication timed out; 4 of 4 gateway pings lost). Connecting the native USB port fixed it. So the UART port alone can't carry the radio's transmit current either, which supports the supply-dip explanation. Note which ports power the board when running the experiments above.

### Possible mod: PWM dimming and a soft switch-on

The LED driver is an MPS **MP3302** (boost, internal soft-start, 10 µF input capacitor recommended). Its EN pin, driven from EXIO2 through a resistor today, also dims ([datasheet application pages](https://www.omo-ic.com/pdf/278578/MP3302DD-LF-P/P7-P9.html), Rev 0.98; [full datasheet](https://www.farnell.com/datasheets/2914860.pdf)):

- **Analog dimming:** a DC level on EN from 0.7 V (0%) to 1.4 V (100%) sets the LED current; bypass EN locally.
- **PWM directly on EN:** 200 Hz-1 kHz, at least 1.5 V. This pulses at full current, so it doesn't soften the switch-on.
- **Filtered PWM:** above 1 kHz, filter to DC with an RC whose cutoff is 10x below the PWM frequency (their example: 20 kHz through 20 kΩ / 100 nF). This gives analog dimming from a GPIO.

The mod: remove the resistor from EXIO2 to EN and run the one free header GPIO through 20 kΩ / 100 nF to EN, with a pull-down on EN so the light stays off (not half on) until the firmware drives it, and check the pin isn't a strapping pin. Firmware: LEDC at ~20 kHz; a switch-on ramps the duty over ~100-200 ms (much slower than the RC's ~2 ms), so the real current rises smoothly. From 3.3 V, 0-100% is ~21-42% duty (a divider can spread it); LEDC still gives a few hundred steps. Brightness control comes with it.

Costs: the macro pad loses its serial pin and moves to I2C (shared with the GT911 and CH422G) or the RS485 port; the rework is hard to undo. The internal soft-start already exists and the glitch still happens, so the ramp may or may not cure it: try the charger and capacitor first.

## State

| Item | Behaviour |
|---|---|
| Boot | On (as today). |
| `SET_BACKLIGHT` | Sets the state. Any non-zero `on` means on. Setting the current state changes nothing, including no touch handling changes. |
| `RESET` | Does not change the backlight. It clears the keys, so a dark device stays dark and shows an empty screen when woken. |
| `RESET_STREAM` | Does not change the backlight. |
| `TAKE_CONTROL` | Does not change the backlight. |
| `RELEASE_CONTROL`, USB unmount, USB suspend | **Turns the backlight on.** Standalone mode has no host to answer a `WAKE`, and Home needs the screen. A disconnect while already standalone turns it on too. |

`SET_BACKLIGHT` is accepted in both modes (it is device state, not keys).

The state is reported in `GET_INFO`: bit 0 of its `flags` field (the old reserved `u16` after `protocol_version`) is set while the backlight is on. A host that reconnects reads it to learn whether the screen is dark. There is no other way to read it, and no change event: the host is the only thing that changes it.

## Touch while the backlight is off

While off, touches are routed to a wake gate instead of the touch router and swipe task:

1. **First finger down:** send one `WAKE` event. Nothing else happens: no key events, no swipe, no pressed look, no page change.
2. **The rest of that touch is swallowed** until the first finger lifts, wherever it moves. Extra fingers are ignored, as in [`interactions.md`](interactions.md).
3. **Each new touch-down sends its own `WAKE`.** A host that is slow to wake the screen sees one `WAKE` per touch, not a stream.
4. The device stays dark. Only `SET_BACKLIGHT` turns it on.

### Turning the backlight off mid-interaction

`SET_BACKLIGHT` with `on = 0` while a finger is down ends the interaction cleanly, then the gate takes over:

- A key in `HELD` gets a `CANCEL` (it was `PRESS`ed, so it needs exactly one `RELEASE` or `CANCEL`). A key still `PENDING` sends nothing and its pressed look is reverted.
- A swipe or glide in progress is let go and settles on the nearest page as usual. A `PAGE_CHANGED` is reported when it settles, as normal.
- The finger that is still down becomes a swallowed touch. It sends no `WAKE`: the user was already touching the screen and wasn't waking it. The next touch-down does.

### Turning the backlight on mid-touch

If `SET_BACKLIGHT` with `on = 1` arrives while a swallowed touch is still down (the usual case: the host reacts to `WAKE` while the finger is still on the glass), the touch stays swallowed until it lifts. Only a touch that goes down after the backlight is on reaches keys and swipes. The same applies to a touch that was already down when the screen turned on.

## Content while dark

The compositor, status bar text, image store and `SET_PAGE` keep working. `SET_KEYS`, `PUT_IMAGE` and `SET_STATUS_TEXT` take effect as usual, so the screen is current when the light comes on. `SET_PAGE` with `ANIMATE` still animates (unseen). Nothing is skipped to save work, to keep the on and off paths the same.

## Events

`WAKE` (`0x82`) is an ordinary event: it uses the same queue, `lost` flag and overflow rules as `KEY`. If the queue overflows and the oldest events are dropped, a lost `WAKE` is fine: the next touch sends another.

Ordering: events already queued when the backlight goes off (for example the `CANCEL` above) are sent before any `WAKE`.

## Host side

- Turn the backlight off with `SET_BACKLIGHT`. When a `WAKE` arrives, turn it on (and refresh anything stale, such as status text).
- To ignore wake-ups for a while (an idle screensaver that shouldn't wake, say), just don't respond; the device stays dark.
- Don't treat `WAKE` as a key press. No `KEY` events follow it for the same touch.
- Read `GET_INFO` `flags` bit 0 after connecting to learn whether the backlight is on. After that, keep your own state.

## Implementation and testing

Tests come first and run on the host (`firmware/test/`); the backlight itself can only be checked on the hardware.

- **Wake gate in the pure touch layer.** Add the gate to the platform-independent touch code (`touch_router.cpp/.h`, or a small wrapper beside it so the router's state machine stays unchanged). It takes `SetEnabled(bool)` and a `wake` callback, and is driven by the same `(id, DOWN|MOVE|UP, x, y, time)` input. `swipe.cpp` feeds touches through it already, so swipe and key code see nothing while the gate is closed.
- **Protocol:** `MSG_SET_BACKLIGHT` also calls an injected `set_backlight` callback as today. Add `EVT_WAKE = 0x82` and `Protocol::PostWake(time_ms)` next to `PostKey`. `host_link.cpp` and `model.cpp` connect the three: backlight state, the gate and `PostWake`.
- **Tests to write first:**
  - Router/gate: touch while off sends one `WAKE` and no key or swipe decisions; moving, a long hold and a second finger send nothing more; lift then touch again sends a second `WAKE`; a touch down before `SetEnabled(true)` stays swallowed after it; off while `HELD` gives `CANCEL`; off while `PENDING` gives nothing and reverts the pressed look; off mid-swipe lets the swipe settle; off with a finger down sends no `WAKE` until the next touch-down; `SetEnabled` with the current value changes nothing.
  - Protocol: the `WAKE` event format and flags, the `lost` flag, event ordering (`CANCEL` before `WAKE`), `RESET` and `RESET_STREAM` leave the backlight alone, any non-zero `on` means on, a short payload gives `BAD_LENGTH`.
  - Harness: `SET_KEYS` and `SET_PAGE` applied while off show correctly after on.
  - GET_INFO: `flags` bit 0 follows the backlight (on at boot, clear after off, set after on), and survives `RESET`.
  - Host library (`host/test`, `FakeDevice.js`): `setBacklight`, a `wake` event and the `GET_INFO` backlight flag.
- **Not automatable:** the CH422G line and the real light, whether the GT911 reports reliably while the light is off, and how the wake touch feels. Check these on the hardware.
- **Docs and host library:** update `protocol.md` (done with this spec), `interactions.md` and `host/` (`protocol.js` event type, a `wake` event on `ControlBlock`).

## Decisions

- **State is reported** in `GET_INFO` `flags` bit 0, so a reconnecting host knows if the screen is dark.
- **No coordinates in `WAKE`:** it carries only a time. Any touch wakes.
- **No brightness:** on or off only. Possible only with the PWM mod above (not planned).

## Open items

- **Standalone idle dim and screen off with no activity** (backlog, from the urem port, `urem2/README.md`): urem
  turned the display off when no activity was selected, dimmed it after 8 s idle, and the first touch only woke it.
  Standalone mode has no idle policy. Pick up once the backlight can switch (see the switch-on glitch above); dimming
  needs the PWM mod.
