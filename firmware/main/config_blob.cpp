// Config blob validation and relocation. Spec: docs/standalone.md.
//
// The struct layouts below are the packed schema (tools/bake/src/schema.js;
// `node cli.js config.json --header types.h` prints the C declarations).
// Offsets are written out here instead of casting to those structs so the
// loader also runs in the 64-bit host tests. test/data/sample_config.bin is
// baked by the real packer, so a schema change that isn't mirrored here
// fails TestConfigBlob.

#include <string.h>
#include "Core.h"
#include "rle565.h"
#include "config_blob.h"

using namespace SimpleLib;

namespace
{
    // --- Packed layouts (bytes) ---
    enum
    {
        KEY_SIZE            = 36,       // 4 x (count, ptr), longPressMs
        BINDING_SIZE        = 16,       // control, event, ops count, ops ptr
        PAGE_SIZE           = KEY_SIZE * COMP_KEYS,
        DEVICE_SIZE         = 20,       // name, turnOn (count, ptr), turnOff (count, ptr)
        ACTIVITY_SIZE       = 64,

        ROOT_SIZE           = 572,
        ROOT_VERSION        = 0,
        ROOT_LONGPRESS      = 4,
        ROOT_HOME_KEYS      = 8,
        ROOT_HOME_BINDINGS  = 8 + PAGE_SIZE,        // count, ptr
        ROOT_DEVICES        = ROOT_HOME_BINDINGS + 8,
        ROOT_ACTIVITIES     = ROOT_DEVICES + 8,

        ACT_NAME = 0, ACT_DISPLAY = 4, ACT_DEVICES = 8, ACT_PAGES = 16, ACT_BINDINGS = 24, ACT_HOOKS = 32,
    };
    static_assert(ROOT_ACTIVITIES + 8 == ROOT_SIZE, "root layout");

    // Op codes (tools/bake/src/consts.js OP)
    enum { OP_DELAY = 1, OP_SWITCH_ACTIVITY, OP_HID_KEY, OP_HTTP_GET, OP_HTTP_POST, OP_UDP_PACKET, OP_SEND_WOL, OP_SCRIPT };
    #define HID_MODE_MAX        3
    #define CONFIG_MAX_OP_DATA  4096

    #define TRY(expr) do { ConfigError e_ = (expr); if (e_ != ConfigError::Ok) return e_; } while (0)

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
        ConfigError Image(uint32_t count_off, ConfigImage *out);
        ConfigError Key(uint32_t key, ConfigImage out[KEY_SLOTS]);
        ConfigError Page(uint32_t page);
        ConfigError Op(uint32_t op);
        ConfigError OpList(uint32_t count_off);
        ConfigError Bindings(uint32_t count_off);
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
        if (m_table < BPAK_HEADER_SIZE + ROOT_SIZE)
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

    // An rle565 image given as (count, ptr): absent, or a whole valid image
    // of the key size, 4-byte aligned in memory
    ConfigError Validator::Image(uint32_t count_off, ConfigImage *out)
    {
        uint32_t p, n;
        TRY(Array(count_off, count_off + 4, 1, 0xFFFFFFFFu, &p, &n));
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
            TRY(Image(key + s * 8, &out[s]));
        return ConfigError::Ok;
    }

    ConfigError Validator::Page(uint32_t page)
    {
        for (int k = 0; k < COMP_KEYS; k++) {
            ConfigImage ignored[KEY_SLOTS];
            TRY(Key(page + k * KEY_SIZE, ignored));
        }
        return ConfigError::Ok;
    }

    ConfigError Validator::Op(uint32_t op)
    {
        // The code, then the whole struct for it
        static const uint8_t sizes[] = { 0, 8, 8, 16, 12, 24, 20, 16, 12 };
        if ((uint64_t)op + 4 > m_table)
            return ConfigError::BadPointer;
        uint32_t code = U32(op);
        if (code < OP_DELAY || code > OP_SCRIPT)
            return ConfigError::BadOp;
        if ((uint64_t)op + sizes[code] > m_table)
            return ConfigError::BadPointer;

        uint32_t p, n;
        switch (code) {
        case OP_SWITCH_ACTIVITY:
            if (U32(op + 4) >= m_activity_count)
                return ConfigError::BadIndex;
            break;
        case OP_HID_KEY:
            if (m_b[op + 4] > HID_MODE_MAX)
                return ConfigError::BadOp;
            break;
        case OP_HTTP_GET:
            TRY(Str(op + 4, false));
            TRY(Str(op + 8, true));
            break;
        case OP_HTTP_POST:
            TRY(Str(op + 4, false));
            TRY(Array(op + 8, op + 12, 1, CONFIG_MAX_OP_DATA, &p, &n));
            TRY(Str(op + 16, false));
            TRY(Str(op + 20, true));
            break;
        case OP_UDP_PACKET:
            TRY(Str(op + 4, false));
            TRY(Array(op + 12, op + 16, 1, CONFIG_MAX_OP_DATA, &p, &n));
            break;
        case OP_SEND_WOL:
            TRY(Str(op + 12, true));
            break;
        case OP_SCRIPT:
            // Source with an explicit length, and the terminator after it
            TRY(Array(op + 4, op + 8, 1, 0xFFFFFFFFu, &p, &n));
            if (p == 0 || (uint64_t)p + n >= m_table || m_b[p + n] != 0)
                return ConfigError::BadPointer;
            break;
        default:
            break;      // delay: any duration
        }
        return ConfigError::Ok;
    }

    // (count, ptr) of op pointers
    ConfigError Validator::OpList(uint32_t count_off)
    {
        uint32_t list, n;
        TRY(Array(count_off, count_off + 4, 4, CONFIG_MAX_OPS, &list, &n));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t op;
            TRY(Ptr(list + i * 4, &op));
            if (op == 0)
                return ConfigError::BadPointer;
            TRY(Op(op));
        }
        return ConfigError::Ok;
    }

    ConfigError Validator::Bindings(uint32_t count_off)
    {
        uint32_t list, n;
        TRY(Array(count_off, count_off + 4, BINDING_SIZE, CONFIG_MAX_BINDINGS, &list, &n));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t b = list + i * BINDING_SIZE;
            TRY(Str(b, false));
            TRY(Str(b + 4, false));
            TRY(OpList(b + 8));
        }
        return ConfigError::Ok;
    }

    ConfigError Validator::Device(uint32_t d)
    {
        TRY(Str(d, false));
        TRY(OpList(d + 4));
        return OpList(d + 12);
    }

    ConfigError Validator::Activity(uint32_t a)
    {
        TRY(Str(a + ACT_NAME, false));
        TRY(Str(a + ACT_DISPLAY, true));

        uint32_t list, n;
        TRY(Array(a + ACT_DEVICES, a + ACT_DEVICES + 4, 4, CONFIG_MAX_DEVICES, &list, &n));
        for (uint32_t i = 0; i < n; i++)
            if (U32(list + i * 4) >= m_device_count)       // int32: a negative index is huge
                return ConfigError::BadIndex;

        TRY(Array(a + ACT_PAGES, a + ACT_PAGES + 4, PAGE_SIZE, CONFIG_MAX_ACTIVITY_PAGES, &list, &n));
        if (n == 0)
            return ConfigError::BadLimit;                   // an activity needs a page
        for (uint32_t i = 0; i < n; i++)
            TRY(Page(list + i * PAGE_SIZE));

        TRY(Bindings(a + ACT_BINDINGS));
        for (int h = 0; h < 4; h++)
            TRY(OpList(a + ACT_HOOKS + h * 8));
        return ConfigError::Ok;
    }

    ConfigError Validator::Run(ConfigSummary *out)
    {
        TRY(Header());
        TRY(Relocations());

        const uint32_t root = BPAK_HEADER_SIZE;
        if (U32(root + ROOT_VERSION) != CONFIG_BLOB_VERSION)
            return ConfigError::BadVersion;
        m_device_count   = U32(root + ROOT_DEVICES);
        m_activity_count = U32(root + ROOT_ACTIVITIES);
        if (m_device_count > CONFIG_MAX_DEVICES || m_activity_count > CONFIG_MAX_ACTIVITIES)
            return ConfigError::BadLimit;

        ConfigSummary s = {};
        s.version       = U32(root + ROOT_VERSION);
        s.long_press_ms = U32(root + ROOT_LONGPRESS);
        s.device_count  = m_device_count;
        s.activity_count = m_activity_count;

        for (int k = 0; k < COMP_KEYS; k++)
            TRY(Key(root + ROOT_HOME_KEYS + k * KEY_SIZE, s.home[k]));
        TRY(Bindings(root + ROOT_HOME_BINDINGS));

        uint32_t list, n;
        TRY(Array(root + ROOT_DEVICES, root + ROOT_DEVICES + 4, DEVICE_SIZE, CONFIG_MAX_DEVICES, &list, &n));
        for (uint32_t i = 0; i < n; i++)
            TRY(Device(list + i * DEVICE_SIZE));
        TRY(Array(root + ROOT_ACTIVITIES, root + ROOT_ACTIVITIES + 4, ACTIVITY_SIZE, CONFIG_MAX_ACTIVITIES, &list, &n));
        for (uint32_t i = 0; i < n; i++)
            TRY(Activity(list + i * ACTIVITY_SIZE));

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
