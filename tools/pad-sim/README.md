# pad-sim

PC tooling for the macro pad link (the one-way serial line from the pad MCU, an ESP32-C3, to the S3).
Spec: `wip/wip-macro-pad.md` ("Final design") and `pad/common/pad_protocol.h`.

- `dump.js` reads the link and prints what the S3 will see. Use it to check the C3 firmware.
- `sim.js` plays a scripted pad onto the link, optionally with line noise. Use it to test the S3 without the C3.

Both use `../../PacketEncoder/packenc.js` for the framing and `src/pad_wire.js` for the payloads. `pad_wire.js` is
the JS twin of `pad/common/pad_protocol.*`; the golden bytes in `test/pad_wire.test.js` (also in
`pad/common/pad_golden.md`, asserted by the C++ tests) stop the two drifting.

## Wiring

An FTDI USB-UART module set to **3.3 V**, 115200 8N1, common ground.

| Test | Wire |
|------|------|
| Debug the C3 with `dump` | C3 link TX -> FTDI RX |
| Test the S3 with `sim` | FTDI TX -> S3 GPIO 6 (UART1 RX) |

Never connect 5 V logic to either board.

## Setup and tests

```
npm install
npm test            # no hardware: unit tests, plus dump and sim run as processes
```

Needs Node 22 or later. The serial port comes from the `serialport` package.

## dump

```
node dump.js COM5                  # a serial port
node dump.js --file capture.bin    # replay a capture ("-" = stdin)
node dump.js COM5 --raw            # also hex-dump every chunk read
node dump.js COM5 --stats 10       # stats line every 10 s (default 5, 0 = off)
```

Ctrl+C prints the final stats and exits. Lines look like:

```
   0.006  HELLO version 1, 6 keys, 1 encoders (seq 0)
   0.127  baseline (held: key2)
   0.249  key3 down
   0.310  enc1 turn -3 (interval 6.5 ms)
   0.420  decode error crc: discarded packet: checksum mismatch (...)
   5.000  stats: ok 52 (states 49, hellos 3), errors crc 2, gaps 2, seq resets 0, repeats 40, bad 0, unknown 0
```

Events are derived the way the S3 will: from the difference between consecutive STATE packets. The first STATE
after start, a HELLO or an offline gap only sets a baseline (keys already down are listed, nothing fires). Silence for
300 ms prints `pad offline`. A position jump over 2^15 is a `reset`, not a turn. The interval is the C3's
urem-style average of the last 4 edge intervals (smaller = faster; 65.5 ms = saturated, i.e. slow).

Stats: `ok` packets, decode errors by name (`crc`, `newSignal` = a packet cut by a new start signal, `tooLarge`,
`terminator`, `separator`, `stuff`), `gaps` = packets missing in the sequence numbers, `seq resets` = the sequence
went backwards (C3 restart), `repeats` = STATEs identical to the previous one (the 100 ms heartbeat), `bad` = wrong
payload length, `unknown` = unknown command.

## sim

```
node sim.js COM6 -e "press 3 200; spin 1 +10 5"
node sim.js COM6 script.txt --loop --noise 0.01 --seed 7
node sim.js - -e "press 1 50" --duration 800 | node dump.js --file -    # no hardware: sim into dump
```

It behaves as the C3 does: HELLO x3 at 50 ms intervals, then STATE at once on a key change, at most one
encoder-only STATE per 5 ms, and a repeat of the current STATE every 100 ms. The script starts after a lead-in
(300 ms) so the S3 has a baseline first. When the script ends it lingers 300 ms, then exits (`--loop` and
`--duration` change that). Timing is `setInterval(1 ms)`: expect 1-2 ms jitter on Windows.

Options: `-e "<commands>"` (also joined with a script file), `--noise <rate>` (per byte, e.g. 0.01: each byte is
dropped, has a bit flipped, or gets a random byte inserted), `--seed <n>` (repeatable noise), `--seq-start <n>`
(first sequence number, e.g. 4294967290 to test the wrap), `--keys <n>` and `--encoders <n>` (reported in HELLO,
default 6 and 1), `--lead <ms>`, `--loop`, `--duration <ms>`.

### Script format

Commands run one after the other, one per line or separated by `;`. `#` starts a comment.

| Command | Meaning |
|---------|---------|
| `wait <ms>` | pause |
| `press <key> <ms>` | key down, hold for `<ms>`, key up (key 1..32) |
| `down <key>` / `up <key>` | a key edge; no time passes |
| `spin <enc> <+n or -n> <ms>` | `n` detents on encoder 1..4; `<ms>` between quadrature edges, so a detent takes 4 x `<ms>`; the STATE interval is `<ms>` in microseconds |
| `hello` | a C3 reboot: HELLO x3 (key and encoder state kept) |

Example: hold key 3 for 200 ms, spin the encoder ten detents clockwise slowly, then fast, then back:

```
press 3 200
spin 1 +10 10
spin 1 +10 2
spin 1 -10 5
```

Not simulated: a held-down key across a reboot, encoder bounce, and the 1 s cap/65535 saturation of a real
slow turn (the interval is whatever `<ms>` gives).
