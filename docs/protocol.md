# ControlBlock USB Protocol (v1)

This is the host ↔ device protocol for **mode 1 (host-controlled)**. The PC software uploads images and key layouts. The device draws them, runs paging, swiping and press feedback on its own, and reports key presses back to the PC.

How images are stored, replaced and freed on the device is in [`image-assets.md`](image-assets.md). Backlight on/off and the wake-on-touch event are in [`backlight.md`](backlight.md). Uploading the standalone config to flash is [below](#configuration-upload); how it's stored is in [`standalone.md`](standalone.md).

## Design summary

- **Transport:** A USB vendor interface with one bulk OUT and one bulk IN endpoint. It is reached through WebUSB in browsers and libusb (`usb` package) in Node.
- **Messages:** Each message is a 12-byte header followed by a payload. Messages run back to back on a byte stream; the protocol does not rely on USB packet boundaries or zero-length packets.
- **Requests and responses:** The host sends requests. Every request gets exactly one response, in order. The host may have several requests in flight (pipelining).
- **Events:** The device sends events (key presses, page changes) without being asked.
- **Images:** Images are stored on the device under 32-bit IDs that the host chooses (`0` = none). Using a content hash (CRC-32, say) as the ID gives caching for free: the host asks the device which IDs it already has and uploads only the missing ones. Uploading to an ID that's already there replaces the image everywhere it's used. All images are **rle565** (`.rle565`) with alpha, `key_w`×`key_h`.
- **Keys:** Each key names four images: a background and a foreground for the normal state, and a pair for the pressed state. The host draws everything, including faces, icons and labels (labels are baked into the images). Images can be shared between keys and kept on the device unused, so switching a key's look or showing a press needs no upload.
- **Paging:** Swiping, the pager and press feedback all run on the device. The host is told about page changes but never drives the animation.

## Byte order and encoding

- All integers are **little-endian**, matching the ESP32-S3 and rle565.
- Colours are RGB565 in a `u16`.
- Strings are UTF-8 with a length prefix; there's no terminating null.
- Payload data has no alignment guarantee. The device copies image data into its own 4-byte-aligned allocations before handing it to rle565.
- **Payloads can grow:** Later protocol versions may add fields at the end of any payload.
  - Readers must ignore trailing bytes they don't understand.
  - Fields missing from the end take their default value.

## USB layer

| Item | Value |
|---|---|
| Interface class | `0xFF` vendor, subclass `0x00`, protocol `0x00` |
| Endpoints | Bulk OUT `0x01` and bulk IN `0x81`, 64-byte max packet (full speed) |
| Windows | MS OS 2.0 descriptors so WinUSB binds automatically (no driver install) |
| Browser | WebUSB BOS descriptor |
| VID/PID | `0x303A`/`0x4020` for development (Espressif's VID, TinyUSB's vendor-only PID); apply to Espressif for a PID before release |
| Device class | `0xEF`/`0x02`/`0x01` (composite, IAD), so later interfaces need no change here |

The device is designed as a composite device from the start. The vendor interface is the only one in v1. Later modes add HID keyboard and/or USB MIDI interfaces alongside it.

### Control request: reset stream

| bmRequestType | bRequest | wValue | wIndex | wLength |
|---|---|---|---|---|
| `0x41` (vendor, host→device, interface) | `0x10` RESET_STREAM | 0 | interface number | 0 |

This brings the byte stream back to a known state:
- The device discards any partly received message and aborts any upload in progress.
- It drops queued events. Responses it has already queued may still arrive, as whole messages. The host ignores any response whose seq it isn't waiting for.
- Layout and images are **not** cleared.

The host library sends this every time it opens the device, then sends `GET_INFO`. It also sends it whenever it gets out of sync, such as after a timeout or a malformed message.

Codes `0x01` and `0x02` are left free for the WebUSB and MS OS 2.0 vendor codes. These are the codes TinyUSB's examples use.

## Message header (12 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u16 | magic | `0x4243` (`"CB"` as bytes `43 42`) |
| 2 | u8 | type | Message type (see tables below) |
| 3 | u8 | flags | bit 0 `RESPONSE`, bit 1 `EVENT`; other bits are 0 |
| 4 | u16 | seq | **Request:** chosen by the host. **Response:** copies the request's seq. **Event:** the device's own counter |
| 6 | u16 | reserved | 0 |
| 8 | u32 | length | Payload length in bytes, not counting the header |

If the device sees a bad magic or a length over `max_payload`, it treats the stream as corrupt. It drops everything it receives until the host sends RESET_STREAM.

### Responses

A response has the same `type` as its request, with the `RESPONSE` flag set. Its payload always starts with:

| Type | Field |
|---|---|
| u16 | status |
| u16 | reserved |

Any data specific to that response follows these two fields.

| Status | Name | Meaning |
|---|---|---|
| 0 | OK | |
| 1 | UNKNOWN_TYPE | Message type not supported |
| 2 | BAD_LENGTH | Payload too short or inconsistent |
| 3 | BAD_ARG | Argument out of range (page, key, ID 0…) |
| 4 | NO_SPACE | Image store full |
| 5 | BAD_IMAGE | Data failed rle565 validation, or isn't `key_w`×`key_h` |
| 6 | NOT_FOUND | Referenced image ID doesn't exist |
| 7 | — | Reserved (was IN_USE; freeing is never refused now) |
| 8 | BAD_SEQUENCE | Upload chunk out of order or not started, or a config commit before all the data arrived |
| 9 | BAD_CRC | Config upload: the data doesn't match the CRC-32 given at `CONFIG_BEGIN` |
| 10 | IO_ERROR | Config upload: a flash erase, write or read-back failed. `WIFI_SET`: the credentials couldn't be stored |

## Host → device requests

| Type | Name | Purpose |
|---|---|---|
| `0x00` | PING | Echoes the payload back (for latency and throughput tests) |
| `0x01` | GET_INFO | Device capabilities and geometry |
| `0x02` | RESET | Clear the layout and every image |
| `0x03` | SET_STATUS_TEXT | Set the status bar's left or right text |
| `0x04` | — | Reserved, not yet defined |
| `0x05` | SET_BACKLIGHT | Turn the backlight on or off |
| `0x10` | PUT_IMAGE | Upload or replace an image, in chunks |
| `0x11` | FREE_IMAGES | Free images (deferred while keys use them) |
| `0x12` | QUERY_IMAGES | Check which image IDs the device already has |
| `0x20` | SET_PAGE_COUNT | Set the number of pages |
| `0x21` | SET_KEYS | Define one or more keys (applied together) |
| `0x22` | SET_PAGE | Go to a page |
| `0x30` | CONFIG_BEGIN | Start uploading the standalone config (erases the stored one) |
| `0x31` | CONFIG_DATA | One chunk of the config |
| `0x32` | CONFIG_COMMIT | Finish: verify, save and restart the device |
| `0x40` | WIFI_SET | Store Wi-Fi credentials and connect; an empty ssid clears |
| `0x41` | WIFI_GET | Read the stored Wi-Fi credentials |
| `0x42` | WIFI_STATUS | Wi-Fi connection state, IP, signal |

### `0x00` PING

**Request and response:** Any bytes. The response echoes them after the status field.

### `0x01` GET_INFO

**Request:** No payload.

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u16 | protocol_version | `1` |
| u16 | flags | Bit 0: backlight is on (see [`backlight.md`](backlight.md)). Bit 1: Wi-Fi credentials are stored (see [`wifi.md`](wifi.md)). Other bits 0 |
| u32 | max_payload | Largest payload the device accepts, for every message type (8192 in v1) |
| u32 | image_capacity | Bytes available for images |
| u32 | image_used | Bytes in use, counting memory not yet released (retired or mid-replace images) |
| u32 | image_count | Live images |
| u32 | image_largest_free | Largest single allocation possible now |
| u16 | screen_w | 800 |
| u16 | screen_h | 480 |
| u8 | cols | 5 |
| u8 | rows | 3 |
| u8 | max_pages | |
| u8 | page_count | Current number of pages |
| u16 | key_w | Size of a key, and of every image |
| u16 | key_h | |
| u16 | icon_w | Area icons are designed to fill within a key image: 80 for now (still being tuned) |
| u16 | icon_h | |
| u8 | current_page | |
| u8 | fw_version_len | Followed by `fw_version` (UTF-8) |
| u8 | serial_len | Followed by `serial` (UTF-8). Lets the host tell several devices apart |
| u32 | config_capacity | Largest config the device can store, in bytes |
| u32 | config_size | Size of the stored config; `0` if there is none (or an upload is in progress) |
| u32 | config_crc | CRC-32 of the stored config; `0` if there is none |

`config_size` and `config_crc` let a host skip an upload when the device already has the same config. They describe what was committed; the CRC is not re-checked against the flash for each `GET_INFO`.

### `0x02` RESET

**Request:** No payload.

Clears all pages (the page count goes to 1 and every key becomes empty), abandons any upload in progress, frees every image, and clears both status bar texts.

### `0x03` SET_STATUS_TEXT

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | field | `0` left text, `1` right text |
| u8 | len | Bytes of text, 0..47 |
| u8[len] | text | UTF-8, no NUL |

Sets one of the two texts in the [status bar](status-bar.md) (left aligned and right aligned). The host decides what they say; the device has no clock. To change both, send two requests (they may be pipelined).

- **Empty text** (`len = 0`) clears the text.
- **Setting the current text** changes nothing on screen.
- **Fixed buffers:** each text is held in a 48-byte buffer on the device, so the limit is 47 bytes (not characters).
- **Rejected texts:** the text is not cut. The request fails and the bar keeps what it had. `BAD_LENGTH` if `len` runs past the payload; `BAD_ARG` for a `field` other than 0 or 1, more than 47 bytes, or text that isn't well-formed UTF-8 (bad or truncated sequences, overlongs, surrogates, code points above U+10FFFF, or a NUL).
- **Characters:** the current font covers ASCII 32–126 only, so send ASCII for now.
- **Lifetime:** the texts stay until changed, or until `RESET`. They're drawn by the device's status task, not at once: the new text appears within a frame or two of the response.

### `0x05` SET_BACKLIGHT

**Request:**

| Type | Field |
|---|---|
| u8 | on |

| Value | Meaning |
|---|---|
| `0` | Backlight off |
| non-zero | Backlight on |

The hardware backlight can only be switched on or off (through the CH422G); it has no dimming. Full behaviour is in [`backlight.md`](backlight.md).

- **Touch while off:** the device ignores touches for its own purposes (no key events, no swiping, no pressed look) and sends a [`WAKE`](#0x82-wake) event on each touch-down instead. It never turns the backlight on itself: the host does, with `SET_BACKLIGHT`.
- **Turning off mid-touch:** a key being held gets a `CANCEL`, and a swipe settles. The finger still down is swallowed and sends no `WAKE`.
- **Turning on mid-touch:** a touch that went down while dark stays swallowed until it lifts, so the wake touch never presses what appears under it.
- **Content keeps updating** while dark (`SET_KEYS`, `PUT_IMAGE`, `SET_STATUS_TEXT`, `SET_PAGE`).
- **Lifetime:** boots on. Only `SET_BACKLIGHT` changes it; `RESET`, `RESET_STREAM` and a host disconnect leave it alone.
- **Reading the state:** `GET_INFO` `flags` bit 0.
- Setting the current state does nothing. A missing `on` byte is `BAD_LENGTH`.

### `0x10` PUT_IMAGE

**Request:**

| Type | Field | Notes |
|---|---|---|
| u32 | id | Chosen by the host; not `0` |
| u32 | total_size | Size of the whole `.rle565` file |
| u32 | offset | Where this chunk goes in the file |
| … | data | Chunk bytes, up to `max_payload − 12` |

**Uploading an image:**
- **Order:** Chunks must arrive in order. The first chunk has `offset = 0`, and each later chunk starts where the previous one ended.
- **Starting:** At `offset = 0` the device allocates `total_size` bytes and returns `NO_SPACE` if it can't.
- **Finishing:** When `offset + len == total_size`, the device checks the image: the header, a size of exactly `key_w`×`key_h`, and a full bounds-checked decode. Bytes after the image (padding) are allowed. If it passes, the image becomes available. If it fails, it's discarded and the response is `BAD_IMAGE`.
- **Uploads in progress:** Only one upload can be in progress at a time. Starting a new one (`offset = 0`) abandons the old one. Any other message type may be interleaved between chunks.
- **ID already exists:** The new image **replaces** it. Every key using the ID shows the new image, in one frame per page. The old image stays until the new one has passed its checks, so a failed replace changes nothing. A replace needs room for both copies while it's in progress.

**Recommended ID:** CRC-32 (or the first 4 bytes of the SHA-256) of the encoded file, with 0 mapped to 1. The same image then always has the same ID, across sessions and across apps. The host should check its own images for colliding IDs, because a collision replaces an image.

### `0x11` FREE_IMAGES

**Request:**

| Type | Field |
|---|---|
| u16 | count |
| u32[count] | ids |

Always `OK`. Each ID is gone at once: `QUERY_IMAGES` no longer reports it and new key definitions can't use it. Keys that already use it keep showing it, and its memory is released once no key uses it. Unknown IDs are ignored. See [image-assets.md, Freeing](image-assets.md#freeing).

### `0x12` QUERY_IMAGES

**Request:**

| Type | Field |
|---|---|
| u16 | count |
| u32[count] | ids |

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u16 | count | |
| u8[ceil(count/8)] | present | Bitmap. Bit `i` (LSB first) is set if `ids[i]` is available (fully uploaded and not freed) |

### `0x20` SET_PAGE_COUNT

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | count | 1..`max_pages` |

Keys on pages that are removed are cleared. If the current page no longer exists, the device moves to the last remaining page.

### `0x21` SET_KEYS

**Request:**

| Type | Field |
|---|---|
| u16 | count |
| KeyDef[count] | keys |

Each `KeyDef`:

| Type | Field | Notes |
|---|---|---|
| u8 | page | |
| u8 | key | 0..`cols*rows−1`, numbered across each row, left to right, top to bottom |
| u16 | flags | See below |
| u32 | bg_id | Normal background. `0` = none |
| u32 | fg_id | Normal foreground. `0` = none |
| u32 | pressed_bg_id | Background while pressed |
| u32 | pressed_fg_id | Foreground while pressed. If both pressed IDs are `0`, the normal pair is shown while pressed (no feedback) |

**Flags:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | `EMPTY` | Key is blank and inactive. The image IDs must be `0` |
| 1 | `DISABLED` | Draw the key but send no events and show no press feedback |

**How keys are drawn:**
- The background, then the foreground, each `key_w`×`key_h` at the key's origin, with alpha. Both come from the pressed pair while the key is held.
- **Typical use:** the background is a button face (shared by many keys, with a pressed version) and the foreground is the icon with its label baked in. Icons are designed to fill about `icon_w`×`icon_h`, centred, with transparent padding. rle565 skips transparent rows and runs, so the padding costs next to nothing.
- **Full custom art:** put it all in the background and leave the foreground `0`.

**Applying the update:**
- The whole message is applied in one go: the device validates every entry first. If any entry is invalid, nothing changes and the response is `BAD_ARG` or `NOT_FOUND`.
- Every image it references must already be uploaded.
- Only the affected page buffers are redrawn, and the new content appears on screen in one frame, so a half-applied update is never shown.

### `0x22` SET_PAGE

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | page | |
| u8 | flags | bit 0 `ANIMATE`: glide there as if the user had swiped |

## Configuration upload

Saves the standalone-mode config (a BPAK blob from `tools/bake`, see [`standalone.md`](standalone.md)) in the device's `config` flash partition. The device doesn't look inside the blob here: it checks the size and a CRC-32 and stores the bytes. The CRC-32 is the usual zlib one (polynomial `0xEDB88320`, initial and final XOR `0xFFFFFFFF`; the CRC of `"123456789"` is `0xCBF43926`).

```
CONFIG_BEGIN(size, crc)  →  CONFIG_DATA(0, …) … CONFIG_DATA(n, …)  →  CONFIG_COMMIT  →  device restarts
```

### `0x30` CONFIG_BEGIN

**Request:**

| Type | Field | Notes |
|---|---|---|
| u32 | total_size | Size of the blob in bytes, at least 1 and at most `config_capacity` |
| u32 | crc32 | CRC-32 of the whole blob |

- Starts a new upload, abandoning one in progress.
- **The stored config is destroyed** as soon as the upload starts (the response is `OK`), even if the upload later fails: the device then has no config and boots to an empty Home.
- `BAD_LENGTH` if the payload is shorter than 8 bytes. `BAD_ARG` if `total_size` is 0, `NO_SPACE` if it is more than `config_capacity`; in both cases nothing changes. `IO_ERROR` if the flash can't be erased.
- **The screen goes dark** (backlight off) until the upload ends. Flash erases and writes stall the display, so it is blanked rather than glitching. A failed upload, `RESET_STREAM` or a new `BEGIN` that fails restores the backlight to the host's setting ([`SET_BACKLIGHT`](#0x05-set_backlight)).

### `0x31` CONFIG_DATA

**Request:**

| Type | Field | Notes |
|---|---|---|
| u32 | offset | Where this chunk goes in the blob |
| … | data | Chunk bytes, at least 1, up to `max_payload − 4` |

- **Order:** the first chunk has `offset = 0` and each later chunk starts where the previous one ended (like [`PUT_IMAGE`](#0x10-put_image)). Chunks may be pipelined; other message types may be interleaved.
- **Errors end the upload:** `BAD_SEQUENCE` (no upload in progress, or the offset isn't the next one), `BAD_ARG` (the chunk runs past `total_size`) or `IO_ERROR`. The stored config stays destroyed and every later `CONFIG_DATA` gets `BAD_SEQUENCE` until a new `CONFIG_BEGIN`. `BAD_LENGTH` (no data) changes nothing.
- A chunk's response arrives once it is in flash. A chunk can take tens of milliseconds (erases), so a host shouldn't have unlimited chunks in flight with a fixed timeout; the `host/` library keeps a small window.

### `0x32` CONFIG_COMMIT

**Request:** No payload.

- `BAD_SEQUENCE` if no upload is in progress, or if fewer than `total_size` bytes have arrived (the upload carries on).
- Otherwise the device checks the CRC-32 of the bytes it received (`BAD_CRC` if wrong), re-reads the flash and checks it again (`IO_ERROR` if wrong), then writes the config's header. Any failure ends the upload with the config destroyed, as above.
- On `OK` the config is saved and **the device restarts** right after sending the response. The host sees its USB connection drop and should wait for the device to come back (about a second or two), then read `config_size` / `config_crc` from `GET_INFO` to confirm.
- Bytes sent after `CONFIG_COMMIT` in the same stream are ignored.

**Other messages and the upload:** `RESET_STREAM` abandons the upload (the config stays destroyed). `RESET` doesn't touch it. Everything else works as usual during an upload, but the screen is dark.

## Wi-Fi

The host sets the Wi-Fi credentials; the device keeps them in NVS and connects in the background. Design, storage and the connection state machine: [`wifi.md`](wifi.md). These messages work in every mode, and `RESET`, `RESET_STREAM`, taking control and releasing leave the credentials and the connection alone.

### `0x40` WIFI_SET

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | ssid_len | 0..32 |
| u8 | pass_len | 0, or 8..63 |
| u8[ssid_len] | ssid | Raw bytes (an SSID need not be UTF-8) |
| u8[pass_len] | pass | Raw bytes. `pass_len` 0 = an open network |

- `BAD_LENGTH` if the payload is shorter than the two lengths say (checked first). Trailing bytes are ignored.
- `BAD_ARG` if `ssid_len` is over 32, `pass_len` is 1..7 or over 63, `ssid_len` is 0 with a non-zero `pass_len`, or either string contains a NUL byte. Nothing changes.
- `ssid_len` 0 (with `pass_len` 0) **clears** the credentials: they are erased, the device disconnects and Wi-Fi stops.
- `IO_ERROR` if the NVS write fails. The previously stored credentials are still in place.
- Setting exactly the credentials already stored does nothing (no flash write, no reconnect) and responds `OK`.
- On `OK` the credentials are stored. The connection attempt starts after the response and does not hold it up: poll `WIFI_STATUS` or watch for `WIFI_STATE` events. A 64-character hex pre-shared key is not accepted.

### `0x41` WIFI_GET

**Request:** No payload (any payload is ignored).

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u8 | ssid_len | 0 if none are stored |
| u8 | pass_len | |
| u8[ssid_len] | ssid | |
| u8[pass_len] | pass | In clear text (see [`wifi.md`](wifi.md#security)) |

### `0x42` WIFI_STATUS

**Request:** No payload (any payload is ignored).

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u8 | state | See below |
| u8 | reason | The last disconnect reason (ESP-IDF `wifi_err_reason_t`), 0 if none. For diagnostics only |
| i8 | rssi | dBm while connected, else 0 |
| u8 | reserved | 0 |
| u8[4] | ip | `a.b.c.d` in that order while `CONNECTED`, else 0 |
| u8[6] | mac | The station MAC. All zeros in `NO_CREDENTIALS` (the driver isn't running then) |

| State | Name | Meaning |
|---|---|---|
| 0 | `NO_CREDENTIALS` | Nothing stored; Wi-Fi is not running |
| 1 | `CONNECTING` | The first attempt: associating or waiting for an IP |
| 2 | `CONNECTED` | Has an IP |
| 3 | `RETRYING` | An attempt failed or the link dropped. It stays `RETRYING` across the retries (1 s, doubling to 30 s; at once to 30 s after an authentication failure) until it connects |

## Device → host events

Events have the `EVENT` flag set, and `seq` is the device's event counter.

**If the host isn't reading:**
- The device queues up to 64 events. It sends an event only when there's room for it in the USB transmit buffer, so a host that isn't reading never holds up the device.
- When the queue overflows, it drops the oldest events and sets bit 0 of `lost` on the next event it sends.
- In that case the host should treat key state as unknown, and treat any key it believes is held as released.

| Type | Name |
|---|---|
| `0x80` | KEY |
| `0x81` | PAGE_CHANGED |
| `0x82` | WAKE |
| `0x83` | HEARTBEAT |
| `0x84` | WIFI_STATE |

### `0x80` KEY

| Type | Field | Notes |
|---|---|---|
| u8 | page | |
| u8 | key | |
| u8 | action | `0` press, `1` release, `2` cancel |
| u8 | lost | Event-overflow flag (see above) |
| u32 | time_ms | Device milliseconds since boot. For working out hold times |

**What each action means:**
See [`interactions.md`](interactions.md) for exactly when these fire.
- **Press** is sent when the short swipe window after touch-down ends with the finger still on the key (or at lift, for a quick tap). A touch that becomes a swipe sends no key events.
- **Release** is sent when the finger lifts while still on the key.
- **Cancel** is sent instead of release if the finger drags out of the key after press. Don't trigger release actions.
- Every press is followed by exactly one release or cancel.

**Host guidance:**
- Press is safe to act on: it only fires once the touch can no longer become a swipe. Use **release** for actions that should only run on a completed click, and ignore **cancel**.
- **Long press and double press are detected by the host, not the device.** The host uses `time_ms` from the press/release events (press is delayed by the swipe window, so measure from press, not touch-down); the `host/` library should provide helpers for this.

### `0x81` PAGE_CHANGED

| Type | Field | Notes |
|---|---|---|
| u8 | page | New current page |
| u8 | cause | `0` the user swiped or tapped the pager, `1` the host sent SET_PAGE |

This is sent once the page settles, not while a swipe is in progress.

### `0x82` WAKE

| Type | Field | Notes |
|---|---|---|
| u8 | lost | Event-overflow flag (see above) |
| u8[3] | reserved | 0 |
| u32 | time_ms | Device milliseconds since boot |

The screen was touched while the backlight is off (see [`SET_BACKLIGHT`](#0x05-set_backlight) and [`backlight.md`](backlight.md)).
- One `WAKE` per touch-down of the first finger. Extra fingers send nothing.
- No `KEY` or `PAGE_CHANGED` events come from that touch: it is swallowed until the finger lifts.
- The device stays dark until the host sends `SET_BACKLIGHT`. A host that doesn't want to wake can ignore the event.
- Queued events (such as the `CANCEL` for a key held when the light went off) arrive before it.

### `0x83` HEARTBEAT

| Type | Field | Notes |
|---|---|---|
| u32 | time_ms | Device milliseconds since boot |

Sent about once a second (`PROTO_HEARTBEAT_MS`), whether or not anything else is happening. **It keeps the host's idle bulk IN read from ever timing out.**

- **Why:** on Windows (WinUSB, `usb` 3.x) a bulk IN read that times out is cancelled, and a cancelled read leaves the pipe deaf: the device still receives requests, but nothing it sends reaches the host until the device is reopened. Neither `clearHalt` nor `RESET_STREAM` recovers it. A host whose reads time out every few hundred milliseconds hits this within seconds of idling.
- **Host rule:** set the read timeout well past the interval (the `host/` library uses 5 s), so a read always completes first. The heartbeat is also why `close()` on an idle connection can take up to a second: a pending read can't be cancelled, so it waits for the next heartbeat. A host that doesn't need this (WebUSB, which can abort reads) may ignore the event.
- **Not queued:** it is written straight out, not through the event queue, so it can never overflow it or set a `lost` flag. It shares the events' `seq` counter.
- **Only into free room:** it is sent only while at least half of the transmit FIFO is free (`PROTO_HEARTBEAT_ROOM`, 1024 of 2048 bytes). With no app reading, the unread ones stop at half full, so responses and key events are never crowded out; they are drained and ignored when a host connects.
- Older firmware doesn't send it. A host that sets a long read timeout needs this firmware.

### `0x84` WIFI_STATE

| Type | Field | Notes |
|---|---|---|
| u8 | state | As in `WIFI_STATUS` |
| u8 | lost | Event-overflow flag (see above) |
| u8[2] | reserved | 0 |
| u8[4] | ip | As in `WIFI_STATUS` |

Sent when the state or the address changes (not on every retry), so a host can show "connected" without polling. A host that doesn't care ignores it.

## Typical session

Uploading a standalone config instead: `RESET_STREAM`, `GET_INFO` (skip the rest if `config_size` and `config_crc` already match), `CONFIG_BEGIN`, pipelined `CONFIG_DATA`, `CONFIG_COMMIT`, then wait for the device to restart.

And for a host-controlled session:

1. Open the device and send RESET_STREAM (control request).
2. Send `GET_INFO`: check `protocol_version` and read the geometry.
3. Render the faces and icons (with labels), encode them with the rle565 encoder and compute their IDs (CRC-32). Send `QUERY_IMAGES`, then `PUT_IMAGE` only for the missing ones, pipelined.
4. Send `SET_PAGE_COUNT`, then one `SET_KEYS` with every key. Set the status bar texts with `SET_STATUS_TEXT`, and again whenever they change.
5. Handle `KEY` events. To sleep the screen, send `SET_BACKLIGHT` off; on a `WAKE` event, send it on. Later changes are `SET_KEYS` with the affected keys (plus `PUT_IMAGE` for any new images), or a `PUT_IMAGE` that replaces an image in place. `FREE_IMAGES` images that are no longer wanted.

## Device state lifetime

- Layout and images are kept in PSRAM while the device is powered. A host app that restarts can reconnect, query what the device has, and avoid uploading it again.
- Layout and images are never saved to flash. Only the standalone config is ([configuration upload](#configuration-upload)); the standalone mode that runs it is in [`standalone.md`](standalone.md).
- The device can't tell when the host app closes, because there's no signal for it. The host can explicitly `RESET` before it exits if it wants the screen cleared.

## Open questions

- Status bar: the left and right texts are host-settable (`SET_STATUS_TEXT`). Still open: whether the device gets a clock, other status bar content, and the pager bar (disabled for now). Message type `0x04` is reserved for them.
- Icon size: Somewhere from 80 to 96 px, to be decided by trying it on the hardware. The device reports the sizes in `GET_INFO`, so hosts shouldn't hard-code them.
