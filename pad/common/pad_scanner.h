// The macro pad's C3 side as pure code: debounces up to 32 keys, decodes up to 4 rotary
// encoders like urem, and writes STATE / HELLO packets (pad_protocol + PacketEncoder)
// to a byte sink. Injected time (microseconds, 64-bit); no platform code. The C3
// firmware wraps it with GPIO reads/ISRs and a UART writer.
//
// Inputs (call from the device layer, with the time of the event):
//   SetKeyLevels(now, mask)       raw key levels, bit n = key n+1 pin HIGH (pull-ups: a
//                                 pressed key reads LOW, so a pressed key is a 0 bit).
//   SetEncoderPins(now, e, a, b)  raw pin levels of encoder e, on every pin edge.
//   Poll(now)                     at least every ~1 ms: filter timers and send policy.
//
// Output: packets written byte by byte to the sink.
//   The sink is a bare function pointer with no context argument because
//   packet_encode() (PacketEncoder, C) takes exactly that. The device layer therefore
//   keeps a file-static TX buffer (as aclock's audio.c does) and the sink appends to it;
//   tests use a file-static buffer too. One scanner per process is the consequence;
//   there is only ever one.
//
// Send policy:
//   - Start: HELLO x PAD_HELLO_COUNT, PAD_HELLO_GAP_MS apart (the first at Init). No STATE
//     goes out until the last HELLO is sent (a HELLO after a STATE would clear the S3's
//     baseline again and could swallow a press); then a STATE goes out at once.
//   - Key change (a debounced edge): STATE immediately.
//   - Encoder change only: at most one STATE per PAD_ENCODER_MIN_GAP_MS (positions are
//     absolute, so nothing is lost). The gap is measured from the last STATE of any kind.
//   - Otherwise a repeat of the current STATE PAD_STATE_REPEAT_MS after the last one.
//   - seq: free running, one per packet (HELLO and STATE share it), starting at 0.
//
// STATE contents: keys = debounced pressed bits (1 = down); per encoder the detent
// position (u32, wraps) and the velocity interval handed over with its latest tick
// (QuadIntervalToU16, so 65535 = 65 ms or slower; 1 s until the first tick).
#pragma once

#include <stdint.h>
#include "pad_protocol.h"
#include "encoder_channel.h"

constexpr uint32_t PAD_KEY_DEBOUNCE_US = 5000;         // urem: 5 ms on the buttons

struct PadScannerConfig
{
    uint8_t key_count;          // 0..PAD_MAX_KEYS
    uint8_t encoder_count;      // 0..PAD_MAX_ENCODERS
};

class PadScanner
{
public:
    typedef void (*ByteSink)(uint8_t byte);

    // key_levels: the initial raw levels (bit set = high = released); a key already down
    // at start is simply reported down. enc_pins: per encoder A << 1 | B, or null for
    // all idle (both high).
    void Init(const PadScannerConfig &config, ByteSink sink, uint64_t now_us,
              uint32_t key_levels, const uint8_t *enc_pins = nullptr);

    void SetKeyLevels(uint64_t now_us, uint32_t key_levels);
    void SetEncoderPins(uint64_t now_us, int encoder, bool a, bool b);
    void Poll(uint64_t now_us);

    // For tests and stats
    uint32_t NextSeq() const { return m_seq; }
    uint32_t PressedKeys() const { return m_pressed; }
    uint32_t Detents(int encoder) const { return m_enc[encoder].Detents(); }

private:
    void Service(uint64_t now_us);
    void SendState(uint64_t now_us);
    void SendHello();

    PadScannerConfig m_cfg = {};
    ByteSink m_sink = nullptr;

    GlitchFilter   m_key[PAD_MAX_KEYS];
    EncoderChannel m_enc[PAD_MAX_ENCODERS];
    uint32_t m_key_levels = 0;                      // raw
    bool     m_pin_a[PAD_MAX_ENCODERS] = {};        // raw
    bool     m_pin_b[PAD_MAX_ENCODERS] = {};
    uint32_t m_pressed = 0;                         // debounced, bit n = key n+1 down

    uint32_t m_seq = 0;
    int      m_hello_sent = 0;
    uint64_t m_next_hello = 0;
    bool     m_state_sent = false;                  // the first STATE has gone out
    uint64_t m_last_state = 0;
    bool     m_keys_dirty = false;                  // a key changed since the last STATE
    bool     m_enc_dirty = false;                   // an encoder moved since the last STATE
};
