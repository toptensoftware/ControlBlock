#pragma once

// Native USB (esp_tinyusb): a composite device. In v1 it has one interface,
// the vendor interface the host protocol runs on (docs/protocol.md): bulk
// OUT 0x01 / IN 0x81, WinUSB through MS OS 2.0 descriptors and WebUSB
// through a BOS descriptor, so no driver install. Modes 2 and 3 add HID
// keyboard and MIDI interfaces (see the interface map in usb.cpp).

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    // Bytes arrived on bulk OUT. Called on the TinyUSB task: just wake the
    // reader.
    void (*rx)(void);
    // The host sent RESET_STREAM. The RX FIFO has been flushed and the
    // stream epoch moved on (see usb_vendor_read). TinyUSB task.
    void (*reset_stream)(void);
    // The host went away: the device was unplugged or unmounted, or the bus
    // suspended. TinyUSB task: just wake the reader.
    void (*disconnected)(void);
} usb_callbacks_t;

// Install TinyUSB (its task on core 0) with our descriptors. `serial` is the
// USB serial number string (kept, must stay valid).
esp_err_t usb_init(const char *serial, const usb_callbacks_t *cb);

// Read up to `len` bytes from bulk OUT. `*epoch` is the stream epoch they
// belong to: it moves on at each RESET_STREAM, and bytes read under a new
// epoch all arrived after it. Returns 0 if nothing is waiting.
size_t usb_vendor_read(void *buf, size_t len, uint32_t *epoch);

// Queue bytes for bulk IN, waiting for room. Gives up (false) if the host
// isn't connected or doesn't read for a while.
bool usb_vendor_write(const void *data, size_t len);

// Start sending whatever is queued
void usb_vendor_flush(void);

// Bytes usb_vendor_write() can take now without waiting (0 if not connected)
size_t usb_vendor_write_space(void);

// Send one HID keyboard report (HID_KB_REPORT_BYTES, hid_keyboard.h), waiting
// a few ms if the last one is still in flight. False if not mounted, the
// wait ran out, or the build has no HID interface.
bool usb_hid_send(const uint8_t *report);
