#include <string.h>
#include "clock_text.h"

#define PLACEHOLDER_LEN (sizeof(CLOCK_PLACEHOLDER) - 1)

size_t utf8_cut_len(const char *text, size_t max)
{
    if (!text)
        return 0;
    size_t len = strlen(text);
    if (len <= max)
        return len;
    // Back up off any continuation bytes: text[max] starts the next character
    len = max;
    while (len > 0 && ((uint8_t)text[len] & 0xC0) == 0x80)
        len--;
    return len;
}

size_t utf8_complete_len(const char *text, size_t len)
{
    // Find the last sequence's lead byte (at most 3 continuation bytes back)
    size_t i = len;
    while (i > 0 && len - i < 3 && ((uint8_t)text[i - 1] & 0xC0) == 0x80)
        i--;
    if (i == 0)
        return len;                         // nothing but continuation bytes: leave it
    uint8_t lead = (uint8_t)text[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return len - (i - 1) >= need ? len : i - 1;
}

void clock_format(char *dst, int hour24, int minute)
{
    int h = hour24 % 12;
    if (h == 0)
        h = 12;
    int i = 0;
    if (h >= 10)
        dst[i++] = '1';
    dst[i++] = (char)('0' + h % 10);
    dst[i++] = ':';
    dst[i++] = (char)('0' + minute / 10);
    dst[i++] = (char)('0' + minute % 10);
    dst[i] = 0;
}

void clock_text_expand(char *dst, size_t size, const char *tmpl, const char *clock)
{
    if (size == 0)
        return;

    // Expand into a scratch, then cut once on a character boundary
    char t[CLOCK_EXPAND_MAX];
    size_t n = 0, cap = sizeof(t) - 1;
    size_t clock_len = clock ? strlen(clock) : 0;
    for (const char *p = tmpl ? tmpl : ""; *p && n < cap; ) {
        if (strncmp(p, CLOCK_PLACEHOLDER, PLACEHOLDER_LEN) == 0) {
            size_t c = clock_len < cap - n ? clock_len : cap - n;
            memcpy(t + n, clock, c);
            n += c;
            p += PLACEHOLDER_LEN;
        } else {
            t[n++] = *p++;
        }
    }
    t[n] = 0;

    size_t len = utf8_cut_len(t, size - 1);
    memcpy(dst, t, len);
    dst[len] = 0;
}

bool clock_text_has_clock(const char *tmpl)
{
    return tmpl && strstr(tmpl, CLOCK_PLACEHOLDER) != nullptr;
}

int64_t clock_wait_ms(int64_t unix_ms)
{
    const int64_t minute = 60000;
    int64_t into = unix_ms % minute;
    if (into < 0)
        into += minute;
    return minute - into + CLOCK_WAKE_MARGIN_MS;
}

bool clock_time_valid(int64_t unix_ms)
{
    return unix_ms >= CLOCK_VALID_FROM_MS;
}
