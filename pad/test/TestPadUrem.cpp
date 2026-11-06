// Differential test against urem: data/urem_vectors.json (generated once by
// gen-urem-vectors.mjs from a port of urem's updateEncoder and a model of pigpio's glitch
// filter, and committed) holds timestamped streams and the ticks/intervals urem gives.
// The C++ GlitchFilter, QuadDecoder and EncoderChannel must reproduce them exactly. The
// test reads the committed file; it never runs node.
//
// File layout: a JSON object whose "cases" array has one case per line, so this reader
// needs only a scanner for the integers of known keys (no STL, no JSON library).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "encoder_channel.h"

using namespace SimpleLib;

namespace
{
    // Growable list of int64
    struct IntList
    {
        int64_t *v = nullptr;
        int n = 0, cap = 0;
        ~IntList() { free(v); }
        void Clear() { n = 0; }
        void Add(int64_t x)
        {
            if (n == cap) {
                cap = cap ? cap * 2 : 256;
                v = (int64_t *)realloc(v, cap * sizeof(int64_t));
                Assert(v != nullptr);
            }
            v[n++] = x;
        }
    };

    char *g_text = nullptr;

    void LoadVectors()
    {
        if (g_text)
            return;
        static const char *paths[] = { "data/urem_vectors.json", "pad/test/data/urem_vectors.json", "../data/urem_vectors.json" };
        FILE *f = nullptr;
        for (const char *p : paths)
            if ((f = fopen(p, "rb")) != nullptr)
                break;
        if (!f) {
            fprintf(stderr, "Can't find data/urem_vectors.json (run from pad/test)\n");
            exit(2);
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        g_text = (char *)malloc(size + 1);
        if (fread(g_text, 1, size, f) != (size_t)size)
            exit(2);
        g_text[size] = 0;
        fclose(f);
    }

    // Finds "key": inside the line [line, end) and returns the text after it, or null
    const char *FindKey(const char *line, const char *end, const char *key)
    {
        char pat[64];
        snprintf(pat, sizeof(pat), "\"%s\":", key);
        size_t plen = strlen(pat);
        for (const char *p = line; p + plen <= end; p++)
            if (memcmp(p, pat, plen) == 0)
                return p + plen;
        return nullptr;
    }

    int64_t ReadInt(const char *p)
    {
        return (int64_t)strtoll(p, nullptr, 10);
    }

    // Reads every integer of the (possibly nested) array starting at p ('[') into 'out'
    void ReadInts(const char *p, IntList &out)
    {
        out.Clear();
        Assert(*p == '[');
        int depth = 0;
        while (*p) {
            if (*p == '[') {
                depth++;
                p++;
            } else if (*p == ']') {
                if (--depth == 0)
                    return;
                p++;
            } else if ((*p >= '0' && *p <= '9') || *p == '-') {
                char *e;
                out.Add(strtoll(p, &e, 10));
                p = e;
            } else {
                p++;
            }
        }
        Assert(false);
    }

    // The name, for failure messages
    void ReadName(const char *line, const char *end, char *out, size_t cap)
    {
        const char *p = FindKey(line, end, "name");
        out[0] = 0;
        if (!p || *p != '"')
            return;
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < cap)
            out[i++] = *p++;
        out[i] = 0;
    }

    struct Counts { int quad = 0, pipe = 0, filter = 0, ticks = 0; };

    // ---- one case of each kind -------------------------------------------------------

    void RunQuad(const char *line, const char *end, Counts &c)
    {
        IntList ev, ticks;
        ReadInts(FindKey(line, end, "events"), ev);
        ReadInts(FindKey(line, end, "ticks"), ticks);
        bool a0 = ReadInt(FindKey(line, end, "a0")) != 0;
        bool b0 = ReadInt(FindKey(line, end, "b0")) != 0;
        uint32_t detents = (uint32_t)ReadInt(FindKey(line, end, "detents"));
        uint32_t interval = (uint32_t)ReadInt(FindKey(line, end, "interval_us"));

        QuadDecoder q;
        q.Init(a0, b0);
        int got = 0;
        Assert(ev.n % 3 == 0 && ticks.n % 3 == 0);
        for (int i = 0; i < ev.n / 3; i++) {
            int tick = q.Edge((uint64_t)ev.v[i * 3], ev.v[i * 3 + 1] != 0, ev.v[i * 3 + 2] != 0);
            if (got < ticks.n / 3 && ticks.v[got * 3] == i) {
                Assert(tick == ticks.v[got * 3 + 1]);
                Assert((int64_t)q.IntervalUs() == ticks.v[got * 3 + 2]);
                got++;
            } else {
                Assert(tick == 0);
            }
        }
        Assert(got == ticks.n / 3);
        Assert(q.Detents() == detents);
        Assert(q.IntervalUs() == interval);
        c.quad++;
        c.ticks += got;
    }

    void RunPipe(const char *line, const char *end, Counts &c)
    {
        IntList ev, ticks;
        ReadInts(FindKey(line, end, "events"), ev);
        ReadInts(FindKey(line, end, "ticks"), ticks);
        bool a = ReadInt(FindKey(line, end, "a0")) != 0;
        bool b = ReadInt(FindKey(line, end, "b0")) != 0;
        uint32_t filter = (uint32_t)ReadInt(FindKey(line, end, "filter_us"));
        uint32_t detents = (uint32_t)ReadInt(FindKey(line, end, "detents"));

        EncoderChannel ch;
        ch.Init(filter, a, b);
        int got = 0;
        uint64_t last_t = 0;
        auto Check = [&](int n, const QuadTick *out) {
            for (int k = 0; k < n; k++, got++) {
                Assert(got < ticks.n / 3);
                Assert((int64_t)out[k].time_us == ticks.v[got * 3]);
                Assert(out[k].dir == ticks.v[got * 3 + 1]);
                Assert((int64_t)out[k].interval_us == ticks.v[got * 3 + 2]);
            }
        };
        Assert(ev.n % 3 == 0);
        for (int i = 0; i < ev.n / 3; i++) {
            last_t = (uint64_t)ev.v[i * 3];
            if (ev.v[i * 3 + 1] == 0) a = ev.v[i * 3 + 2] != 0; else b = ev.v[i * 3 + 2] != 0;
            QuadTick out[2];
            Check(ch.Update(last_t, a, b, out), out);
        }
        // Let the last pending changes settle
        for (uint64_t t = last_t + 1000; t <= last_t + 3000; t += 1000) {
            QuadTick out[2];
            Check(ch.Update(t, a, b, out), out);
        }
        Assert(got == ticks.n / 3);
        Assert(ch.Detents() == detents);
        c.pipe++;
        c.ticks += got;
    }

    void RunFilter(const char *line, const char *end, Counts &c)
    {
        IntList ev, acc;
        ReadInts(FindKey(line, end, "events"), ev);
        ReadInts(FindKey(line, end, "accepted"), acc);
        bool init = ReadInt(FindKey(line, end, "init")) != 0;
        uint32_t steady = (uint32_t)ReadInt(FindKey(line, end, "steady_us"));

        GlitchFilter f;
        f.Init(steady, init);
        int got = 0;
        auto Check = [&]() {
            Assert(got < acc.n / 2);
            Assert((int64_t)f.EdgeTime() == acc.v[got * 2]);
            Assert((acc.v[got * 2 + 1] != 0) == f.Level());
            got++;
        };
        uint64_t last_t = 0;
        for (int i = 0; i < ev.n / 2; i++) {
            last_t = (uint64_t)ev.v[i * 2];
            if (f.Update(last_t, ev.v[i * 2 + 1] != 0))
                Check();
        }
        bool level = ev.n ? ev.v[ev.n - 1] != 0 : init;
        for (uint64_t t = last_t + steady; t <= last_t + 2 * (uint64_t)steady; t += steady)
            if (f.Update(t, level))
                Check();
        Assert(got == acc.n / 2);
        c.filter++;
    }
}

Fact("urem vectors: glitch filter, quadrature decoder and encoder channel match urem")
{
    LoadVectors();
    Assert(strstr(g_text, "\"format\":\"urem-vectors-1\"") != nullptr);

    Counts c;
    int line_no = 0;
    const char *p = g_text;
    while (*p) {
        const char *end = strchr(p, '\n');
        if (!end)
            end = p + strlen(p);
        if (*p == '{' && p[1] == '"' && memcmp(p, "{\"kind\":", 8) == 0) {
            char name[96];
            ReadName(p, end, name, sizeof(name));
            try {
                if (memcmp(p + 8, "\"quad\"", 6) == 0)
                    RunQuad(p, end, c);
                else if (memcmp(p + 8, "\"pipe\"", 6) == 0)
                    RunPipe(p, end, c);
                else if (memcmp(p + 8, "\"filter\"", 8) == 0)
                    RunFilter(p, end, c);
                else
                    Assert(false);
            } catch (const AssertionFailed &) {
                fprintf(stderr, "  (urem vector line %d: %s)\n", line_no, name);
                throw;
            }
        }
        line_no++;
        p = *end ? end + 1 : end;
    }

    printf("  urem vectors: %d quad, %d pipe, %d filter cases, %d ticks\n", c.quad, c.pipe, c.filter, c.ticks);
    Assert(c.quad > 30 && c.pipe > 20 && c.filter > 10);
    Assert(c.ticks > 1000);
}
