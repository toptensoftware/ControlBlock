// Layout blob validation and relocation. Spec: docs/standalone.md.
//
// The struct shapes are the generated ones (layout_types.h, from the packed
// schema). The blob isn't trusted yet and isn't relocated, so the walk is by
// bounds-checked offsets taken from those structs (offsetof / sizeof), never by
// casting. test/data/sample_layout.bin is baked by the real packer, so a schema
// change that isn't mirrored there fails TestLayoutBlob.

#include <string.h>
#include "Core.h"
#include "rle565.h"
#include "layout_blob.h"

using namespace SimpleLib;

namespace
{
    #define TRY(expr) do { LayoutError e_ = (expr); if (e_ != LayoutError::Ok) return e_; } while (0)

    // Offset of `field` in the generated struct `type`
    #define OFF(type, field)    ((uint32_t)offsetof(layoutdata::type, field))
    // The (count, pointer) offsets, in the file, of the array `field` of the struct at `base`
    #define PAIR(base, type, field)     ((base) + OFF(type, field##_count)), ((base) + OFF(type, field))
    #define SIZE(type)          ((uint32_t)sizeof(layoutdata::type))

    class Validator
    {
    public:
        Validator(const uint8_t *blob, uint32_t size) : m_b(blob), m_size(size) {}

        LayoutError Run(LayoutSummary *out);

    private:
        uint32_t U32(uint32_t off) const { uint32_t v; memcpy(&v, m_b + off, 4); return v; }

        LayoutError Header();
        LayoutError Relocations();
        int         FindReloc(uint32_t slot) const;
        LayoutError Ptr(uint32_t slot, uint32_t *value);
        LayoutError Array(uint32_t count_off, uint32_t ptr_off, uint32_t elem, uint32_t max, uint32_t *ptr, uint32_t *count);
        LayoutError Str(uint32_t slot, bool nullable);
        LayoutError HttpUrl(uint32_t slot);
        LayoutError Image(uint32_t count_off, uint32_t ptr_off, KeyPixelRect px, LayoutImage *out);
        LayoutError Key(uint32_t key, LayoutKey *out);
        LayoutError Keys(uint32_t count_off, uint32_t ptr_off, LayoutKey *out, uint32_t *count);
        LayoutError Op(uint32_t op);
        LayoutError OpList(uint32_t count_off, uint32_t ptr_off);
        LayoutError Bindings(uint32_t count_off, uint32_t ptr_off);
        LayoutError Device(uint32_t device);
        LayoutError Activity(uint32_t activity);

        const uint8_t *m_b;
        uint32_t       m_size;
        uint32_t       m_table = 0;         // the relocation table's offset, and the end of the data
        uint32_t       m_relocs = 0;
        uint32_t       m_device_count = 0, m_activity_count = 0;
        List<uint8_t>  m_seen;              // a bit per relocation entry: reached by the walk
        uint32_t       m_seen_count = 0;
        Map<uint32_t, uint32_t> m_checked;  // image offset -> size, already through check_image (and opaque)
        Set<uint32_t>  m_ids;               // key ids seen so far (unique across the layout)
    };

    LayoutError Validator::Header()
    {
        if (m_size < BPAK_HEADER_SIZE)
            return LayoutError::Truncated;
        if (U32(0) != BPAK_SIGNATURE)
            return LayoutError::BadSignature;
        if (U32(4) != BPAK_FORMAT_VERSION)
            return LayoutError::BadVersion;
        m_relocs = U32(8);
        m_table  = U32(12);
        // The table is the end of the file, exactly
        if (m_table % 4 != 0 || m_table > m_size || (uint64_t)m_table + (uint64_t)m_relocs * 4 != m_size)
            return LayoutError::BadHeader;
        if (m_table < BPAK_HEADER_SIZE + SIZE(layoutRoot))
            return LayoutError::Truncated;
        return LayoutError::Ok;
    }

    // Entries are aligned slots inside the data, strictly ascending (so no slot
    // is relocated twice)
    LayoutError Validator::Relocations()
    {
        uint32_t prev = 0;
        for (uint32_t i = 0; i < m_relocs; i++) {
            uint32_t e = U32(m_table + i * 4);
            if (e % 4 != 0 || e < BPAK_HEADER_SIZE || e > m_table - 4 || (i > 0 && e <= prev))
                return LayoutError::BadRelocation;
            prev = e;
        }
        m_seen.SetCount((int)((m_relocs + 7) / 8), 0);
        return LayoutError::Ok;
    }

    // Index of `slot` in the relocation table, or -1
    int Validator::FindReloc(uint32_t slot) const
    {
        int lo = 0, hi = (int)m_relocs - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            uint32_t v = U32(m_table + (uint32_t)mid * 4);
            if (v == slot)
                return mid;
            if (v < slot)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        return -1;
    }

    // The pointer in `slot` (inside the data): 0, or an offset into the data.
    // A slot is in the relocation table exactly when it's non-null.
    LayoutError Validator::Ptr(uint32_t slot, uint32_t *value)
    {
        if ((uint64_t)slot + 4 > m_table || slot < BPAK_HEADER_SIZE)
            return LayoutError::BadPointer;
        uint32_t v = U32(slot);
        int index = FindReloc(slot);
        if ((v == 0) != (index < 0))
            return LayoutError::BadRelocation;
        if (index >= 0) {
            uint8_t &byte = m_seen.GetBuffer()[index / 8];
            if (!(byte & (1 << (index % 8)))) {
                byte |= 1 << (index % 8);
                m_seen_count++;
            }
            if (v < BPAK_HEADER_SIZE || v > m_table)
                return LayoutError::BadPointer;
        }
        *value = v;
        return LayoutError::Ok;
    }

    // A (count, pointer) pair: `count` elements of `elem` bytes inside the data.
    // An empty array may have a null pointer or point anywhere in the data.
    LayoutError Validator::Array(uint32_t count_off, uint32_t ptr_off, uint32_t elem, uint32_t max, uint32_t *ptr, uint32_t *count)
    {
        if ((uint64_t)count_off + 4 > m_table)
            return LayoutError::BadPointer;
        uint32_t n = U32(count_off);
        if (n > max)
            return LayoutError::BadLimit;
        uint32_t p;
        TRY(Ptr(ptr_off, &p));
        if (n > 0 && (p == 0 || (uint64_t)p + (uint64_t)n * elem > m_table))
            return LayoutError::BadPointer;
        *ptr   = p;
        *count = n;
        return LayoutError::Ok;
    }

    // A NUL-terminated string inside the data
    LayoutError Validator::Str(uint32_t slot, bool nullable)
    {
        uint32_t p;
        TRY(Ptr(slot, &p));
        if (p == 0)
            return nullable ? LayoutError::Ok : LayoutError::BadPointer;
        if (p >= m_table || !memchr(m_b + p, 0, m_table - p))
            return LayoutError::BadPointer;
        return LayoutError::Ok;
    }

    // A string that starts "http://": the net worker speaks plain HTTP only (the bake
    // checks this too; a blob can come from anywhere)
    LayoutError Validator::HttpUrl(uint32_t slot)
    {
        TRY(Str(slot, false));
        uint32_t p;
        TRY(Ptr(slot, &p));
        if (strncmp((const char *)m_b + p, "http://", 7) != 0)      // the string is NUL-terminated inside the data
            return LayoutError::BadOp;
        return LayoutError::Ok;
    }

    // An rle565 image given as (count, ptr): absent, or a whole valid opaque
    // image the size of its key (`px`), 4-byte aligned in memory
    LayoutError Validator::Image(uint32_t count_off, uint32_t ptr_off, KeyPixelRect px_rect, LayoutImage *out)
    {
        uint32_t p, n;
        TRY(Array(count_off, ptr_off, 1, 0xFFFFFFFFu, &p, &n));
        if (n == 0) {
            if (p != 0)
                return LayoutError::BadPointer;
            *out = {};
            return LayoutError::Ok;
        }
        if (((uintptr_t)m_b + p) % 4 != 0)
            return LayoutError::BadAlignment;

        // Once through check_image per (offset, size): a shared image is checked
        // once, and only opaque ones are remembered
        const void *px = m_b + p;
        uint32_t size = 0;
        if (!m_checked.TryGetValue(p, size) || size != n) {
            bool opaque = false;
            if (!check_image_header(px, n) || !check_image(px, n, &opaque) || !opaque)
                return LayoutError::BadImage;
            m_checked.Add(p, n);
        }
        if (get_image_width(px) != (uint32_t)px_rect.w || get_image_height(px) != (uint32_t)px_rect.h)
            return LayoutError::BadImage;
        out->offset = p;
        out->size   = n;
        return LayoutError::Ok;
    }

    // A key: an id not 0 and not seen before in the layout, a rect inside the
    // grid, and its images
    LayoutError Validator::Key(uint32_t key, LayoutKey *out)
    {
        uint16_t id;
        memcpy(&id, m_b + key + OFF(key, id), sizeof(id));
        KeyRect rect = LayoutKeyRect(m_b + key + OFF(key, rect));
        if (id == KEY_ID_NONE || m_ids.Contains(id) || !KeyRectValid(rect))
            return LayoutError::BadKey;
        m_ids.Add(id);

        LayoutKey k = {};
        k.id   = id;
        k.rect = rect;
        KeyPixelRect px = KeyRectPixels(rect);
        TRY(Image(key + LayoutKeyImageOffset(LAYOUT_KEY_NORMAL), key + LayoutKeyImagePtrOffset(LAYOUT_KEY_NORMAL), px, &k.normal));
        TRY(Image(key + LayoutKeyImageOffset(LAYOUT_KEY_PRESSED), key + LayoutKeyImagePtrOffset(LAYOUT_KEY_PRESSED), px, &k.pressed));
        if (out)
            *out = k;
        return LayoutError::Ok;
    }

    // A page's (count, ptr) of keys: at most COMP_MAX_KEYS, none overlapping.
    // `out` (COMP_MAX_KEYS entries, may be null) and `count` get them.
    LayoutError Validator::Keys(uint32_t count_off, uint32_t ptr_off, LayoutKey *out, uint32_t *count)
    {
        uint32_t list, n;
        TRY(Array(count_off, ptr_off, SIZE(key), COMP_MAX_KEYS, &list, &n));
        KeyRect rects[COMP_MAX_KEYS];
        for (uint32_t i = 0; i < n; i++) {
            LayoutKey k;
            TRY(Key(list + i * SIZE(key), &k));
            rects[i] = k.rect;
            if (out)
                out[i] = k;
        }
        if (KeyRectsCheck(rects, (int)n) != KEY_GEOM_OK)
            return LayoutError::BadKey;
        if (count)
            *count = n;
        return LayoutError::Ok;
    }

    LayoutError Validator::Op(uint32_t op)
    {
        // The code, then the whole struct for it
        static const uint8_t sizes[] = {
            0, SIZE(delayOp), SIZE(switchActivityOp), SIZE(hidKeyOp), SIZE(httpGetOp),
            SIZE(httpPostOp), SIZE(udpPacketOp), SIZE(sendWolOp), SIZE(scriptOp),
        };
        static_assert(sizeof(sizes) == OP_SCRIPT + 1, "a size for every op code");
        if ((uint64_t)op + SIZE(op) > m_table)
            return LayoutError::BadPointer;
        uint32_t code = U32(op + OFF(op, op));
        if (code < OP_DELAY || code > OP_SCRIPT)
            return LayoutError::BadOp;
        if ((uint64_t)op + sizes[code] > m_table)
            return LayoutError::BadPointer;

        uint32_t p, n;
        switch (code) {
        case OP_SWITCH_ACTIVITY:
            p = U32(op + OFF(switchActivityOp, index));
            if (p >= m_activity_count && p != LAYOUT_ACTIVITY_NONE)
                return LayoutError::BadIndex;
            break;
        case OP_HID_KEY:
            if (m_b[op + OFF(hidKeyOp, mode)] > HID_MODE_MAX)
                return LayoutError::BadOp;
            break;
        case OP_HTTP_GET:
            TRY(HttpUrl(op + OFF(httpGetOp, url)));
            TRY(Str(op + OFF(httpGetOp, headers), true));
            break;
        case OP_HTTP_POST:
            TRY(HttpUrl(op + OFF(httpPostOp, url)));
            TRY(Array(PAIR(op, httpPostOp, data), 1, LAYOUT_MAX_OP_DATA, &p, &n));
            TRY(Str(op + OFF(httpPostOp, contentType), false));
            TRY(Str(op + OFF(httpPostOp, headers), true));
            break;
        case OP_UDP_PACKET:
            TRY(Str(op + OFF(udpPacketOp, host), false));
            TRY(Array(PAIR(op, udpPacketOp, data), 1, LAYOUT_MAX_OP_DATA, &p, &n));
            break;
        case OP_SEND_WOL:
            TRY(Str(op + OFF(sendWolOp, broadcast), true));
            break;
        case OP_SCRIPT:
            // Source with an explicit length, and the terminator after it
            TRY(Array(PAIR(op, scriptOp, source), 1, LAYOUT_MAX_SCRIPT, &p, &n));
            if (p == 0 || (uint64_t)p + n >= m_table || m_b[p + n] != 0)
                return LayoutError::BadPointer;
            break;
        default:
            break;      // delay: any duration
        }
        return LayoutError::Ok;
    }

    // (count, ptr) of op pointers
    LayoutError Validator::OpList(uint32_t count_off, uint32_t ptr_off)
    {
        uint32_t list, n;
        TRY(Array(count_off, ptr_off, sizeof(layoutdata::op *), LAYOUT_MAX_OPS, &list, &n));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t op;
            TRY(Ptr(list + i * sizeof(layoutdata::op *), &op));
            if (op == 0)
                return LayoutError::BadPointer;
            TRY(Op(op));
        }
        return LayoutError::Ok;
    }

    LayoutError Validator::Bindings(uint32_t count_off, uint32_t ptr_off)
    {
        uint32_t list, n;
        TRY(Array(count_off, ptr_off, SIZE(binding), LAYOUT_MAX_BINDINGS, &list, &n));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t b = list + i * SIZE(binding);
            TRY(Str(b + OFF(binding, control), false));
            TRY(Str(b + OFF(binding, event), false));
            TRY(OpList(PAIR(b, binding, ops)));
        }
        return LayoutError::Ok;
    }

    LayoutError Validator::Device(uint32_t d)
    {
        TRY(Str(d + OFF(device, name), false));
        TRY(OpList(PAIR(d, device, turnOn)));
        return OpList(PAIR(d, device, turnOff));
    }

    LayoutError Validator::Activity(uint32_t a)
    {
        TRY(Str(a + OFF(activity, name), false));
        TRY(Str(a + OFF(activity, displayName), true));

        uint32_t list, n;
        TRY(Array(PAIR(a, activity, devices), sizeof(int32_t), LAYOUT_MAX_DEVICES, &list, &n));
        for (uint32_t i = 0; i < n; i++)
            if (U32(list + i * sizeof(int32_t)) >= m_device_count)       // int32: a negative index is huge
                return LayoutError::BadIndex;

        TRY(Array(PAIR(a, activity, pages), SIZE(page), LAYOUT_MAX_ACTIVITY_PAGES, &list, &n));
        if (n == 0)
            return LayoutError::BadLimit;                   // an activity needs a page
        for (uint32_t i = 0; i < n; i++) {
            uint32_t pg = list + i * SIZE(page);
            TRY(Keys(PAIR(pg, page, keys), nullptr, nullptr));
        }

        TRY(Bindings(PAIR(a, activity, bindings)));
        TRY(OpList(PAIR(a, activity, willDeactivate)));
        TRY(OpList(PAIR(a, activity, willActivate)));
        TRY(OpList(PAIR(a, activity, didActivate)));
        TRY(OpList(PAIR(a, activity, didDeactivate)));
        return LayoutError::Ok;
    }

    LayoutError Validator::Run(LayoutSummary *out)
    {
        TRY(Header());
        TRY(Relocations());

        const uint32_t root = BPAK_HEADER_SIZE;
        const uint32_t home = root + OFF(layoutRoot, homePage);
        if (U32(root + OFF(layoutRoot, version)) != LAYOUT_BLOB_VERSION)
            return LayoutError::BadVersion;
        m_device_count   = U32(root + OFF(layoutRoot, devices_count));
        m_activity_count = U32(root + OFF(layoutRoot, activities_count));
        if (m_device_count > LAYOUT_MAX_DEVICES || m_activity_count > LAYOUT_MAX_ACTIVITIES)
            return LayoutError::BadLimit;

        LayoutSummary s = {};
        s.version       = U32(root + OFF(layoutRoot, version));
        s.long_press_ms = U32(root + OFF(layoutRoot, longPressMs));
        s.device_count  = m_device_count;
        s.activity_count = m_activity_count;

        TRY(Keys(PAIR(home, home, keys), s.home, &s.home_count));
        TRY(Bindings(PAIR(home, home, bindings)));

        uint32_t list, n;
        TRY(Array(PAIR(root, layoutRoot, devices), SIZE(device), LAYOUT_MAX_DEVICES, &list, &n));
        for (uint32_t i = 0; i < n; i++)
            TRY(Device(list + i * SIZE(device)));
        TRY(Array(PAIR(root, layoutRoot, activities), SIZE(activity), LAYOUT_MAX_ACTIVITIES, &list, &n));
        for (uint32_t i = 0; i < n; i++)
            TRY(Activity(list + i * SIZE(activity)));

        // Every table entry was a pointer slot we walked, so relocating can't
        // touch anything else
        if (m_seen_count != m_relocs)
            return LayoutError::BadRelocation;

        *out = s;
        return LayoutError::Ok;
    }
}

const char *LayoutErrorName(LayoutError e)
{
    switch (e) {
    case LayoutError::Ok:            return "ok";
    case LayoutError::Truncated:     return "truncated";
    case LayoutError::BadSignature:  return "bad signature";
    case LayoutError::BadVersion:    return "unsupported version";
    case LayoutError::BadHeader:     return "bad header";
    case LayoutError::BadRelocation: return "bad relocation table";
    case LayoutError::BadPointer:    return "pointer out of range";
    case LayoutError::BadAlignment:  return "misaligned";
    case LayoutError::BadLimit:      return "over a limit";
    case LayoutError::BadOp:         return "bad op";
    case LayoutError::BadIndex:      return "index out of range";
    case LayoutError::BadImage:      return "bad image";
    case LayoutError::BadKey:        return "bad key";
    }
    return "?";
}

LayoutKey LayoutKeyOf(const layoutdata::key &key, const uint8_t *blob)
{
    auto image = [&](uint32_t count, const uint8_t *ptr) {
        return count && ptr ? LayoutImage{ (uint32_t)(ptr - blob), count } : LayoutImage{};
    };
    LayoutKey k = {};
    k.id      = key.id;
    k.rect    = LayoutKeyRect(key.rect);
    k.normal  = image(key.normal_count, key.normal);
    k.pressed = image(key.pressed_count, key.pressed);
    return k;
}

LayoutError LayoutValidate(const void *blob, uint32_t size, LayoutSummary *summary)
{
    if (((uintptr_t)blob & 3) != 0)
        return LayoutError::BadAlignment;
    Validator v((const uint8_t *)blob, size);
    return v.Run(summary);
}

void LayoutRelocate(void *blob, uint32_t size, uint32_t base)
{
    uint8_t *b = (uint8_t *)blob;
    uint32_t count, table;
    memcpy(&count, b + 8, 4);
    memcpy(&table, b + 12, 4);
    (void)size;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot, v;
        memcpy(&slot, b + table + i * 4, 4);
        memcpy(&v, b + slot, 4);
        v += base;
        memcpy(b + slot, &v, 4);
    }
}
