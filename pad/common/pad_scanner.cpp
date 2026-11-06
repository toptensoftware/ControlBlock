#include "pad_scanner.h"
#include "packenc.h"

namespace
{
    constexpr uint64_t MS = 1000;

    uint32_t KeyMask(int count)
    {
        return count >= 32 ? 0xFFFFFFFFu : ((1u << count) - 1);
    }
}

void PadScanner::Init(const PadScannerConfig &config, ByteSink sink, uint64_t now_us,
                      uint32_t key_levels, const uint8_t *enc_pins)
{
    m_cfg = config;
    if (m_cfg.key_count > PAD_MAX_KEYS)
        m_cfg.key_count = PAD_MAX_KEYS;
    if (m_cfg.encoder_count > PAD_MAX_ENCODERS)
        m_cfg.encoder_count = PAD_MAX_ENCODERS;
    m_sink = sink;

    m_key_levels = key_levels & KeyMask(m_cfg.key_count);
    m_pressed = 0;
    for (int k = 0; k < m_cfg.key_count; k++) {
        bool high = (key_levels >> k) & 1;
        m_key[k].Init(PAD_KEY_DEBOUNCE_US, high);
        if (!high)
            m_pressed |= 1u << k;
    }
    for (int e = 0; e < m_cfg.encoder_count; e++) {
        uint8_t p = enc_pins ? enc_pins[e] : 3;
        m_pin_a[e] = (p & 2) != 0;
        m_pin_b[e] = (p & 1) != 0;
        m_enc[e].Init(PAD_ENCODER_FILTER_US, m_pin_a[e], m_pin_b[e]);
    }

    m_seq = 0;
    m_hello_sent = 0;
    m_next_hello = now_us;
    m_state_sent = false;
    m_last_state = now_us;
    m_keys_dirty = false;
    m_enc_dirty = false;
    Service(now_us);        // the first HELLO
}

void PadScanner::SetKeyLevels(uint64_t now_us, uint32_t key_levels)
{
    m_key_levels = key_levels;
    Service(now_us);
}

void PadScanner::SetEncoderPins(uint64_t now_us, int encoder, bool a, bool b)
{
    if (encoder < 0 || encoder >= m_cfg.encoder_count)
        return;
    m_pin_a[encoder] = a;
    m_pin_b[encoder] = b;
    Service(now_us);
}

void PadScanner::Poll(uint64_t now_us)
{
    Service(now_us);
}

void PadScanner::Service(uint64_t now_us)
{
    // HELLOs first: one per call at most, so they are always PAD_HELLO_GAP_MS apart
    if (m_hello_sent < PAD_HELLO_COUNT && now_us >= m_next_hello) {
        SendHello();
        m_hello_sent++;
        m_next_hello = now_us + PAD_HELLO_GAP_MS * MS;
    }

    // Debounce the keys
    for (int k = 0; k < m_cfg.key_count; k++) {
        if (m_key[k].Update(now_us, (m_key_levels >> k) & 1)) {
            if (m_key[k].Level())
                m_pressed &= ~(1u << k);
            else
                m_pressed |= 1u << k;
            m_keys_dirty = true;
        }
    }

    // Decode the encoders
    for (int e = 0; e < m_cfg.encoder_count; e++) {
        QuadTick ticks[2];
        if (m_enc[e].Update(now_us, m_pin_a[e], m_pin_b[e], ticks) > 0)
            m_enc_dirty = true;
    }

    if (m_hello_sent < PAD_HELLO_COUNT)
        return;

    if (!m_state_sent || m_keys_dirty
        || (m_enc_dirty && now_us - m_last_state >= PAD_ENCODER_MIN_GAP_MS * MS)
        || now_us - m_last_state >= PAD_STATE_REPEAT_MS * MS)
        SendState(now_us);
}

void PadScanner::SendState(uint64_t now_us)
{
    PadState s = {};
    s.keys = m_pressed;
    s.encoder_count = m_cfg.encoder_count;
    for (int e = 0; e < m_cfg.encoder_count; e++) {
        s.position[e] = m_enc[e].Detents();
        s.interval[e] = QuadIntervalToU16(m_enc[e].LastTickIntervalUs());
    }

    uint8_t payload[PAD_STATE_MAX_BYTES];
    size_t n = PadBuildState(s, payload, sizeof(payload));
    packet_encode(m_sink, m_seq++, PAD_CMD_STATE, payload, (uint32_t)n);

    m_state_sent = true;
    m_last_state = now_us;
    m_keys_dirty = false;
    m_enc_dirty = false;
}

void PadScanner::SendHello()
{
    PadHello h = {};
    h.version = PAD_PROTOCOL_VERSION;
    h.key_count = m_cfg.key_count;
    h.encoder_count = m_cfg.encoder_count;

    uint8_t payload[PAD_HELLO_BYTES];
    size_t n = PadBuildHello(h, payload, sizeof(payload));
    packet_encode(m_sink, m_seq++, PAD_CMD_HELLO, payload, (uint32_t)n);
}
