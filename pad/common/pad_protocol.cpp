#include "pad_protocol.h"

namespace
{
    void Put16(uint8_t *p, uint16_t v)
    {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }

    void Put32(uint8_t *p, uint32_t v)
    {
        Put16(p, (uint16_t)v);
        Put16(p + 2, (uint16_t)(v >> 16));
    }

    uint16_t Get16(const uint8_t *p)
    {
        return (uint16_t)(p[0] | (p[1] << 8));
    }

    uint32_t Get32(const uint8_t *p)
    {
        return (uint32_t)Get16(p) | ((uint32_t)Get16(p + 2) << 16);
    }
}

size_t PadBuildState(const PadState &state, uint8_t *out, size_t cap)
{
    if (state.encoder_count > PAD_MAX_ENCODERS)
        return 0;
    size_t len = PadStateBytes(state.encoder_count);
    if (cap < len)
        return 0;
    Put32(out, state.keys);
    for (int i = 0; i < state.encoder_count; i++) {
        uint8_t *p = out + PAD_STATE_BASE_BYTES + PAD_STATE_ENCODER_BYTES * i;
        Put32(p, state.position[i]);
        Put16(p + 4, state.interval[i]);
    }
    return len;
}

bool PadParseState(const void *payload, size_t len, PadState &out)
{
    if (len < PAD_STATE_BASE_BYTES || (len - PAD_STATE_BASE_BYTES) % PAD_STATE_ENCODER_BYTES != 0)
        return false;
    size_t n = (len - PAD_STATE_BASE_BYTES) / PAD_STATE_ENCODER_BYTES;
    if (n > PAD_MAX_ENCODERS || !payload)
        return false;
    const uint8_t *in = (const uint8_t *)payload;
    PadState s = {};
    s.keys = Get32(in);
    s.encoder_count = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = in + PAD_STATE_BASE_BYTES + PAD_STATE_ENCODER_BYTES * i;
        s.position[i] = Get32(p);
        s.interval[i] = Get16(p + 4);
    }
    out = s;
    return true;
}

size_t PadBuildHello(const PadHello &hello, uint8_t *out, size_t cap)
{
    if (cap < PAD_HELLO_BYTES)
        return 0;
    out[0] = hello.version;
    out[1] = hello.key_count;
    out[2] = hello.encoder_count;
    return PAD_HELLO_BYTES;
}

bool PadParseHello(const void *payload, size_t len, PadHello &out)
{
    if (len != PAD_HELLO_BYTES || !payload)
        return false;
    const uint8_t *in = (const uint8_t *)payload;
    out.version = in[0];
    out.key_count = in[1];
    out.encoder_count = in[2];
    return true;
}
