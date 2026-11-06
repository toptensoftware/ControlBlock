// Pin map for the macro pad (ESP32-C3, zero-style board). PLACEHOLDER: adjust it to your
// wiring. All inputs use the internal pull-up and are active low (a switch to ground).
//
// ESP32-C3 pins to avoid:
//   GPIO 2, 8, 9   strapping pins (read at reset; a key held at boot can stop it booting).
//                  8 and 9 also feed the boot-mode / download logic: leave them alone.
//   GPIO 18, 19    native USB (D-, D+): the board's USB port.
//   GPIO 20, 21    UART0 RX / TX: the log console (and the ROM boot log).
//   GPIO 11..17    the module's SPI flash on most modules.
// Free for inputs: 0, 1, 3, 4, 5, 6, 7, 10 (eight pins).
//
// The default map needs nine pins for 6 keys, 1 encoder and the link, so ONE exception is
// made: the link TX is on GPIO 2. It idles high (what the strapping pin wants) and drives
// nothing before the UART starts. If your wiring makes that a problem, move the link to
// another pin or drop a key. Keep the link TX off the UART0 pins (20/21) or the ROM boot
// log would feed garbage to the S3.
#pragma once

#include <stdint.h>
#include "pad_protocol.h"

// Key n (0-based, reported as key{n+1}) is on PAD_KEY_PINS[n]. An encoder's push button is
// an ordinary key: put its pin in this list. At most PAD_MAX_KEYS (32) entries.
constexpr int8_t PAD_KEY_PINS[] = { 0, 1, 3, 4, 5, 6 };

// Encoder e (reported as enc{e+1}): {A pin, B pin}. At most PAD_MAX_ENCODERS (4) entries.
// At least one entry (a zero-length array is not valid C++); for a pad without an encoder,
// leave the pins unwired: it would only add a harmless idle encoder.
constexpr int8_t PAD_ENCODER_PINS[][2] = { { 7, 10 } };

// Link: UART1, TX only (to the S3's GPIO 6), 115200 8N1.
constexpr int PAD_LINK_TX_GPIO = 2;
constexpr int PAD_LINK_BAUD = 115200;

constexpr int PAD_KEY_COUNT = (int)(sizeof(PAD_KEY_PINS) / sizeof(PAD_KEY_PINS[0]));
constexpr int PAD_ENCODER_COUNT = (int)(sizeof(PAD_ENCODER_PINS) / sizeof(PAD_ENCODER_PINS[0]));

static_assert(PAD_KEY_COUNT <= PAD_MAX_KEYS, "too many keys for the protocol");
static_assert(PAD_ENCODER_COUNT <= PAD_MAX_ENCODERS, "too many encoders for the protocol");
