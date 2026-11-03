// Config blob loader: validation and relocation of the BPAK blob that
// tools/bake writes (docs/standalone.md "Loading"). The sample is baked from
// data/sample_config.json, so these tests also check that the loader's idea
// of the layout matches the real packer.

#include <stdlib.h>
#include "UnitTesting.h"
#include "Core.h"
#include "rle565.h"
#include "config_env.h"

using namespace SimpleLib;
using namespace config_env;

namespace
{
    ConfigError Validate(const List<uint8_t> &b, ConfigSummary *s = nullptr)
    {
        Guarded g(b);
        ConfigSummary tmp;
        return ConfigValidate(g.Data(), g.Size(), s ? s : &tmp);
    }

    // Offset of the first op with `code` in Home's bindings, or 0
    uint32_t FindHomeOp(const List<uint8_t> &b, uint32_t code)
    {
        uint32_t n = U32(b, CB_HOME_BINDINGS), bindings = U32(b, CB_HOME_BINDINGS + 4);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t bind = bindings + i * 16;
            uint32_t ops = U32(b, bind + 12);
            for (uint32_t k = 0; k < U32(b, bind + 8); k++) {
                uint32_t op = U32(b, ops + k * 4);
                if (U32(b, op) == code)
                    return op;
            }
        }
        return 0;
    }

    // The sample with one 32-bit word changed
    List<uint8_t> With(uint32_t off, uint32_t v)
    {
        List<uint8_t> b = LoadSample();
        Put32(b, off, v);
        return b;
    }

    uint32_t Table(const List<uint8_t> &b) { return TableOffset(b); }
}

Fact("ConfigBlob Sample Is Valid")
{
    List<uint8_t> b = LoadSample();
    ConfigSummary s = {};
    Assert(Validate(b, &s) == ConfigError::Ok);
    Assert(s.version == CONFIG_BLOB_VERSION);
    Assert(s.long_press_ms == 650);
    Assert(s.device_count == 2);
    Assert(s.activity_count == 2);

    // Home key 0: face, icon, pressed face, and the icon again as the pressed icon
    Assert(s.home[0][KEY_SLOT_BG].offset != 0 && s.home[0][KEY_SLOT_BG].size > 0);
    Assert(s.home[0][KEY_SLOT_FG].offset != 0);
    Assert(s.home[0][KEY_SLOT_PRESSED_BG].offset != 0);
    Assert(s.home[0][KEY_SLOT_PRESSED_FG].offset == s.home[0][KEY_SLOT_FG].offset);
    // Shared images are stored once: the same offset from every key
    Assert(s.home[1][KEY_SLOT_BG].offset == s.home[0][KEY_SLOT_BG].offset);
    Assert(s.home[1][KEY_SLOT_PRESSED_BG].offset == s.home[0][KEY_SLOT_PRESSED_BG].offset);
    Assert(s.home[1][KEY_SLOT_FG].offset != s.home[0][KEY_SLOT_FG].offset);
    // Absent images, and a face-only key
    for (int slot = 0; slot < KEY_SLOTS; slot++)
        Assert(s.home[3][slot].offset == 0 && s.home[3][slot].size == 0);
    Assert(s.home[4][KEY_SLOT_BG].offset != 0 && s.home[4][KEY_SLOT_FG].offset == 0);
    Assert(s.home[14][KEY_SLOT_BG].offset == 0);

    // Every reported image is 4-aligned, inside the data, and a real image
    for (int k = 0; k < COMP_KEYS; k++) {
        for (int slot = 0; slot < KEY_SLOTS; slot++) {
            ConfigImage im = s.home[k][slot];
            if (!im.offset)
                continue;
            Assert(im.offset % 4 == 0 && im.offset + im.size <= Table(b));
            const void *px = b.GetBuffer() + im.offset;
            Assert(check_image(px, im.size, nullptr));
            Assert(get_image_width(px) == COMP_KEY_SIZE && get_image_height(px) == COMP_KEY_SIZE);
        }
    }
}

Fact("ConfigBlob Summary Is Untouched On Error")
{
    List<uint8_t> b = LoadSample();
    Put32(b, 0, 0);
    ConfigSummary s;
    memset(&s, 0x5A, sizeof(s));
    Assert(Validate(b, &s) == ConfigError::BadSignature);
    Assert(((uint8_t *)&s)[0] == 0x5A && ((uint8_t *)&s)[sizeof(s) - 1] == 0x5A);
}

Fact("ConfigBlob Relocate Adds The Base To Table Slots Only")
{
    List<uint8_t> b = LoadSample();
    List<uint8_t> r = LoadSample();
    ConfigRelocate(r.GetBuffer(), (uint32_t)r.GetCount(), 0x3C000000);

    List<uint32_t> relocs = Relocs(b);
    Assert(relocs.GetCount() > 50);
    for (int i = 0; i < relocs.GetCount(); i++)
        Assert(U32(r, relocs[i]) == U32(b, relocs[i]) + 0x3C000000);

    // Every other byte is as it was
    List<uint8_t> expect = LoadSample();
    for (int i = 0; i < relocs.GetCount(); i++)
        Put32(expect, relocs[i], U32(r, relocs[i]));
    Assert(memcmp(expect.GetBuffer(), r.GetBuffer(), b.GetCount()) == 0);
}

Fact("ConfigBlob Relocate By Zero Changes Nothing")
{
    List<uint8_t> b = LoadSample();
    List<uint8_t> r = LoadSample();
    ConfigRelocate(r.GetBuffer(), (uint32_t)r.GetCount(), 0);
    Assert(memcmp(b.GetBuffer(), r.GetBuffer(), b.GetCount()) == 0);
}

Fact("ConfigBlob Rejects Truncated Files")
{
    List<uint8_t> b = LoadSample();
    for (uint32_t n : { 0u, 1u, 16u, 31u, 32u, 100u, 604u }) {
        Guarded g(b.GetBuffer(), n);
        ConfigSummary s;
        ConfigError e = ConfigValidate(g.Data(), g.Size(), &s);
        Assert(e == ConfigError::Truncated || e == ConfigError::BadHeader);
    }
    // Every truncation point fails, without reading past the end
    for (uint32_t n = 0; n < (uint32_t)b.GetCount(); n += 61) {
        Guarded g(b.GetBuffer(), n);
        ConfigSummary s;
        Assert(ConfigValidate(g.Data(), g.Size(), &s) != ConfigError::Ok);
    }
    // ...and the last byte
    {
        Guarded g(b.GetBuffer(), (uint32_t)b.GetCount() - 1);
        ConfigSummary s;
        Assert(ConfigValidate(g.Data(), g.Size(), &s) != ConfigError::Ok);
    }
}

Fact("ConfigBlob Rejects Trailing Bytes")
{
    List<uint8_t> b = LoadSample();
    b.Add(0); b.Add(0); b.Add(0); b.Add(0);
    Assert(Validate(b) == ConfigError::BadHeader);
}

Fact("ConfigBlob Rejects A Bad Header")
{
    Assert(Validate(With(0, 0x4B415043)) == ConfigError::BadSignature);        // "CPAK"
    Assert(Validate(With(0, 0x34365042)) == ConfigError::BadSignature);        // "BP64"
    Assert(Validate(With(4, 2)) == ConfigError::BadVersion);
    Assert(Validate(With(4, 0)) == ConfigError::BadVersion);
    Assert(Validate(With(CB_ROOT, 2)) == ConfigError::BadVersion);             // the root's version

    List<uint8_t> b = LoadSample();
    uint32_t table = Table(b), count = RelocCount(b);
    Assert(Validate(With(8, count + 1)) == ConfigError::BadHeader);            // table runs off the end
    Assert(Validate(With(8, count - 1)) == ConfigError::BadHeader);            // gap before the end
    Assert(Validate(With(8, 0xFFFFFFFF)) == ConfigError::BadHeader);           // count * 4 overflows
    Assert(Validate(With(12, table + 4)) == ConfigError::BadHeader);
    Assert(Validate(With(12, table - 1)) == ConfigError::BadHeader);           // unaligned
    Assert(Validate(With(12, 0)) == ConfigError::BadHeader);
    Assert(Validate(With(12, 0xFFFFFFFC)) == ConfigError::BadHeader);
    Assert(Validate(With(12, 40)) == ConfigError::BadHeader);                  // no room for the root
}

Fact("ConfigBlob Rejects Bad Relocation Entries")
{
    List<uint8_t> b = LoadSample();
    uint32_t table = Table(b);
    List<uint32_t> relocs = Relocs(b);
    int last = relocs.GetCount() - 1;

    auto with = [&](int index, uint32_t v) {
        List<uint32_t> r = Relocs(b);
        r.GetBuffer()[index] = v;
        return WithRelocs(b, r);
    };
    Assert(Validate(with(last, table)) == ConfigError::BadRelocation);         // past the data
    Assert(Validate(with(last, table - 2)) == ConfigError::BadRelocation);     // unaligned
    Assert(Validate(with(last, 0xFFFFFFFC)) == ConfigError::BadRelocation);
    Assert(Validate(with(0, 8)) == ConfigError::BadRelocation);                // into the header
    Assert(Validate(with(5, relocs[4])) == ConfigError::BadRelocation);        // duplicate
    Assert(Validate(with(4, relocs[5] + 4)) == ConfigError::BadRelocation);    // out of order

    // A slot that isn't a pointer in the structure (the root's version word)
    {
        List<uint32_t> r = Relocs(b);
        r.InsertAt(0, CB_ROOT);
        Assert(Validate(WithRelocs(b, r)) == ConfigError::BadRelocation);
    }
    // Inside image data
    {
        ConfigSummary s;
        Assert(Validate(b, &s) == ConfigError::Ok);
        uint32_t inside = (s.home[0][KEY_SLOT_BG].offset + 64) & ~3u;
        List<uint32_t> r = Relocs(b);
        int at = 0;
        while (at < r.GetCount() && r[at] < inside)
            at++;
        r.InsertAt(at, inside);
        Assert(Validate(WithRelocs(b, r)) == ConfigError::BadRelocation);
    }
    // A pointer slot missing from the table (it would stay a file offset)
    for (int i : { 0, 7, 20, last / 2, last }) {
        List<uint32_t> r = Relocs(b);
        r.RemoveAt(i);
        Assert(Validate(WithRelocs(b, r)) == ConfigError::BadRelocation);
    }
    // A null pointer in the table would become a bogus non-null one
    {
        uint32_t null_slot = CB_KEY_PTR(CB_HOME_KEY(3), 0);
        Assert(U32(b, null_slot) == 0);
        List<uint32_t> r = Relocs(b);
        int at = 0;
        while (at < r.GetCount() && r[at] < null_slot)
            at++;
        r.InsertAt(at, null_slot);
        Assert(Validate(WithRelocs(b, r)) == ConfigError::BadRelocation);
    }
}

Fact("ConfigBlob Rejects Bad Pointers And Counts")
{
    List<uint8_t> b = LoadSample();
    uint32_t table = Table(b);
    uint32_t k0 = CB_HOME_KEY(0);

    Assert(Validate(With(CB_KEY_PTR(k0, 0), table + 4)) == ConfigError::BadPointer);
    Assert(Validate(With(CB_KEY_PTR(k0, 0), 8)) == ConfigError::BadPointer);          // into the header
    Assert(Validate(With(CB_KEY_COUNT(k0, 0), 0xFFFFFFF0)) == ConfigError::BadPointer);
    Assert(Validate(With(CB_KEY_COUNT(k0, 0), table)) == ConfigError::BadPointer);
    // A pointer that's set while its count is zero is still a pointer into the data
    Assert(Validate(With(CB_KEY_COUNT(CB_HOME_KEY(1), 1), 0)) != ConfigError::Ok);
    // Home bindings, devices and activities
    Assert(Validate(With(CB_HOME_BINDINGS, 5000)) == ConfigError::BadLimit);
    Assert(Validate(With(CB_HOME_BINDINGS + 4, table)) == ConfigError::BadPointer);
    Assert(Validate(With(CB_ROOT_DEVICES + 4, table - 4)) == ConfigError::BadPointer);
    Assert(Validate(With(CB_ROOT_ACTIVITIES + 4, table - 8)) == ConfigError::BadPointer);
}

Fact("ConfigBlob Rejects Strings Without A Terminator")
{
    List<uint8_t> b = LoadSample();
    uint32_t table = Table(b);
    // The last thing in the data is a script's source (and its padding)
    for (uint32_t i = table - 8; i < table; i++)
        b.GetBuffer()[i] = 'x';
    Assert(Validate(b) == ConfigError::BadPointer);
}

Fact("ConfigBlob Enforces Limits")
{
    List<uint8_t> b = LoadSample();
    uint32_t acts = U32(b, CB_ROOT_ACTIVITIES + 4);
    // The first activity: devices at +8, pages at +16, bindings at +24
    Assert(Validate(With(CB_ROOT_DEVICES, CONFIG_MAX_DEVICES + 1)) == ConfigError::BadLimit);
    Assert(Validate(With(CB_ROOT_ACTIVITIES, CONFIG_MAX_ACTIVITIES + 1)) == ConfigError::BadLimit);
    Assert(Validate(With(acts + 16, CONFIG_MAX_ACTIVITY_PAGES + 1)) == ConfigError::BadLimit);
    Assert(Validate(With(acts + 16, 0)) == ConfigError::BadLimit);            // an activity needs a page
    Assert(Validate(With(acts + 24, CONFIG_MAX_BINDINGS + 1)) == ConfigError::BadLimit);
    Assert(Validate(With(acts + 8, CONFIG_MAX_DEVICES + 1)) == ConfigError::BadLimit);

    // An op list over the limit (the first activity's didActivate: +48)
    Assert(Validate(With(acts + 48, CONFIG_MAX_OPS + 1)) == ConfigError::BadLimit);
}

Fact("ConfigBlob Rejects Bad Ops")
{
    List<uint8_t> b = LoadSample();
    uint32_t hid = FindHomeOp(b, 3);
    uint32_t sw  = FindHomeOp(b, 2);
    uint32_t del = FindHomeOp(b, 1);
    Assert(hid && sw && del);

    Assert(Validate(With(hid, 0)) == ConfigError::BadOp);
    Assert(Validate(With(hid, 9)) == ConfigError::BadOp);
    Assert(Validate(With(hid, 0xFFFFFFFF)) == ConfigError::BadOp);
    Assert(Validate(With(hid + 4, 4)) == ConfigError::BadOp);                  // hid mode 0..3
    Assert(Validate(With(sw + 4, 2)) == ConfigError::BadIndex);                // activity 2 of 2
    Assert(Validate(With(sw + 4, 0xFFFFFFFF)) == ConfigError::BadIndex);

    // An op pointer too close to the end for its struct
    {
        uint32_t bindings = U32(b, CB_HOME_BINDINGS + 4);
        uint32_t ops = U32(b, bindings + 12);
        Assert(Validate(With(ops, Table(b) - 4)) != ConfigError::Ok);
    }
    // An activity's device index past the device list
    {
        uint32_t acts = U32(b, CB_ROOT_ACTIVITIES + 4);
        uint32_t devs = U32(b, acts + 12);
        Assert(Validate(With(devs, 2)) == ConfigError::BadIndex);
        Assert(Validate(With(devs, 0xFFFFFFFF)) == ConfigError::BadIndex);
    }
}

Fact("ConfigBlob Rejects Bad Images")
{
    List<uint8_t> b = LoadSample();
    ConfigSummary s;
    Assert(Validate(b, &s) == ConfigError::Ok);
    uint32_t k0 = CB_HOME_KEY(0);
    ConfigImage face = s.home[0][KEY_SLOT_BG];

    // Not 4-aligned (the slot stays a valid pointer into the data)
    Assert(Validate(With(CB_KEY_PTR(k0, 0), face.offset + 2)) == ConfigError::BadAlignment);

    // Too short for its data
    Assert(Validate(With(CB_KEY_COUNT(k0, 0), face.size - 8)) == ConfigError::BadImage);
    Assert(Validate(With(CB_KEY_COUNT(k0, 0), 8)) == ConfigError::BadImage);

    // Wrong size: the header has the wrong width or height
    for (uint32_t field : { 8u, 12u }) {            // width, height
        List<uint8_t> c = LoadSample();
        uint32_t v = U32(c, face.offset + field);
        Put32(c, face.offset + field, v + 1);
        Assert(Validate(c) == ConfigError::BadImage);
    }

    // Runs that go past the data
    {
        List<uint8_t> c = LoadSample();
        uint32_t end = face.offset + face.size;
        for (uint32_t i = face.offset + 16; i < end; i++)
            c.GetBuffer()[i] = 0xFF;
        Assert(Validate(c) == ConfigError::BadImage);
    }

    // Garbage instead of an image
    {
        List<uint8_t> c = LoadSample();
        for (uint32_t i = 0; i < 64; i++)
            c.GetBuffer()[face.offset + i] = (uint8_t)(i * 37 + 1);
        Assert(Validate(c) == ConfigError::BadImage);
    }
}

Fact("ConfigBlob Rejects A Misaligned Blob")
{
    List<uint8_t> b = LoadSample();
    Guarded g(b.GetBuffer(), (uint32_t)b.GetCount() + 8);
    memmove(g.Data() + 1, g.Data(), b.GetCount());
    ConfigSummary s;
    Assert(ConfigValidate(g.Data() + 1, (uint32_t)b.GetCount(), &s) == ConfigError::BadAlignment);
}

Fact("ConfigBlob Names Every Error")
{
    for (int e = 0; e <= (int)ConfigError::BadImage; e++)
        Assert(ConfigErrorName((ConfigError)e)[0] != 0);
}

// Random damage to the sample: validation must never fault (the buffer is
// followed by a guard page) and anything it accepts must relocate cleanly.
Fact("ConfigBlob Fuzz Never Faults")
{
    int seeds = 400;
    if (const char *env = getenv("FUZZ_SEEDS"))
        seeds = atoi(env) * 10;
    uint32_t state = 12345;
    auto next = [&]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };

    List<uint8_t> good = LoadSample();
    uint32_t size = (uint32_t)good.GetCount();
    int accepted = 0;

    for (int i = 0; i < seeds; i++) {
        List<uint8_t> b = LoadSample();
        int edits = 1 + next() % 4;
        for (int e = 0; e < edits; e++) {
            // Hit the structures more often than the images
            uint32_t at = next() % 4 == 0 ? next() % size : next() % 620;
            switch (next() % 4) {
            case 0: b.GetBuffer()[at] = (uint8_t)next(); break;
            case 1: b.GetBuffer()[at] ^= 1 << (next() % 8); break;
            case 2: Put32(b, at & ~3u, next()); break;
            default: Put32(b, at & ~3u, next() % (size + 64)); break;      // plausible offsets
            }
        }
        uint32_t len = next() % 8 == 0 ? next() % size : size;
        Guarded g(b.GetBuffer(), len);
        ConfigSummary s;
        if (ConfigValidate(g.Data(), g.Size(), &s) == ConfigError::Ok) {
            accepted++;
            ConfigRelocate(g.Data(), g.Size(), 0x10000000);
        }
    }
    // Most edits land in places that matter, but some are harmless (padding, reserved bytes)
    Assert(accepted < seeds);
}
