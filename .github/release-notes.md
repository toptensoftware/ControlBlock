ControlBlock {{version}}: firmware for the Waveshare ESP32-S3-Touch-LCD-4.3, the host library and the CLI.

## Flash the firmware

**Web flasher (Chrome or Edge, nothing to install):** {{flasher_url}}

Plug the board in by its native USB port, hold BOOT and press RESET, then click Install. Press RESET again when it
has finished. Updating this way keeps your Wi-Fi, name and clock settings and the stored layout (unless you tick
"erase device", which wipes everything).

**esptool** (Espressif's standalone `esptool` executable, or `pip install esptool`), with the board in download
mode as above. An update that keeps your settings and the stored layout writes the three parts:

```
esptool --chip esp32s3 -p COM5 write-flash {{flash_parts}}
```

`controlblock-{{version}}-full.bin` is the same three parts merged into one image, written at 0x0. It is for a
fresh install: its padding **wipes the Wi-Fi, name and clock settings** (NVS), but it keeps the stored layout.

```
esptool --chip esp32s3 -p COM5 write-flash 0x0 controlblock-{{version}}-full.bin
```

## The CLI

Needs git and Node 24 or later. This release's CLI, pinned:

```
npx github:{{repo}}#cli-v{{version}} info
```

`npx github:{{repo}}#cli ...` runs the latest release's.

## Files

- `controlblock-{{version}}.bin`, `bootloader.bin`, `partition-table.bin`: the three parts; offsets in
  `flasher_args.json`.
- `controlblock-{{version}}-full.bin`: the merged image (fresh install; wipes settings).
- `controlblock-{{version}}-elf.zip`: the firmware's ELF, for decoding crash backtraces.
