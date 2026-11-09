#include "UnitTesting.h"
#include "Core.h"
#include "clock_text.h"

using namespace SimpleLib;

namespace
{
    char fmt[CLOCK_TEXT_MAX];

    bool Format(int h24, int m, const char *want)
    {
        clock_format(fmt, h24, m);
        return strcmp(fmt, want) == 0;
    }

    char out[64];

    const char *Expand(const char *tmpl, const char *clock, size_t size = 48)
    {
        memset(out, 0xAB, sizeof(out));
        clock_text_expand(out, size, tmpl, clock);
        return out;
    }

    const int64_t MINUTE = 60000;
    const int64_t T2025 = 1735689600000LL;      // 2025-01-01 00:00:00 UTC
}

// ---------------------------------------------------------------------------
// clock_format: 12 hour, no am/pm, no leading zero

Fact("ClockText Format Midnight And Noon Are 12")
{
    Assert(Format(0, 0, "12:00"));
    Assert(Format(12, 0, "12:00"));
    Assert(Format(12, 30, "12:30"));
    Assert(Format(0, 59, "12:59"));
}

Fact("ClockText Format Hours")
{
    Assert(Format(2, 5, "2:05"));
    Assert(Format(14, 5, "2:05"));
    Assert(Format(11, 59, "11:59"));
    Assert(Format(23, 59, "11:59"));
    Assert(Format(1, 0, "1:00"));
    Assert(Format(13, 0, "1:00"));
}

// ---------------------------------------------------------------------------
// clock_text_expand

Fact("ClockText Expand Replaces Every Clock")
{
    Assert(strcmp(Expand("{clock}", "2:05"), "2:05") == 0);
    Assert(strcmp(Expand("Time {clock}", "2:05"), "Time 2:05") == 0);
    Assert(strcmp(Expand("{clock} - {clock}", "2:05"), "2:05 - 2:05") == 0);
    Assert(strcmp(Expand("{clock}{clock}", "12:30"), "12:3012:30") == 0);
}

Fact("ClockText Expand Leaves Other Braces")
{
    Assert(strcmp(Expand("{clk}", "2:05"), "{clk}") == 0);
    Assert(strcmp(Expand("{", "2:05"), "{") == 0);
    Assert(strcmp(Expand("a{clock", "2:05"), "a{clock") == 0);
    Assert(strcmp(Expand("{Clock}", "2:05"), "{Clock}") == 0);
    Assert(strcmp(Expand("{{clock}}", "2:05"), "{2:05}") == 0);
    Assert(strcmp(Expand("}{", "2:05"), "}{") == 0);
}

Fact("ClockText Expand Plain And Empty")
{
    Assert(strcmp(Expand("AppleTV", "2:05"), "AppleTV") == 0);
    Assert(strcmp(Expand("", "2:05"), "") == 0);
    Assert(strcmp(Expand(nullptr, "2:05"), "") == 0);
}

Fact("ClockText Expand Unknown Clock Is Blank")
{
    Assert(strcmp(Expand("{clock}", ""), "") == 0);
    Assert(strcmp(Expand("{clock}", nullptr), "") == 0);
    Assert(strcmp(Expand("At {clock}.", ""), "At .") == 0);
}

Fact("ClockText Expand Cuts To Size")
{
    // 45 'a' + "{clock}" expands to 45 + 5 bytes: cut to 47
    char tmpl[64];
    memset(tmpl, 'a', 45);
    strcpy(tmpl + 45, "{clock}");
    Expand(tmpl, "12:30");
    Assert(strlen(out) == 47);
    Assert(memcmp(out + 45, "12", 2) == 0);

    // A small buffer
    Assert(strcmp(Expand("{clock}", "12:30", 4), "12:") == 0);
    Assert(strcmp(Expand("abc", "12:30", 1), "") == 0);
}

Fact("ClockText Expand Cuts On A Character Boundary")
{
    // "{clock}" then 2-byte characters: the cut mustn't split one
    char tmpl[80] = "{clock}";
    for (int i = 0; i < 30; i++) {
        tmpl[7 + i * 2]     = (char)0xC3;      // U+00E9
        tmpl[7 + i * 2 + 1] = (char)0xA9;
    }
    tmpl[67] = 0;
    Expand(tmpl, "2:05");                       // 4 + 60 bytes
    size_t len = strlen(out);
    Assert(len == 46);                          // 4 + 21 whole characters
    Assert(((uint8_t)out[len - 1] & 0xC0) == 0x80);
}

Fact("ClockText Utf8 Cut Length")
{
    Assert(utf8_cut_len("abc", 10) == 3);
    Assert(utf8_cut_len("abc", 3) == 3);
    Assert(utf8_cut_len("abc", 2) == 2);
    Assert(utf8_cut_len("abc", 0) == 0);
    Assert(utf8_cut_len(nullptr, 5) == 0);
    Assert(utf8_cut_len("a\xC3\xA9", 2) == 1);      // don't split U+00E9
    Assert(utf8_cut_len("a\xC3\xA9", 3) == 3);
    Assert(utf8_cut_len("\xE2\x82\xAC", 2) == 0);   // nor U+20AC
}

Fact("ClockText Utf8 Complete Length")
{
    Assert(utf8_complete_len("abc", 3) == 3);
    Assert(utf8_complete_len("", 0) == 0);
    Assert(utf8_complete_len("a\xC3\xA9", 3) == 3);         // whole U+00E9
    Assert(utf8_complete_len("a\xC3", 2) == 1);             // its lead byte alone
    Assert(utf8_complete_len("a\xE2\x94\x80", 4) == 4);     // whole U+2500
    Assert(utf8_complete_len("a\xE2\x94", 3) == 1);         // two of its three bytes
    Assert(utf8_complete_len("a\xE2", 2) == 1);
    Assert(utf8_complete_len("\xF0\x9F\x98", 3) == 0);      // three of a four-byte one
    Assert(utf8_complete_len("\xF0\x9F\x98\x80", 4) == 4);
}

Fact("ClockText Has Clock")
{
    Assert(clock_text_has_clock("{clock}"));
    Assert(clock_text_has_clock("x {clock} y"));
    Assert(clock_text_has_clock("{{clock}}"));
    Assert(!clock_text_has_clock("{clk}"));
    Assert(!clock_text_has_clock("{clock"));
    Assert(!clock_text_has_clock(""));
    Assert(!clock_text_has_clock(nullptr));
}

// ---------------------------------------------------------------------------
// clock_wait_ms: to the next minute boundary plus a margin

Fact("ClockText Wait To Next Minute")
{
    int64_t t = T2025 + 5 * MINUTE;
    Assert(clock_wait_ms(t) == MINUTE + CLOCK_WAKE_MARGIN_MS);              // on the boundary: a whole minute
    Assert(clock_wait_ms(t + 1) == MINUTE - 1 + CLOCK_WAKE_MARGIN_MS);
    Assert(clock_wait_ms(t - 10) == 10 + CLOCK_WAKE_MARGIN_MS);              // just before
    Assert(clock_wait_ms(t + CLOCK_WAKE_MARGIN_MS) == MINUTE);               // woken on time
    Assert(clock_wait_ms(0) == MINUTE + CLOCK_WAKE_MARGIN_MS);
    Assert(clock_wait_ms(30000) == 30000 + CLOCK_WAKE_MARGIN_MS);
}

// ---------------------------------------------------------------------------
// clock_time_valid: from 2025-01-01 UTC

Fact("ClockText Time Valid From 2025")
{
    Assert(clock_time_valid(T2025));
    Assert(clock_time_valid(T2025 + 1));
    Assert(!clock_time_valid(T2025 - 1));
    Assert(!clock_time_valid(0));
    Assert(!clock_time_valid(-1));
    Assert(clock_time_valid(1793836800000LL));                               // 2026-11-05
}

Fact("ClockText Expand In Place")
{
    char buf[48] = "a {clock} b {clock}";
    clock_text_expand(buf, sizeof(buf), buf, "12:30");
    Assert(strcmp(buf, "a 12:30 b 12:30") == 0);
}
