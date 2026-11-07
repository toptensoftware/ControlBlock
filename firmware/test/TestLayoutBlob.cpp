// Layout blob loader: validation and relocation of the BPAK blob that
// host/src/bake writes (docs/standalone.md "Loading"). The sample is baked from
// data/sample_layout.json, so these tests also check that the loader's idea
// of the layout matches the real packer.

#include <stdlib.h>
#include "UnitTesting.h"
#include "Core.h"
#include "rle565.h"
#include "layout_env.h"

using namespace SimpleLib;
using namespace layout_env;

namespace
{
    LayoutError Validate(const List<uint8_t> &b, LayoutSummary *s = nullptr)
    {
        Guarded g(b);
        LayoutSummary tmp;
        return LayoutValidate(g.Data(), g.Size(), s ? s : &tmp);
    }

    // Offset of the `nth` (from 0) op with `code` in Home's bindings, or 0
    uint32_t FindHomeOp(const List<uint8_t> &b, uint32_t code, int nth = 0)
    {
        uint32_t n = U32(b, CB_HOME_BINDINGS), bindings = U32(b, CB_HOME_BINDINGS_PTR);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t bind = bindings + i * (uint32_t)sizeof(layoutdata::binding);
            uint32_t ops = U32(b, bind + CB_OFF(binding, ops));
            for (uint32_t k = 0; k < U32(b, bind + CB_OFF(binding, ops_count)); k++) {
                uint32_t op = U32(b, ops + k * (uint32_t)sizeof(layoutdata::op *));
                if (U32(b, op) == code && nth-- == 0)
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

Fact("LayoutBlob Sample Is Valid")
{
    List<uint8_t> b = LoadSample();
    LayoutSummary s = {};
    Assert(Validate(b, &s) == LayoutError::Ok);
    Assert(s.version == LAYOUT_BLOB_VERSION);
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
            LayoutImage im = s.home[k][slot];
            if (!im.offset)
                continue;
            Assert(im.offset % 4 == 0 && im.offset + im.size <= Table(b));
            const void *px = b.GetBuffer() + im.offset;
            Assert(check_image(px, im.size, nullptr));
            Assert(get_image_width(px) == COMP_KEY_SIZE && get_image_height(px) == COMP_KEY_SIZE);
        }
    }
}

Fact("LayoutBlob Summary Is Untouched On Error")
{
    List<uint8_t> b = LoadSample();
    Put32(b, 0, 0);
    LayoutSummary s;
    memset(&s, 0x5A, sizeof(s));
    Assert(Validate(b, &s) == LayoutError::BadSignature);
    Assert(((uint8_t *)&s)[0] == 0x5A && ((uint8_t *)&s)[sizeof(s) - 1] == 0x5A);
}

Fact("LayoutBlob Relocate Adds The Base To Table Slots Only")
{
    List<uint8_t> b = LoadSample();
    List<uint8_t> r = LoadSample();
    LayoutRelocate(r.GetBuffer(), (uint32_t)r.GetCount(), 0x3C000000);

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

Fact("LayoutBlob Relocate By Zero Changes Nothing")
{
    List<uint8_t> b = LoadSample();
    List<uint8_t> r = LoadSample();
    LayoutRelocate(r.GetBuffer(), (uint32_t)r.GetCount(), 0);
    Assert(memcmp(b.GetBuffer(), r.GetBuffer(), b.GetCount()) == 0);
}

Fact("LayoutBlob Rejects Truncated Files")
{
    List<uint8_t> b = LoadSample();
    for (uint32_t n : { 0u, 1u, 16u, 31u, 32u, 100u, 604u }) {
        Guarded g(b.GetBuffer(), n);
        LayoutSummary s;
        LayoutError e = LayoutValidate(g.Data(), g.Size(), &s);
        Assert(e == LayoutError::Truncated || e == LayoutError::BadHeader);
    }
    // Every truncation point fails, without reading past the end
    for (uint32_t n = 0; n < (uint32_t)b.GetCount(); n += 61) {
        Guarded g(b.GetBuffer(), n);
        LayoutSummary s;
        Assert(LayoutValidate(g.Data(), g.Size(), &s) != LayoutError::Ok);
    }
    // ...and the last byte
    {
        Guarded g(b.GetBuffer(), (uint32_t)b.GetCount() - 1);
        LayoutSummary s;
        Assert(LayoutValidate(g.Data(), g.Size(), &s) != LayoutError::Ok);
    }
}

Fact("LayoutBlob Rejects Trailing Bytes")
{
    List<uint8_t> b = LoadSample();
    b.Add(0); b.Add(0); b.Add(0); b.Add(0);
    Assert(Validate(b) == LayoutError::BadHeader);
}

Fact("LayoutBlob Rejects A Bad Header")
{
    Assert(Validate(With(0, 0x4B415043)) == LayoutError::BadSignature);        // "CPAK"
    Assert(Validate(With(0, 0x34365042)) == LayoutError::BadSignature);        // "BP64"
    Assert(Validate(With(4, 2)) == LayoutError::BadVersion);
    Assert(Validate(With(4, 0)) == LayoutError::BadVersion);
    Assert(Validate(With(CB_ROOT, 2)) == LayoutError::BadVersion);             // the root's version

    List<uint8_t> b = LoadSample();
    uint32_t table = Table(b), count = RelocCount(b);
    Assert(Validate(With(8, count + 1)) == LayoutError::BadHeader);            // table runs off the end
    Assert(Validate(With(8, count - 1)) == LayoutError::BadHeader);            // gap before the end
    Assert(Validate(With(8, 0xFFFFFFFF)) == LayoutError::BadHeader);           // count * 4 overflows
    Assert(Validate(With(12, table + 4)) == LayoutError::BadHeader);
    Assert(Validate(With(12, table - 1)) == LayoutError::BadHeader);           // unaligned
    Assert(Validate(With(12, 0)) == LayoutError::BadHeader);
    Assert(Validate(With(12, 0xFFFFFFFC)) == LayoutError::BadHeader);
    Assert(Validate(With(12, 40)) == LayoutError::BadHeader);                  // no room for the root
}

Fact("LayoutBlob Rejects Bad Relocation Entries")
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
    Assert(Validate(with(last, table)) == LayoutError::BadRelocation);         // past the data
    Assert(Validate(with(last, table - 2)) == LayoutError::BadRelocation);     // unaligned
    Assert(Validate(with(last, 0xFFFFFFFC)) == LayoutError::BadRelocation);
    Assert(Validate(with(0, 8)) == LayoutError::BadRelocation);                // into the header
    Assert(Validate(with(5, relocs[4])) == LayoutError::BadRelocation);        // duplicate
    Assert(Validate(with(4, relocs[5] + 4)) == LayoutError::BadRelocation);    // out of order

    // A slot that isn't a pointer in the structure (the root's version word)
    {
        List<uint32_t> r = Relocs(b);
        r.InsertAt(0, CB_ROOT);
        Assert(Validate(WithRelocs(b, r)) == LayoutError::BadRelocation);
    }
    // Inside image data
    {
        LayoutSummary s;
        Assert(Validate(b, &s) == LayoutError::Ok);
        uint32_t inside = (s.home[0][KEY_SLOT_BG].offset + 64) & ~3u;
        List<uint32_t> r = Relocs(b);
        int at = 0;
        while (at < r.GetCount() && r[at] < inside)
            at++;
        r.InsertAt(at, inside);
        Assert(Validate(WithRelocs(b, r)) == LayoutError::BadRelocation);
    }
    // A pointer slot missing from the table (it would stay a file offset)
    for (int i : { 0, 7, 20, last / 2, last }) {
        List<uint32_t> r = Relocs(b);
        r.RemoveAt(i);
        Assert(Validate(WithRelocs(b, r)) == LayoutError::BadRelocation);
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
        Assert(Validate(WithRelocs(b, r)) == LayoutError::BadRelocation);
    }
}

Fact("LayoutBlob Rejects Bad Pointers And Counts")
{
    List<uint8_t> b = LoadSample();
    uint32_t table = Table(b);
    uint32_t k0 = CB_HOME_KEY(0);

    Assert(Validate(With(CB_KEY_PTR(k0, 0), table + 4)) == LayoutError::BadPointer);
    Assert(Validate(With(CB_KEY_PTR(k0, 0), 8)) == LayoutError::BadPointer);          // into the header
    Assert(Validate(With(CB_KEY_COUNT(k0, 0), 0xFFFFFFF0)) == LayoutError::BadPointer);
    Assert(Validate(With(CB_KEY_COUNT(k0, 0), table)) == LayoutError::BadPointer);
    // A pointer that's set while its count is zero is still a pointer into the data
    Assert(Validate(With(CB_KEY_COUNT(CB_HOME_KEY(1), 1), 0)) != LayoutError::Ok);
    // Home bindings, devices and activities
    Assert(Validate(With(CB_HOME_BINDINGS, 5000)) == LayoutError::BadLimit);
    Assert(Validate(With(CB_HOME_BINDINGS_PTR, table)) == LayoutError::BadPointer);
    Assert(Validate(With(CB_ROOT_DEVICES_PTR, table - 4)) == LayoutError::BadPointer);
    Assert(Validate(With(CB_ROOT_ACTIVITIES_PTR, table - 8)) == LayoutError::BadPointer);
}

Fact("LayoutBlob Rejects Strings Without A Terminator")
{
    List<uint8_t> b = LoadSample();
    uint32_t table = Table(b);
    // The last thing in the data is a script's source (and its padding)
    for (uint32_t i = table - 8; i < table; i++)
        b.GetBuffer()[i] = 'x';
    Assert(Validate(b) == LayoutError::BadPointer);
}

Fact("LayoutBlob Caps A Script's Source")
{
    List<uint8_t> b = LoadSample();
    // The sample's script op (op 8, a 12 byte source "return param"): its source_count
    uint32_t count_off = 0;
    for (uint32_t i = 32; i + 12 < Table(b); i += 4)
        if (U32(b, i) == OP_SCRIPT && U32(b, i + 4) == 12) {
            count_off = i + 4;
            break;
        }
    Assert(count_off != 0);
    Assert(Validate(With(count_off, LAYOUT_MAX_SCRIPT + 1)) == LayoutError::BadLimit);
    Assert(Validate(With(count_off, 0)) != LayoutError::Ok);                    // an empty source is no script
}

Fact("LayoutBlob Enforces Limits")
{
    List<uint8_t> b = LoadSample();
    uint32_t acts = U32(b, CB_ROOT_ACTIVITIES_PTR);
    // The first activity's device, page and binding counts
    Assert(Validate(With(CB_ROOT_DEVICES, LAYOUT_MAX_DEVICES + 1)) == LayoutError::BadLimit);
    Assert(Validate(With(CB_ROOT_ACTIVITIES, LAYOUT_MAX_ACTIVITIES + 1)) == LayoutError::BadLimit);
    Assert(Validate(With(acts + CB_OFF(activity, pages_count), LAYOUT_MAX_ACTIVITY_PAGES + 1)) == LayoutError::BadLimit);
    Assert(Validate(With(acts + CB_OFF(activity, pages_count), 0)) == LayoutError::BadLimit);            // an activity needs a page
    Assert(Validate(With(acts + CB_OFF(activity, bindings_count), LAYOUT_MAX_BINDINGS + 1)) == LayoutError::BadLimit);
    Assert(Validate(With(acts + CB_OFF(activity, devices_count), LAYOUT_MAX_DEVICES + 1)) == LayoutError::BadLimit);

    // An op list over the limit (the first activity's didActivate)
    Assert(Validate(With(acts + CB_OFF(activity, didActivate_count), LAYOUT_MAX_OPS + 1)) == LayoutError::BadLimit);
}

Fact("LayoutBlob Rejects Bad Ops")
{
    List<uint8_t> b = LoadSample();
    uint32_t hid = FindHomeOp(b, 3);
    uint32_t sw  = FindHomeOp(b, 2);
    uint32_t del = FindHomeOp(b, 1);
    Assert(hid && sw && del);

    Assert(Validate(With(hid, 0)) == LayoutError::BadOp);
    Assert(Validate(With(hid, 9)) == LayoutError::BadOp);
    Assert(Validate(With(hid, 0xFFFFFFFF)) == LayoutError::BadOp);
    Assert(Validate(With(hid + CB_OFF(hidKeyOp, mode), 4)) == LayoutError::BadOp);                  // hid mode 0..3
    Assert(Validate(With(sw + CB_OFF(switchActivityOp, index), 2)) == LayoutError::BadIndex);       // activity 2 of 2
    // Only the none sentinel is allowed past the activity count
    Assert(Validate(With(sw + CB_OFF(switchActivityOp, index), 0xFFFFFFFE)) == LayoutError::BadIndex);
    Assert(Validate(With(sw + CB_OFF(switchActivityOp, index), LAYOUT_ACTIVITY_NONE)) == LayoutError::Ok);
    Assert(Validate(With(sw + CB_OFF(switchActivityOp, index), 1)) == LayoutError::Ok);
    // The sample has a real "none" switch (the second one on Home)
    uint32_t none = FindHomeOp(b, 2, 1);
    Assert(none && U32(b, none + CB_OFF(switchActivityOp, index)) == LAYOUT_ACTIVITY_NONE);
    Assert(LAYOUT_ACTIVITY_NONE == 0xFFFFFFFF);

    // An op pointer too close to the end for its struct
    {
        uint32_t bindings = U32(b, CB_HOME_BINDINGS_PTR);
        uint32_t ops = U32(b, bindings + CB_OFF(binding, ops));
        Assert(Validate(With(ops, Table(b) - 4)) != LayoutError::Ok);
    }
    // An activity's device index past the device list
    {
        uint32_t acts = U32(b, CB_ROOT_ACTIVITIES_PTR);
        uint32_t devs = U32(b, acts + CB_OFF(activity, devices));
        Assert(Validate(With(devs, 2)) == LayoutError::BadIndex);
        Assert(Validate(With(devs, 0xFFFFFFFF)) == LayoutError::BadIndex);
    }
}

Fact("LayoutBlob Only Accepts http URLs In HTTP Ops")
{
    // The sample's httpPost and httpGet URLs (in device turnOff / turnOn lists)
    const char *urls[] = { "http://tv.local/off", "http://avr.local/on" };
    for (const char *url : urls) {
        List<uint8_t> good = LoadSample();
        uint32_t len = (uint32_t)strlen(url), at = 0;
        for (uint32_t i = 0; i + len < Table(good); i++) {
            if (memcmp(good.GetBuffer() + i, url, len + 1) == 0) {
                at = i;
                break;
            }
        }
        Assert(at != 0);
        Assert(Validate(good) == LayoutError::Ok);

        // Same length, wrong scheme: not http:// (the bake rejects https:// too)
        const char *bad[] = { "htxp://", "ftp:///", "Http://", "http:/x/", "http:", "" };
        for (const char *scheme : bad) {
            List<uint8_t> b = LoadSample();
            uint32_t n = (uint32_t)strlen(scheme);
            memcpy(b.GetBuffer() + at, scheme, n);
            if (n < 7)
                b.GetBuffer()[at + n] = 0;          // a short string: ends early
            Assert(Validate(b) == LayoutError::BadOp);
        }
    }
}

Fact("LayoutBlob Rejects Bad Images")
{
    List<uint8_t> b = LoadSample();
    LayoutSummary s;
    Assert(Validate(b, &s) == LayoutError::Ok);
    uint32_t k0 = CB_HOME_KEY(0);
    LayoutImage face = s.home[0][KEY_SLOT_BG];

    // Not 4-aligned (the slot stays a valid pointer into the data)
    Assert(Validate(With(CB_KEY_PTR(k0, 0), face.offset + 2)) == LayoutError::BadAlignment);

    // Too short for its data
    Assert(Validate(With(CB_KEY_COUNT(k0, 0), face.size - 8)) == LayoutError::BadImage);
    Assert(Validate(With(CB_KEY_COUNT(k0, 0), 8)) == LayoutError::BadImage);

    // Wrong size: the header has the wrong width or height
    for (uint32_t field : { 8u, 12u }) {            // width, height
        List<uint8_t> c = LoadSample();
        uint32_t v = U32(c, face.offset + field);
        Put32(c, face.offset + field, v + 1);
        Assert(Validate(c) == LayoutError::BadImage);
    }

    // Runs that go past the data
    {
        List<uint8_t> c = LoadSample();
        uint32_t end = face.offset + face.size;
        for (uint32_t i = face.offset + 16; i < end; i++)
            c.GetBuffer()[i] = 0xFF;
        Assert(Validate(c) == LayoutError::BadImage);
    }

    // Garbage instead of an image
    {
        List<uint8_t> c = LoadSample();
        for (uint32_t i = 0; i < 64; i++)
            c.GetBuffer()[face.offset + i] = (uint8_t)(i * 37 + 1);
        Assert(Validate(c) == LayoutError::BadImage);
    }
}

Fact("LayoutBlob Rejects A Misaligned Blob")
{
    List<uint8_t> b = LoadSample();
    Guarded g(b.GetBuffer(), (uint32_t)b.GetCount() + 8);
    memmove(g.Data() + 1, g.Data(), b.GetCount());
    LayoutSummary s;
    Assert(LayoutValidate(g.Data() + 1, (uint32_t)b.GetCount(), &s) == LayoutError::BadAlignment);
}

Fact("LayoutBlob Names Every Error")
{
    for (int e = 0; e <= (int)LayoutError::BadImage; e++)
        Assert(LayoutErrorName((LayoutError)e)[0] != 0);
}

// Random damage to the sample: validation must never fault (the buffer is
// followed by a guard page) and anything it accepts must relocate cleanly.
Fact("LayoutBlob Fuzz Never Faults")
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
        LayoutSummary s;
        if (LayoutValidate(g.Data(), g.Size(), &s) == LayoutError::Ok) {
            accepted++;
            LayoutRelocate(g.Data(), g.Size(), 0x10000000);
        }
    }
    // Most edits land in places that matter, but some are harmless (padding, reserved bytes)
    Assert(accepted < seeds);
}
