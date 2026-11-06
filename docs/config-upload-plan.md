# Config upload: implementation plan

Scope: get a baked BPAK blob ([`standalone.md`](standalone.md)) from `tools/bake` into a flash partition over the mode 1 USB protocol. **Not in scope:** reading or loading the blob at boot, the "take control" / "release" messages, A/B partitions.

## Design

**Flash partition.** A raw data partition `config` (type `data`, subtype `0x40`, 4 MB at `0x210000`) in a new `firmware/partitions.csv`. The app (`factory`) grows to 2 MB; the remaining ~8 MB stays free for A/B config or OTA later.

**Layout inside the partition.** Sector 0 (4 KB) is a header; the blob starts at `0x1000` (4-aligned, so it can be flash-mapped later):

| Offset | Field |
|---|---|
| 0 | u32 magic `0x46434243` (`"CBCF"` bytes `43 42 43 46`) |
| 4 | u32 header version (`1`) |
| 8 | u32 blob size |
| 12 | u32 blob CRC-32 |
| 16 | u32 CRC-32 of bytes 0..15 |

The header is written **last**. Starting an upload erases the header sector first, so an interrupted upload leaves no valid header: the boot loader (later) finds no config and shows an empty Home. This is the "one partition, falls back to empty Home" decision in `standalone.md`, without needing a CRC pass over the blob to detect a half-written one.

**Messages** (details in [`protocol.md`](protocol.md#configuration-upload)): `CONFIG_BEGIN` (size, CRC-32), `CONFIG_DATA` (offset, bytes; in order), `CONFIG_COMMIT`. `GET_INFO` gains `config_capacity`, `config_size`, `config_crc` at the end, so the host can skip an upload when the stored blob is already the one it has.

**Device flow.**
1. `BEGIN` validates the size, tells the screen to blank, erases the header sector.
2. `DATA` erases ahead (up to the next 64 KB boundary, so the flash driver can use block erases) and writes each chunk where it belongs; keeps a running CRC. Erasing lazily keeps every request fast (a whole-partition erase would run for seconds and time the host out).
3. `COMMIT` checks the byte count and the running CRC, re-reads the flash and checks the CRC again (catches a bad write), writes the header, responds, then restarts the device.
4. Any error ends the upload (the header stays erased), so later chunks get `BAD_SEQUENCE`.

**Blanking.** Flash erase and write stall the bounce-buffer ISR. The device turns the backlight off for the duration of an upload (`ConfigFlash.busy`) and restores it on failure or abort; a successful commit restarts anyway. Confirmed on the hardware with a 16 KB blob; larger blobs untested (see below).

**Code shape** (CLAUDE.md: platform independent code is separated and host tested):

| File | What |
|---|---|
| `firmware/main/crc32_table.h` | zlib-compatible CRC-32 (header only, pure) |
| `firmware/main/config_store.cpp/.h` | the upload state machine and header format over an injected `ConfigFlash` (erase, write, read, busy). Pure, host tested |
| `firmware/main/config_flash.cpp/.h` | device only: `ConfigFlash` over `esp_partition` |
| `firmware/main/protocol.cpp/.h` | the three messages, the `GET_INFO` fields, restart after the commit response |
| `firmware/main/host_link.cpp` | wires the partition, the blanking and `esp_restart` |
| `host/src/protocol.js`, `ControlBlock.js` | encoders, `crc32`, `uploadConfig()`, `waitOpen()` |
| `tools/bake/src/upload.js`, `cli.js` | `--upload`, and uploading a `.bin` directly |

## Order of work (test first for each)

1. [x] Spec: `protocol.md`, `standalone.md`, this plan
2. [x] Firmware tests: `TestConfigStore.cpp` (CRC vector, header, lazy erase, ordering, errors, interruption, readback failure, flash errors, busy), `TestProtocol.cpp` (messages, `GET_INFO`, `RESET_STREAM`, restart)
3. [x] Firmware code: `crc32_table.h`, `config_store`, `protocol`; host tests pass
4. [x] Host tests: `protocol.test.js` (encoders, CRC, `GET_INFO`), `ControlBlock.test.js` + `FakeDevice` config emulation (chunking, windowing, skip when unchanged, errors, `waitOpen`)
5. [x] Host code
6. [x] Bake tests (`upload.test.js` against `FakeDevice`), then `upload.js` and the CLI
7. [x] Device layer: `partitions.csv`, `sdkconfig.defaults`, `config_flash`, `host_link`; debug build compiles
8. [x] Run all test suites
9. [x] Hardware: upload, restart, verify, skip, `--force` and reboot all work (16 KB sample, `tools/bake/examples/upload-test.json`). Closing an idle connection stalled up to 5 s (the read timeout); the timeout is now 250 ms

## Issues

No blockers. Things to know:

- **Hardware:** a 16 KB upload, the restart, re-enumeration and the partition table change all work. Still untested: a large blob (many 64 KB erases; look for display glitches and a host timeout during erase), and the unplug-mid-upload case below.
- **A new partition table needs a full flash** (`idf.py flash` writes it; `app-flash` does not). Delete `build/<preset>/sdkconfig` so `sdkconfig.defaults` applies (the generated sdkconfig wins over the defaults). Nothing is in NVS yet, so nothing is lost.
- **A host that vanishes mid-upload leaves the screen dark** until the next `RESET_STREAM` (which aborts the upload and restores the light). The device can't see a USB unmount today; an idle timeout on the upload would fix it if it matters.
- **Windows re-enumeration:** after the restart the device keeps its VID/PID/serial, so no driver prompt is expected.
- **The blob is not validated as BPAK at upload.** Only size and CRC are checked; the loader (later) checks the structure.
- **Upload while hosted:** the upload messages work in any state for now, because "take control" doesn't exist yet. Revisit then.
