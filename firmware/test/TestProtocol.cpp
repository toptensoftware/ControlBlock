// Host protocol: framing, every request, events and stream resets, fed as
// raw bytes (docs/protocol.md)

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "protocol.h"
#include "store_env.h"
#include "flash_env.h"
#include "blob_env.h"
#include "crc32_table.h"
#include "layout_env.h"

using namespace SimpleLib;

namespace
{
    // Little-endian byte builder
    struct Bytes
    {
        List<uint8_t> b;
        Bytes &U8(uint32_t v)  { b.Add((uint8_t)v); return *this; }
        Bytes &U16(uint32_t v) { return U8(v).U8(v >> 8); }
        Bytes &U32(uint32_t v) { return U16(v).U16(v >> 16); }
        Bytes &U64(uint64_t v) { return U32((uint32_t)v).U32((uint32_t)(v >> 32)); }
        Bytes &Raw(const void *p, size_t n) { for (size_t i = 0; i < n; i++) U8(((const uint8_t *)p)[i]); return *this; }
        Bytes &Add(const Bytes &o) { return Raw(o.b.GetBuffer(), o.b.GetCount()); }
        int Count() const { return b.GetCount(); }
    };

    Bytes Message(uint8_t type, uint16_t seq, const Bytes &payload = Bytes())
    {
        Bytes m;
        m.U16(PROTO_MAGIC).U8(type).U8(0).U16(seq).U16(0).U32(payload.Count());
        m.Add(payload);
        return m;
    }

    // A message read back. `data` points into the fixture's read buffer,
    // valid until the next Read().
    struct Msg
    {
        uint8_t type, flags;
        uint16_t seq;
        const uint8_t *data;
        int len;
        uint16_t Status() const { return len >= 2 ? (uint16_t)(data[0] | data[1] << 8) : 0xFFFF; }
        uint32_t U8At(int i) const  { return data[i]; }
        uint32_t U16At(int i) const { return data[i] | data[i + 1] << 8; }
        uint32_t U32At(int i) const { return U16At(i) | U16At(i + 2) << 16; }
        uint64_t U64At(int i) const { return U32At(i) | (uint64_t)U32At(i + 4) << 32; }
    };

    struct Fixture
    {
        StoreEnv   env;
        ImageStore store;
        KeyLayout  layout;
        FakeFlash  flash;
        LayoutStore layout_store;
        FakeBlobStorage wifi_storage;
        WifiConfig wifi;
        FakeBlobStorage clock_storage;
        ClockConfig clock;
        FakeBlobStorage name_storage;
        DeviceName name;
        int  name_changes = 0;
        FakeBlobStorage display_storage;
        DisplayConfig display;
        int  display_changes = 0;
        Protocol   proto;

        List<uint8_t> out;
        int  lock_depth = 0, comp_resets = 0, page_count = -1, page_set = -1, current = 0;
        size_t space = 1 << 20;         // write_space: room for events
        bool animate = false, backlight = true;
        char status_text[2][64] = {};           // last text per STATUS_LEFT / STATUS_RIGHT
        int  status_sets = 0;
        int  restarts = 0;
        WifiStatus wifi_st = {};                // what the device layer reports
        int  wifi_changes = 0;                  // credential changes the device layer was told of
        int  clock_changes = 0;                 // clock setting changes
        int64_t time_ms = 0;                    // the device clock (1970 at power-up: not known)
        ClockSource time_source = ClockSource::None;
        int  time_sets = 0;
        List<int> changed_pages;
        List<int> mode_changes;                 // 1 = hosted, 0 = standalone, per mode_changed call
        List<uint8_t> home_blob;                // UseHome(): the sample layout
        LayoutSummary home = {};

        // `hosted`: take control first (most tests are of a hosted session);
        // the device boots standalone, and the takeover's effects are not counted.
        // The store refuses non-opaque images, as the device's does.
        explicit Fixture(uint32_t capacity = 1u << 20, bool hosted = true) : flash(16 * LAYOUT_SECTOR)
        {
            layout_store.Init(flash.Host());
            wifi.Init(wifi_storage.Host());
            wifi.SetChangedCallback([](const WifiCredentials &, void *c) { ((Fixture *)c)->wifi_changes++; }, this);
            clock.Init(clock_storage.Host());
            clock.SetChangedCallback([](const ClockSettings &, void *c) { ((Fixture *)c)->clock_changes++; }, this);
            static const uint8_t mac[6] = { 1, 2, 3, 0xa1, 0xb2, 0xc3 };
            name.Init(name_storage.Host(), mac);
            name.SetChangedCallback([](const char *, void *c) { ((Fixture *)c)->name_changes++; }, this);
            display.Init(display_storage.Host(), DisplayTimingLimitsFor(20));      // as the device's 20 MHz build
            display.SetChangedCallback([](const DisplayTimingSetting &, void *c) { ((Fixture *)c)->display_changes++; }, this);

            store.Init(env.Host(), capacity, KEY_GRID_W, KEY_GRID_H, true);
            store.SetListener(&layout);
            layout.Init(&store, Changed, this);

            ProtocolHost h = {};
            h.write            = [](const void *d, size_t n, void *c) {
                Fixture *f = (Fixture *)c;
                for (size_t i = 0; i < n; i++)
                    f->out.Add(((const uint8_t *)d)[i]);
                f->space = f->space > n ? f->space - n : 0;
            };
            h.write_space      = [](void *c) { return ((Fixture *)c)->space; };
            h.flush            = [](void *) {};
            h.lock             = [](void *c) { Check(((Fixture *)c)->lock_depth++ == 0); };
            h.unlock           = [](void *c) { Check(--((Fixture *)c)->lock_depth == 0); };
            h.reset_compositor = [](void *c) { Check(((Fixture *)c)->lock_depth == 0); ((Fixture *)c)->comp_resets++; };
            h.set_page_count   = [](int n, void *c) { ((Fixture *)c)->page_count = n; };
            h.set_page         = [](int p, bool a, void *c) { ((Fixture *)c)->page_set = p; ((Fixture *)c)->animate = a; };
            h.current_page     = [](void *c) { return ((Fixture *)c)->current; };
            h.set_backlight    = [](bool on, void *c) { ((Fixture *)c)->backlight = on; };
            h.backlight_on     = [](void *c) { return ((Fixture *)c)->backlight; };
            h.set_status_text  = [](int field, const char *t, void *c) {
                Fixture *f = (Fixture *)c;
                Check(f->lock_depth == 0 && (field == STATUS_TEXT_LEFT || field == STATUS_TEXT_RIGHT));
                Check(strlen(t) < STATUS_TEXT_MAX);
                strcpy(f->status_text[field], t);
                f->status_sets++;
            };
            h.mode_changed     = [](bool hosted, void *c) { ((Fixture *)c)->mode_changes.Add(hosted ? 1 : 0); };
            h.restart          = [](void *c) { ((Fixture *)c)->restarts++; };
            h.wifi_status      = [](WifiStatus *st, void *c) { *st = ((Fixture *)c)->wifi_st; };
            h.set_time         = [](int64_t ms, void *c) {
                Fixture *f = (Fixture *)c;
                Check(f->lock_depth == 0);
                f->time_ms = ms;
                f->time_source = ClockSource::Host;
                f->time_sets++;
            };
            h.get_time         = [](int64_t *ms, ClockSource *source, bool *known, void *c) {
                Fixture *f = (Fixture *)c;
                *ms = f->time_ms;
                *known = clock_time_valid(f->time_ms);
                *source = *known ? f->time_source : ClockSource::None;
            };
            h.fw_version       = "1.2.3";
            h.serial           = "ABCDEF";
            h.ctx              = this;
            proto.Init(&store, &layout, &layout_store, &wifi, &clock, &name, &display, h);

            if (hosted) {
                Request(MSG_TAKE_CONTROL);
                comp_resets = 0;
                page_count = -1;
                status_sets = 0;
                memset(status_text, 0, sizeof(status_text));
                changed_pages.Clear();
                mode_changes.Clear();
            }
        }

        // The sample layout is the device's: its Home is shown now (as at boot)
        // and the protocol rebuilds it on release. Use with hosted = false.
        void UseHome()
        {
            home_blob = layout_env::LoadSample();
            Assert(LayoutValidate(home_blob.GetBuffer(), (uint32_t)home_blob.GetCount(), &home) == LayoutError::Ok);
            proto.SetHome(&home, home_blob.GetBuffer());
            lock_depth++;                       // model changes are made under the lock
            Assert(LayoutApplyHome(home, home_blob.GetBuffer(), &store, &layout));
            lock_depth--;
            changed_pages.Clear();
        }

        // Is Home's key `i` (in the blob's order) there on page 0, showing its image?
        bool ShowsHome(int i)
        {
            const LayoutKey &k = home.home[i];
            int page = -1;
            return layout.Find(k.id, &page) && page == 0 &&
                   ShownId(k.id) == home_blob.GetBuffer() + k.normal.offset;
        }

        // What key `id` shows now (null: no such key, or no image)
        const void *ShownId(uint16_t id)
        {
            const KeyState *key = layout.Find(id);
            const ImageEntry *e = key ? KeyLayout::ShownEntry(*key) : nullptr;
            return e ? e->pixels : nullptr;
        }

        // The same for a plain grid slot's key (the tests' Slot() keys and Home's,
        // which have the plain grid's ids)
        const void *Shown(int page, int k) { return ShownId(KeySlotId(page, k)); }
        bool Active(int page, int k) { return layout.IsActive(KeySlotId(page, k)); }

        const void *Pixels(uint32_t image) { return store.Find(image)->data->Pixels(); }

        static void Changed(int page, uint32_t, void *c)
        {
            Fixture *f = (Fixture *)c;
            Check(f->lock_depth == 1);          // model changes happen under the lock
            f->changed_pages.Add(page);
        }

        // Send bytes, all at once or `piece` bytes at a time
        void Send(const Bytes &m, int piece = 0)
        {
            int n = m.Count();
            if (!piece)
                piece = n;
            for (int i = 0; i < n; i += piece)
                proto.Receive(m.b.GetBuffer() + i, (size_t)(i + piece <= n ? piece : n - i));
        }

        // Parse and clear what's been written
        List<Msg> Read()
        {
            read.Clear();
            read.AddRange(out);
            out.Clear();

            List<Msg> msgs;
            int i = 0;
            while (i + PROTO_HEADER_SIZE <= read.GetCount()) {
                const uint8_t *h = read.GetBuffer() + i;
                Check((h[0] | h[1] << 8) == PROTO_MAGIC);
                Msg m;
                m.type  = h[2];
                m.flags = h[3];
                m.seq   = (uint16_t)(h[4] | h[5] << 8);
                m.len   = (int)(h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24);
                m.data  = h + PROTO_HEADER_SIZE;
                Check(i + PROTO_HEADER_SIZE + m.len <= read.GetCount());
                i += PROTO_HEADER_SIZE + m.len;
                msgs.Add(m);
            }
            Check(i == read.GetCount());
            return msgs;
        }
        List<uint8_t> read;

        // One request, one response
        Msg Request(uint8_t type, const Bytes &payload = Bytes(), int piece = 0)
        {
            static uint16_t seq = 100;
            Send(Message(type, ++seq, payload), piece);
            List<Msg> r = Read();
            Check(r.GetCount() == 1);
            Check(r[0].type == type && r[0].flags == PROTO_FLAG_RESPONSE && r[0].seq == seq);
            return r[0];
        }

        Bytes PutImage(uint32_t id, const void *img, uint32_t offset = 0, uint32_t len = 0)
        {
            uint32_t size = env.Size(img);
            if (!len)
                len = size - offset;
            Bytes p;
            p.U32(id).U32(size).U32(offset).Raw((const uint8_t *)img + offset, len);
            return p;
        }

        uint16_t Upload(uint32_t id, const void *img) { return Request(MSG_PUT_IMAGE, PutImage(id, img)).Status(); }
    };
}

namespace
{
    Bytes Ids(int count, const uint32_t *ids)
    {
        Bytes p;
        p.U16(count);
        for (int i = 0; i < count; i++)
            p.U32(ids[i]);
        return p;
    }

    // SET_KEYS' KeyDef (docs/protocol.md): rect in quarter units
    struct WireKey
    {
        uint16_t id;
        uint8_t  page, x, y, w, h;
        uint16_t flags;
        uint32_t normal, pressed;
    };

    Bytes Keys(int count, const WireKey *defs)
    {
        Bytes p;
        p.U16(count);
        for (int i = 0; i < count; i++) {
            const WireKey &d = defs[i];
            int start = p.Count();
            p.U16(d.id).U8(d.page).U8(d.x).U8(d.y).U8(d.w).U8(d.h).U8(0);
            p.U16(d.flags).U16(0).U32(d.normal).U32(d.pressed);
            Check(p.Count() - start == PROTO_KEYDEF_SIZE);
        }
        return p;
    }

    // A 1U key in plain grid slot `slot` with the plain grid's id
    WireKey Slot(int page, int slot, uint32_t normal, uint32_t pressed = 0, uint16_t flags = 0)
    {
        KeyRect r = KeySlotRect(slot);
        return WireKey{ KeySlotId(page, slot), (uint8_t)page, r.x, r.y, r.w, r.h, flags, normal, pressed };
    }

    // Removes key `id`
    WireKey Remove(uint16_t id) { return WireKey{ id, 0, 0, 0, 0, 0, PROTO_KEYDEF_REMOVE, 0, 0 }; }
}

Fact("Protocol Ping Echoes And Copies Seq")
{
    Fixture f;
    Bytes p;
    p.U32(0xDEADBEEF).U8(7);
    Msg m = f.Request(MSG_PING, p);
    Assert(m.Status() == ST_OK);
    Assert(m.len == 4 + 5);
    Assert(m.U32At(4) == 0xDEADBEEF && m.U8At(8) == 7);
    Assert(f.proto.HostSeen());
}

Fact("Protocol Messages Can Arrive In Any Pieces And Pipelined")
{
    Fixture f;
    Bytes all;
    for (int i = 0; i < 5; i++) {
        Bytes p;
        p.U8(i);
        all.Add(Message(MSG_PING, (uint16_t)(1000 + i), p));
    }
    all.Add(Message(MSG_PUT_IMAGE, 2000, f.PutImage(5, f.env.Key1U(0))));
    all.Add(Message(MSG_PING, 2001));

    static const int pieces[] = { 1, 3, 7, 64, 1000 };
    for (int piece : pieces) {
        f.Send(all, piece);
        List<Msg> r = f.Read();
        Assert(r.GetCount() == 7);
        for (int i = 0; i < 5; i++)
            Assert(r[i].seq == 1000 + i && r[i].Status() == ST_OK && r[i].U8At(4) == (uint32_t)i);
        Assert(r[5].seq == 2000 && r[5].Status() == ST_OK);
        Assert(r[6].seq == 2001 && r[6].len == 4);
    }
    Assert(f.store.IsLive(5));
}

Fact("Protocol Get Info")
{
    Fixture f;
    f.current = 0;
    Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);
    Msg m = f.Request(MSG_GET_INFO);
    Assert(m.Status() == ST_OK);
    int i = 4;
    Assert(PROTO_VERSION == 2);
    Assert(m.U16At(i) == PROTO_VERSION);                      i += 2;
    Assert(m.U16At(i) == (INFO_FLAG_BACKLIGHT | INFO_FLAG_HOSTED)); i += 2;     // flags: backlight on, hosted
    Assert(m.U32At(i) == PROTO_MAX_PAYLOAD);                  i += 4;
    Assert(m.U32At(i) == 1u << 20);                           i += 4;     // capacity
    Assert(m.U32At(i) == sizeof(ImageData) + f.env.Size(f.env.Key1U(0))); i += 4;
    Assert(m.U32At(i) == 1);                                  i += 4;     // count
    Assert(m.U32At(i) > 0);                                   i += 4;     // largest free
    Assert(m.U16At(i) == SCREEN_W && m.U16At(i + 2) == SCREEN_H); i += 4;
    Assert(m.U8At(i) == 5 && m.U8At(i + 1) == 3);             i += 2;     // grid cols, rows (1U cells)
    Assert(m.U8At(i) == KEY_LAYOUT_MAX_PAGES && m.U8At(i + 1) == 1);   i += 2;
    Assert(m.U16At(i) == 144 && m.U16At(i + 2) == 144);       i += 4;     // key_w/h: the 1U cell
    Assert(m.U16At(i) == KEY_ICON_SIZE && m.U16At(i + 2) == KEY_ICON_SIZE); i += 4;
    Assert(m.U8At(i) == 0);                                   i += 1;     // current page
    Assert(m.U8At(i) == 5 && memcmp(m.data + i + 1, "1.2.3", 5) == 0); i += 6;
    Assert(m.U8At(i) == 6 && memcmp(m.data + i + 1, "ABCDEF", 6) == 0); i += 7;
    Assert(m.U32At(i) == 15 * LAYOUT_SECTOR);                 i += 4;     // layout capacity
    Assert(m.U32At(i) == 0 && m.U32At(i + 4) == 0);           i += 8;     // nothing stored
    // v2: the grid on the screen, the quarter unit, the largest image, keys per page
    Assert(m.U16At(i) == 40 && m.U16At(i + 2) == SCREEN_PAGE_TOP + 4); i += 4;  // grid origin
    Assert(m.U16At(i) == 720 && m.U16At(i + 2) == 432);       i += 4;     // grid size
    Assert(m.U16At(i) == 36);                                 i += 2;     // quarter unit
    Assert(m.U16At(i) == 720 && m.U16At(i + 2) == 432);       i += 4;     // max image size
    Assert(m.U8At(i) == 32);                                  i += 1;     // max keys per page
    Assert(m.U16At(i) == LAYOUT_BLOB_VERSION);                i += 2;     // layout blob version
    Assert(m.len == i);
}

Fact("Protocol Upload Query And Set Keys")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);

    // In chunks, with other messages between them
    const void *icon = f.env.Key1U(3);
    uint32_t size = f.env.Size(icon), half = size / 2;
    Assert(f.Request(MSG_PUT_IMAGE, f.PutImage(2, icon, 0, half)).Status() == ST_OK);
    Assert(f.Request(MSG_PING).Status() == ST_OK);
    Assert(!f.store.IsLive(2));
    Assert(f.Request(MSG_PUT_IMAGE, f.PutImage(2, icon, half)).Status() == ST_OK);

    uint32_t ids[] = { 1, 2, 3, 0 };
    Msg q = f.Request(MSG_QUERY_IMAGES, Ids(4, ids));
    Assert(q.Status() == ST_OK && q.U16At(4) == 4 && q.len == 7);
    Assert(q.U8At(6) == 0x03);

    WireKey defs[] = { { 500, 0, 4, 0, 4, 4, 0, 1, 2 } };     // slot 1, any id
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    Assert(f.ShownId(500) == f.Pixels(1));
    const KeyState *k = f.layout.Find(500);
    Assert(k && k->rect.x == 4 && k->rect.y == 0 && k->rect.w == 4 && k->rect.h == 4 && k->flags == 0);
    Assert(f.changed_pages.GetCount() == 1 && f.changed_pages[0] == 0);
    f.lock_depth++;                     // model changes are made under the lock
    Assert(f.layout.SetPressed(500, true));
    f.lock_depth--;
    Assert(f.ShownId(500) == f.Pixels(2));

    // Another key on the page leaves this one as it is (still pressed)
    WireKey more[] = { Slot(0, 4, 2) };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, more)).Status() == ST_OK);
    Assert(f.layout.KeyCount(0) == 2 && f.ShownId(500) == f.Pixels(2));
    Assert(f.Shown(0, 4) == f.Pixels(2));
}

Fact("Protocol Set Keys Spans And Quarter Units")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Key2U(0)) == ST_OK);       // 288x144
    Assert(f.Upload(2, f.env.Key1U(0)) == ST_OK);
    Assert(f.Upload(3, f.env.Key1U(1)) == ST_OK);
    // A 2U key, a 1U key a quarter unit to its right (x 2.25U), and a disabled
    // 1U key in the bottom right corner
    WireKey defs[] = {
        { 10, 0, 0, 0, 8, 4, 0, 1, 0 },
        { 11, 0, 9, 0, 4, 4, 0, 2, 3 },
        { 12, 0, 16, 8, 4, 4, PROTO_KEYDEF_DISABLED, 3, 0 },
    };
    Assert(f.Request(MSG_SET_KEYS, Keys(3, defs)).Status() == ST_OK);
    Assert(f.layout.KeyCount(0) == 3);
    Assert(f.ShownId(10) == f.Pixels(1) && f.ShownId(11) == f.Pixels(2) && f.ShownId(12) == f.Pixels(3));
    Assert(f.layout.IsActive(10) && f.layout.IsActive(11) && !f.layout.IsActive(12));
    comp_page_t page;
    f.lock_depth++;
    Assert(f.layout.GetPage(0, &page));
    f.lock_depth--;
    Assert(page.count == 3);
    Assert(page.keys[1].rect.x == KEY_GRID_X0 + 9 * KEY_QUARTER && page.keys[1].rect.w == KEY_CELL);
}

Fact("Protocol Set Keys Edits Keys By Id")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);
    Assert(f.Upload(2, f.env.Key1U(1)) == ST_OK);
    Bytes n;
    n.U8(3);
    Assert(f.Request(MSG_SET_PAGE_COUNT, n).Status() == ST_OK);
    WireKey defs[] = { Slot(0, 0, 1), Slot(0, 1, 2), Slot(1, 0, 1) };
    Assert(f.Request(MSG_SET_KEYS, Keys(3, defs)).Status() == ST_OK);

    // Same id: the key is redefined (new image, new place); the others stay
    WireKey moved[] = { Slot(0, 0, 2) };
    moved[0].x = 8;
    moved[0].y = 4;                     // slot 7
    Assert(f.Request(MSG_SET_KEYS, Keys(1, moved)).Status() == ST_OK);
    const KeyState *k = f.layout.Find(KeySlotId(0, 0));
    Assert(k && k->rect.x == 8 && k->rect.y == 4 && f.Shown(0, 0) == f.Pixels(2));
    Assert(f.layout.KeyCount(0) == 2 && f.layout.KeyCount(1) == 1);

    // A key stays on its page: naming its id on another page is refused...
    WireKey other_page[] = { Slot(0, 1, 1) };
    other_page[0].page = 2;
    Assert(f.Request(MSG_SET_KEYS, Keys(1, other_page)).Status() == ST_BAD_ARG);
    // ...so it moves by being removed, then defined there
    f.changed_pages.Clear();
    WireKey gone[] = { Remove(KeySlotId(0, 1)), Remove(999) };      // an unknown id is ignored
    Assert(f.Request(MSG_SET_KEYS, Keys(2, gone)).Status() == ST_OK);
    Assert(!f.layout.Find(KeySlotId(0, 1)) && f.layout.KeyCount(0) == 1);
    Assert(f.changed_pages.GetCount() == 1 && f.changed_pages[0] == 0);
    Assert(f.Request(MSG_SET_KEYS, Keys(1, other_page)).Status() == ST_OK);
    int page = -1;
    Assert(f.layout.Find(KeySlotId(0, 1), &page) && page == 2);

    // In one message: page 2's key moves from slot 1 to slot 0 while a new key
    // takes slot 1 (applied together, so they never overlap), and page 1's key
    // gets a new image
    WireKey moved_key = Slot(0, 1, 2);
    moved_key.page = 2;
    moved_key.x = 0;
    WireKey new_key = Slot(2, 1, 1);
    WireKey restyled = Slot(1, 0, 2);
    WireKey together[] = { new_key, moved_key, restyled };
    Assert(f.Request(MSG_SET_KEYS, Keys(3, together)).Status() == ST_OK);
    Assert(f.layout.KeyCount(2) == 2 && f.layout.Find(KeySlotId(0, 1))->rect.x == 0);
    Assert(f.layout.Find(KeySlotId(2, 1))->rect.x == 4 && f.Shown(1, 0) == f.Pixels(2));

    // Nothing at all
    Assert(f.Request(MSG_SET_KEYS, Keys(0, nullptr)).Status() == ST_OK);
}

Fact("Protocol Set Keys Refusals Change Nothing")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);
    Assert(f.Upload(2, f.env.Key2U(0)) == ST_OK);
    Assert(f.Upload(3, f.env.Face(0, true)) == ST_OK);   // opaque, but 128x128
    Bytes n;
    n.U8(2);
    Assert(f.Request(MSG_SET_PAGE_COUNT, n).Status() == ST_OK);
    WireKey base[] = { Slot(0, 0, 1), Slot(1, 0, 1) };
    Assert(f.Request(MSG_SET_KEYS, Keys(2, base)).Status() == ST_OK);
    f.changed_pages.Clear();

    struct Case { WireKey key; WireKey second; int count; uint16_t status; };
    WireKey none = {};
    WireKey overlap = Slot(0, 2, 1);
    overlap.x = 2;                                          // half over slot 0's key
    WireKey out_x = Slot(0, 4, 1);
    out_x.x = 17;                                           // past the right edge
    WireKey out_y = Slot(0, 10, 1);
    out_y.y = 9;                                            // past the bottom
    WireKey empty = Slot(0, 3, 1);
    empty.w = 0;
    WireKey zero_id = Slot(0, 3, 1);
    zero_id.id = 0;
    WireKey dup_other = Slot(0, 3, 1);
    dup_other.id = KeySlotId(1, 0);                         // page 1's key's id
    WireKey wrong_size = Slot(0, 3, 2);                     // a 2U image on a 1U key
    WireKey wrong_pressed = Slot(0, 3, 1, 2);
    WireKey small = Slot(0, 3, 3);                          // a 128 image
    WireKey missing = Slot(0, 3, 9);
    WireKey bad_page = Slot(0, 3, 1);
    bad_page.page = 2;                                      // past the page count
    WireKey bad_flag = Slot(0, 3, 1, 0, 0x8000);
    const Case cases[] = {
        { overlap,       none,              1, ST_BAD_ARG },
        { out_x,         none,              1, ST_BAD_ARG },
        { out_y,         none,              1, ST_BAD_ARG },
        { empty,         none,              1, ST_BAD_ARG },
        { zero_id,       none,              1, ST_BAD_ARG },
        { dup_other,     none,              1, ST_BAD_ARG },
        { Slot(0, 3, 1), Slot(0, 3, 1),     2, ST_BAD_ARG },   // the same id twice in one message
        { Slot(0, 3, 1), Remove(KeySlotId(0, 3)), 2, ST_BAD_ARG },
        { wrong_size,    none,              1, ST_BAD_IMAGE },
        { wrong_pressed, none,              1, ST_BAD_IMAGE },
        { small,         none,              1, ST_BAD_IMAGE },
        { missing,       none,              1, ST_NOT_FOUND },
        { bad_page,      none,              1, ST_BAD_ARG },
        { bad_flag,      none,              1, ST_BAD_ARG },
        { Remove(0),     none,              1, ST_BAD_ARG },
    };
    for (const Case &c : cases) {
        WireKey defs[] = { c.key, c.second };
        Assert(f.Request(MSG_SET_KEYS, Keys(c.count, defs)).Status() == c.status);
        // A bad key alongside a good one changes nothing either
        WireKey with_good[] = { Slot(0, 14, 1), c.key, c.second };
        Assert(f.Request(MSG_SET_KEYS, Keys(c.count + 1, with_good)).Status() == c.status);
    }
    Assert(f.changed_pages.GetCount() == 0);
    Assert(f.layout.KeyCount(0) == 1 && f.layout.KeyCount(1) == 1 && f.Shown(0, 0) == f.Pixels(1));
    Assert(f.store.Find(2)->key_refs == 0 && f.store.Find(3)->key_refs == 0);

    // Too many keys on a page: 0.25U x 1U keys to the right of slot 0's key
    List<WireKey> many;
    for (int i = 0; i < COMP_MAX_KEYS + 1; i++) {
        WireKey k = { (uint16_t)(100 + i), 0, (uint8_t)(8 + i % 12), (uint8_t)(i / 12 * 4), 1, 4, 0, 0, 0 };
        many.Add(k);
    }
    Assert(f.Request(MSG_SET_KEYS, Keys(COMP_MAX_KEYS + 1, many.GetBuffer())).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_SET_KEYS, Keys(COMP_MAX_KEYS - 1, many.GetBuffer())).Status() == ST_OK);  // + slot 0's
    WireKey one_more[] = { many[COMP_MAX_KEYS - 1] };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, one_more)).Status() == ST_BAD_ARG);
    Assert(f.layout.KeyCount(0) == COMP_MAX_KEYS);
}

Fact("Protocol Set Keys Length Errors")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);
    WireKey defs[] = { Slot(0, 0, 1) };
    Bytes p = Keys(1, defs);
    p.b.GetBuffer()[0] = 2;             // count says more than there is
    Assert(f.Request(MSG_SET_KEYS, p).Status() == ST_BAD_LENGTH);
    Bytes cut;
    cut.Raw(Keys(1, defs).b.GetBuffer(), 2 + PROTO_KEYDEF_SIZE - 1);
    Assert(f.Request(MSG_SET_KEYS, cut).Status() == ST_BAD_LENGTH);
    Assert(f.Request(MSG_SET_KEYS).Status() == ST_BAD_LENGTH);
    Assert(f.store.Find(1)->key_refs == 0 && f.changed_pages.GetCount() == 0);
}

Fact("Protocol Put Image Refuses A Non-Opaque Image")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Icon(0)) == ST_BAD_IMAGE);     // transparent padding
    Assert(f.Upload(2, f.env.Face(0)) == ST_BAD_IMAGE);
    Assert(!f.store.IsLive(1) && !f.store.IsLive(2));
    // Any opaque size up to the grid
    Assert(f.Upload(3, f.env.Face(0, true)) == ST_OK);
    Assert(f.Upload(4, f.env.Key2U(0)) == ST_OK);
    // A replace with a transparent image keeps the old one
    Assert(f.Upload(4, f.env.Icon(1)) == ST_BAD_IMAGE);
    Assert(f.Pixels(4) && memcmp(f.Pixels(4), f.env.Key2U(0), f.env.Size(f.env.Key2U(0))) == 0);
}

Fact("Protocol Upload Errors")
{
    Fixture f;
    const void *icon = f.env.Key1U(0);
    uint32_t size = f.env.Size(icon);

    // Out of order: the data is skipped and the stream stays in step
    Assert(f.Request(MSG_PUT_IMAGE, f.PutImage(2, icon, 100, 50)).Status() == ST_BAD_SEQUENCE);
    Assert(f.Request(MSG_PING).Status() == ST_OK);

    Bytes bad;
    bad.U32(0).U32(size).U32(0).Raw(icon, size);
    Assert(f.Request(MSG_PUT_IMAGE, bad).Status() == ST_BAD_ARG);

    Bytes junk;
    junk.U32(3).U32(64).U32(0);
    for (int i = 0; i < 64; i++)
        junk.U8(i);
    Assert(f.Request(MSG_PUT_IMAGE, junk).Status() == ST_BAD_IMAGE);

    Bytes short_prefix;
    short_prefix.U32(3).U32(64);
    Assert(f.Request(MSG_PUT_IMAGE, short_prefix).Status() == ST_BAD_LENGTH);

    f.env.fail_alloc = true;
    Assert(f.Upload(4, icon) == ST_NO_SPACE);
    f.env.fail_alloc = false;
    Assert(f.Upload(4, icon) == ST_OK);
}

Fact("Protocol Free Images Is Never Refused")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);
    Assert(f.Upload(2, f.env.Key1U(1)) == ST_OK);
    WireKey defs[] = { Slot(0, 0, 2) };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    const void *px = f.Pixels(2);

    uint32_t ids[] = { 2, 77 };
    Assert(f.Request(MSG_FREE_IMAGES, Ids(2, ids)).Status() == ST_OK);
    Msg q = f.Request(MSG_QUERY_IMAGES, Ids(2, ids));
    Assert(q.U8At(6) == 0);
    Assert(f.Shown(0, 0) == px);        // still drawn

    // Another key on its page leaves it there, and so does re-sending its key
    WireKey other[] = { Slot(0, 1, 1) };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, other)).Status() == ST_OK);
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    Assert(f.Shown(0, 0) == px);

    // Its key changes image; once swept, the next message frees it
    WireKey changed[] = { Slot(0, 0, 1) };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, changed)).Status() == ST_OK);
    Assert(f.env.IsAllocated(px));
    Assert(f.proto.Idle());             // still waiting
    f.env.Sweep();
    Assert(!f.proto.Idle());
    Assert(!f.env.IsAllocated(px));
}

Fact("Protocol Replace Via Put Image")
{
    Fixture f;
    Assert(f.Upload(2, f.env.Key1U(0)) == ST_OK);
    WireKey defs[] = { Slot(0, 4, 2) };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    Assert(f.Upload(2, f.env.Key1U(1)) == ST_OK);
    Assert(memcmp(f.Shown(0, 4), f.env.Key1U(1), f.env.Size(f.env.Key1U(1))) == 0);
}

Fact("Protocol Reset Clears Keys And Images")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);
    Bytes n;
    n.U8(3);
    Assert(f.Request(MSG_SET_PAGE_COUNT, n).Status() == ST_OK);
    WireKey defs[] = { Slot(2, 0, 1) };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    Bytes part;
    part.U32(9).U32(5000).U32(0).U32(0);           // an upload in progress
    Assert(f.Request(MSG_PUT_IMAGE, part).Status() == ST_OK);

    Assert(f.Request(MSG_RESET).Status() == ST_OK);
    Assert(f.comp_resets == 1);
    Assert(f.page_count == 1 && f.layout.PageCount() == 1);
    Assert(f.env.LiveBlocks() == 0);
    Assert(!f.store.IsLive(1));
}

Fact("Protocol Page Count Page And Backlight")
{
    Fixture f;
    Bytes n;
    n.U8(4);
    Assert(f.Request(MSG_SET_PAGE_COUNT, n).Status() == ST_OK);
    Assert(f.page_count == 4 && f.layout.PageCount() == 4);
    Bytes zero;
    zero.U8(0);
    Assert(f.Request(MSG_SET_PAGE_COUNT, zero).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_SET_PAGE_COUNT).Status() == ST_BAD_LENGTH);

    Bytes go;
    go.U8(3).U8(1);
    Assert(f.Request(MSG_SET_PAGE, go).Status() == ST_OK);
    Assert(f.page_set == 3 && f.animate);

    // A missing trailing field takes its default; extra bytes are ignored
    Bytes go2;
    go2.U8(2);
    Assert(f.Request(MSG_SET_PAGE, go2).Status() == ST_OK);
    Assert(f.page_set == 2 && !f.animate);
    Bytes go3;
    go3.U8(1).U8(0).U32(12345);
    Assert(f.Request(MSG_SET_PAGE, go3).Status() == ST_OK && f.page_set == 1);
    Bytes beyond;
    beyond.U8(4);
    Assert(f.Request(MSG_SET_PAGE, beyond).Status() == ST_BAD_ARG);

    Bytes off;
    off.U8(0);
    Assert(f.Request(MSG_SET_BACKLIGHT, off).Status() == ST_OK && !f.backlight);
}

// --- Backlight (docs/backlight.md) ---

namespace
{
    Bytes OnByte(uint32_t on) { Bytes p; p.U8(on); return p; }
    // The backlight tests don't care about the mode bit
    uint32_t InfoFlags(Fixture &f) { return f.Request(MSG_GET_INFO).U16At(6) & ~INFO_FLAG_HOSTED; }
}

Fact("Protocol Backlight Any Non-Zero Means On")
{
    Fixture f;
    Assert(f.Request(MSG_SET_BACKLIGHT, OnByte(0)).Status() == ST_OK && !f.backlight);
    Assert(f.Request(MSG_SET_BACKLIGHT, OnByte(1)).Status() == ST_OK && f.backlight);
    f.Request(MSG_SET_BACKLIGHT, OnByte(0));
    Assert(f.Request(MSG_SET_BACKLIGHT, OnByte(200)).Status() == ST_OK && f.backlight);
    f.Request(MSG_SET_BACKLIGHT, OnByte(0));
    Assert(f.Request(MSG_SET_BACKLIGHT, OnByte(2)).Status() == ST_OK && f.backlight);
}

Fact("Protocol Backlight Short Payload Is Bad Length And Changes Nothing")
{
    Fixture f;
    Assert(f.Request(MSG_SET_BACKLIGHT).Status() == ST_BAD_LENGTH && f.backlight);
    f.Request(MSG_SET_BACKLIGHT, OnByte(0));
    Assert(f.Request(MSG_SET_BACKLIGHT).Status() == ST_BAD_LENGTH && !f.backlight);
}

Fact("Protocol Get Info Flags Follow The Backlight")
{
    Fixture f;
    Assert(InfoFlags(f) == INFO_FLAG_BACKLIGHT);            // on at boot
    f.Request(MSG_SET_BACKLIGHT, OnByte(0));
    Assert(InfoFlags(f) == 0);
    f.Request(MSG_SET_BACKLIGHT, OnByte(1));
    Assert(InfoFlags(f) == INFO_FLAG_BACKLIGHT);
}

Fact("Protocol Reset And Reset Stream Leave The Backlight Alone")
{
    Fixture f;
    f.Request(MSG_SET_BACKLIGHT, OnByte(0));
    Assert(f.Request(MSG_RESET).Status() == ST_OK);
    Assert(!f.backlight && InfoFlags(f) == 0);
    f.proto.ResetStream();
    Assert(!f.backlight && InfoFlags(f) == 0);

    f.Request(MSG_SET_BACKLIGHT, OnByte(1));
    f.Request(MSG_RESET);
    f.proto.ResetStream();
    Assert(f.backlight && InfoFlags(f) == INFO_FLAG_BACKLIGHT);
}

Fact("Protocol Content Requests Work While The Backlight Is Off")
{
    Fixture f;
    f.Request(MSG_SET_BACKLIGHT, OnByte(0));
    Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);
    Bytes n;
    n.U8(3);
    Assert(f.Request(MSG_SET_PAGE_COUNT, n).Status() == ST_OK);
    WireKey defs[] = { Slot(2, 4, 1) };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    Bytes go;
    go.U8(2).U8(1);
    Assert(f.Request(MSG_SET_PAGE, go).Status() == ST_OK && f.page_set == 2 && f.animate);
    Bytes t;
    t.U8(STATUS_TEXT_LEFT).U8(2).Raw("hi", 2);
    Assert(f.Request(MSG_SET_STATUS_TEXT, t).Status() == ST_OK && !strcmp(f.status_text[0], "hi"));

    // Same content after the light comes on
    f.Request(MSG_SET_BACKLIGHT, OnByte(1));
    Assert(f.Shown(2, 4) == f.Pixels(1));
    Assert(f.layout.PageCount() == 3 && f.page_set == 2);
}

Fact("Protocol Wake Event Layout")
{
    Fixture f;
    f.proto.PostWake(987654);
    Assert(f.proto.SendEvents());
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 1);
    Assert(e[0].type == EVT_WAKE && e[0].type == 0x82 && e[0].flags == PROTO_FLAG_EVENT);
    Assert(e[0].len == 8);
    Assert(e[0].U8At(0) == 0 && e[0].U8At(1) == 0 && e[0].U8At(2) == 0 && e[0].U8At(3) == 0);
    Assert(e[0].U32At(4) == 987654);
}

Fact("Protocol Wake Event Reports Overflow")
{
    Fixture f;
    for (int i = 0; i < PROTO_EVENT_QUEUE + 5; i++)
        f.proto.PostWake(i);
    f.proto.SendEvents();
    List<Msg> e = f.Read();
    Assert(e.GetCount() == PROTO_EVENT_QUEUE);
    Assert(e[0].U32At(4) == 5 && e[0].U8At(0) == 1);        // lost flag, in the WAKE's own layout
    Assert(e[1].U8At(0) == 0);
}

Fact("Protocol Wake Comes After The Cancel Queued Before It")
{
    Fixture f;
    f.proto.PostKey(0, 3, KEY_ACTION_DOWN, 10);
    f.proto.PostKey(0, 3, KEY_ACTION_CANCEL, 20);
    f.proto.PostWake(30);
    f.proto.SendEvents();
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 3);
    Assert(e[1].type == EVT_KEY && e[1].U8At(3) == KEY_ACTION_CANCEL);
    Assert(e[2].type == EVT_WAKE && e[2].U32At(4) == 30);
    Assert(e[2].seq == (uint16_t)(e[1].seq + 1));
}

Fact("Protocol Set Status Text")
{
    Fixture f;
    auto text = [](int field, const char *s, int len = -1) {
        Bytes p;
        p.U8(field).U8(len < 0 ? (int)strlen(s) : len).Raw(s, strlen(s));
        return p;
    };

    Assert(f.Request(MSG_SET_STATUS_TEXT, text(STATUS_TEXT_LEFT, "AppleTV")).Status() == ST_OK);
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(STATUS_TEXT_RIGHT, "12:35 am")).Status() == ST_OK);
    Assert(!strcmp(f.status_text[0], "AppleTV") && !strcmp(f.status_text[1], "12:35 am"));

    // Empty clears; trailing bytes are ignored; split across reads
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(STATUS_TEXT_LEFT, "")).Status() == ST_OK);
    Assert(!strcmp(f.status_text[0], ""));
    Bytes extra = text(STATUS_TEXT_LEFT, "Hi");
    extra.U32(7);
    Assert(f.Request(MSG_SET_STATUS_TEXT, extra, 3).Status() == ST_OK && !strcmp(f.status_text[0], "Hi"));

    // Multi-byte UTF-8 is accepted; the longest text is STATUS_TEXT_MAX - 1 bytes
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(STATUS_TEXT_LEFT, "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB5")).Status() == ST_OK);
    char longest[STATUS_TEXT_MAX + 1];
    memset(longest, 'x', sizeof(longest));
    longest[STATUS_TEXT_MAX - 1] = 0;
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(STATUS_TEXT_RIGHT, longest)).Status() == ST_OK);
    Assert(strlen(f.status_text[1]) == STATUS_TEXT_MAX - 1);

    // Rejected, and nothing changes
    int sets = f.status_sets;
    longest[STATUS_TEXT_MAX - 1] = 'x';
    longest[STATUS_TEXT_MAX] = 0;
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(STATUS_TEXT_RIGHT, longest)).Status() == ST_BAD_ARG);   // too long
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(2, "x")).Status() == ST_BAD_ARG);                       // no such field
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(0, "ab\xC3")).Status() == ST_BAD_ARG);                  // cut sequence
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(0, "\xC3(")).Status() == ST_BAD_ARG);                   // bad continuation
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(0, "\xC0\x80")).Status() == ST_BAD_ARG);                // overlong
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(0, "\xED\xA0\x80")).Status() == ST_BAD_ARG);            // surrogate
    Assert(f.Request(MSG_SET_STATUS_TEXT, text(0, "\xF5\x80\x80\x80")).Status() == ST_BAD_ARG);        // beyond U+10FFFF
    Bytes nul;
    nul.U8(0).U8(3).U8('a').U8(0).U8('b');
    Assert(f.Request(MSG_SET_STATUS_TEXT, nul).Status() == ST_BAD_ARG);                                // embedded NUL
    Bytes shortText;
    shortText.U8(0).U8(5).Raw("abc", 3);
    Assert(f.Request(MSG_SET_STATUS_TEXT, shortText).Status() == ST_BAD_LENGTH);                       // length past the payload
    Bytes noLen;
    noLen.U8(0);
    Assert(f.Request(MSG_SET_STATUS_TEXT, noLen).Status() == ST_BAD_LENGTH);
    Assert(f.Request(MSG_SET_STATUS_TEXT).Status() == ST_BAD_LENGTH);
    Assert(f.status_sets == sets);
}

Fact("Protocol Reset Clears Status Text")
{
    Fixture f;
    Bytes p;
    p.U8(STATUS_TEXT_LEFT).U8(1).U8('a');
    f.Request(MSG_SET_STATUS_TEXT, p);
    Bytes q;
    q.U8(STATUS_TEXT_RIGHT).U8(1).U8('b');
    f.Request(MSG_SET_STATUS_TEXT, q);
    Assert(f.Request(MSG_RESET).Status() == ST_OK);
    Assert(!strcmp(f.status_text[0], "") && !strcmp(f.status_text[1], ""));
}

Fact("Protocol Unknown Type Is Skipped")
{
    Fixture f;
    Bytes p;
    p.U32(1).U32(2);
    Assert(f.Request(0x7E, p).Status() == ST_UNKNOWN_TYPE);
    Assert(f.Request(MSG_PING).Status() == ST_OK);
}

Fact("Protocol Bad Header Stops The Stream Until Reset")
{
    Fixture f;
    Bytes junk;
    junk.U32(0x12345678).U32(0).U32(0);
    f.Send(junk);
    Assert(f.proto.Corrupt());
    f.Send(Message(MSG_PING, 1));
    Assert(f.Read().GetCount() == 0);

    f.proto.ResetStream();
    Assert(!f.proto.Corrupt());
    Assert(f.Request(MSG_PING).Status() == ST_OK);

    // Too long
    Bytes big;
    big.U16(PROTO_MAGIC).U8(MSG_PING).U8(0).U16(1).U16(0).U32(PROTO_MAX_PAYLOAD + 1);
    f.Send(big);
    Assert(f.proto.Corrupt());
}

Fact("Protocol Reset Stream Drops A Partial Message And Upload")
{
    Fixture f;
    Bytes m = Message(MSG_PUT_IMAGE, 5, f.PutImage(3, f.env.Icon(0)));
    Bytes half;
    half.Raw(m.b.GetBuffer(), m.Count() / 2);
    f.Send(half);
    Assert(f.env.LiveBlocks() == 1);

    f.proto.ResetStream();
    Assert(f.env.LiveBlocks() == 0);
    Assert(f.Request(MSG_PING).Status() == ST_OK);
    Assert(!f.store.IsLive(3));
}

Fact("Protocol Events Are Queued And Report Overflow")
{
    Fixture f;
    f.proto.PostKey(1, 0xBEEF, KEY_ACTION_DOWN, 1234);
    f.proto.PostPageChanged(2, PAGE_CAUSE_USER);
    Assert(f.proto.SendEvents());
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 2);
    Assert(e[0].type == EVT_KEY && e[0].flags == PROTO_FLAG_EVENT);
    // u16 id, u8 page, u8 action, u8 lost, u8[3] reserved, u32 time_ms
    Assert(e[0].len == 12);
    Assert(e[0].U16At(0) == 0xBEEF && e[0].U8At(2) == 1 && e[0].U8At(3) == KEY_ACTION_DOWN);
    Assert(e[0].U8At(4) == 0 && e[0].U8At(5) == 0 && e[0].U8At(6) == 0 && e[0].U8At(7) == 0);
    Assert(e[0].U32At(8) == 1234);
    Assert(e[1].type == EVT_PAGE_CHANGED && e[1].U8At(0) == 2 && e[1].U8At(1) == PAGE_CAUSE_USER);
    Assert(e[1].seq == (uint16_t)(e[0].seq + 1));
    Assert(!f.proto.SendEvents());

    // Overflow: the oldest go, and the next KEY event says so
    for (int i = 0; i < PROTO_EVENT_QUEUE + 10; i++)
        f.proto.PostKey(0, (uint16_t)(1 + i), KEY_ACTION_UP, i);
    f.proto.SendEvents();
    e = f.Read();
    Assert(e.GetCount() == PROTO_EVENT_QUEUE);
    Assert(e[0].U32At(8) == 10 && e[0].U16At(0) == 11);
    Assert(e[0].U8At(4) == 1);
    Assert(e[1].U8At(4) == 0);

    // A stream reset drops queued events
    f.proto.PostKey(0, 0, KEY_ACTION_DOWN, 0);
    f.proto.ResetStream();
    Assert(!f.proto.SendEvents());
}

Fact("Protocol Events Wait While The Host Isn't Reading")
{
    Fixture f;
    f.space = 30;                       // room for one event (12 + 12 bytes), not two
    f.proto.PostKey(0, 1, KEY_ACTION_DOWN, 1);
    f.proto.PostKey(0, 1, KEY_ACTION_UP, 2);
    Assert(f.proto.SendEvents());
    Assert(f.Read().GetCount() == 1);
    f.space = 0;
    Assert(!f.proto.SendEvents());
    f.space = 1000;
    Assert(f.proto.SendEvents());
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 1 && e[0].U8At(3) == KEY_ACTION_UP);
}

// ---------------------------------------------------------------------------
// Heartbeat (docs/protocol.md): keeps the host's idle reads from timing out
// ---------------------------------------------------------------------------

Fact("Protocol Heartbeat Event Layout")
{
    Fixture f;
    Assert(f.proto.SendHeartbeat(5000));
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 1);
    Assert(e[0].type == EVT_HEARTBEAT && e[0].type == 0x83 && e[0].flags == PROTO_FLAG_EVENT);
    Assert(e[0].len == 4 && e[0].U32At(0) == 5000);
}

Fact("Protocol Heartbeat Is Sent Once Per Interval")
{
    Fixture f;
    Assert(f.proto.SendHeartbeat(5000));
    Assert(!f.proto.SendHeartbeat(5000));
    Assert(!f.proto.SendHeartbeat(5000 + PROTO_HEARTBEAT_MS - 1));
    Assert(f.proto.SendHeartbeat(5000 + PROTO_HEARTBEAT_MS));
    Assert(!f.proto.SendHeartbeat(5000 + PROTO_HEARTBEAT_MS + 1));
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 2);
    Assert(e[1].seq == (uint16_t)(e[0].seq + 1));           // it shares the event counter
    Assert(e[1].U32At(0) == 5000 + PROTO_HEARTBEAT_MS);
}

Fact("Protocol Heartbeat Interval Survives The Millisecond Clock Wrapping")
{
    Fixture f;
    Assert(f.proto.SendHeartbeat(0xFFFFFF00u));
    Assert(!f.proto.SendHeartbeat(0xFFFFFF00u + 10));
    Assert(f.proto.SendHeartbeat(0xFFFFFF00u + PROTO_HEARTBEAT_MS));       // wraps past zero
    Assert(f.Read().GetCount() == 2);
}

Fact("Protocol Heartbeat Waits For Room And Is Sent As Soon As There Is Some")
{
    // A host that isn't reading (no app running) must not have its transmit
    // FIFO filled with heartbeats: they stop while less than the room is free
    Fixture f;
    f.space = PROTO_HEARTBEAT_ROOM - 1;
    Assert(!f.proto.SendHeartbeat(5000));
    Assert(f.Read().GetCount() == 0);
    f.space = PROTO_HEARTBEAT_ROOM;
    Assert(f.proto.SendHeartbeat(5001));                    // the refused attempt didn't start the interval
    Assert(f.Read().GetCount() == 1);
}

Fact("Protocol Heartbeat Does Not Use The Event Queue")
{
    Fixture f;
    for (int i = 0; i < PROTO_EVENT_QUEUE; i++)
        f.proto.PostKey(0, 1, KEY_ACTION_DOWN, i);
    Assert(f.proto.SendHeartbeat(7000));
    f.proto.SendEvents();
    List<Msg> e = f.Read();
    Assert(e.GetCount() == PROTO_EVENT_QUEUE + 1);
    Assert(e[0].type == EVT_HEARTBEAT);
    Assert(e[1].U8At(4) == 0);                            // nothing lost: the queue was untouched
}

// ---------------------------------------------------------------------------
// Layout upload (docs/protocol.md "Layout upload")
// ---------------------------------------------------------------------------

namespace
{
    Bytes Blob(uint32_t size, uint32_t seed = 7)
    {
        Bytes b;
        uint32_t x = seed;
        for (uint32_t i = 0; i < size; i++) {
            x = x * 1664525u + 1013904223u;
            b.U8(x >> 24);
        }
        return b;
    }

    uint32_t CrcOf(const Bytes &b) { return crc32(0, b.b.GetBuffer(), (size_t)b.Count()); }

    Bytes LayoutBegin(uint32_t size, uint32_t crc)
    {
        Bytes p;
        p.U32(size).U32(crc);
        return p;
    }

    Bytes LayoutData(const Bytes &blob, uint32_t offset, uint32_t len)
    {
        Bytes p;
        p.U32(offset).Raw(blob.b.GetBuffer() + offset, len);
        return p;
    }

    // Begin, chunked data, commit: returns the commit's status
    uint16_t UploadLayout(Fixture &f, const Bytes &blob, uint32_t chunk, int piece = 0)
    {
        uint32_t size = (uint32_t)blob.Count();
        Check(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(size, CrcOf(blob))).Status() == ST_OK);
        for (uint32_t off = 0; off < size; off += chunk) {
            uint32_t n = size - off < chunk ? size - off : chunk;
            Check(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, off, n), piece).Status() == ST_OK);
        }
        return f.Request(MSG_LAYOUT_COMMIT).Status();
    }
}

Fact("Protocol Layout Upload Saves Blanks And Restarts")
{
    Fixture f;
    Bytes blob = Blob(20000);
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(20000, CrcOf(blob))).Status() == ST_OK);
    Assert(f.flash.busy);                           // the screen is dark for the upload
    for (uint32_t off = 0; off < 20000; off += 8188)
        Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, off, 20000 - off < 8188 ? 20000 - off : 8188)).Status() == ST_OK);
    Assert(f.restarts == 0);
    Assert(f.Request(MSG_LAYOUT_COMMIT).Status() == ST_OK);     // the response was sent...
    Assert(f.restarts == 1);                                    // ...then the device restarted

    LayoutInfo info;
    Assert(f.layout_store.Stored(&info) && info.size == 20000 && info.crc == CrcOf(blob));
    Assert(memcmp(f.flash.mem + LAYOUT_DATA_OFFSET, blob.b.GetBuffer(), 20000) == 0);
}

// A host can skip an upload of the layout the device already has
Fact("Protocol Get Info Reports The Stored Layout")
{
    Fixture f;
    Bytes blob = Blob(5000);
    uint8_t scratch[512];
    Assert(f.layout_store.Begin(5000, CrcOf(blob)) == LayoutStoreResult::Ok);
    Assert(f.layout_store.Data(0, blob.b.GetBuffer(), 5000) == LayoutStoreResult::Ok);
    const int V2_FIELDS = 17;                            // after the layout fields (Get Info): v2's 15, the blob version's 2
    Msg during = f.Request(MSG_GET_INFO);                   // an upload under way stores nothing yet
    int at = during.len - V2_FIELDS - 12;
    Assert(during.U32At(at + 4) == 0 && during.U32At(at + 8) == 0);
    Assert(f.layout_store.Commit(scratch, sizeof(scratch)) == LayoutStoreResult::Ok);

    Msg m = f.Request(MSG_GET_INFO);
    int tail = m.len - V2_FIELDS - 12;
    Assert(m.U32At(tail) == 15 * LAYOUT_SECTOR && m.U32At(tail + 4) == 5000 && m.U32At(tail + 8) == CrcOf(blob));
}

Fact("Protocol Layout Upload Works Arriving In Any Pieces")
{
    static const int pieces[] = { 1, 5, 64 };
    for (int piece : pieces) {
        Fixture f;
        Bytes blob = Blob(3000, piece);
        Assert(UploadLayout(f, blob, 1000, piece) == ST_OK);
        Assert(f.restarts == 1);
        LayoutInfo info;
        Assert(f.layout_store.Stored(&info) && info.crc == CrcOf(blob));
    }
}

Fact("Protocol Layout Upload Pipelined")
{
    Fixture f;
    Bytes blob = Blob(3000);
    Bytes all;
    all.Add(Message(MSG_LAYOUT_BEGIN, 1, LayoutBegin(3000, CrcOf(blob))));
    for (uint32_t off = 0; off < 3000; off += 1000)
        all.Add(Message(MSG_LAYOUT_DATA, (uint16_t)(2 + off / 1000), LayoutData(blob, off, 1000)));
    all.Add(Message(MSG_LAYOUT_COMMIT, 9));
    f.Send(all);
    List<Msg> r = f.Read();
    Assert(r.GetCount() == 5);
    for (int i = 0; i < 5; i++)
        Assert(r[i].Status() == ST_OK);
    Assert(f.restarts == 1);
}

Fact("Protocol Layout Messages After The Commit Are Ignored")
{
    Fixture f;
    Bytes blob = Blob(500);
    Bytes all;
    all.Add(Message(MSG_LAYOUT_BEGIN, 1, LayoutBegin(500, CrcOf(blob))));
    all.Add(Message(MSG_LAYOUT_DATA, 2, LayoutData(blob, 0, 500)));
    all.Add(Message(MSG_LAYOUT_COMMIT, 3));
    all.Add(Message(MSG_PING, 4));
    f.Send(all);
    List<Msg> r = f.Read();
    Assert(r.GetCount() == 3 && r[2].type == MSG_LAYOUT_COMMIT);
    Assert(f.restarts == 1);
}

Fact("Protocol Layout Begin Errors")
{
    Fixture f;
    Bytes short_begin;
    short_begin.U32(100).U16(1);
    Assert(f.Request(MSG_LAYOUT_BEGIN, short_begin).Status() == ST_BAD_LENGTH);
    Assert(f.Request(MSG_LAYOUT_BEGIN).Status() == ST_BAD_LENGTH);
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(0, 0)).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(15 * LAYOUT_SECTOR + 1, 0)).Status() == ST_NO_SPACE);
    Assert(!f.flash.busy && f.flash.erase_calls == 0);

    // The biggest that fits
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(15 * LAYOUT_SECTOR, 0)).Status() == ST_OK);

    f.flash.fail_erase_in = 1;
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(100, 0)).Status() == ST_IO_ERROR);
    Assert(!f.flash.busy);
}

Fact("Protocol Layout Data Errors")
{
    Fixture f;
    Bytes blob = Blob(3000);

    // No begin
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 0, 100)).Status() == ST_BAD_SEQUENCE);

    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(3000, CrcOf(blob))).Status() == ST_OK);
    Bytes no_offset;
    no_offset.U16(0);
    Assert(f.Request(MSG_LAYOUT_DATA, no_offset).Status() == ST_BAD_LENGTH);
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 0, 0)).Status() == ST_BAD_LENGTH);      // no data
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 0, 1000)).Status() == ST_OK);            // the upload survived

    // A gap ends it; the stream stays in step
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 2000, 1000)).Status() == ST_BAD_SEQUENCE);
    Assert(!f.flash.busy);
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 1000, 1000)).Status() == ST_BAD_SEQUENCE);
    Assert(f.Request(MSG_LAYOUT_COMMIT).Status() == ST_BAD_SEQUENCE);
    Assert(f.Request(MSG_PING).Status() == ST_OK);
    Assert(f.restarts == 0);

    // Past the end
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(100, 0)).Status() == ST_OK);
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 0, 101)).Status() == ST_BAD_ARG);

    // A flash error
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(3000, CrcOf(blob))).Status() == ST_OK);
    f.flash.fail_write_in = 1;
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 0, 1000)).Status() == ST_IO_ERROR);
    Assert(!f.flash.busy);
}

Fact("Protocol Layout Commit Errors")
{
    Fixture f;
    Bytes blob = Blob(3000);
    Assert(f.Request(MSG_LAYOUT_COMMIT).Status() == ST_BAD_SEQUENCE);

    // Incomplete: the upload carries on
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(3000, CrcOf(blob))).Status() == ST_OK);
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 0, 1000)).Status() == ST_OK);
    Assert(f.Request(MSG_LAYOUT_COMMIT).Status() == ST_BAD_SEQUENCE);
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 1000, 2000)).Status() == ST_OK);
    Assert(f.restarts == 0);

    // Wrong CRC
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(3000, CrcOf(blob) + 1)).Status() == ST_OK);
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 0, 3000)).Status() == ST_OK);
    Assert(f.Request(MSG_LAYOUT_COMMIT).Status() == ST_BAD_CRC);
    LayoutInfo info;
    Assert(!f.layout_store.Stored(&info) && f.restarts == 0 && !f.flash.busy);

    // A write that went wrong, found by the read-back
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(3000, CrcOf(blob))).Status() == ST_OK);
    f.flash.corrupt_next_write = true;
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 0, 3000)).Status() == ST_OK);
    Assert(f.Request(MSG_LAYOUT_COMMIT).Status() == ST_IO_ERROR);
    Assert(!f.layout_store.Stored(&info) && f.restarts == 0);

    // And a good one still works afterwards
    Assert(UploadLayout(f, blob, 1500) == ST_OK);
    Assert(f.restarts == 1);
}

Fact("Protocol Reset Stream Abandons A Layout Upload But Reset Does Not")
{
    Fixture f;
    Bytes blob = Blob(3000);
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(3000, CrcOf(blob))).Status() == ST_OK);
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 0, 1000)).Status() == ST_OK);
    Assert(f.Request(MSG_RESET).Status() == ST_OK);
    Assert(f.layout_store.Active() && f.flash.busy);
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 1000, 1000)).Status() == ST_OK);

    f.proto.ResetStream();
    Assert(!f.layout_store.Active() && !f.flash.busy);
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 2000, 1000)).Status() == ST_BAD_SEQUENCE);
    LayoutInfo info;
    Assert(!f.layout_store.Stored(&info));
}

Fact("Protocol Layout Upload Is Independent Of Image Uploads")
{
    Fixture f;
    Bytes blob = Blob(2000);
    const void *icon = f.env.Key1U(0);
    uint32_t size = f.env.Size(icon), half = size / 2;
    Assert(f.Request(MSG_PUT_IMAGE, f.PutImage(2, icon, 0, half)).Status() == ST_OK);
    Assert(f.Request(MSG_LAYOUT_BEGIN, LayoutBegin(2000, CrcOf(blob))).Status() == ST_OK);
    Assert(f.Request(MSG_PUT_IMAGE, f.PutImage(2, icon, half)).Status() == ST_OK);
    Assert(f.Request(MSG_LAYOUT_DATA, LayoutData(blob, 0, 2000)).Status() == ST_OK);
    Assert(f.Request(MSG_LAYOUT_COMMIT).Status() == ST_OK);
    Assert(f.store.IsLive(2));
}

// ===========================================================================
// Wi-Fi (docs/wifi.md)
// ===========================================================================

namespace
{
    Bytes WifiSet(const char *ssid, const char *pass)
    {
        Bytes p;
        p.U8((uint32_t)strlen(ssid)).U8((uint32_t)strlen(pass)).Raw(ssid, strlen(ssid)).Raw(pass, strlen(pass));
        return p;
    }

    // Both lengths as given, whatever follows
    Bytes WifiSetRaw(uint32_t ssid_len, uint32_t pass_len, const void *data = "", size_t n = 0)
    {
        Bytes p;
        p.U8(ssid_len).U8(pass_len).Raw(data, n);
        return p;
    }

    uint32_t WifiFlag(Fixture &f) { return InfoFlags(f) & INFO_FLAG_WIFI_CREDENTIALS; }
}

Fact("Protocol Wifi Message Types")
{
    Assert(MSG_WIFI_SET == 0x40 && MSG_WIFI_GET == 0x41 && MSG_WIFI_STATUS == 0x42 && EVT_WIFI_STATE == 0x84);
    Assert(INFO_FLAG_WIFI_CREDENTIALS == 0x0002);
}

Fact("Protocol Wifi Set Stores And Get Reads Back")
{
    Fixture f;
    Msg g = f.Request(MSG_WIFI_GET);
    Assert(g.Status() == ST_OK && g.len == 4 + 2 && g.U8At(4) == 0 && g.U8At(5) == 0);      // none stored

    Assert(f.Request(MSG_WIFI_SET, WifiSet("Home", "password1")).Status() == ST_OK);
    Assert(f.wifi_storage.saves == 1 && f.wifi_changes == 1);
    g = f.Request(MSG_WIFI_GET);
    Assert(g.Status() == ST_OK && g.len == 4 + 2 + 4 + 9);
    Assert(g.U8At(4) == 4 && g.U8At(5) == 9);
    Assert(!memcmp(g.data + 6, "Home", 4) && !memcmp(g.data + 10, "password1", 9));

    // Open network
    Assert(f.Request(MSG_WIFI_SET, WifiSet("Cafe", "")).Status() == ST_OK);
    g = f.Request(MSG_WIFI_GET);
    Assert(g.len == 4 + 2 + 4 && g.U8At(4) == 4 && g.U8At(5) == 0 && !memcmp(g.data + 6, "Cafe", 4));
}

Fact("Protocol Wifi Set Same Credentials Changes Nothing")
{
    Fixture f;
    f.Request(MSG_WIFI_SET, WifiSet("Home", "password1"));
    Assert(f.Request(MSG_WIFI_SET, WifiSet("Home", "password1")).Status() == ST_OK);
    Assert(f.wifi_storage.saves == 1 && f.wifi_changes == 1);
}

Fact("Protocol Wifi Empty Ssid Clears")
{
    Fixture f;
    f.Request(MSG_WIFI_SET, WifiSet("Home", "password1"));
    Assert(f.Request(MSG_WIFI_SET, WifiSetRaw(0, 0)).Status() == ST_OK);
    Assert(f.wifi_storage.erases == 1 && f.wifi_changes == 2);
    Msg g = f.Request(MSG_WIFI_GET);
    Assert(g.len == 6 && g.U8At(4) == 0 && g.U8At(5) == 0);
    Assert(WifiFlag(f) == 0);
}

Fact("Protocol Wifi Set Argument Errors Change Nothing")
{
    Fixture f;
    f.Request(MSG_WIFI_SET, WifiSet("Home", "password1"));

    char ssid[40], pass[70];
    memset(ssid, 'a', sizeof(ssid));
    memset(pass, 'b', sizeof(pass));
    Assert(f.Request(MSG_WIFI_SET, WifiSetRaw(33, 0, ssid, 33)).Status() == ST_BAD_ARG);            // ssid over 32
    Assert(f.Request(MSG_WIFI_SET, WifiSetRaw(1, 1, "ab", 2)).Status() == ST_BAD_ARG);               // password 1..7
    Assert(f.Request(MSG_WIFI_SET, WifiSetRaw(1, 7, "abcdefgh", 8)).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_WIFI_SET, WifiSetRaw(1, 64, pass, 65)).Status() == ST_BAD_ARG);             // over 63
    Assert(f.Request(MSG_WIFI_SET, WifiSetRaw(0, 8, pass, 8)).Status() == ST_BAD_ARG);               // password, no ssid
    Assert(f.Request(MSG_WIFI_SET, WifiSetRaw(3, 0, "a\0b", 3)).Status() == ST_BAD_ARG);             // NUL in ssid
    Assert(f.Request(MSG_WIFI_SET, WifiSetRaw(3, 9, "abcpass\0word", 12)).Status() == ST_BAD_ARG);   // NUL in password

    Msg g = f.Request(MSG_WIFI_GET);
    Assert(g.U8At(4) == 4 && !memcmp(g.data + 6, "Home", 4));
    Assert(f.wifi_storage.saves == 1 && f.wifi_storage.erases == 0 && f.wifi_changes == 1);
}

Fact("Protocol Wifi Set Length Errors")
{
    Fixture f;
    Assert(f.Request(MSG_WIFI_SET).Status() == ST_BAD_LENGTH);                                       // no lengths
    Bytes one;
    one.U8(4);
    Assert(f.Request(MSG_WIFI_SET, one).Status() == ST_BAD_LENGTH);                                  // one length
    Assert(f.Request(MSG_WIFI_SET, WifiSetRaw(4, 9, "Homepass", 8)).Status() == ST_BAD_LENGTH);      // shorter than they say
    Assert(f.Request(MSG_WIFI_SET, WifiSetRaw(4, 0, "Hom", 3)).Status() == ST_BAD_LENGTH);
    Assert(!f.wifi.Stored() && f.wifi_storage.saves == 0);
}

Fact("Protocol Wifi Set Ignores Trailing Bytes And Works In Pieces")
{
    Fixture f;
    Bytes p = WifiSet("Home", "password1");
    p.U32(0xDEADBEEF);
    Assert(f.Request(MSG_WIFI_SET, p, 3).Status() == ST_OK);
    Assert(f.wifi.Stored() && f.wifi.Get().ssid_len == 4 && f.wifi.Get().pass_len == 9);
}

Fact("Protocol Wifi Set Reports A Storage Failure")
{
    Fixture f;
    f.Request(MSG_WIFI_SET, WifiSet("Home", "password1"));
    f.wifi_storage.fail_save = true;
    Assert(f.Request(MSG_WIFI_SET, WifiSet("Other", "password2")).Status() == ST_IO_ERROR);
    Msg g = f.Request(MSG_WIFI_GET);
    Assert(g.U8At(4) == 4 && !memcmp(g.data + 6, "Home", 4));                                        // old ones stay
    Assert(f.wifi_changes == 1);
}

Fact("Protocol Wifi Get Info Flag Follows The Credentials")
{
    Fixture f;
    Assert(WifiFlag(f) == 0);
    f.Request(MSG_WIFI_SET, WifiSet("Home", "password1"));
    Assert(WifiFlag(f) == INFO_FLAG_WIFI_CREDENTIALS);
    Assert((InfoFlags(f) & INFO_FLAG_BACKLIGHT) != 0);                                               // independent of bit 0
    f.Request(MSG_SET_BACKLIGHT, OnByte(0));
    Assert(InfoFlags(f) == INFO_FLAG_WIFI_CREDENTIALS);
    f.Request(MSG_WIFI_SET, WifiSetRaw(0, 0));
    Assert(InfoFlags(f) == 0);
}

Fact("Protocol Wifi Get Info Flag Is Set For Credentials Stored Before Boot")
{
    FakeBlobStorage storage;
    WifiCredentials c;
    Assert(wifi_credentials_make(&c, (const uint8_t *)"Home", 4, (const uint8_t *)"password1", 9));
    uint8_t blob[WIFI_BLOB_MAX];
    storage.Preload(blob, wifi_blob_encode(c, blob));
    Fixture f;
    f.wifi.Init(storage.Host());
    Assert(WifiFlag(f) == INFO_FLAG_WIFI_CREDENTIALS);
}

Fact("Protocol Reset And Reset Stream Leave The Wifi Credentials Alone")
{
    Fixture f;
    f.Request(MSG_WIFI_SET, WifiSet("Home", "password1"));
    Assert(f.Request(MSG_RESET).Status() == ST_OK);
    f.proto.ResetStream();
    Assert(f.wifi.Stored() && WifiFlag(f) == INFO_FLAG_WIFI_CREDENTIALS);
    Assert(f.wifi_changes == 1 && f.wifi_storage.erases == 0);
    Msg g = f.Request(MSG_WIFI_GET);
    Assert(g.U8At(4) == 4 && !memcmp(g.data + 6, "Home", 4));
}

Fact("Protocol Wifi Status Layout")
{
    Fixture f;
    f.wifi_st.state  = (uint8_t)WifiState::Connected;
    f.wifi_st.reason = 201;
    f.wifi_st.rssi   = -57;
    f.wifi_st.ip[0] = 192; f.wifi_st.ip[1] = 168; f.wifi_st.ip[2] = 1; f.wifi_st.ip[3] = 50;
    for (int i = 0; i < 6; i++)
        f.wifi_st.mac[i] = (uint8_t)(0xA0 + i);

    Msg m = f.Request(MSG_WIFI_STATUS);
    Assert(m.Status() == ST_OK && m.len == 4 + 14);
    Assert(m.U8At(4) == 2);                                     // CONNECTED
    Assert(m.U8At(5) == 201);                                   // reason
    Assert((int8_t)m.U8At(6) == -57);                           // rssi
    Assert(m.U8At(7) == 0);                                     // reserved
    Assert(m.U8At(8) == 192 && m.U8At(9) == 168 && m.U8At(10) == 1 && m.U8At(11) == 50);
    for (int i = 0; i < 6; i++)
        Assert(m.U8At(12 + i) == (uint32_t)(0xA0 + i));
}

Fact("Protocol Wifi Status Without Credentials")
{
    Fixture f;
    Msg m = f.Request(MSG_WIFI_STATUS);
    Assert(m.Status() == ST_OK && m.len == 4 + 14);
    for (int i = 4; i < m.len; i++)
        Assert(m.U8At(i) == 0);                                 // NO_CREDENTIALS, everything else zero
}

Fact("Protocol Wifi Status And Get Ignore A Payload")
{
    Fixture f;
    Bytes p;
    p.U32(1);
    Assert(f.Request(MSG_WIFI_STATUS, p).Status() == ST_OK);
    Assert(f.Request(MSG_WIFI_GET, p).Status() == ST_OK);
}

Fact("Protocol Wifi State Event Layout")
{
    Fixture f;
    const uint8_t ip[4] = { 10, 0, 0, 7 };
    f.proto.PostWifiState(WifiState::Connected, ip);
    Assert(f.proto.SendEvents());
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 1);
    Assert(e[0].type == EVT_WIFI_STATE && e[0].type == 0x84 && e[0].flags == PROTO_FLAG_EVENT);
    Assert(e[0].len == 8);
    Assert(e[0].U8At(0) == 2 && e[0].U8At(1) == 0 && e[0].U8At(2) == 0 && e[0].U8At(3) == 0);
    Assert(e[0].U8At(4) == 10 && e[0].U8At(5) == 0 && e[0].U8At(6) == 0 && e[0].U8At(7) == 7);

    const uint8_t none[4] = {};
    f.proto.PostWifiState(WifiState::Retrying, none);
    f.proto.SendEvents();
    e = f.Read();
    Assert(e[0].U8At(0) == 3 && e[0].U8At(4) == 0);
}

Fact("Protocol Wifi State Event Reports Overflow")
{
    Fixture f;
    const uint8_t ip[4] = {};
    for (int i = 0; i < PROTO_EVENT_QUEUE + 3; i++)
        f.proto.PostWifiState(i & 1 ? WifiState::Connected : WifiState::Retrying, ip);
    f.proto.SendEvents();
    List<Msg> e = f.Read();
    Assert(e.GetCount() == PROTO_EVENT_QUEUE);
    Assert(e[0].U8At(1) == 1);                                  // lost flag, in this event's own layout
    Assert(e[1].U8At(1) == 0);
}

Fact("Protocol Wifi State Event Queues In Order With The Others")
{
    Fixture f;
    const uint8_t ip[4] = { 1, 2, 3, 4 };
    f.proto.PostKey(0, 3, KEY_ACTION_DOWN, 10);
    f.proto.PostWifiState(WifiState::Connecting, ip);
    f.proto.PostPageChanged(1, PAGE_CAUSE_USER);
    f.proto.SendEvents();
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 3);
    Assert(e[0].type == EVT_KEY && e[1].type == EVT_WIFI_STATE && e[2].type == EVT_PAGE_CHANGED);
    Assert(e[1].seq == (uint16_t)(e[0].seq + 1));
}

// --- Take control and release (docs/standalone.md, docs/protocol.md) ---

namespace
{
    uint32_t InfoFlagsRaw(Fixture &f) { return f.Request(MSG_GET_INFO).U16At(6); }

    // Every request that defines the layout, sent with a valid payload (i < DEFINING_COUNT)
    const int DEFINING_COUNT = 7;
    uint16_t SendDefining(Fixture &f, int i, int piece)
    {
        Bytes count, go, text;
        count.U8(2);
        go.U8(0);
        text.U8(STATUS_TEXT_LEFT).U8(2).Raw("hi", 2);
        uint32_t ids[] = { 1 };
        WireKey defs[] = { Slot(0, 0, 1) };
        switch (i) {
        case 0:  return f.Request(MSG_RESET, Bytes(), piece).Status();
        case 1:  return f.Request(MSG_SET_STATUS_TEXT, text, piece).Status();
        case 2:  return f.Request(MSG_PUT_IMAGE, f.PutImage(1, f.env.Key1U(0)), piece).Status();
        case 3:  return f.Request(MSG_FREE_IMAGES, Ids(1, ids), piece).Status();
        case 4:  return f.Request(MSG_SET_PAGE_COUNT, count, piece).Status();
        case 5:  return f.Request(MSG_SET_KEYS, Keys(1, defs), piece).Status();
        default: return f.Request(MSG_SET_PAGE, go, piece).Status();
        }
    }

    // Home's distinct images in the sample layout
    uint32_t HomeImageCount(const LayoutSummary &home)
    {
        Set<uint32_t> s;
        for (uint32_t i = 0; i < home.home_count; i++)
            for (const LayoutImage &im : { home.home[i].normal, home.home[i].pressed })
                if (im.offset)
                    s.Add(im.offset);
        return (uint32_t)s.GetCount();
    }
}

Fact("Protocol Take Control Message Types And Status")
{
    Assert(MSG_TAKE_CONTROL == 0x06 && MSG_RELEASE_CONTROL == 0x07);
    Assert(ST_NOT_HOSTED == 11);
    Assert(INFO_FLAG_HOSTED == 0x0004);
}

Fact("Protocol Boots Standalone")
{
    Fixture f(1u << 20, false);
    Assert(!f.proto.Hosted());
    Assert(InfoFlagsRaw(f) == INFO_FLAG_BACKLIGHT);
}

Fact("Protocol Standalone Refuses Layout Requests And Changes Nothing")
{
    Fixture f(1u << 20, false);
    f.UseHome();
    for (int i = 0; i < DEFINING_COUNT; i++) {
        // Whole, and in pieces (a refused PUT_IMAGE is skipped, keeping the stream in step)
        Assert(SendDefining(f, i, 0) == ST_NOT_HOSTED);
        Assert(SendDefining(f, i, 7) == ST_NOT_HOSTED);
        Assert(f.Request(MSG_PING).Status() == ST_OK);
    }
    // PUT_IMAGE shorter than its prefix, and with no payload at all, too
    Bytes shorter;
    shorter.U32(3).U32(64);
    Assert(f.Request(MSG_PUT_IMAGE, shorter).Status() == ST_NOT_HOSTED);
    Assert(f.Request(MSG_PUT_IMAGE).Status() == ST_NOT_HOSTED);
    Assert(f.Request(MSG_SET_KEYS).Status() == ST_NOT_HOSTED);

    Assert(f.ShowsHome(0));
    Assert(f.changed_pages.GetCount() == 0 && f.comp_resets == 0 && f.status_sets == 0);
    Assert(f.page_count == -1 && f.page_set == -1);
    Assert(!f.store.IsLive(1));
    Assert(!f.proto.Corrupt());
}

Fact("Protocol Standalone Still Answers Ping Info Query Backlight Layout And Wifi")
{
    Fixture f(1u << 20, false);
    Assert(f.Request(MSG_PING).Status() == ST_OK);
    Assert(f.Request(MSG_GET_INFO).Status() == ST_OK);
    uint32_t ids[] = { 1, 2 };
    Assert(f.Request(MSG_QUERY_IMAGES, Ids(2, ids)).Status() == ST_OK);
    Assert(f.Request(MSG_SET_BACKLIGHT, OnByte(0)).Status() == ST_OK && !f.backlight);
    Assert(f.Request(MSG_WIFI_GET).Status() == ST_OK);
    Assert(f.Request(MSG_WIFI_STATUS).Status() == ST_OK);
    Bytes cfg;
    cfg.U32(64).U32(0);
    Assert(f.Request(MSG_LAYOUT_BEGIN, cfg).Status() == ST_OK);       // reaches LayoutStore
    Assert(f.Request(MSG_LAYOUT_DATA).Status() == ST_BAD_LENGTH);    // not NOT_HOSTED
}

Fact("Protocol Take Control Resets The Model Like Reset")
{
    Fixture f(1u << 20, false);
    f.UseHome();
    Assert(f.ShowsHome(0));

    Msg m = f.Request(MSG_TAKE_CONTROL);
    Assert(m.Status() == ST_OK && m.len == 4);
    Assert(f.proto.Hosted());
    Assert(InfoFlagsRaw(f) == (INFO_FLAG_BACKLIGHT | INFO_FLAG_HOSTED));
    Assert(f.comp_resets == 1 && f.page_count == 1);
    Assert(f.status_sets == 2 && f.status_text[0][0] == 0 && f.status_text[1][0] == 0);
    Assert(f.store.Find(f.home.home[0].normal.offset) == nullptr);   // Home's images are gone
    Assert(f.layout.KeyCount(0) == 0);
    Assert(f.mode_changes.GetCount() == 1 && f.mode_changes[0] == 1);

    // Now the layout requests work
    Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);
    WireKey defs[] = { Slot(0, 3, 1) };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    Assert(f.Active(0, 3));
}

Fact("Protocol Take Control Abandons An Image Upload In Progress")
{
    Fixture f(1u << 20, false);
    f.Request(MSG_TAKE_CONTROL);
    Bytes part;
    part.U32(9).U32(5000).U32(0).U32(0);
    Assert(f.Request(MSG_PUT_IMAGE, part).Status() == ST_OK);
    Assert(f.Request(MSG_TAKE_CONTROL).Status() == ST_OK);      // again: hosted already
    Assert(f.env.LiveBlocks() == 0);
}

Fact("Protocol Take Control While Hosted Resets Again And Stays Hosted")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);
    Assert(f.Request(MSG_TAKE_CONTROL).Status() == ST_OK);
    Assert(f.proto.Hosted() && !f.store.IsLive(1));
    Assert(f.comp_resets == 1);
}

Fact("Protocol Release Rebuilds Home And Goes Standalone")
{
    Fixture f(1u << 20, false);
    f.UseHome();
    f.Request(MSG_TAKE_CONTROL);
    f.Upload(1, f.env.Key1U(0));
    Bytes n;
    n.U8(3);
    f.Request(MSG_SET_PAGE_COUNT, n);
    Bytes t;
    t.U8(STATUS_TEXT_LEFT).U8(2).Raw("hi", 2);
    f.Request(MSG_SET_STATUS_TEXT, t);
    f.Request(MSG_SET_BACKLIGHT, OnByte(0));
    f.comp_resets = 0;
    f.mode_changes.Clear();

    Msg m = f.Request(MSG_RELEASE_CONTROL);
    Assert(m.Status() == ST_OK && m.len == 4);
    Assert(!f.proto.Hosted());
    Assert(InfoFlagsRaw(f) == INFO_FLAG_BACKLIGHT);                 // light back on
    Assert(f.backlight);
    Assert(f.comp_resets == 1);
    Assert(f.page_count == 1 && f.layout.PageCount() == 1);
    Assert(f.page_set == 0 && !f.animate);
    Assert(f.status_text[0][0] == 0 && !strcmp(f.status_text[1], STATUS_TEXT_STANDALONE_RIGHT));
    Assert(!f.store.IsLive(1));
    Assert(f.ShowsHome(0) && f.Active(0, 0) && !f.Active(0, 3));
    Assert(f.store.Find(f.home.home[0].normal.offset) != nullptr);
    Assert(f.mode_changes.GetCount() == 1 && f.mode_changes[0] == 0);

    // Standalone again: layout requests refused
    Assert(f.Request(MSG_SET_PAGE_COUNT, n).Status() == ST_NOT_HOSTED);
    Assert(f.layout.PageCount() == 1);
}

Fact("Protocol Release Without A Layout Leaves An Empty Home")
{
    Fixture f;
    f.Upload(1, f.env.Key1U(0));
    WireKey defs[] = { Slot(0, 3, 1) };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    Assert(f.Request(MSG_RELEASE_CONTROL).Status() == ST_OK);
    Assert(!f.proto.Hosted() && f.layout.PageCount() == 1);
    Assert(f.layout.KeyCount(0) == 0);
    Assert(f.env.LiveBlocks() == 0);
}

Fact("Protocol Release When Home Can't Be Shown Leaves It Empty But Standalone")
{
    Fixture f(1u << 20, false);
    f.UseHome();
    f.Request(MSG_TAKE_CONTROL);
    f.home.home[7].pressed = LayoutImage{ 700, 64 };                // refused by the store
    Assert(f.Request(MSG_RELEASE_CONTROL).Status() == ST_OK);
    Assert(!f.proto.Hosted());
    Assert(f.layout.KeyCount(0) == 0);
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 0);
}

Fact("Protocol Release While Standalone Changes Nothing")
{
    Fixture f(1u << 20, false);
    f.UseHome();
    Assert(f.Request(MSG_RELEASE_CONTROL).Status() == ST_OK);
    Assert(f.comp_resets == 0 && f.mode_changes.GetCount() == 0 && f.ShowsHome(0));
}

Fact("Protocol Release And Take Control Are Repeatable")
{
    Fixture f(1u << 20, false);
    f.UseHome();
    for (int i = 0; i < 3; i++) {
        Assert(f.Request(MSG_TAKE_CONTROL).Status() == ST_OK && f.proto.Hosted());
        Assert(f.Upload(1, f.env.Key1U(0)) == ST_OK);
        Assert(f.Request(MSG_RELEASE_CONTROL).Status() == ST_OK && !f.proto.Hosted());
        Assert(f.ShowsHome(0) && !f.store.IsLive(1));
    }
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == HomeImageCount(f.home));
}

Fact("Protocol Disconnect Releases Like The Message")
{
    Fixture f(1u << 20, false);
    f.UseHome();
    f.Request(MSG_TAKE_CONTROL);
    f.Upload(1, f.env.Key1U(0));
    f.Request(MSG_SET_BACKLIGHT, OnByte(0));
    f.mode_changes.Clear();

    // Posting alone (any task) changes nothing; the protocol task services it
    f.proto.PostDisconnect();
    Assert(f.proto.Hosted());
    f.proto.ServiceDisconnect();
    Assert(!f.proto.Hosted() && f.backlight);
    Assert(f.ShowsHome(0) && !f.store.IsLive(1));
    Assert(f.mode_changes.GetCount() == 1 && f.mode_changes[0] == 0);
    Assert(f.status_text[0][0] == 0 && !strcmp(f.status_text[1], STATUS_TEXT_STANDALONE_RIGHT));

    // Once per post
    f.mode_changes.Clear();
    f.proto.ServiceDisconnect();
    Assert(f.mode_changes.GetCount() == 0);
}

Fact("Protocol Disconnect Drops A Partial Message And A Layout Upload")
{
    Fixture f;
    Bytes begin;
    begin.U32(64).U32(0);
    Assert(f.Request(MSG_LAYOUT_BEGIN, begin).Status() == ST_OK);
    Bytes ping;
    ping.U32(1);
    Bytes half = Message(MSG_PING, 1, ping);
    f.proto.Receive(half.b.GetBuffer(), 14);                         // mid-message

    f.proto.PostDisconnect();
    f.proto.ServiceDisconnect();
    Assert(f.Request(MSG_PING).Status() == ST_OK);                   // the parser is at a header again
    Bytes data;
    data.U32(0).U8(1);
    Assert(f.Request(MSG_LAYOUT_DATA, data).Status() == ST_BAD_SEQUENCE);
}

Fact("Protocol Disconnect While Standalone Only Restores The Backlight")
{
    Fixture f(1u << 20, false);
    f.UseHome();
    f.Request(MSG_SET_BACKLIGHT, OnByte(0));
    f.proto.PostDisconnect();
    f.proto.ServiceDisconnect();
    Assert(f.backlight);
    Assert(f.comp_resets == 0 && f.mode_changes.GetCount() == 0 && f.ShowsHome(0));
}

Fact("Protocol Key And Page Events Only Exist While Hosted")
{
    Fixture f(1u << 20, false);
    f.proto.PostKey(0, 1, KEY_ACTION_DOWN, 5);
    f.proto.PostPageChanged(1, PAGE_CAUSE_USER);
    const uint8_t ip[4] = {};
    f.proto.PostWake(7);
    f.proto.PostWifiState(WifiState::Connecting, ip);
    f.proto.SendEvents();
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 2 && e[0].type == EVT_WAKE && e[1].type == EVT_WIFI_STATE);

    f.Request(MSG_TAKE_CONTROL);
    f.proto.PostKey(0, 1, KEY_ACTION_DOWN, 5);
    f.proto.PostPageChanged(1, PAGE_CAUSE_USER);
    f.proto.SendEvents();
    e = f.Read();
    Assert(e.GetCount() == 2 && e[0].type == EVT_KEY && e[1].type == EVT_PAGE_CHANGED);
}

Fact("Protocol Release Drops Queued Key And Page Events But Keeps The Others")
{
    Fixture f;
    const uint8_t ip[4] = {};
    f.proto.PostKey(0, 1, KEY_ACTION_DOWN, 5);
    f.proto.PostWake(7);
    f.proto.PostPageChanged(1, PAGE_CAUSE_USER);
    f.proto.PostWifiState(WifiState::Connecting, ip);
    Assert(f.Request(MSG_RELEASE_CONTROL).Status() == ST_OK);
    f.proto.SendEvents();
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 2 && e[0].type == EVT_WAKE && e[1].type == EVT_WIFI_STATE);
}

// ===========================================================================
// Clock (TIME_SET, TIME_GET, CLOCK_SET)
// ===========================================================================

namespace
{
    const int64_t SOME_TIME_MS = 1762560000123LL;       // 2025-11-08 00:00:00.123 UTC

    Bytes TimeSet(int64_t ms)
    {
        Bytes p;
        p.U64((uint64_t)ms);
        return p;
    }

    // u8 mask, then u8 len + text for the time zone (bit 0) and server (bit 1)
    Bytes ClockSet(uint32_t mask, const char *tz, const char *server)
    {
        Bytes p;
        p.U8(mask);
        if (mask & CLOCK_SET_TZ)
            p.U8((uint32_t)strlen(tz)).Raw(tz, strlen(tz));
        if (mask & CLOCK_SET_SERVER)
            p.U8((uint32_t)strlen(server)).Raw(server, strlen(server));
        return p;
    }

    // TIME_GET read back
    struct TimeInfo
    {
        uint32_t known, source;
        int64_t  ms;
        char     tz[64], server[64];
    };

    TimeInfo TimeGet(Fixture &f)
    {
        Msg m = f.Request(MSG_TIME_GET);
        Check(m.Status() == ST_OK);
        TimeInfo t = {};
        t.known  = m.U8At(4);
        t.source = m.U8At(5);
        t.ms     = (int64_t)m.U64At(6);
        int i = 14;
        uint32_t n = m.U8At(i++);
        Check(n < sizeof(t.tz));
        memcpy(t.tz, m.data + i, n);
        i += (int)n;
        uint32_t s = m.U8At(i++);
        Check(s < sizeof(t.server));
        memcpy(t.server, m.data + i, s);
        i += (int)s;
        Check(i == m.len);
        return t;
    }
}

Fact("Protocol Clock Message Types")
{
    Assert(MSG_TIME_SET == 0x50 && MSG_TIME_GET == 0x51 && MSG_CLOCK_SET == 0x52);
    Assert(INFO_FLAG_TIME_SET == 0x0008);
}

Fact("Protocol Time Set Sets The Clock")
{
    Fixture f;
    Assert(f.Request(MSG_TIME_SET, TimeSet(SOME_TIME_MS)).Status() == ST_OK);
    Assert(f.time_sets == 1 && f.time_ms == SOME_TIME_MS);

    // The earliest time accepted
    Assert(f.Request(MSG_TIME_SET, TimeSet(CLOCK_VALID_FROM_MS)).Status() == ST_OK);
    Assert(f.time_ms == CLOCK_VALID_FROM_MS);

    // Trailing bytes are ignored; pieces work
    Bytes p = TimeSet(SOME_TIME_MS + 1);
    p.U32(0xDEADBEEF);
    Assert(f.Request(MSG_TIME_SET, p, 3).Status() == ST_OK && f.time_ms == SOME_TIME_MS + 1);
}

Fact("Protocol Time Set Errors Change Nothing")
{
    Fixture f;
    Bytes shortp;
    shortp.U32(1).U16(2).U8(3);                         // 7 bytes
    Assert(f.Request(MSG_TIME_SET, shortp).Status() == ST_BAD_LENGTH);
    Assert(f.Request(MSG_TIME_SET).Status() == ST_BAD_LENGTH);
    Assert(f.Request(MSG_TIME_SET, TimeSet(CLOCK_VALID_FROM_MS - 1)).Status() == ST_BAD_ARG);   // before 2025
    Assert(f.Request(MSG_TIME_SET, TimeSet(0)).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_TIME_SET, TimeSet(-1)).Status() == ST_BAD_ARG);
    Assert(f.time_sets == 0 && f.time_ms == 0);
}

Fact("Protocol Time Get Layout")
{
    Fixture f;
    Msg m = f.Request(MSG_TIME_GET);
    Assert(m.Status() == ST_OK);
    Assert(m.len == 4 + 1 + 1 + 8 + 1 + 4 + 1 + 12);   // known, source, ms, "UTC0", "pool.ntp.org"
    TimeInfo t = TimeGet(f);
    Assert(t.known == 0 && t.source == 0 && t.ms == 0);
    Assert(!strcmp(t.tz, CLOCK_TZ_DEFAULT) && !strcmp(t.server, CLOCK_SERVER_DEFAULT));

    f.Request(MSG_TIME_SET, TimeSet(SOME_TIME_MS));
    t = TimeGet(f);
    Assert(t.known == 1 && t.source == (uint32_t)ClockSource::Host && t.ms == SOME_TIME_MS);

    // Shows what the device layer reports (NTP set it)
    f.time_source = ClockSource::Ntp;
    t = TimeGet(f);
    Assert(t.known == 1 && t.source == 2);

    // A payload is ignored
    Bytes p;
    p.U32(1);
    Assert(f.Request(MSG_TIME_GET, p).Status() == ST_OK);
}

Fact("Protocol Clock Set Mask Combinations")
{
    Fixture f;
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ, "AEST-10AEDT,M10.1.0,M4.1.0/3", "")).Status() == ST_OK);
    TimeInfo t = TimeGet(f);
    Assert(!strcmp(t.tz, "AEST-10AEDT,M10.1.0,M4.1.0/3") && !strcmp(t.server, CLOCK_SERVER_DEFAULT));
    Assert(f.clock_changes == 1 && f.clock_storage.saves == 1);

    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_SERVER, "", "time.example.com")).Status() == ST_OK);
    t = TimeGet(f);
    Assert(!strcmp(t.tz, "AEST-10AEDT,M10.1.0,M4.1.0/3") && !strcmp(t.server, "time.example.com"));

    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ | CLOCK_SET_SERVER, "EST5EDT", "10.0.0.1")).Status() == ST_OK);
    t = TimeGet(f);
    Assert(!strcmp(t.tz, "EST5EDT") && !strcmp(t.server, "10.0.0.1"));
    Assert(f.clock_changes == 3);

    // Nothing masked, or nothing different: no change
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(0, "", "")).Status() == ST_OK);
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ, "EST5EDT", "")).Status() == ST_OK);
    Assert(f.clock_changes == 3 && f.clock_storage.saves == 3);

    // Trailing bytes and pieces
    Bytes p = ClockSet(CLOCK_SET_SERVER, "", "a.b");
    p.U8(9);
    Assert(f.Request(MSG_CLOCK_SET, p, 2).Status() == ST_OK && !strcmp(TimeGet(f).server, "a.b"));
}

Fact("Protocol Clock Set Empty Means The Default")
{
    Fixture f;
    f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ | CLOCK_SET_SERVER, "EST5EDT", "a.b"));
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ, "", "")).Status() == ST_OK);
    TimeInfo t = TimeGet(f);
    Assert(!strcmp(t.tz, CLOCK_TZ_DEFAULT) && !strcmp(t.server, "a.b"));
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_SERVER, "", "")).Status() == ST_OK);
    t = TimeGet(f);
    Assert(!strcmp(t.tz, CLOCK_TZ_DEFAULT) && !strcmp(t.server, CLOCK_SERVER_DEFAULT));
}

Fact("Protocol Clock Set Errors Change Nothing")
{
    Fixture f;
    f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ | CLOCK_SET_SERVER, "EST5EDT", "a.b"));
    int changes = f.clock_changes;

    char longest[80];
    memset(longest, 'a', sizeof(longest));
    longest[CLOCK_FIELD_MAX + 1] = 0;                                                              // 64 bytes
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ, longest, "")).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_SERVER, "", longest)).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ, "UTC 0", "")).Status() == ST_BAD_ARG);      // space
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_SERVER, "", "a_b")).Status() == ST_BAD_ARG);    // server chars
    // All or nothing: a good time zone with a bad server changes neither
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ | CLOCK_SET_SERVER, "UTC0", "a b")).Status() == ST_BAD_ARG);
    // Unknown mask bits
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(0x04, "", "")).Status() == ST_BAD_ARG);

    // Lengths
    Assert(f.Request(MSG_CLOCK_SET).Status() == ST_BAD_LENGTH);                                      // no mask
    Bytes noLen;
    noLen.U8(CLOCK_SET_TZ);
    Assert(f.Request(MSG_CLOCK_SET, noLen).Status() == ST_BAD_LENGTH);
    Bytes shortTz;
    shortTz.U8(CLOCK_SET_TZ).U8(5).Raw("UTC", 3);
    Assert(f.Request(MSG_CLOCK_SET, shortTz).Status() == ST_BAD_LENGTH);
    Bytes noServer;
    noServer.U8(CLOCK_SET_TZ | CLOCK_SET_SERVER).U8(4).Raw("UTC0", 4);
    Assert(f.Request(MSG_CLOCK_SET, noServer).Status() == ST_BAD_LENGTH);

    TimeInfo t = TimeGet(f);
    Assert(!strcmp(t.tz, "EST5EDT") && !strcmp(t.server, "a.b"));
    Assert(f.clock_changes == changes);
}

Fact("Protocol Clock Set Reports A Storage Failure")
{
    Fixture f;
    f.clock_storage.fail_save = true;
    Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ, "EST5EDT", "")).Status() == ST_IO_ERROR);
    Assert(!strcmp(TimeGet(f).tz, CLOCK_TZ_DEFAULT) && f.clock_changes == 0);
}

Fact("Protocol Clock Requests Work Standalone And Hosted")
{
    for (int hosted = 0; hosted < 2; hosted++) {
        Fixture f(1u << 20, hosted != 0);
        Assert(f.proto.Hosted() == (hosted != 0));
        Assert(f.Request(MSG_TIME_SET, TimeSet(SOME_TIME_MS)).Status() == ST_OK);
        Assert(f.Request(MSG_TIME_GET).Status() == ST_OK);
        Assert(f.Request(MSG_CLOCK_SET, ClockSet(CLOCK_SET_TZ, "EST5EDT", "")).Status() == ST_OK);
        Assert(f.time_ms == SOME_TIME_MS && f.clock_changes == 1);
        Assert(f.proto.Hosted() == (hosted != 0));                  // the mode is left alone
        Assert(f.comp_resets == 0 && f.status_sets == 0);
    }
}

Fact("Protocol Get Info Time Flag Follows The Clock")
{
    Fixture f;
    Assert((InfoFlags(f) & INFO_FLAG_TIME_SET) == 0);
    f.Request(MSG_TIME_SET, TimeSet(SOME_TIME_MS));
    Assert(InfoFlags(f) == (INFO_FLAG_BACKLIGHT | INFO_FLAG_TIME_SET));
    f.time_ms = 1000;                                                   // power cycled: 1970 again
    Assert((InfoFlags(f) & INFO_FLAG_TIME_SET) == 0);
}

Fact("Protocol Standalone Right Text Is The Clock After Release And Cleared By A Takeover")
{
    Fixture f;
    Bytes t;
    t.U8(STATUS_TEXT_RIGHT).U8(3).Raw("abc", 3);
    f.Request(MSG_SET_STATUS_TEXT, t);
    f.Request(MSG_RELEASE_CONTROL);
    Assert(!strcmp(f.status_text[1], STATUS_TEXT_STANDALONE_RIGHT) && f.status_text[0][0] == 0);

    f.Request(MSG_TAKE_CONTROL);
    Assert(f.status_text[0][0] == 0 && f.status_text[1][0] == 0);
    f.Request(MSG_SET_STATUS_TEXT, t);
    Assert(f.Request(MSG_RESET).Status() == ST_OK);
    Assert(f.status_text[1][0] == 0);

    // A disconnect is a release too
    f.Request(MSG_SET_STATUS_TEXT, t);
    f.proto.PostDisconnect();
    f.proto.ServiceDisconnect();
    Assert(!strcmp(f.status_text[1], STATUS_TEXT_STANDALONE_RIGHT));
}

// ===========================================================================
// Device name (NAME_SET, NAME_GET)
// ===========================================================================

namespace
{
    Bytes NameSet(const char *name)
    {
        Bytes p;
        p.U8((uint32_t)strlen(name)).Raw(name, strlen(name));
        return p;
    }

    // NAME_GET read back: u8 is_default, u8 len, name
    struct NameInfo
    {
        uint32_t is_default;
        char     name[DEVICE_NAME_MAX + 1];
    };

    NameInfo NameGet(Fixture &f)
    {
        Msg m = f.Request(MSG_NAME_GET);
        Check(m.Status() == ST_OK);
        NameInfo n = {};
        n.is_default = m.U8At(4);
        uint32_t len = m.U8At(5);
        Check(len <= DEVICE_NAME_MAX);
        memcpy(n.name, m.data + 6, len);
        Check(6 + (int)len == m.len);
        return n;
    }
}

Fact("Protocol Name Message Types")
{
    Assert(MSG_NAME_SET == 0x60 && MSG_NAME_GET == 0x61);
}

Fact("Protocol Name Get Is The Default At First")
{
    Fixture f;
    NameInfo n = NameGet(f);
    Assert(n.is_default == 1 && !strcmp(n.name, "controlblock-a1b2c3"));
}

Fact("Protocol Name Set Valid Name")
{
    Fixture f;
    Assert(f.Request(MSG_NAME_SET, NameSet("Den-TV")).Status() == ST_OK);
    NameInfo n = NameGet(f);
    Assert(n.is_default == 0 && !strcmp(n.name, "den-tv"));
    Assert(f.name_changes == 1 && f.name_storage.present);

    Assert(f.Request(MSG_NAME_SET, NameSet("")).Status() == ST_OK);        // back to the default
    n = NameGet(f);
    Assert(n.is_default == 1 && !strcmp(n.name, "controlblock-a1b2c3"));
    Assert(!f.name_storage.present);
}

Fact("Protocol Name Set Errors Change Nothing")
{
    Fixture f;
    f.Request(MSG_NAME_SET, NameSet("den"));
    int changes = f.name_changes;
    Assert(f.Request(MSG_NAME_SET, NameSet("-den")).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_NAME_SET, NameSet("a.b")).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_NAME_SET, NameSet("abcdefghijklmnopqrstuvwxyz0123456")).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_NAME_SET).Status() == ST_BAD_LENGTH);             // no length
    Bytes shortName;
    shortName.U8(5).Raw("den", 3);
    Assert(f.Request(MSG_NAME_SET, shortName).Status() == ST_BAD_LENGTH);
    Bytes extra;
    extra.U8(3).Raw("abcd", 4);
    Assert(f.Request(MSG_NAME_SET, extra).Status() == ST_BAD_LENGTH);
    Assert(!strcmp(NameGet(f).name, "den"));
    Assert(f.name_changes == changes);

    f.name_storage.fail_save = true;
    Assert(f.Request(MSG_NAME_SET, NameSet("other")).Status() == ST_IO_ERROR);
    Assert(!strcmp(NameGet(f).name, "den"));
}

Fact("Protocol Name Allowed While Standalone")
{
    Fixture f(1u << 20, false);
    Assert(f.Request(MSG_NAME_SET, NameSet("den")).Status() == ST_OK);
    Assert(!strcmp(NameGet(f).name, "den"));
}

// ===========================================================================
// Display timing (DISPLAY_SET, DISPLAY_GET)
// ===========================================================================

namespace
{
    Bytes DisplaySet(uint32_t mhz, uint32_t lines)
    {
        Bytes p;
        p.U8(mhz).U16(lines);
        return p;
    }

    // DISPLAY_GET read back: u8 pclk_mhz, u16 v_total, u8 flags, u8 default_mhz,
    // u16 default_v_total, u8 min_mhz, u8 max_mhz, u16 min_v_total, u16 max_v_total,
    // u16 h_total, u16 fill_lines
    struct DisplayInfo
    {
        uint32_t mhz, lines, flags, default_mhz, default_lines, min_mhz, max_mhz, min_lines, max_lines,
                 h_total, fill_lines;
    };

    DisplayInfo DisplayGet(Fixture &f)
    {
        Msg m = f.Request(MSG_DISPLAY_GET);
        Check(m.Status() == ST_OK);
        Check(m.len == 4 + PROTO_DISPLAY_INFO_SIZE);
        const int b = 4;                // after u16 status, u16 reserved
        DisplayInfo d;
        d.mhz           = m.U8At(b + 0);
        d.lines         = m.U16At(b + 1);
        d.flags         = m.U8At(b + 3);
        d.default_mhz   = m.U8At(b + 4);
        d.default_lines = m.U16At(b + 5);
        d.min_mhz       = m.U8At(b + 7);
        d.max_mhz       = m.U8At(b + 8);
        d.min_lines     = m.U16At(b + 9);
        d.max_lines     = m.U16At(b + 11);
        d.h_total       = m.U16At(b + 13);
        d.fill_lines    = m.U16At(b + 15);
        return d;
    }
}

Fact("Protocol Display Message Types")
{
    Assert(MSG_DISPLAY_SET == 0x70 && MSG_DISPLAY_GET == 0x71);
    Assert(PROTO_DISPLAY_INFO_SIZE == 17);
}

Fact("Protocol Display Get Is The Default At First, With The Limits")
{
    Fixture f;
    DisplayInfo d = DisplayGet(f);
    Assert(d.mhz == 20 && d.lines == 500 && (d.flags & PROTO_DISPLAY_FLAG_DEFAULT));
    Assert(d.default_mhz == 20 && d.default_lines == 500);
    Assert(d.min_mhz == 16 && d.max_mhz == 20 && d.min_lines == 500 && d.max_lines == 1024);
    Assert(d.h_total == 820 && d.fill_lines == 15);
}

Fact("Protocol Display Set Stores And Applies")
{
    Fixture f;
    Assert(f.Request(MSG_DISPLAY_SET, DisplaySet(20, 750)).Status() == ST_OK);
    DisplayInfo d = DisplayGet(f);
    Assert(d.mhz == 20 && d.lines == 750 && !(d.flags & PROTO_DISPLAY_FLAG_DEFAULT));
    Assert(f.display_changes == 1 && f.display_storage.present);

    Assert(f.Request(MSG_DISPLAY_SET, DisplaySet(16, 0)).Status() == ST_OK);    // 0: the default lines
    d = DisplayGet(f);
    Assert(d.mhz == 16 && d.lines == 500);

    Assert(f.Request(MSG_DISPLAY_SET, DisplaySet(0, 0)).Status() == ST_OK);     // back to the defaults
    d = DisplayGet(f);
    Assert(d.mhz == 20 && d.lines == 500 && (d.flags & PROTO_DISPLAY_FLAG_DEFAULT));
    Assert(!f.display_storage.present);
}

Fact("Protocol Display Set Errors Change Nothing")
{
    Fixture f;
    f.Request(MSG_DISPLAY_SET, DisplaySet(20, 750));
    int changes = f.display_changes;
    Assert(f.Request(MSG_DISPLAY_SET, DisplaySet(15, 750)).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_DISPLAY_SET, DisplaySet(21, 750)).Status() == ST_BAD_ARG);   // above the build's clock
    Assert(f.Request(MSG_DISPLAY_SET, DisplaySet(20, 499)).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_DISPLAY_SET, DisplaySet(20, 1025)).Status() == ST_BAD_ARG);
    Assert(f.Request(MSG_DISPLAY_SET).Status() == ST_BAD_LENGTH);
    Bytes shortSet;
    shortSet.U8(20).U8(0xEE);
    Assert(f.Request(MSG_DISPLAY_SET, shortSet).Status() == ST_BAD_LENGTH);
    Bytes longSet = DisplaySet(20, 600);
    longSet.U8(0);
    Assert(f.Request(MSG_DISPLAY_SET, longSet).Status() == ST_BAD_LENGTH);
    DisplayInfo d = DisplayGet(f);
    Assert(d.mhz == 20 && d.lines == 750);
    Assert(f.display_changes == changes);

    f.display_storage.fail_save = true;
    Assert(f.Request(MSG_DISPLAY_SET, DisplaySet(16, 750)).Status() == ST_IO_ERROR);
    Assert(DisplayGet(f).mhz == 20);
}

Fact("Protocol Display Allowed While Standalone")
{
    Fixture f(1u << 20, false);
    Assert(f.Request(MSG_DISPLAY_SET, DisplaySet(20, 900)).Status() == ST_OK);
    Assert(DisplayGet(f).lines == 900);
}
