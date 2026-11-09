#include "UnitTesting.h"
#include "Core.h"
#include "log_ring.h"
#include <string.h>

using namespace SimpleLib;

// The diagnostics log ring: lines with sequence numbers, line-aligned eviction

static String ReadAll(LogRing &r, bool hasSince, uint32_t since, LogReadResult *res = nullptr, size_t cap = 4096)
{
    char out[4096];
    LogReadResult rr = r.Read(hasSince, since, out, cap);
    if (res)
        *res = rr;
    return String(out, (int)rr.len);
}

static bool Eq(const String &a, const char *b) { return strcmp(a.sz(), b) == 0; }

static void Put(LogRing &r, const char *s) { r.Append(s, strlen(s)); }

Fact("LogRing Empty Reads Nothing")
{
    uint8_t buf[256];
    LogRing r(buf, sizeof(buf));
    LogReadResult res;
    Assert(Eq(ReadAll(r, false, 0, &res), ""));
    Assert(res.next == 0 && !res.dropped);
}

Fact("LogRing Lines Get Sequence Numbers")
{
    uint8_t buf[256];
    LogRing r(buf, sizeof(buf));
    Put(r, "one\ntwo\n");
    LogReadResult res;
    Assert(Eq(ReadAll(r, false, 0, &res), "one\ntwo\n"));
    Assert(res.next == 2);
    Assert(Eq(ReadAll(r, true, 1, &res), "two\n"));
    Assert(res.next == 2);
    Assert(Eq(ReadAll(r, true, 2, &res), ""));
    Assert(res.next == 2 && !res.dropped);
}

Fact("LogRing Partial Lines Wait For The Newline")
{
    uint8_t buf[256];
    LogRing r(buf, sizeof(buf));
    Put(r, "hel");
    Assert(Eq(ReadAll(r, false, 0), ""));
    Put(r, "lo\r\nwor");
    Put(r, "ld\n");
    Assert(Eq(ReadAll(r, false, 0), "hello\nworld\n"));
}

Fact("LogRing Long Lines Are Truncated")
{
    uint8_t buf[1024];
    LogRing r(buf, sizeof(buf));
    char line[LOG_RING_LINE_MAX + 50];
    memset(line, 'x', sizeof(line));
    r.Append(line, sizeof(line));
    Put(r, "\n");
    String s = ReadAll(r, false, 0);
    Assert(s.GetLength() == LOG_RING_LINE_MAX + 1);
}

// "─" is 3 bytes: a cut that lands inside one drops the whole character
Fact("LogRing Long Lines Are Cut On A Character Boundary")
{
    uint8_t buf[1024];
    LogRing r(buf, sizeof(buf));
    char line[LOG_RING_LINE_MAX + 1];
    memset(line, 'x', LOG_RING_LINE_MAX - 2);
    line[LOG_RING_LINE_MAX - 2] = 0;
    Put(r, line);
    Put(r, "\xE2\x94\x80yy\n");                         // "─yy": the cut falls inside "─"
    String s = ReadAll(r, false, 0);
    Assert(s.GetLength() == LOG_RING_LINE_MAX - 2 + 1);
    Assert(strstr(s.sz(), "\xE2") == nullptr);
}

// A line that ends in half a character (a message cut short) loses the half
Fact("LogRing Drops A Half Character At The End Of A Line")
{
    uint8_t buf[256];
    LogRing r(buf, sizeof(buf));
    Put(r, "ab\xE2\x94\n");
    Put(r, "\xE2\x94\x80\xE2\x94\x80\n");                // whole characters stay
    Assert(Eq(ReadAll(r, false, 0), "ab\n\xE2\x94\x80\xE2\x94\x80\n"));
}

// A box drawn over several lines in one message keeps every line
Fact("LogRing Multi-Line Message Keeps Every Line")
{
    uint8_t buf[1024];
    LogRing r(buf, sizeof(buf));
    Put(r, "I (1230) tusb_desc: \n\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90\n\xE2\x94\x82 USB \xE2\x94\x82\n");
    Assert(Eq(ReadAll(r, false, 0),
              "I (1230) tusb_desc: \n\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90\n\xE2\x94\x82 USB \xE2\x94\x82\n"));
}

Fact("LogRing Wrap Evicts Whole Oldest Lines")
{
    uint8_t buf[LOG_RING_LINE_MAX + 64];
    LogRing r(buf, sizeof(buf));
    char line[32];
    for (int i = 1; i <= 100; i++)
    {
        snprintf(line, sizeof(line), "line %d\n", i);
        Put(r, line);
    }
    LogReadResult res;
    String s = ReadAll(r, false, 0, &res);
    Assert(res.next == 100);
    Assert(s.EndsWith("line 100\n"));
    // Starts on a whole line, consecutive to the end
    int first = r.FirstSeq();
    Assert(first > 1);
    snprintf(line, sizeof(line), "line %d\n", first);
    Assert(s.StartsWith(line));
    Assert(r.Count() == (uint32_t)(100 - first + 1));
}

Fact("LogRing Since Before The Oldest Reports Dropped")
{
    uint8_t buf[LOG_RING_LINE_MAX + 64];
    LogRing r(buf, sizeof(buf));
    char line[32];
    for (int i = 1; i <= 100; i++)
    {
        snprintf(line, sizeof(line), "line %d\n", i);
        Put(r, line);
    }
    LogReadResult res;
    String all = ReadAll(r, false, 0);
    Assert(Eq(ReadAll(r, true, 3, &res), all.sz()));
    Assert(res.dropped);
    // Exactly at the oldest - 1: nothing missed
    ReadAll(r, true, r.FirstSeq() - 1, &res);
    Assert(!res.dropped);
}

Fact("LogRing Since In The Future Returns Everything As Dropped")
{
    uint8_t buf[256];
    LogRing r(buf, sizeof(buf));
    Put(r, "a\nb\n");
    LogReadResult res;
    Assert(Eq(ReadAll(r, true, 500, &res), "a\nb\n"));
    Assert(res.dropped && res.next == 2);
}

Fact("LogRing Read Stops At A Whole Line When Out Is Full")
{
    uint8_t buf[256];
    LogRing r(buf, sizeof(buf));
    Put(r, "aaaa\nbbbb\ncccc\n");
    LogReadResult res;
    Assert(Eq(ReadAll(r, false, 0, &res, 11), "aaaa\nbbbb\n"));
    Assert(res.next == 2);
    Assert(Eq(ReadAll(r, true, res.next, &res), "cccc\n"));
}

Fact("LogRing Sequence Numbers Wrap")
{
    uint8_t buf[256];
    LogRing r(buf, sizeof(buf), 0xFFFFFFFE);
    Put(r, "a\nb\nc\n");               // 0xFFFFFFFE, 0xFFFFFFFF, 0
    LogReadResult res;
    Assert(Eq(ReadAll(r, true, 0xFFFFFFFE, &res), "b\nc\n"));
    Assert(res.next == 0 && !res.dropped);
    Assert(Eq(ReadAll(r, true, 0xFFFFFFFF, &res), "c\n"));
    Assert(Eq(ReadAll(r, true, 0, &res), ""));
}

Fact("LogRing Survives Many Wraps With Varied Lengths")
{
    uint8_t buf[LOG_RING_LINE_MAX * 3];
    LogRing r(buf, sizeof(buf));
    char line[LOG_RING_LINE_MAX + 2];
    uint32_t seed = 1;
    for (int i = 1; i <= 2000; i++)
    {
        seed = seed * 1103515245 + 12345;
        int n = (int)((seed >> 16) % (LOG_RING_LINE_MAX + 20));
        int p = snprintf(line, sizeof(line), "%d:", i);
        for (; p < n && p < LOG_RING_LINE_MAX; p++)
            line[p] = 'z';
        line[p++] = '\n';
        r.Append(line, p);

        LogReadResult res;
        String s = ReadAll(r, false, 0, &res);
        Assert(res.next == (uint32_t)i);
        char head[16];
        snprintf(head, sizeof(head), "%u:", r.FirstSeq());
        Assert(s.StartsWith(head));
    }
}
