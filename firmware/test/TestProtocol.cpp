// Host protocol: framing, every request, events and stream resets, fed as
// raw bytes (docs/protocol.md)

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "protocol.h"
#include "store_env.h"

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
    };

    struct Fixture
    {
        StoreEnv   env;
        ImageStore store;
        KeyLayout  layout;
        Protocol   proto;

        List<uint8_t> out;
        int  lock_depth = 0, comp_resets = 0, page_count = -1, page_set = -1, current = 0;
        size_t space = 1 << 20;         // write_space: room for events
        bool animate = false, backlight = true;
        List<int> changed_pages;

        explicit Fixture(uint32_t capacity = 1u << 20)
        {
            store.Init(env.Host(), capacity, COMP_KEY_SIZE, COMP_KEY_SIZE);
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
            h.fw_version       = "1.2.3";
            h.serial           = "ABCDEF";
            h.ctx              = this;
            proto.Init(&store, &layout, h);
        }

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

    Bytes Keys(int count, const KeyDef *defs)
    {
        Bytes p;
        p.U16(count);
        for (int i = 0; i < count; i++) {
            p.U8(defs[i].page).U8(defs[i].key).U16(defs[i].flags);
            for (int s = 0; s < KEY_SLOTS; s++)
                p.U32(defs[i].ids[s]);
        }
        return p;
    }
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
    all.Add(Message(MSG_PUT_IMAGE, 2000, f.PutImage(5, f.env.Icon(0))));
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
    Assert(f.Upload(1, f.env.Icon(0)) == ST_OK);
    Msg m = f.Request(MSG_GET_INFO);
    Assert(m.Status() == ST_OK);
    int i = 4;
    Assert(m.U16At(i) == PROTO_VERSION);                      i += 4;
    Assert(m.U32At(i) == PROTO_MAX_PAYLOAD);                  i += 4;
    Assert(m.U32At(i) == 1u << 20);                           i += 4;     // capacity
    Assert(m.U32At(i) == sizeof(ImageData) + f.env.Size(f.env.Icon(0))); i += 4;
    Assert(m.U32At(i) == 1);                                  i += 4;     // count
    Assert(m.U32At(i) > 0);                                   i += 4;     // largest free
    Assert(m.U16At(i) == SCREEN_W && m.U16At(i + 2) == SCREEN_H); i += 4;
    Assert(m.U8At(i) == COMP_COLS && m.U8At(i + 1) == COMP_ROWS);  i += 2;
    Assert(m.U8At(i) == LAYOUT_MAX_PAGES && m.U8At(i + 1) == 1);   i += 2;
    Assert(m.U16At(i) == COMP_KEY_SIZE && m.U16At(i + 2) == COMP_KEY_SIZE); i += 4;
    Assert(m.U16At(i) == KEY_ICON_SIZE && m.U16At(i + 2) == KEY_ICON_SIZE); i += 4;
    Assert(m.U8At(i) == 0);                                   i += 1;     // current page
    Assert(m.U8At(i) == 5 && memcmp(m.data + i + 1, "1.2.3", 5) == 0); i += 6;
    Assert(m.U8At(i) == 6 && memcmp(m.data + i + 1, "ABCDEF", 6) == 0); i += 7;
    Assert(m.len == i);
}

Fact("Protocol Upload Query And Set Keys")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Face(0)) == ST_OK);

    // In chunks, with other messages between them
    const void *icon = f.env.Icon(3);
    uint32_t size = f.env.Size(icon), half = size / 2;
    Assert(f.Request(MSG_PUT_IMAGE, f.PutImage(2, icon, 0, half)).Status() == ST_OK);
    Assert(f.Request(MSG_PING).Status() == ST_OK);
    Assert(!f.store.IsLive(2));
    Assert(f.Request(MSG_PUT_IMAGE, f.PutImage(2, icon, half)).Status() == ST_OK);

    uint32_t ids[] = { 1, 2, 3, 0 };
    Msg q = f.Request(MSG_QUERY_IMAGES, Ids(4, ids));
    Assert(q.Status() == ST_OK && q.U16At(4) == 4 && q.len == 7);
    Assert(q.U8At(6) == 0x03);

    KeyDef defs[] = { { 0, 3, 0, { 1, 2, 0, 0 } } };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    comp_key_t keys[COMP_KEYS];
    f.layout.GetPage(0, keys);
    Assert(keys[3].bg == f.store.Find(1)->data->Pixels() && keys[3].fg == f.store.Find(2)->data->Pixels());
    Assert(f.changed_pages.GetCount() == 1 && f.changed_pages[0] == 0);
}

Fact("Protocol Upload Errors")
{
    Fixture f;
    const void *icon = f.env.Icon(0);
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

Fact("Protocol Set Keys Errors Change Nothing")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Face(0)) == ST_OK);
    KeyDef ok[] = { { 0, 0, 0, { 1, 0, 0, 0 } }, { 0, 1, 0, { 9, 0, 0, 0 } } };
    Assert(f.Request(MSG_SET_KEYS, Keys(2, ok)).Status() == ST_NOT_FOUND);
    KeyDef page[] = { { 1, 0, 0, { 1, 0, 0, 0 } } };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, page)).Status() == ST_BAD_ARG);

    // Count says more than there is
    Bytes p = Keys(1, ok);
    p.b.GetBuffer()[0] = 2;
    Assert(f.Request(MSG_SET_KEYS, p).Status() == ST_BAD_LENGTH);
    Assert(f.store.Find(1)->key_refs == 0);
    Assert(f.changed_pages.GetCount() == 0);
}

Fact("Protocol Free Images Is Never Refused")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Face(0)) == ST_OK);
    Assert(f.Upload(2, f.env.Icon(0)) == ST_OK);
    KeyDef defs[] = { { 0, 0, 0, { 1, 2, 0, 0 } } };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    const void *px = f.store.Find(2)->data->Pixels();

    uint32_t ids[] = { 2, 77 };
    Assert(f.Request(MSG_FREE_IMAGES, Ids(2, ids)).Status() == ST_OK);
    Msg q = f.Request(MSG_QUERY_IMAGES, Ids(2, ids));
    Assert(q.U8At(6) == 0);
    comp_key_t keys[COMP_KEYS];
    f.layout.GetPage(0, keys);
    Assert(keys[0].fg == px);           // still drawn

    // Its key goes; once swept, the next message frees it
    KeyDef none[] = { { 0, 0, 0, { 1, 0, 0, 0 } } };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, none)).Status() == ST_OK);
    Assert(f.env.IsAllocated(px));
    Assert(f.proto.Idle());             // still waiting
    f.env.Sweep();
    Assert(!f.proto.Idle());
    Assert(!f.env.IsAllocated(px));
}

Fact("Protocol Replace Via Put Image")
{
    Fixture f;
    Assert(f.Upload(2, f.env.Icon(0)) == ST_OK);
    KeyDef defs[] = { { 0, 4, 0, { 0, 2, 0, 0 } } };
    Assert(f.Request(MSG_SET_KEYS, Keys(1, defs)).Status() == ST_OK);
    Assert(f.Upload(2, f.env.Icon(1)) == ST_OK);
    comp_key_t keys[COMP_KEYS];
    f.layout.GetPage(0, keys);
    Assert(memcmp(keys[4].fg, f.env.Icon(1), f.env.Size(f.env.Icon(1))) == 0);
}

Fact("Protocol Reset Clears Keys And Images")
{
    Fixture f;
    Assert(f.Upload(1, f.env.Face(0)) == ST_OK);
    Bytes n;
    n.U8(3);
    Assert(f.Request(MSG_SET_PAGE_COUNT, n).Status() == ST_OK);
    KeyDef defs[] = { { 2, 0, 0, { 1, 0, 0, 0 } } };
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
    Bytes far;
    far.U8(4);
    Assert(f.Request(MSG_SET_PAGE, far).Status() == ST_BAD_ARG);

    Bytes off;
    off.U8(0);
    Assert(f.Request(MSG_SET_BACKLIGHT, off).Status() == ST_OK && !f.backlight);
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
    f.proto.PostKey(1, 7, KEY_ACTION_DOWN, 1234);
    f.proto.PostPageChanged(2, PAGE_CAUSE_USER);
    Assert(f.proto.SendEvents());
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 2);
    Assert(e[0].type == EVT_KEY && e[0].flags == PROTO_FLAG_EVENT);
    Assert(e[0].len == 8);
    Assert(e[0].U8At(0) == 1 && e[0].U8At(1) == 7 && e[0].U8At(2) == KEY_ACTION_DOWN && e[0].U8At(3) == 0);
    Assert(e[0].U32At(4) == 1234);
    Assert(e[1].type == EVT_PAGE_CHANGED && e[1].U8At(0) == 2 && e[1].U8At(1) == PAGE_CAUSE_USER);
    Assert(e[1].seq == (uint16_t)(e[0].seq + 1));
    Assert(!f.proto.SendEvents());

    // Overflow: the oldest go, and the next KEY event says so
    for (int i = 0; i < PROTO_EVENT_QUEUE + 10; i++)
        f.proto.PostKey(0, i % COMP_KEYS, KEY_ACTION_UP, i);
    f.proto.SendEvents();
    e = f.Read();
    Assert(e.GetCount() == PROTO_EVENT_QUEUE);
    Assert(e[0].U32At(4) == 10);
    Assert(e[0].U8At(3) == 1);
    Assert(e[1].U8At(3) == 0);

    // A stream reset drops queued events
    f.proto.PostKey(0, 0, KEY_ACTION_DOWN, 0);
    f.proto.ResetStream();
    Assert(!f.proto.SendEvents());
}

Fact("Protocol Events Wait While The Host Isn't Reading")
{
    Fixture f;
    f.space = 30;                       // room for one event (12 + 8 bytes), not two
    f.proto.PostKey(0, 1, KEY_ACTION_DOWN, 1);
    f.proto.PostKey(0, 1, KEY_ACTION_UP, 2);
    Assert(f.proto.SendEvents());
    Assert(f.Read().GetCount() == 1);
    f.space = 0;
    Assert(!f.proto.SendEvents());
    f.space = 1000;
    Assert(f.proto.SendEvents());
    List<Msg> e = f.Read();
    Assert(e.GetCount() == 1 && e[0].U8At(2) == KEY_ACTION_UP);
}
