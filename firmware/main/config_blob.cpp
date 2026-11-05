// Config blob validation and relocation. Spec: docs/standalone.md.
//
// The struct layouts are the generated ones (config_types.h, from the packed
// schema). The blob isn't trusted yet and isn't relocated, so the walk is by
// bounds-checked offsets taken from those structs (offsetof / sizeof), never by
// casting. test/data/sample_config.bin is baked by the real packer, so a schema
// change that isn't mirrored there fails TestConfigBlob.

#include <string.h>
#include "Core.h"
#include "rle565.h"
#include "config_blob.h"

using namespace SimpleLib;

namespace
{
    #define TRY(expr) do { ConfigError e_ = (expr); if (e_ != ConfigError::Ok) return e_; } while (0)

    // Offset of `field` in the generated struct `type`
    #define OFF(type, field)    ((uint32_t)offsetof(cfgdata::type, field))
    // The (count, pointer) offsets, in the file, of the array `field` of the struct at `base`
    #define PAIR(base, type, field)     ((base) + OFF(type, field##_count)), ((base) + OFF(type, field))
    #define SIZE(type)          ((uint32_t)sizeof(cfgdata::type))

    class Validator
    {
    public:
        Validator(const uint8_t *blob, uint32_t size) : m_b(blob), m_size(size) {}

        ConfigError Run(ConfigSummary *out);

    private:
        uint32_t U32(uint32_t off) const { uint32_t v; memcpy(&v, m_b + off, 4); return v; }

        ConfigError Header();
        ConfigError Relocations();
        int         FindReloc(uint32_t slot) const;
        ConfigError Ptr(uint32_t slot, uint32_t *value);
        ConfigError Array(uint32_t count_off, uint32_t ptr_off, uint32_t elem, uint32_t max, uint32_t *ptr, uint32_t *count);
        ConfigError Str(uint32_t slot, bool nullable);
        ConfigError HttpUrl(uint32_t slot);
        ConfigError Image(uint32_t count_off, uint32_t ptr_off, ConfigImage *out);
        ConfigError Key(uint32_t key, ConfigImage out[KEY_SLOTS]);
        ConfigError Page(uint32_t page);
        ConfigError Op(uint32_t op);
        ConfigError OpList(uint32_t count_off, uint32_t ptr_off);
        ConfigError Bindings(uint32_t count_off, uint32_t ptr_off);
        ConfigError Device(uint32_t device);
        ConfigError Activity(uint32_t activity);

        const uint8_t *m_b;
        uint32_t       m_size;
        uint32_t       m_table = 0;         // the relocation table's offset, and the end of the data
        uint32_t       m_relocs = 0;
        uint32_t       m_device_count = 0, m_activity_count = 0;
        List<uint8_t>  m_seen;              // a bit per relocation entry: reached by the walk
        uint32_t       m_seen_count = 0;
        Map<uint32_t, uint32_t> m_checked;  // image offset -> size, already through check_image
    };

    ConfigError Validator::Header()
    {
        if (m_size < BPAK_HEADER_SIZE)
            return ConfigError::Truncated;
        if (U32(0) != BPAK_SIGNATURE)
            return ConfigError::BadSignature;
        if (U32(4) != BPAK_FORMAT_VERSION)
            return ConfigError::BadVersion;
        m_relocs = U32(8);
        m_table  = U32(12);
        // The table is the end of the file, exactly
        if (m_table % 4 != 0 || m_table > m_size || (uint64_t)m_table + (uint64_t)m_relocs * 4 != m_size)
            return ConfigError::BadHeader;
        if (m_table < BPAK_HEADER_SIZE + SIZE(configRoot))
            return ConfigError::Truncated;
        return ConfigError::Ok;
    }

    // Entries are aligned slots inside the data, strictly ascending (so no slot
    // is relocated twice)
    ConfigError Validator::Relocations()
    {
        uint32_t prev = 0;
        for (uint32_t i = 0; i < m_relocs; i++) {
            uint32_t e = U32(m_table + i * 4);
            if (e % 4 != 0 || e < BPAK_HEADER_SIZE || e > m_table - 4 || (i > 0 && e <= prev))
                return ConfigError::BadRelocation;
            prev = e;
        }
        m_seen.SetCount((int)((m_relocs + 7) / 8), 0);
        return ConfigError::Ok;
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
    ConfigError Validator::Ptr(uint32_t slot, uint32_t *value)
    {
        if ((uint64_t)slot + 4 > m_table || slot < BPAK_HEADER_SIZE)
            return ConfigError::BadPointer;
        uint32_t v = U32(slot);
        int index = FindReloc(slot);
        if ((v == 0) != (index < 0))
            return ConfigError::BadRelocation;
        if (index >= 0) {
            uint8_t &byte = m_seen.GetBuffer()[index / 8];
            if (!(byte & (1 << (index % 8)))) {
                byte |= 1 << (index % 8);
                m_seen_count++;
            }
            if (v < BPAK_HEADER_SIZE || v > m_table)
                return ConfigError::BadPointer;
        }
        *value = v;
        return ConfigError::Ok;
    }

    // A (count, pointer) pair: `count` elements of `elem` bytes inside the data.
    // An empty array may have a null pointer or point anywhere in the data.
    ConfigError Validator::Array(uint32_t count_off, uint32_t ptr_off, uint32_t elem, uint32_t max, uint32_t *ptr, uint32_t *count)
    {
        if ((uint64_t)count_off + 4 > m_table)
            return ConfigError::BadPointer;
        uint32_t n = U32(count_off);
        if (n > max)
            return ConfigError::BadLimit;
        uint32_t p;
        TRY(Ptr(ptr_off, &p));
        if (n > 0 && (p == 0 || (uint64_t)p + (uint64_t)n * elem > m_table))
            return ConfigError::BadPointer;
        *ptr   = p;
        *count = n;
        return ConfigError::Ok;
    }

    // A NUL-terminated string inside the data
    ConfigError Validator::Str(uint32_t slot, bool nullable)
    {
        uint32_t p;
        TRY(Ptr(slot, &p));
        if (p == 0)
            return nullable ? ConfigError::Ok : ConfigError::BadPointer;
        if (p >= m_table || !memchr(m_b + p, 0, m_table - p))
            return ConfigError::BadPointer;
        return ConfigError::Ok;
    }

    // A string that starts "http://": the net worker speaks plain HTTP only (the bake
    // checks this too; a blob can come from anywhere)
    ConfigError Validator::HttpUrl(uint32_t slot)
    {
        TRY(Str(slot, false));
        uint32_t p;
        TRY(Ptr(slot, &p));
        if (strncmp((const char *)m_b + p, "http://", 7) != 0)      // the string is NUL-terminated inside the data
            return ConfigError::BadOp;
        return ConfigError::Ok;
    }

    // An rle565 image given as (count, ptr): absent, or a whole valid image
    // of the key size, 4-byte aligned in memory
    ConfigError Validator::Image(uint32_t count_off, uint32_t ptr_off, ConfigImage *out)
    {
        uint32_t p, n;
        TRY(Array(count_off, ptr_off, 1, 0xFFFFFFFFu, &p, &n));
        if (n == 0) {
            if (p != 0)
                return ConfigError::BadPointer;
            *out = {};
            return ConfigError::Ok;
        }
        if (((uintptr_t)m_b + p) % 4 != 0)
            return ConfigError::BadAlignment;

        uint32_t size = 0;
        if (!m_checked.TryGetValue(p, size) || size != n) {
            const void *px = m_b + p;
            if (!check_image_header(px, n) || get_image_width(px) != COMP_KEY_SIZE ||
                get_image_height(px) != COMP_KEY_SIZE || !check_image(px, n, nullptr))
                return ConfigError::BadImage;
            m_checked.Add(p, n);
        }
        out->offset = p;
        out->size   = n;
        return ConfigError::Ok;
    }

    ConfigError Validator::Key(uint32_t key, ConfigImage out[KEY_SLOTS])
    {
        for (int s = 0; s < KEY_SLOTS; s++)
            TRY(Image(key + ConfigKeyImageOffset(s), key + ConfigKeyImagePtrOffset(s), &out[s]));
        return ConfigError::Ok;
    }

    ConfigError Validator::Page(uint32_t page)
    {
        for (int k = 0; k < COMP_KEYS; k++) {
            ConfigImage ignored[KEY_SLOTS];
            TRY(Key(page + OFF(page, keys) + k * SIZE(key), ignored));
        }
        return ConfigError::Ok;
    }

    ConfigError Validator::Op(uint32_t op)
    {
        // The code, then the whole struct for it
        static const uint8_t sizes[] = {
            0, SIZE(delayOp), SIZE(switchActivityOp), SIZE(hidKeyOp), SIZE(httpGetOp),
            SIZE(httpPostOp), SIZE(udpPacketOp), SIZE(sendWolOp), SIZE(scriptOp),
        };
        static_assert(sizeof(sizes) == OP_SCRIPT + 1, "a size for every op code");
        if ((uint64_t)op + SIZE(op) > m_table)
            return ConfigError::BadPointer;
        uint32_t code = U32(op + OFF(op, op));
        if (code < OP_DELAY || code > OP_SCRIPT)
            return ConfigError::BadOp;
        if ((uint64_t)op + sizes[code] > m_table)
            return ConfigError::BadPointer;

        uint32_t p, n;
        switch (code) {
        case OP_SWITCH_ACTIVITY:
            p = U32(op + OFF(switchActivityOp, index));
            if (p >= m_activity_count && p != CONFIG_ACTIVITY_NONE)
                return ConfigError::BadIndex;
            break;
        case OP_HID_KEY:
            if (m_b[op + OFF(hidKeyOp, mode)] > HID_MODE_MAX)
                return ConfigError::BadOp;
            break;
        case OP_HTTP_GET:
            TRY(HttpUrl(op + OFF(httpGetOp, url)));
            TRY(Str(op + OFF(httpGetOp, headers), true));
            break;
        case OP_HTTP_POST:
            TRY(HttpUrl(op + OFF(httpPostOp, url)));
            TRY(Array(PAIR(op, httpPostOp, data), 1, CONFIG_MAX_OP_DATA, &p, &n));
            TRY(Str(op + OFF(httpPostOp, contentType), false));
            TRY(Str(op + OFF(httpPostOp, headers), true));
            break;
        case OP_UDP_PACKET:
            TRY(Str(op + OFF(udpPacketOp, host), false));
            TRY(Array(PAIR(op, udpPacketOp, data), 1, CONFIG_MAX_OP_DATA, &p, &n));
            break;
        case OP_SEND_WOL:
            TRY(Str(op + OFF(sendWolOp, broadcast), true));
            break;
        case OP_SCRIPT:
            // Source with an explicit length, and the terminator after it
            TRY(Array(PAIR(op, scriptOp, source), 1, CONFIG_MAX_SCRIPT, &p, &n));
            if (p == 0 || (uint64_t)p + n >= m_table || m_b[p + n] != 0)
                return ConfigError::BadPointer;
            break;
        default:
            break;      // delay: any duration
        }
        return ConfigError::Ok;
    }

    // (count, ptr) of op pointers
    ConfigError Validator::OpList(uint32_t count_off, uint32_t ptr_off)
    {
        uint32_t list, n;
        TRY(Array(count_off, ptr_off, sizeof(cfgdata::op *), CONFIG_MAX_OPS, &list, &n));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t op;
            TRY(Ptr(list + i * sizeof(cfgdata::op *), &op));
            if (op == 0)
                return ConfigError::BadPointer;
            TRY(Op(op));
        }
        return ConfigError::Ok;
    }

    ConfigError Validator::Bindings(uint32_t count_off, uint32_t ptr_off)
    {
        uint32_t list, n;
        TRY(Array(count_off, ptr_off, SIZE(binding), CONFIG_MAX_BINDINGS, &list, &n));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t b = list + i * SIZE(binding);
            TRY(Str(b + OFF(binding, control), false));
            TRY(Str(b + OFF(binding, event), false));
            TRY(OpList(PAIR(b, binding, ops)));
        }
        return ConfigError::Ok;
    }

    ConfigError Validator::Device(uint32_t d)
    {
        TRY(Str(d + OFF(device, name), false));
        TRY(OpList(PAIR(d, device, turnOn)));
        return OpList(PAIR(d, device, turnOff));
    }

    ConfigError Validator::Activity(uint32_t a)
    {
        TRY(Str(a + OFF(activity, name), false));
        TRY(Str(a + OFF(activity, displayName), true));

        uint32_t list, n;
        TRY(Array(PAIR(a, activity, devices), sizeof(int32_t), CONFIG_MAX_DEVICES, &list, &n));
        for (uint32_t i = 0; i < n; i++)
            if (U32(list + i * sizeof(int32_t)) >= m_device_count)       // int32: a negative index is huge
                return ConfigError::BadIndex;

        TRY(Array(PAIR(a, activity, pages), SIZE(page), CONFIG_MAX_ACTIVITY_PAGES, &list, &n));
        if (n == 0)
            return ConfigError::BadLimit;                   // an activity needs a page
        for (uint32_t i = 0; i < n; i++)
            TRY(Page(list + i * SIZE(page)));

        TRY(Bindings(PAIR(a, activity, bindings)));
        TRY(OpList(PAIR(a, activity, willDeactivate)));
        TRY(OpList(PAIR(a, activity, willActivate)));
        TRY(OpList(PAIR(a, activity, didActivate)));
        TRY(OpList(PAIR(a, activity, didDeactivate)));
        return ConfigError::Ok;
    }

    ConfigError Validator::Run(ConfigSummary *out)
    {
        TRY(Header());
        TRY(Relocations());

        const uint32_t root = BPAK_HEADER_SIZE;
        const uint32_t home = root + OFF(configRoot, homePage);
        if (U32(root + OFF(configRoot, version)) != CONFIG_BLOB_VERSION)
            return ConfigError::BadVersion;
        m_device_count   = U32(root + OFF(configRoot, devices_count));
        m_activity_count = U32(root + OFF(configRoot, activities_count));
        if (m_device_count > CONFIG_MAX_DEVICES || m_activity_count > CONFIG_MAX_ACTIVITIES)
            return ConfigError::BadLimit;

        ConfigSummary s = {};
        s.version       = U32(root + OFF(configRoot, version));
        s.long_press_ms = U32(root + OFF(configRoot, longPressMs));
        s.device_count  = m_device_count;
        s.activity_count = m_activity_count;

        for (int k = 0; k < COMP_KEYS; k++)
            TRY(Key(home + OFF(home, keys) + k * SIZE(key), s.home[k]));
        TRY(Bindings(PAIR(home, home, bindings)));

        uint32_t list, n;
        TRY(Array(PAIR(root, configRoot, devices), SIZE(device), CONFIG_MAX_DEVICES, &list, &n));
        for (uint32_t i = 0; i < n; i++)
            TRY(Device(list + i * SIZE(device)));
        TRY(Array(PAIR(root, configRoot, activities), SIZE(activity), CONFIG_MAX_ACTIVITIES, &list, &n));
        for (uint32_t i = 0; i < n; i++)
            TRY(Activity(list + i * SIZE(activity)));

        // Every table entry was a pointer slot we walked, so relocating can't
        // touch anything else
        if (m_seen_count != m_relocs)
            return ConfigError::BadRelocation;

        *out = s;
        return ConfigError::Ok;
    }
}

const char *ConfigErrorName(ConfigError e)
{
    switch (e) {
    case ConfigError::Ok:            return "ok";
    case ConfigError::Truncated:     return "truncated";
    case ConfigError::BadSignature:  return "bad signature";
    case ConfigError::BadVersion:    return "unsupported version";
    case ConfigError::BadHeader:     return "bad header";
    case ConfigError::BadRelocation: return "bad relocation table";
    case ConfigError::BadPointer:    return "pointer out of range";
    case ConfigError::BadAlignment:  return "misaligned";
    case ConfigError::BadLimit:      return "over a limit";
    case ConfigError::BadOp:         return "bad op";
    case ConfigError::BadIndex:      return "index out of range";
    case ConfigError::BadImage:      return "bad image";
    }
    return "?";
}

ConfigError ConfigValidate(const void *blob, uint32_t size, ConfigSummary *summary)
{
    if (((uintptr_t)blob & 3) != 0)
        return ConfigError::BadAlignment;
    Validator v((const uint8_t *)blob, size);
    return v.Run(summary);
}

void ConfigRelocate(void *blob, uint32_t size, uint32_t base)
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
