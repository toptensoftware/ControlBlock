// Macro pad wire protocol (pad/common/pad_protocol.h) and its transport, the real
// PacketEncoder (framing, stuffing, CRC-32): payload build/parse, golden wire bytes,
// round trips, noise and interrupted packets. The invariant: nothing corrupt is ever
// delivered.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "pad_protocol.h"
#include "packenc.h"

using namespace SimpleLib;

namespace
{
    // ---- Wire helpers -------------------------------------------------------

    constexpr int WIRE_MAX = 4096;

    // packet_encode takes a plain function pointer, so the output goes to a static
    uint8_t g_wire[WIRE_MAX];
    int     g_wire_len = 0;

    void WireByte(uint8_t b)
    {
        Assert(g_wire_len < WIRE_MAX);
        g_wire[g_wire_len++] = b;
    }

    // Appends one packet to g_wire
    void Encode(uint32_t seq, uint32_t cmd, const void *payload, size_t len)
    {
        packet_encode(WireByte, seq, cmd, payload, (uint32_t)len);
    }

    // What the decoder delivered (and the errors it counted)
    struct Delivered
    {
        uint32_t seq, cmd;
        uint8_t  payload[PAD_PAYLOAD_MAX_BYTES];
        size_t   len;
    };

    constexpr int MAX_DELIVERED = 512;
    Delivered g_got[MAX_DELIVERED];
    int       g_got_count = 0;
    int       g_errors[8];
    int       g_error_total = 0;

    void OnPacket(uint32_t seq, uint32_t cmd, const void *data, uint32_t len)
    {
        Assert(g_got_count < MAX_DELIVERED);
        Assert(len <= PAD_PAYLOAD_MAX_BYTES);
        Delivered &d = g_got[g_got_count++];
        d.seq = seq;
        d.cmd = cmd;
        d.len = len;
        if (len)
            memcpy(d.payload, data, len);
    }

    void OnError(int code)
    {
        Assert(code >= 0 && code < 8);
        g_errors[code]++;
        g_error_total++;
    }

    // A decoder with the payload buffer the device uses
    struct Rx
    {
        uint8_t        buf[PAD_PAYLOAD_MAX_BYTES];
        decode_context ctx;

        Rx()
        {
            g_got_count = 0;
            g_error_total = 0;
            memset(g_errors, 0, sizeof(g_errors));
            memset(&ctx, 0, sizeof(ctx));
            ctx.onPacket = OnPacket;
            ctx.onError = OnError;
            ctx.pBuf = buf;
            ctx.cbBuf = sizeof(buf);
        }

        void Feed(const uint8_t *p, int n)
        {
            for (int i = 0; i < n; i++)
                packet_decode(&ctx, p[i]);
        }
    };

    void ResetWire() { g_wire_len = 0; }

    PadState MakeState(uint32_t keys, int encoders, uint32_t p0 = 0, uint16_t i0 = 0)
    {
        PadState s = {};
        s.keys = keys;
        s.encoder_count = (uint8_t)encoders;
        for (int e = 0; e < encoders; e++) {
            s.position[e] = p0 + (uint32_t)e * 1000;
            s.interval[e] = (uint16_t)(i0 + e);
        }
        return s;
    }

    bool SameState(const PadState &a, const PadState &b)
    {
        if (a.keys != b.keys || a.encoder_count != b.encoder_count)
            return false;
        for (int e = 0; e < a.encoder_count; e++)
            if (a.position[e] != b.position[e] || a.interval[e] != b.interval[e])
                return false;
        return true;
    }

    // Encodes a state as a packet and returns the payload it used
    size_t EncodeState(uint32_t seq, const PadState &s, uint8_t *payload, size_t cap)
    {
        size_t n = PadBuildState(s, payload, cap);
        Assert(n > 0);
        Encode(seq, PAD_CMD_STATE, payload, n);
        return n;
    }

    // Deterministic RNG
    struct Rng
    {
        uint32_t s;
        explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
        uint32_t Next()
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return s;
        }
        uint32_t Below(uint32_t n) { return Next() % n; }
        // Biased towards the bytes the framing cares about
        uint8_t Byte()
        {
            switch (Below(6)) {
            case 0: return 0xAA;
            case 1: return 0x00;
            case 2: return 0x55;
            default: return (uint8_t)Next();
            }
        }
    };

    PadState RandomState(Rng &r)
    {
        PadState s = {};
        s.encoder_count = (uint8_t)r.Below(PAD_MAX_ENCODERS + 1);
        s.keys = r.Below(3) == 0 ? 0xAAAAAAAAu : r.Next();
        for (int e = 0; e < s.encoder_count; e++) {
            s.position[e] = r.Below(3) == 0 ? 0xAAAAAAAAu : r.Next();
            s.interval[e] = r.Below(3) == 0 ? (uint16_t)0xAAAA : (uint16_t)r.Next();
        }
        return s;
    }
}

// ---- Payloads ---------------------------------------------------------------

Fact("PadProtocol Constants")
{
    Assert(PAD_MAX_KEYS == 32);
    Assert(PAD_MAX_ENCODERS == 4);
    Assert(PAD_CMD_STATE == 1);
    Assert(PAD_CMD_HELLO == 2);
    Assert(PAD_PROTOCOL_VERSION == 1);
    Assert(PAD_STATE_REPEAT_MS == 100);
    Assert(PAD_OFFLINE_TIMEOUT_MS == 300);
    Assert(PAD_ENCODER_MIN_GAP_MS == 5);
    Assert(PadStateBytes(0) == 4);
    Assert(PadStateBytes(1) == 10);
    Assert(PadStateBytes(4) == 28);
    Assert(PAD_PAYLOAD_MAX_BYTES == 28);
}

Fact("PadProtocol State Layout")
{
    // Little-endian: keys, then position and interval per encoder
    PadState s = MakeState(0x04030201, 0);
    uint8_t b[PAD_STATE_MAX_BYTES];
    Assert(PadBuildState(s, b, sizeof(b)) == 4);
    Assert(b[0] == 1 && b[1] == 2 && b[2] == 3 && b[3] == 4);

    s = MakeState(0, 2, 0x11223344, 0x5566);
    Assert(PadBuildState(s, b, sizeof(b)) == 16);
    Assert(b[4] == 0x44 && b[5] == 0x33 && b[6] == 0x22 && b[7] == 0x11);
    Assert(b[8] == 0x66 && b[9] == 0x55);
    // Encoder 1: position + 1000 = 0x112237 2C, interval + 1
    Assert(b[10] == 0x2C && b[11] == 0x37 && b[12] == 0x22 && b[13] == 0x11);
    Assert(b[14] == 0x67 && b[15] == 0x55);
}

Fact("PadProtocol State RoundTrip All Counts")
{
    for (int n = 0; n <= PAD_MAX_ENCODERS; n++) {
        PadState in = MakeState(0xDEADBEEF, n, 0xFFFFFF00, 65535);
        uint8_t b[PAD_STATE_MAX_BYTES];
        size_t len = PadBuildState(in, b, sizeof(b));
        Assert(len == 4 + 6 * (size_t)n);
        PadState out = {};
        out.keys = 7;   // overwritten
        Assert(PadParseState(b, len, out));
        Assert(SameState(in, out));
    }
}

Fact("PadProtocol State Rejects")
{
    uint8_t b[64] = {};
    PadState out = MakeState(0x1234, 1, 5, 6);
    PadState before = out;

    // Every length that is not 4 + 6n for n = 0..4
    for (size_t len = 0; len <= 40; len++) {
        bool valid = len == 4 || len == 10 || len == 16 || len == 22 || len == 28;
        Assert(PadParseState(b, len, out) == valid);
        if (!valid)
            Assert(SameState(out, before));         // untouched on failure
        else
            out = before;
    }

    // Five encoders: the length is right for n = 5 (34) but it is over the limit
    Assert(!PadParseState(b, 34, out));
    Assert(SameState(out, before));
    Assert(!PadParseState(nullptr, 4, out));

    // Building: too many encoders, or a buffer too small
    PadState big = MakeState(0, 4);
    big.encoder_count = 5;
    Assert(PadBuildState(big, b, sizeof(b)) == 0);
    PadState four = MakeState(0, 4);
    Assert(PadBuildState(four, b, 27) == 0);
    Assert(PadBuildState(four, b, 28) == 28);
}

Fact("PadProtocol Hello")
{
    PadHello h = { PAD_PROTOCOL_VERSION, 6, 1 };
    uint8_t b[8];
    Assert(PadBuildHello(h, b, sizeof(b)) == 3);
    Assert(b[0] == 1 && b[1] == 6 && b[2] == 1);
    Assert(PadBuildHello(h, b, 2) == 0);

    PadHello out = {};
    Assert(PadParseHello(b, 3, out));
    Assert(out.version == 1 && out.key_count == 6 && out.encoder_count == 1);

    for (size_t len = 0; len <= 8; len++)
        Assert(PadParseHello(b, len, out) == (len == 3));
}

// ---- Golden wire bytes (also in pad/common/pad_golden.md) --------------------

namespace
{
    const uint8_t GOLDEN_STATE[] = {
        0xAA, 0xAA, 0xAA, 0x00, 0x05, 0x01, 0x0A, 0x05, 0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00,
        0x64, 0x19, 0xB3, 0x86, 0x92, 0x41, 0x55 };
    const uint8_t GOLDEN_HELLO[] = {
        0xAA, 0xAA, 0xAA, 0x00, 0x00, 0x02, 0x03, 0x01, 0x06, 0x01, 0xD5, 0x86, 0xDE, 0xD7, 0x55 };
}

Fact("PadProtocol Golden Bytes")
{
    // STATE seq 5: key1 and key3 down, encoder 1 at 0x1234 with interval 6500 us
    ResetWire();
    PadState s = {};
    s.keys = 5;
    s.encoder_count = 1;
    s.position[0] = 0x1234;
    s.interval[0] = 6500;
    uint8_t payload[PAD_STATE_MAX_BYTES];
    EncodeState(5, s, payload, sizeof(payload));
    Assert(g_wire_len == (int)sizeof(GOLDEN_STATE));
    Assert(memcmp(g_wire, GOLDEN_STATE, sizeof(GOLDEN_STATE)) == 0);

    Rx rx;
    rx.Feed(GOLDEN_STATE, sizeof(GOLDEN_STATE));
    Assert(g_got_count == 1 && g_error_total == 0);
    PadState back = {};
    Assert(g_got[0].seq == 5 && g_got[0].cmd == PAD_CMD_STATE);
    Assert(PadParseState(g_got[0].payload, g_got[0].len, back));
    Assert(SameState(s, back));

    // HELLO seq 0
    ResetWire();
    PadHello h = { PAD_PROTOCOL_VERSION, 6, 1 };
    uint8_t hp[PAD_HELLO_BYTES];
    Assert(PadBuildHello(h, hp, sizeof(hp)) == 3);
    Encode(0, PAD_CMD_HELLO, hp, 3);
    Assert(g_wire_len == (int)sizeof(GOLDEN_HELLO));
    Assert(memcmp(g_wire, GOLDEN_HELLO, sizeof(GOLDEN_HELLO)) == 0);

    Rx rx2;
    rx2.Feed(GOLDEN_HELLO, sizeof(GOLDEN_HELLO));
    Assert(g_got_count == 1 && g_got[0].cmd == PAD_CMD_HELLO && g_got[0].len == 3);
}

// ---- Round trips through the real PacketEncoder -------------------------------

Fact("PadProtocol Packet RoundTrip")
{
    for (int n = 0; n <= PAD_MAX_ENCODERS; n++) {
        ResetWire();
        PadState in = MakeState(0x00FF0F01u, n, 0x1000, 6500);
        uint8_t payload[PAD_STATE_MAX_BYTES];
        EncodeState(300 + n, in, payload, sizeof(payload));      // seq over 127: a 2-byte varint

        Rx rx;
        rx.Feed(g_wire, g_wire_len);
        Assert(g_got_count == 1 && g_error_total == 0);
        Assert(g_got[0].seq == (uint32_t)(300 + n) && g_got[0].cmd == PAD_CMD_STATE);
        PadState out = {};
        Assert(PadParseState(g_got[0].payload, g_got[0].len, out));
        Assert(SameState(in, out));
    }
}

Fact("PadProtocol Stuffing")
{
    // 0xAA runs in the payload get a stuffing byte after every pair; nothing on the
    // wire after the start signal may hold three 0xAA in a row, or the decoder would
    // restart there
    PadState cases[3];
    cases[0] = MakeState(0xAAAAAAAAu, 0);
    cases[1] = MakeState(0xAAAAAAAAu, 1, 0xAAAAAAAAu, 0xAAAA);
    cases[2] = MakeState(0xAAAAAAAAu, 4, 0xAAAAAAAAu, 0xAAAA);
    for (int c = 0; c < 3; c++) {
        ResetWire();
        uint8_t payload[PAD_STATE_MAX_BYTES];
        size_t n = EncodeState(0xAA, cases[c], payload, sizeof(payload));
        Assert(g_wire_len > (int)n + 12);                       // grown by the stuffing

        int run = 0;
        for (int i = 3; i < g_wire_len; i++) {
            run = g_wire[i] == 0xAA ? run + 1 : 0;
            Assert(run < 3);
        }

        Rx rx;
        rx.Feed(g_wire, g_wire_len);
        Assert(g_got_count == 1 && g_error_total == 0);
        PadState out = {};
        Assert(PadParseState(g_got[0].payload, g_got[0].len, out));
        Assert(SameState(cases[c], out));
        Assert(g_got[0].seq == 0xAA);
    }
}

Fact("PadProtocol Junk Prefix And Back To Back")
{
    ResetWire();
    uint8_t payload[PAD_STATE_MAX_BYTES];
    PadState a = MakeState(1, 1, 10, 20);
    PadState b = MakeState(2, 0);
    EncodeState(1, a, payload, sizeof(payload));
    EncodeState(2, b, payload, sizeof(payload));
    int len = g_wire_len;
    uint8_t packets[256];
    memcpy(packets, g_wire, len);

    // Garbage first (including single and double 0xAA, which must not start a packet)
    const uint8_t junk[] = { 0x13, 0xAA, 0x00, 0xAA, 0xAA, 0x77, 0x55, 0xAA, 0x01, 0x55, 0x00, 0xFF };
    Rx rx;
    rx.Feed(junk, sizeof(junk));
    Assert(g_got_count == 0);
    rx.Feed(packets, len);
    Assert(g_got_count == 2);
    PadState out = {};
    Assert(PadParseState(g_got[0].payload, g_got[0].len, out) && SameState(a, out) && g_got[0].seq == 1);
    Assert(PadParseState(g_got[1].payload, g_got[1].len, out) && SameState(b, out) && g_got[1].seq == 2);

    // A random-byte prefix of every length 0..40 (and every junk seed) never hides the packet
    for (uint32_t seed = 1; seed <= 200; seed++) {
        Rng r(seed);
        int jn = (int)r.Below(41);
        Rx rx2;
        for (int i = 0; i < jn; i++) {
            uint8_t j = r.Byte();
            packet_decode(&rx2.ctx, j);
        }
        rx2.Feed(packets, len);
        // The junk can end in 0xAA bytes; the first packet may then be lost only if the
        // junk itself opened a packet that swallowed it. The second one always arrives.
        Assert(g_got_count >= 1);
        Assert(g_got[g_got_count - 1].seq == 2);
        for (int i = 0; i < g_got_count; i++) {
            Assert(g_got[i].seq == 1 || g_got[i].seq == 2);
            Assert(g_got[i].cmd == PAD_CMD_STATE);
        }
    }
}

Fact("PadProtocol Interrupted By New Start")
{
    // Packet A cut at every possible point, then packet B whole: only B is delivered
    // (A never, however much of it arrived), and B is never damaged by the cut.
    ResetWire();
    uint8_t payload[PAD_STATE_MAX_BYTES];
    PadState a = MakeState(0xAAAAAAAAu, 2, 0xAAAAAAAAu, 100);
    PadState b = MakeState(0x1234, 1, 99, 98);
    EncodeState(10, a, payload, sizeof(payload));
    int a_len = g_wire_len;
    uint8_t a_bytes[256];
    memcpy(a_bytes, g_wire, a_len);
    ResetWire();
    EncodeState(11, b, payload, sizeof(payload));
    int b_len = g_wire_len;
    uint8_t b_bytes[256];
    memcpy(b_bytes, g_wire, b_len);

    for (int cut = 1; cut < a_len; cut++) {
        Rx rx;
        rx.Feed(a_bytes, cut);
        rx.Feed(b_bytes, b_len);
        Assert(g_got_count == 1);
        Assert(g_got[0].seq == 11);
        PadState out = {};
        Assert(PadParseState(g_got[0].payload, g_got[0].len, out) && SameState(b, out));
        // A cut after the start signal is reported as an interrupted packet
        if (cut >= 4)
            Assert(g_errors[packet_error_new_packet] >= 1 || g_error_total >= 1);
    }

    // The uncut A, then B
    Rx rx;
    rx.Feed(a_bytes, a_len);
    rx.Feed(b_bytes, b_len);
    Assert(g_got_count == 2 && g_error_total == 0);
}

Fact("PadProtocol Decoder Limits")
{
    // A payload bigger than the decoder's buffer is refused, never truncated or overrun
    ResetWire();
    uint8_t big[PAD_PAYLOAD_MAX_BYTES + 1];
    memset(big, 0x42, sizeof(big));
    Encode(1, PAD_CMD_STATE, big, sizeof(big));
    Rx rx;
    rx.Feed(g_wire, g_wire_len);
    Assert(g_got_count == 0);
    Assert(g_errors[packet_error_too_large] == 1);

    // An unknown cmd is delivered (the receiver ignores it); a wrong-length STATE is
    // delivered by the transport but PadParseState rejects it
    ResetWire();
    uint8_t five[5] = { 1, 2, 3, 4, 5 };
    Encode(2, 99, five, 5);
    Encode(3, PAD_CMD_STATE, five, 5);
    Encode(4, PAD_CMD_HELLO, five, 5);
    Rx rx2;
    rx2.Feed(g_wire, g_wire_len);
    Assert(g_got_count == 3);
    PadState s;
    PadHello h;
    Assert(g_got[0].cmd == 99);
    Assert(!PadParseState(g_got[1].payload, g_got[1].len, s));
    Assert(!PadParseHello(g_got[2].payload, g_got[2].len, h));
}

// ---- Noise -------------------------------------------------------------------

namespace
{
    // Builds a stream of packets (unique seqs), recording each payload to check
    // deliveries against. Returns the stream length in 'wire'.
    constexpr int FUZZ_PACKETS = 40;

    struct Original
    {
        uint32_t cmd;
        uint8_t  payload[PAD_PAYLOAD_MAX_BYTES];
        size_t   len;
    };

    Original g_orig[FUZZ_PACKETS];
    int      g_fuzz_delivered = 0;      // across seeds: proves the noisy runs still deliver something

    void BuildStream(Rng &r)
    {
        ResetWire();
        for (int i = 0; i < FUZZ_PACKETS; i++) {
            Original &o = g_orig[i];
            if (r.Below(8) == 0) {
                PadHello h = { PAD_PROTOCOL_VERSION, (uint8_t)r.Below(33), (uint8_t)r.Below(5) };
                o.cmd = PAD_CMD_HELLO;
                o.len = PadBuildHello(h, o.payload, sizeof(o.payload));
            } else {
                PadState s = RandomState(r);
                o.cmd = PAD_CMD_STATE;
                o.len = PadBuildState(s, o.payload, sizeof(o.payload));
            }
            Encode((uint32_t)i, o.cmd, o.payload, o.len);
        }
    }

    // Everything delivered must be a bit-exact original; returns how many were
    void CheckDelivered()
    {
        for (int i = 0; i < g_got_count; i++) {
            const Delivered &d = g_got[i];
            Assert(d.seq < (uint32_t)FUZZ_PACKETS);
            const Original &o = g_orig[d.seq];
            Assert(d.cmd == o.cmd);
            Assert(d.len == o.len);
            Assert(memcmp(d.payload, o.payload, o.len) == 0);
        }
    }

    void FuzzOne(uint32_t seed)
    {
        Rng r(seed);
        BuildStream(r);

        // Clean stream first: everything arrives, in order, with no errors
        {
            Rx rx;
            rx.Feed(g_wire, g_wire_len);
            Assert(g_got_count == FUZZ_PACKETS && g_error_total == 0);
            for (int i = 0; i < g_got_count; i++)
                Assert(g_got[i].seq == (uint32_t)i);
            CheckDelivered();
        }

        // Then damaged: drop, flip, insert and duplicate bytes at a random rate
        static uint8_t noisy[WIRE_MAX * 2];
        int n = 0;
        uint32_t rate = 1 + r.Below(60);                // roughly 1 in 'rate' bytes is hit
        for (int i = 0; i < g_wire_len; i++) {
            if (r.Below(rate) != 0) {
                noisy[n++] = g_wire[i];
                continue;
            }
            switch (r.Below(4)) {
            case 0: break;                                                      // dropped
            case 1: noisy[n++] = g_wire[i] ^ (uint8_t)(1u << r.Below(8)); break;   // bit flip
            case 2: noisy[n++] = g_wire[i]; noisy[n++] = r.Byte(); break;       // inserted
            case 3: noisy[n++] = r.Byte(); break;                               // replaced
            }
        }
        Rx rx;
        rx.Feed(noisy, n);
        Assert(g_got_count <= FUZZ_PACKETS);
        CheckDelivered();
        g_fuzz_delivered += g_got_count;

        // Delivered payloads must also parse (or be rejected by length) without a crash
        for (int i = 0; i < g_got_count; i++) {
            PadState s;
            PadHello h;
            if (g_got[i].cmd == PAD_CMD_STATE)
                Assert(PadParseState(g_got[i].payload, g_got[i].len, s));
            else
                Assert(PadParseHello(g_got[i].payload, g_got[i].len, h));
        }
    }
}

Fact("PadProtocol Fuzz")
{
    const char *env = getenv("FUZZ_SEEDS");
    int count = env && atoi(env) > 0 ? atoi(env) : 200;
    for (uint32_t seed = 1; seed <= (uint32_t)count; seed++) {
        try {
            FuzzOne(seed * 104729);
        } catch (const AssertionFailed &) {
            fprintf(stderr, "  (fuzz seed %u)\n", seed * 104729);
            throw;
        }
    }
    Assert(g_fuzz_delivered > 0);
}
