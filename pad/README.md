# Macro pad firmware (ESP32-C3)

A second MCU that reads up to 32 key switches and up to 4 quadrature rotary encoders and sends
their state to the ControlBlock (the ESP32-S3) over a one-way serial line. The protocol is in
`wip/wip-macro-pad.md` ("Final design") and `common/pad_golden.md` (golden wire bytes).

All logic is in `common/` (pure C++, host tested by `test/`): glitch filters, urem's quadrature
decoder, the send policy (HELLO x3, STATE on change, 100 ms repeat). `main/main.cpp` is only glue:
GPIO setup, an encoder edge ISR + ring, a 1 kHz task, the UART writer and a stats log line.

**None of `main/` can run in the host tests.** It builds, but it has not been run on a board yet.

## Build

ESP-IDF v6.0 with the esp32c3 toolchain (`riscv32-esp-elf`), from PowerShell in `pad/`:

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
idf.py set-target esp32c3      # once; creates build/ and sdkconfig
idf.py build
idf.py -p COMxx flash monitor  # the C3 board's USB port (GPIO 18/19) or its UART0 adapter
```

After changing `sdkconfig.defaults`, delete `sdkconfig` (it wins over the defaults) and run `set-target` again.

## Pins (`main/pad_pins.h`, a placeholder: edit it to your wiring)

Inputs use the internal pull-up and are active low (switch to ground).

| Function | GPIO |
|---|---|
| key1..key6 | 0, 1, 3, 4, 5, 6 |
| enc1 A / B | 7 / 10 |
| link TX (UART1, to the S3) | 2 |

Avoided: 8 and 9 (strapping), 18 and 19 (USB), 20 and 21 (UART0 console and ROM boot log), 11..17
(flash). GPIO 2 is also a strapping pin; it is used for the link TX because it idles high and
nothing pulls it down. An encoder's push button is an ordinary key: add its pin to `PAD_KEY_PINS`.

## Wiring to the S3

- Link TX (GPIO 2) to the S3's GPIO 6 (UART1 RX). One way: nothing goes back.
- Common ground, 3.3 V logic on both sides (do not use a 5 V adapter).
- At most 100 mm of wire (the S3's RGB bus is noisy; the packets carry a CRC-32 anyway).
- Keep the console (UART0, GPIO 20/21) off the link: the ROM boot log would feed garbage to the S3.

## Testing it

With an FTDI/USB-UART module at 3.3 V: its RX on the link TX pin, common ground. Then from
`tools/pad-sim`:

```
node dump.js COMxx            # prints HELLO / STATE / key and turn events, errors and seq gaps
```

See `tools/pad-sim/README.md` for the options. The C3's own log (stats once a second on UART0):
`pad: sent N (+rate) overruns .. txdrop .. maxdrain .. us keys .. det ..`.

## What to check on the board (hardware only)

- It boots and prints the stats line; `dump.js` shows HELLO x3 about 50 ms apart, then STATEs every
  100 ms with a clean seq (no gaps, no CRC errors).
- Each key shows down/up in `dump.js`; a contact bounce under 5 ms is ignored; no pin-map mistakes
  (key numbers match `PAD_KEY_PINS` order).
- The encoder: direction is right (flip A and B in `pad_pins.h` if not), one detent = one step, a fast
  spin loses no detents (compare `det` in the stats line after a known number of turns), and the
  `interval` values look like urem's (smaller = faster; about 6000 us/edge at a fast spin).
- `overruns` and `txdrop` stay 0, and `maxdrain` stays well under 1000 us at a fast spin.
- Holding a key through a C3 reset gives no spurious press on the S3 side (HELLO rebaselines).
- The wiring boots with the link connected (GPIO 2 not pulled low by the S3 side).
- Timing assumptions to confirm: `FreeRTOS_HZ` 1000 gives a 1 kHz poll (the scanner needs 1 kHz or faster),
  and the GPIO ISR (both edges, IRAM) keeps up with a fast encoder.
