// The C3's pure modules: GlitchFilter, QuadDecoder (urem's rules), EncoderChannel and
// PadScanner (key debounce, encoder decoding and the STATE/HELLO send policy), all on a
// fake clock. The differential test against urem's own code is in TestPadUrem.cpp.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "pad_scanner.h"
#include "packenc.h"

using namespace SimpleLib;

namespace
{
    constexpr uint64_t MS = 1000;

    // ---- Rng ---------------------------------------------------------------------

    struct Rng
    {
        uint32_t s;
        explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
        uint32_t Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
        uint32_t Below(uint32_t n) { return Next() % n; }
        uint32_t Between(uint32_t lo, uint32_t hi) { return lo + Below(hi - lo + 1); }
    };

    // ---- Quadrature helpers ----------------------------------------------------------

    // Gray cycle, CW: 00 > 01 > 11 > 10 (A << 1 | B)
    const uint8_t GRAY[4] = { 0, 1, 3, 2 };
    bool GA(int idx) { return (GRAY[idx & 3] & 2) != 0; }
    bool GB(int idx) { return (GRAY[idx & 3] & 1) != 0; }

    // ---- Wire capture: the scanner writes bytes, the real packet_decode reads them ------

    constexpr int WIRE_MAX = 16384;
    uint8_t g_wire[WIRE_MAX];
    int     g_wire_len = 0;

    void Sink(uint8_t b)
    {
        Assert(g_wire_len < WIRE_MAX);
        g_wire[g_wire_len++] = b;
    }

    struct Pkt
    {
        uint64_t  t;
        uint32_t  seq, cmd;
        PadState  state;
        PadHello  hello;
    };

    constexpr int MAX_PKTS = 8192;
    Pkt      g_pk[MAX_PKTS];
    int      g_pk_n = 0;
    uint64_t g_now = 0;
    int      g_decode_errors = 0;

    void OnPacket(uint32_t seq, uint32_t cmd, const void *data, uint32_t len)
    {
        Assert(g_pk_n < MAX_PKTS);
        Pkt &p = g_pk[g_pk_n++];
        memset(&p, 0, sizeof(p));
        p.t = g_now;
        p.seq = seq;
        p.cmd = cmd;
        if (cmd == PAD_CMD_STATE)
            Assert(PadParseState(data, len, p.state));
        else
            Assert(PadParseHello(data, len, p.hello));
    }

    void OnError(int) { g_decode_errors++; }

    // A scanner on a 1 ms poll, with a decoder on its output
    struct Sim
    {
        PadScanner s;
        uint8_t    buf[PAD_PAYLOAD_MAX_BYTES];
        decode_context ctx;
        int        enc_idx[PAD_MAX_ENCODERS];       // where each encoder sits on the Gray cycle
        uint64_t   polled = 0;

        Sim(int keys, int encoders, uint32_t key_levels = 0xFFFFFFFFu, const uint8_t *enc_pins = nullptr)
        {
            g_wire_len = 0;
            g_pk_n = 0;
            g_now = 0;
            g_decode_errors = 0;
            memset(&ctx, 0, sizeof(ctx));
            ctx.onPacket = OnPacket;
            ctx.onError = OnError;
            ctx.pBuf = buf;
            ctx.cbBuf = sizeof(buf);
            for (int e = 0; e < PAD_MAX_ENCODERS; e++)
                enc_idx[e] = enc_pins ? 0 : 2;      // idle = both high = 11 = index 2
            PadScannerConfig cfg = {};
            cfg.key_count = (uint8_t)keys;
            cfg.encoder_count = (uint8_t)encoders;
            s.Init(cfg, Sink, 0, key_levels, enc_pins);
            Drain();
        }

        void Drain()
        {
            for (int i = 0; i < g_wire_len; i++)
                packet_decode(&ctx, g_wire[i]);
            g_wire_len = 0;
        }

        // Advance to 'until' (us), polling every 1 ms, and make that the time of events
        void At(uint64_t until)
        {
            while (polled + MS <= until) {
                polled += MS;
                g_now = polled;
                s.Poll(polled);
                Drain();
            }
            g_now = until;
        }

        void Keys(uint32_t levels)
        {
            s.SetKeyLevels(g_now, levels);
            Drain();
        }

        void Pins(int e, bool a, bool b)
        {
            s.SetEncoderPins(g_now, e, a, b);
            Drain();
        }

        // One quadrature edge of encoder e at time t
        void Edge(int e, int dir, uint64_t t)
        {
            At(t);
            enc_idx[e] += dir;
            Pins(e, GA(enc_idx[e]), GB(enc_idx[e]));
        }

        // n edges, 'gap' us apart, the first at 't' (us); returns the time of the last
        uint64_t Spin(int e, int dir, int n, uint64_t t, uint64_t gap)
        {
            for (int i = 0; i < n; i++, t += gap)
                Edge(e, dir, t);
            return t - gap;
        }
    };

    const Pkt &Last() { Assert(g_pk_n > 0); return g_pk[g_pk_n - 1]; }
}

// ---- GlitchFilter -----------------------------------------------------------------------

Fact("GlitchFilter accepts a level that has been stable long enough")
{
    GlitchFilter f;
    f.Init(1000, false);
    Assert(!f.Update(10000, false));            // nothing happens
    Assert(!f.Update(20000, true));             // raw goes high at 20 ms: pending
    Assert(f.Pending());
    Assert(!f.Update(20999, true));             // 999 us: not yet
    Assert(f.Level() == false);
    Assert(f.Update(21000, true));              // exactly 1000 us: accepted (>=)
    Assert(f.Level() == true);
    Assert(f.EdgeTime() == 20000);              // reported with the original edge time
    Assert(!f.Pending());
    Assert(!f.Update(30000, true));             // nothing more
}

Fact("GlitchFilter ignores a pulse shorter than the stable time")
{
    GlitchFilter f;
    f.Init(5000, true);
    Assert(!f.Update(100000, false));           // low edge
    Assert(!f.Update(104999, true));            // back high after 4999 us: a glitch
    Assert(f.Level() == true);
    Assert(!f.Pending());
    Assert(!f.Update(200000, true));            // and it never shows up
}

Fact("GlitchFilter passes a pulse of exactly the stable time")
{
    GlitchFilter f;
    f.Init(5000, true);
    Assert(!f.Update(100000, false));
    Assert(f.Update(105000, true));             // the low level held 5000 us: accepted now...
    Assert(f.Level() == false);
    Assert(f.EdgeTime() == 100000);
    Assert(f.Pending());                        // ...and the high edge starts its own stable period
    Assert(f.Update(110000, true));
    Assert(f.Level() == true && f.EdgeTime() == 105000);
}

Fact("GlitchFilter restarts the stable period after a bounce")
{
    GlitchFilter f;
    f.Init(1000, false);
    Assert(!f.Update(1000000, true));           // high
    Assert(!f.Update(1000400, false));          // glitch
    Assert(!f.Update(1000800, true));           // high again: the period restarts here
    Assert(!f.Update(1001700, true));           // only 900 us since 1000800
    Assert(f.Update(1001800, true));
    Assert(f.EdgeTime() == 1000800);
}

Fact("GlitchFilter accepts by polling when no further edge comes")
{
    GlitchFilter f;
    f.Init(5000, false);
    Assert(!f.Update(0, true));
    for (uint64_t t = 1000; t < 5000; t += 1000)
        Assert(!f.Update(t, true));
    Assert(f.Update(5000, true));
    Assert(f.EdgeTime() == 0);
}

// ---- QuadDecoder -------------------------------------------------------------------------

Fact("QuadDecoder interval saturates to u16")
{
    Assert(QuadIntervalToU16(0) == 0);
    Assert(QuadIntervalToU16(1000) == 1000);
    Assert(QuadIntervalToU16(65535) == 65535);
    Assert(QuadIntervalToU16(65536) == 65535);
    Assert(QuadIntervalToU16(1000000) == 65535);
}

Fact("QuadDecoder fires one tick per 4 valid edges, both directions")
{
    QuadDecoder q;
    q.Init(false, false);                       // 00
    uint64_t t = 5000000;
    int ticks = 0;
    for (int i = 1; i <= 8; i++, t += 2000) {
        int r = q.Edge(t, GA(i), GB(i));
        if (i % 4 == 0) { Assert(r == 1); ticks++; }
        else Assert(r == 0);
    }
    Assert(ticks == 2 && q.Detents() == 2 && q.EdgePosition() == 8);

    q.Init(false, false);
    for (int i = 1; i <= 8; i++, t += 2000) {
        int r = q.Edge(t, GA(-i), GB(-i));
        if (i % 4 == 0) Assert(r == -1);
        else Assert(r == 0);
    }
    Assert(q.Detents() == 0xFFFFFFFEu);         // wraps below zero
}

Fact("QuadDecoder detent position wraps as a uint32")
{
    QuadDecoder q;
    q.Init(false, false, 0xFFFFFFFFu);
    uint64_t t = 5000000;
    for (int i = 1; i <= 4; i++, t += 2000)
        q.Edge(t, GA(i), GB(i));
    Assert(q.Detents() == 0);                   // 0xFFFFFFFF + 1
    for (int i = 3; i >= 0; i--, t += 2000)
        q.Edge(t, GA(i), GB(i));                // 3 edges back, then...
    // ...edge position went 4 > 3 > 2 > 1 > 0: a -1 tick at 0 (a multiple of 4)
    Assert(q.Detents() == 0xFFFFFFFFu);
}

Fact("QuadDecoder reversals follow urem's multiple-of-4 rule")
{
    // Out two edges and back two: the running edge position returns to 0 (a multiple of
    // 4) so a tick of -1 fires, though no +1 ever did
    QuadDecoder q;
    q.Init(false, false);
    uint64_t t = 5000000;
    int idx = 0;
    const int moves[] = { 1, 1, -1, -1 };
    int results[4];
    for (int i = 0; i < 4; i++, t += 3000) {
        idx += moves[i];
        results[i] = q.Edge(t, GA(idx), GB(idx));
    }
    Assert(results[0] == 0 && results[1] == 0 && results[2] == 0 && results[3] == -1);
    Assert(q.Detents() == 0xFFFFFFFFu && q.EdgePosition() == 0);

    // Wiggle across a boundary: 3 out, 3 back is silent, 4 out ticks +1 once, 1 back and out again
    // fires -1 on the way back to 4 and +1 on the way out again
    q.Init(false, false);
    idx = 0;
    int net = 0;
    const int seq[] = { 1, 1, 1, 1, -1, 1, -1, 1 };     // pos: 1 2 3 4(+1) 3 4(+1) 3 4(+1)
    for (int i = 0; i < 8; i++, t += 3000) {
        idx += seq[i];
        net += q.Edge(t, GA(idx), GB(idx));
    }
    Assert(q.EdgePosition() == 4 && net == 3 && q.Detents() == 3);
}

Fact("QuadDecoder ignores invalid transitions but remembers the new state")
{
    QuadDecoder q;
    q.Init(false, false);
    uint64_t t = 5000000;
    Assert(q.Edge(t += 1000, true, true) == 0);     // 00 > 11: both bits changed
    Assert(q.EdgePosition() == 0);
    Assert(q.Edge(t += 1000, true, true) == 0);     // no change
    Assert(q.EdgePosition() == 0);
    Assert(q.Edge(t += 1000, true, false) == 0);    // 11 > 10: valid CW from 11
    Assert(q.EdgePosition() == 1);
    Assert(q.Edge(t += 1000, false, true) == 0);    // 10 > 01: invalid
    Assert(q.EdgePosition() == 1);
    Assert(q.Edge(t += 1000, true, true) == 0);     // 01 > 11: CW
    Assert(q.EdgePosition() == 2);
}

Fact("QuadDecoder interval is the average of the last 4 call intervals, every call counted")
{
    QuadDecoder q;
    q.Init(true, true);
    Assert(q.IntervalUs() == 1000000);              // starts at the 1 s cap
    uint64_t t = 5000000;
    q.Edge(t, true, true);                          // no change; first call counts as the cap
    Assert(q.IntervalUs() == 1000000);
    q.Edge(t += 2000, true, true);                  // an invalid call (no change) still counts
    Assert(q.IntervalUs() == (3 * 1000000 + 2000) / 4);
    q.Edge(t += 2000, true, false);
    q.Edge(t += 2000, true, true);
    Assert(q.IntervalUs() == (1000000 + 3 * 2000) / 4);
    q.Edge(t += 2000, true, false);
    Assert(q.IntervalUs() == 2000);                 // the cap has left the ring
    q.Edge(t += 8000, true, true);
    Assert(q.IntervalUs() == (3 * 2000 + 8000) / 4);
}

Fact("QuadDecoder caps each interval at 1 s and tolerates time going backwards")
{
    QuadDecoder q;
    q.Init(true, true);
    uint64_t t = 5000000;
    for (int i = 0; i < 5; i++, t += 1000)
        q.Edge(t, true, true);
    Assert(q.IntervalUs() == 1000);
    q.Edge(t += 9000000, true, true);               // 9 s idle
    Assert(q.IntervalUs() == (3 * 1000 + 1000000) / 4);
    q.Edge(t - 500, true, true);                    // backwards: 0 elapsed
    Assert(q.IntervalUs() == (2 * 1000 + 1000000) / 4);
}

// ---- EncoderChannel -------------------------------------------------------------------

Fact("EncoderChannel filters 1 ms bounce and reports a tick with the edge time")
{
    EncoderChannel c;
    c.Init(PAD_ENCODER_FILTER_US, true, true);      // idle 11
    QuadTick out[2];
    uint64_t t = 10000000;
    int ticks = 0;
    // 4 CW edges from 11: 10, 00, 01, 11, each pin edge with a 300 us bounce
    for (int i = 1; i <= 4; i++, t += 3000) {
        int idx = 2 + i;
        bool a = GA(idx), b = GB(idx);
        bool pin_a = (GA(idx) != GA(idx - 1));
        // the edge, then a 300 us flip back and forth
        bool ra = a, rb = b;
        c.Update(t, ra, rb, out);
        if (pin_a) ra = !ra; else rb = !rb;
        c.Update(t + 100, ra, rb, out);
        if (pin_a) ra = !ra; else rb = !rb;
        c.Update(t + 400, ra, rb, out);
        int n = c.Update(t + 2000, ra, rb, out);
        if (i == 4) {
            Assert(n == 1 && out[0].dir == 1);
            Assert(out[0].time_us == t + 400);      // the stable edge, not the glitch
            ticks++;
        } else {
            Assert(n == 0);
        }
    }
    Assert(ticks == 1 && c.Detents() == 1);
}

Fact("EncoderChannel orders two changes accepted in one call by edge time")
{
    QuadTick out[2];
    // A rises at 10.000 ms, B rises at 10.400 ms (00 > 10 > 11: CCW), both accepted by the call at 12 ms
    EncoderChannel c;
    c.Init(1000, false, false);
    Assert(c.Update(10000000, true, false, out) == 0);
    Assert(c.Update(10400000, true, true, out) == 0);
    Assert(c.Update(12000000, true, true, out) == 0);
    Assert(c.EdgePosition() == -2);
    // B first, then A (00 > 01 > 11: CW)
    EncoderChannel d;
    d.Init(1000, false, false);
    d.Update(10000000, false, true, out);
    d.Update(10400000, true, true, out);
    d.Update(12000000, true, true, out);
    Assert(d.EdgePosition() == 2);
}

// ---- PadScanner: startup and the repeat ---------------------------------------------------

Fact("PadScanner sends HELLO three times 50 ms apart, then STATE at once and every 100 ms")
{
    Sim sim(6, 1);
    sim.At(300000);
    Assert(g_decode_errors == 0);
    Assert(g_pk_n == 6);
    for (int i = 0; i < 3; i++) {
        Assert(g_pk[i].cmd == PAD_CMD_HELLO);
        Assert(g_pk[i].t == (uint64_t)i * 50 * MS);
        Assert(g_pk[i].hello.version == PAD_PROTOCOL_VERSION);
        Assert(g_pk[i].hello.key_count == 6 && g_pk[i].hello.encoder_count == 1);
    }
    Assert(g_pk[3].cmd == PAD_CMD_STATE && g_pk[3].t == 100 * MS);      // after the last HELLO
    Assert(g_pk[3].state.keys == 0 && g_pk[3].state.encoder_count == 1);
    Assert(g_pk[3].state.position[0] == 0);
    Assert(g_pk[3].state.interval[0] == 65535);                         // the 1 s start, saturated
    Assert(g_pk[4].t == 200 * MS && g_pk[5].t == 300 * MS);
    for (int i = 0; i < g_pk_n; i++)
        Assert(g_pk[i].seq == (uint32_t)i);                             // free running
}

Fact("PadScanner holds STATE back until the HELLOs are out")
{
    Sim sim(6, 0);
    sim.At(20000);
    sim.Keys(~(1u << 0));                           // key 1 down at 20 ms (accepted at 25 ms)
    sim.At(99000);
    Assert(g_pk_n == 2);                            // HELLO at 0 and 50, nothing else
    sim.At(100000);
    Assert(g_pk_n == 4);
    Assert(g_pk[2].cmd == PAD_CMD_HELLO && g_pk[3].cmd == PAD_CMD_STATE);
    Assert(g_pk[3].state.keys == 1);                // the key is already in the baseline
}

Fact("PadScanner counts a key held at start as down")
{
    Sim sim(6, 0, 0xFFFFFFFFu & ~(1u << 3) & ~(1u << 5));
    sim.At(100000);
    Assert(Last().cmd == PAD_CMD_STATE && Last().state.keys == ((1u << 3) | (1u << 5)));
}

// ---- PadScanner: keys ---------------------------------------------------------------------

Fact("PadScanner reports a key 5 ms after it settles, then again after the release")
{
    Sim sim(6, 0);
    sim.At(120000);
    Assert(g_pk_n == 4);
    sim.Keys(~(1u << 2));                           // key 3 pressed (pin low)
    sim.At(124999);
    Assert(g_pk_n == 4);                            // still debouncing
    sim.At(125000);
    Assert(g_pk_n == 5 && Last().t == 125 * MS && Last().state.keys == 4);
    sim.At(130000);
    sim.Keys(0xFFFFFFFFu);                          // released
    sim.At(134999);
    Assert(g_pk_n == 5);
    sim.At(135000);
    Assert(g_pk_n == 6 && Last().t == 135 * MS && Last().state.keys == 0);
    sim.At(400000);
    Assert(g_pk[6].t == 235 * MS);                  // the repeat counts from the last STATE
}

Fact("PadScanner filters contact bounce on a key")
{
    Sim sim(6, 0);
    sim.At(120000);
    const uint32_t down = ~(1u << 1), up = 0xFFFFFFFFu;
    sim.Keys(down);
    sim.At(121000); sim.Keys(up);
    sim.At(123000); sim.Keys(down);
    sim.At(124000); sim.Keys(up);
    sim.At(126000); sim.Keys(down);                 // settles low from here
    sim.At(130999);
    Assert(g_pk_n == 4);
    sim.At(131000);
    Assert(g_pk_n == 5 && Last().t == 131 * MS && Last().state.keys == 2);
    sim.At(200000);
    Assert(g_pk_n == 5);                            // nothing else until the repeat at 231
}

Fact("PadScanner ignores a key glitch shorter than 5 ms")
{
    Sim sim(6, 0);
    sim.At(120000);
    sim.Keys(~(1u << 4));
    sim.At(124000);
    sim.Keys(0xFFFFFFFFu);
    sim.At(190000);
    Assert(g_pk_n == 4);
    Assert(sim.s.PressedKeys() == 0);
}

Fact("PadScanner sends a key change at once even right after another STATE")
{
    Sim sim(6, 0);
    sim.At(120000);
    sim.Keys(~(1u << 0));
    sim.At(122000);
    sim.Keys(~(1u << 0) & ~(1u << 1));
    sim.At(140000);
    Assert(g_pk_n == 6);
    Assert(g_pk[4].t == 125 * MS && g_pk[4].state.keys == 1);
    Assert(g_pk[5].t == 127 * MS && g_pk[5].state.keys == 3);       // 2 ms later, not held back
}

Fact("PadScanner handles key 32 and ignores pins beyond the key count")
{
    Sim sim(32, 0);
    sim.At(120000);
    sim.Keys(~(1u << 31));
    sim.At(130000);
    Assert(Last().state.keys == 0x80000000u);

    Sim six(6, 0);
    six.At(120000);
    six.Keys(~(1u << 20));                          // a pin that isn't a key
    six.At(150000);
    Assert(g_pk_n == 4);
}

// ---- PadScanner: encoders ---------------------------------------------------------------

Fact("PadScanner sends a detent once it completes, with urem's interval")
{
    Sim sim(0, 1);
    sim.At(110000);
    Assert(g_pk_n == 4);
    // Two detents: 8 edges 3 ms apart from 120 ms. The first detent's tick (4th edge, 129 ms,
    // accepted at 130) carries the start-up 1 s in its average; the second (141 ms, accepted at 142) is 3000 us.
    sim.Spin(0, +1, 8, 120000, 3000);
    sim.At(143000);
    Assert(g_pk_n == 6);
    Assert(g_pk[4].t == 130 * MS && g_pk[4].state.position[0] == 1);
    Assert(g_pk[4].state.interval[0] == QuadIntervalToU16((1000000 + 3 * 3000) / 4));   // saturated
    Assert(g_pk[5].t == 142 * MS && g_pk[5].state.position[0] == 2);
    Assert(g_pk[5].state.interval[0] == 3000);
}

Fact("PadScanner limits encoder-only STATEs to one per 5 ms and loses no detent")
{
    Sim sim(0, 1);
    sim.At(110000);
    sim.Spin(0, +1, 8, 120000, 3000);               // as above: STATEs at 130 and 142
    // Three more detents at 1 ms per edge: ticks accepted at 154, 158, 162
    sim.Spin(0, +1, 12, 150000, 1000);
    sim.At(170000);
    int mark = 6;
    Assert(g_pk[mark].t == 154 * MS && g_pk[mark].state.position[0] == 3);      // gap 12 ms: at once
    Assert(g_pk[mark + 1].t == 159 * MS && g_pk[mark + 1].state.position[0] == 4);   // 158 held to 159
    Assert(g_pk[mark + 2].t == 164 * MS && g_pk[mark + 2].state.position[0] == 5);   // 162 held to 164
    for (int i = 1; i < g_pk_n; i++)
        if (g_pk[i].cmd == PAD_CMD_STATE && g_pk[i - 1].cmd == PAD_CMD_STATE && g_pk[i].t >= 130 * MS)
            Assert(g_pk[i].t - g_pk[i - 1].t >= 5 * MS);
    Assert(sim.s.Detents(0) == 5);
}

Fact("PadScanner position wraps below zero and back")
{
    Sim sim(0, 1);
    sim.At(110000);
    sim.Spin(0, -1, 4, 120000, 3000);
    sim.At(140000);
    Assert(Last().state.position[0] == 0xFFFFFFFFu);
    sim.Spin(0, +1, 4, 150000, 3000);
    sim.At(180000);
    Assert(Last().state.position[0] == 0);
}

Fact("PadScanner saturates a slow interval to 65535 us")
{
    Sim sim(0, 1);
    sim.At(110000);
    sim.Spin(0, +1, 8, 120000, 100000);             // 100 ms per edge
    sim.At(1000000);
    Assert(sim.s.Detents(0) == 2);
    // find the last STATE with position 2
    const Pkt *p = nullptr;
    for (int i = 0; i < g_pk_n; i++)
        if (g_pk[i].cmd == PAD_CMD_STATE && g_pk[i].state.position[0] == 2)
            p = &g_pk[i];
    Assert(p && p->state.interval[0] == 65535);
}

Fact("PadScanner keeps the interval of the latest tick in the repeats")
{
    Sim sim(0, 1);
    sim.At(110000);
    sim.Spin(0, +1, 8, 120000, 3000);
    sim.At(600000);
    Assert(Last().state.position[0] == 2 && Last().state.interval[0] == 3000);
}

Fact("PadScanner rejects an encoder pin glitch and survives bounce on every edge")
{
    Sim sim(0, 1);
    sim.At(110000);
    // glitch: A low for 500 us
    sim.At(120000); sim.Pins(0, false, true);
    sim.At(120500); sim.Pins(0, true, true);
    sim.At(140000);
    Assert(sim.s.Detents(0) == 0 && g_pk_n == 4);
    // a full detent whose every edge bounces for 400 us
    uint64_t t = 150000;
    for (int i = 1; i <= 4; i++, t += 4000) {
        int idx = 2 + i;
        bool pin_a = GA(idx) != GA(idx - 1);
        bool a = GA(idx), b = GB(idx);
        sim.At(t);       sim.Pins(0, a, b);
        sim.At(t + 100); sim.Pins(0, pin_a ? !a : a, pin_a ? b : !b);
        sim.At(t + 500); sim.Pins(0, a, b);
    }
    sim.At(180000);
    Assert(sim.s.Detents(0) == 1);
}

Fact("PadScanner runs two encoders independently in one STATE")
{
    Sim sim(0, 2);
    sim.At(110000);
    sim.Spin(0, +1, 4, 120000, 3000);
    sim.Spin(1, -1, 4, 135000, 3000);
    sim.At(160000);
    Assert(Last().state.encoder_count == 2);
    Assert(Last().state.position[0] == 1 && Last().state.position[1] == 0xFFFFFFFFu);
}

Fact("PadScanner starts from the given encoder pin levels")
{
    const uint8_t pins[1] = { 0 };                  // 00
    Sim sim(0, 1, 0xFFFFFFFFu, pins);
    sim.At(120000);
    // 4 CW edges from 00: 01, 11, 10, 00
    sim.enc_idx[0] = 0;
    sim.Spin(0, +1, 4, 130000, 3000);
    sim.At(160000);
    Assert(sim.s.Detents(0) == 1);
}

// ---- PadScanner: invariants under random input --------------------------------------------

namespace
{
    // Runs random key and encoder activity for 'seconds', with a reference EncoderChannel
    // fed the same raw pin calls, and checks the packet stream.
    void FuzzOne(uint32_t seed)
    {
        Rng r(seed);
        const int keys = (int)r.Between(0, 32);
        const int encs = (int)r.Between(0, 4);
        Sim sim(keys, encs);
        EncoderChannel ref[PAD_MAX_ENCODERS];
        bool pa[PAD_MAX_ENCODERS], pb[PAD_MAX_ENCODERS];
        for (int e = 0; e < encs; e++) {
            ref[e].Init(PAD_ENCODER_FILTER_US, true, true);
            pa[e] = pb[e] = true;
        }
        uint32_t levels = 0xFFFFFFFFu;
        uint64_t t = 0;
        const uint64_t end = 5000 * MS;
        const uint64_t quiet = 3000 * MS;           // then still, so everything settles
        QuadTick tmp[2];

        while (t < end) {
            t += r.Between(50, 4000);
            sim.At(t);
            // the reference sees every raw call, like the scanner (poll calls included)
            for (int e = 0; e < encs; e++)
                ref[e].Update(g_now, pa[e], pb[e], tmp);
            uint32_t pick = r.Below(10);
            if (t >= quiet) {
                // quiet period: just time passing
            } else if (pick < 4 && keys > 0) {
                levels ^= 1u << r.Below((uint32_t)keys);
                sim.Keys(levels);
            } else if (encs > 0) {
                int e = (int)r.Below((uint32_t)encs);
                if (r.Below(2))
                    pa[e] = !pa[e];
                else
                    pb[e] = !pb[e];
                sim.Pins(e, pa[e], pb[e]);
                ref[e].Update(g_now, pa[e], pb[e], tmp);
            }
            // keep the reference polled on the same 1 ms grid as the scanner
            for (int e = 0; e < encs; e++)
                ref[e].Update(g_now, pa[e], pb[e], tmp);
        }
        sim.At(end + 500 * MS);

        Assert(g_decode_errors == 0);
        Assert(g_pk_n > 3);
        int hellos = 0;
        for (int i = 0; i < g_pk_n; i++) {
            Assert(g_pk[i].seq == (uint32_t)i);
            if (g_pk[i].cmd == PAD_CMD_HELLO) { hellos++; Assert(i < 3); }
        }
        Assert(hellos == 3);

        // Send policy
        const Pkt *prev = nullptr;
        for (int i = 0; i < g_pk_n; i++) {
            const Pkt &p = g_pk[i];
            if (p.cmd != PAD_CMD_STATE)
                continue;
            if (prev) {
                uint64_t gap = p.t - prev->t;
                Assert(gap <= 100 * MS + 1500);                 // never silent longer than the repeat
                bool keys_changed = p.state.keys != prev->state.keys;
                bool moved = false;
                for (int e = 0; e < encs; e++)
                    if (p.state.position[e] != prev->state.position[e])
                        moved = true;
                (void)moved;
                if (!keys_changed)
                    Assert(gap >= PAD_ENCODER_MIN_GAP_MS * MS);     // no key change: encoder cap or repeat
            }
            prev = &p;
        }

        // Settled: the last STATE is the truth
        Assert(prev);
        uint32_t want = 0;
        for (int k = 0; k < keys; k++)
            if (!((levels >> k) & 1))
                want |= 1u << k;
        Assert(prev->state.keys == want);
        for (int e = 0; e < encs; e++) {
            Assert(prev->state.position[e] == ref[e].Detents());
            Assert(prev->state.interval[e] == QuadIntervalToU16(ref[e].LastTickIntervalUs()));
        }
    }
}

Fact("PadScanner random input (policy invariants, settled state is the truth)")
{
    const char *env = getenv("FUZZ_SEEDS");
    int count = env && atoi(env) > 0 ? atoi(env) : 40;
    for (uint32_t seed = 1; seed <= (uint32_t)count; seed++) {
        try {
            FuzzOne(seed * 7919);
        } catch (const AssertionFailed &) {
            fprintf(stderr, "  (scanner fuzz seed %u)\n", seed * 7919);
            throw;
        }
    }
}
