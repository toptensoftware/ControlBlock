// Macro pad wire protocol: the payloads carried in PacketEncoder packets from the
// pad MCU (ESP32-C3) to the S3, one way. Pure C++ with no platform code, shared by
// the C3 firmware, the S3 firmware and the host tests. See wip/wip-macro-pad.md
// ("Final design") and pad/common/pad_golden.md (golden wire bytes).
//
// All multi-byte payload fields are little-endian.
//   PAD_CMD_STATE  payload: u32 keys (bit n = key n+1 down), then per encoder
//                  u32 position (detents, absolute, wraps) and u16 interval
//                  (microseconds, saturating); length = 4 + 6 * encoders.
//   PAD_CMD_HELLO  payload: u8 version, u8 key_count, u8 encoder_count.
#pragma once

#include <stdint.h>
#include <stddef.h>

constexpr int PAD_MAX_KEYS     = 32;
constexpr int PAD_MAX_ENCODERS = 4;

constexpr uint32_t PAD_CMD_STATE = 1;
constexpr uint32_t PAD_CMD_HELLO = 2;

constexpr uint8_t PAD_PROTOCOL_VERSION = 1;

// Timing (starting points, tune on the hardware)
constexpr uint32_t PAD_STATE_REPEAT_MS     = 100;   // C3: resend the current STATE
constexpr uint32_t PAD_OFFLINE_TIMEOUT_MS  = 300;   // S3: no valid STATE for this long = pad offline
constexpr uint32_t PAD_ENCODER_MIN_GAP_MS  = 5;     // C3: at most one encoder-only STATE per this

constexpr int PAD_HELLO_COUNT  = 3;                 // C3: HELLOs sent at boot
constexpr uint32_t PAD_HELLO_GAP_MS = 50;

// Payload sizes
constexpr size_t PAD_STATE_BASE_BYTES    = 4;
constexpr size_t PAD_STATE_ENCODER_BYTES = 6;
constexpr size_t PAD_STATE_MAX_BYTES     = PAD_STATE_BASE_BYTES + PAD_STATE_ENCODER_BYTES * PAD_MAX_ENCODERS;
constexpr size_t PAD_HELLO_BYTES         = 3;

// The decoder's payload buffer must be at least this big
constexpr size_t PAD_PAYLOAD_MAX_BYTES   = PAD_STATE_MAX_BYTES;

struct PadState
{
    uint32_t keys;                          // bit n = key n+1 down
    uint8_t  encoder_count;                 // 0..PAD_MAX_ENCODERS
    uint32_t position[PAD_MAX_ENCODERS];    // detents, absolute
    uint16_t interval[PAD_MAX_ENCODERS];    // microseconds per edge (average of the last 4), saturated
};

struct PadHello
{
    uint8_t version;
    uint8_t key_count;
    uint8_t encoder_count;
};

// Payload size of a STATE with this many encoders
constexpr size_t PadStateBytes(int encoders)
{
    return PAD_STATE_BASE_BYTES + PAD_STATE_ENCODER_BYTES * (size_t)encoders;
}

// Writes the payload; returns its length, or 0 if encoder_count is out of range or cap is too small
size_t PadBuildState(const PadState &state, uint8_t *out, size_t cap);

// Parses a payload; false (and 'out' unchanged) for any length other than 4 + 6n, n = 0..4
bool PadParseState(const void *payload, size_t len, PadState &out);

size_t PadBuildHello(const PadHello &hello, uint8_t *out, size_t cap);
bool PadParseHello(const void *payload, size_t len, PadHello &out);
