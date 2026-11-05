// A script's hid.key("ctrl+shift+esc") chord: same names and rules as the bake (tools/bake/src/hid.js).

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "hid_chord.h"

using namespace SimpleLib;

namespace
{
    const char *ParseChord(const char *text, int *mods, int *key)
    {
        uint8_t m = 0xEE, k = 0xEE;
        const char *err = HidParseChord(text, strlen(text), &m, &k);
        *mods = m;
        *key = k;
        return err;
    }
}

Fact("HidChord Single Keys And Modifiers")
{
    int m, k;
    Assert(!ParseChord("a", &m, &k) && m == 0 && k == 0x04);
    Assert(!ParseChord("Z", &m, &k) && m == 0 && k == 0x1d);
    Assert(!ParseChord("0", &m, &k) && k == 0x27);
    Assert(!ParseChord("f5", &m, &k) && k == 0x3e);
    Assert(!ParseChord("f13", &m, &k) && k == 0x68);
    Assert(!ParseChord("enter", &m, &k) && k == 0x28);
    Assert(!ParseChord("shift", &m, &k) && m == 0x02 && k == 0);       // modifiers only
}

Fact("HidChord Chords Combine Modifiers With One Key")
{
    int m, k;
    Assert(!ParseChord("ctrl+shift+esc", &m, &k) && m == 0x03 && k == 0x29);
    Assert(!ParseChord(" Ctrl + C ", &m, &k) && m == 0x01 && k == 0x06);
    Assert(!ParseChord("win+r", &m, &k) && m == 0x08 && k == 0x15);
    Assert(!ParseChord("rctrl+ralt+delete", &m, &k) && m == 0x50 && k == 0x4c);
    Assert(!ParseChord("ctrl+plus", &m, &k) && m == 0x01 && k == 0x2e);
}

Fact("HidChord Rejects Bad Chords")
{
    int m, k;
    Assert(ParseChord("", &m, &k) != nullptr);
    Assert(ParseChord("ctrl+", &m, &k) != nullptr);
    Assert(ParseChord("+a", &m, &k) != nullptr);
    Assert(ParseChord("nope", &m, &k) != nullptr);
    Assert(ParseChord("a+b", &m, &k) != nullptr);                // two keys
    Assert(ParseChord("ctrl+ctrl+a", &m, &k) != nullptr);        // a modifier twice
    Assert(ParseChord("ctrl+lctrl+a", &m, &k) != nullptr);       // same bit by two names
}
