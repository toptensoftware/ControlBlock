#pragma once
#include <stddef.h>
#include <stdint.h>

// The status bar clock's text, platform independent (host tested): the
// `{clock}` placeholder, its 12 hour format, when to redraw and when the
// time is known. Turning UTC into local time is the device's (clock.cpp).

#define CLOCK_PLACEHOLDER       "{clock}"
#define CLOCK_TEXT_MAX          6           // "12:59" with the NUL
#define CLOCK_WAKE_MARGIN_MS    20          // wake this long after a minute turns over
#define CLOCK_VALID_FROM_MS     1735689600000LL     // 2025-01-01 00:00:00 UTC
#define CLOCK_EXPAND_MAX        128         // clock_text_expand() output is cut to this (NUL included)

// Where the time came from (TIME_GET's source byte). RAM only: unknown after a restart.
enum class ClockSource : uint8_t { None = 0, Host = 1, Ntp = 2 };

// Bytes of `text` that fit in `max` bytes without splitting a UTF-8 sequence
size_t utf8_cut_len(const char *text, size_t max);

// Local time (hour 0-23, minute) as "h:mm", 12 hour, no am/pm: "12:05", "2:30".
// `dst` holds CLOCK_TEXT_MAX bytes.
void clock_format(char *dst, int hour24, int minute);

// Copy `tmpl` to `dst` (size bytes, NUL included) with every `{clock}`
// replaced by `clock` ("" or NULL while the time isn't known); anything else,
// other braces included, is copied as it is. Cut to fit on a UTF-8 boundary.
// `dst` may be `tmpl` (expanded in place).
void clock_text_expand(char *dst, size_t size, const char *tmpl, const char *clock);

// `tmpl` holds `{clock}`
bool clock_text_has_clock(const char *tmpl);

// Milliseconds from `unix_ms` to just after the next minute boundary (UTC and
// local minutes turn over together: every zone offset is whole minutes)
int64_t clock_wait_ms(int64_t unix_ms);

// The clock has been set: it reads 2025 or later (it starts at 1970 at power-up)
bool clock_time_valid(int64_t unix_ms);
