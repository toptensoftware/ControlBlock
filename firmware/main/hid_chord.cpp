#include <string.h>
#include "hid_chord.h"
#include "hid_keys.gen.h"

namespace
{
    template <size_t N>
    bool Find(const HidName (&table)[N], const char *name, size_t len, uint8_t *value)
    {
        for (const HidName &e : table)
            if (strlen(e.name) == len && memcmp(e.name, name, len) == 0) {
                *value = e.value;
                return true;
            }
        return false;
    }
}

const char *HidParseChord(const char *text, size_t len, uint8_t *modifiers, uint8_t *key)
{
    *modifiers = 0;
    *key = 0;
    bool have_key = false;
    size_t i = 0;
    for (;;) {
        size_t end = i;
        while (end < len && text[end] != '+')
            end++;

        // The part [i, end) trimmed and lower cased
        size_t a = i, b = end;
        while (a < b && text[a] == ' ')
            a++;
        while (b > a && text[b - 1] == ' ')
            b--;
        if (b == a)
            return len == 0 ? "empty key chord" : "empty name in chord";
        char name[16];
        if (b - a >= sizeof(name))
            return "unknown key";
        for (size_t j = 0; j < b - a; j++) {
            char c = text[a + j];
            name[j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }

        uint8_t v;
        if (Find(HID_MODIFIER_NAMES, name, b - a, &v)) {
            if (*modifiers & v)
                return "modifier repeated in chord";
            *modifiers |= v;
        } else if (Find(HID_KEY_NAMES, name, b - a, &v)) {
            if (have_key)
                return "a chord has only one key besides modifiers";
            *key = v;
            have_key = true;
        } else {
            return "unknown key";
        }

        if (end >= len)
            return nullptr;
        i = end + 1;
        if (i >= len)
            return "empty name in chord";               // a trailing "+"
    }
}
