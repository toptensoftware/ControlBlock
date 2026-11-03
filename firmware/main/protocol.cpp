// Mode 1 host protocol. Spec: docs/protocol.md.

#include <string.h>
#include "protocol.h"

// KeyDefs are read straight out of the payload: the wire layout is the
// struct's (little endian, natural alignment)
static_assert(sizeof(KeyDef) == PROTO_KEYDEF_SIZE, "KeyDef must match the wire format");

namespace
{
    // Little-endian reader over a payload. Reads past the end return the
    // default, so fields missing from the end of a payload take it.
    struct Reader
    {
        const uint8_t *p;
        uint32_t       len, pos;

        bool     Has(uint32_t n) const { return pos + n <= len; }
        uint32_t U8(uint32_t def = 0)  { return Has(1) ? p[pos++] : def; }
        uint32_t U16(uint32_t def = 0)
        {
            if (!Has(2))
                return def;
            uint32_t v = p[pos] | (uint32_t)p[pos + 1] << 8;
            pos += 2;
            return v;
        }
        uint32_t U32(uint32_t def = 0)
        {
            if (!Has(4))
                return def;
            uint32_t lo = U16();
            return lo | U16() << 16;
        }
    };

    struct Writer
    {
        uint8_t *p;
        uint32_t cap, len;

        void U8(uint32_t v)  { if (len < cap) p[len++] = (uint8_t)v; }
        void U16(uint32_t v) { U8(v); U8(v >> 8); }
        void U32(uint32_t v) { U16(v); U16(v >> 16); }
        void Str(const char *s)
        {
            size_t n = s ? strlen(s) : 0;
            if (n > 255)
                n = 255;
            U8((uint32_t)n);
            for (size_t i = 0; i < n; i++)
                U8((uint8_t)s[i]);
        }
    };

    // Well-formed UTF-8 (no overlongs, surrogates or code points over
    // U+10FFFF) with no NUL
    bool valid_utf8(const uint8_t *s, uint32_t n)
    {
        for (uint32_t i = 0; i < n; ) {
            uint8_t c = s[i++];
            if (c == 0)
                return false;
            if (c < 0x80)
                continue;
            uint32_t extra, min, cp;
            if      (c >= 0xC2 && c <= 0xDF) { extra = 1; min = 0x80;    cp = c & 0x1F; }
            else if (c >= 0xE0 && c <= 0xEF) { extra = 2; min = 0x800;   cp = c & 0x0F; }
            else if (c >= 0xF0 && c <= 0xF4) { extra = 3; min = 0x10000; cp = c & 0x07; }
            else return false;
            if (i + extra > n)
                return false;
            for (uint32_t k = 0; k < extra; k++) {
                if ((s[i] & 0xC0) != 0x80)
                    return false;
                cp = cp << 6 | (s[i++] & 0x3F);
            }
            if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                return false;
        }
        return true;
    }

    uint16_t status_of(ImageResult r)
    {
        switch (r) {
        case ImageResult::Ok:          return ST_OK;
        case ImageResult::BadArg:      return ST_BAD_ARG;
        case ImageResult::NoSpace:     return ST_NO_SPACE;
        case ImageResult::BadImage:    return ST_BAD_IMAGE;
        case ImageResult::BadSequence: return ST_BAD_SEQUENCE;
        }
        return ST_BAD_ARG;
    }

    uint16_t status_of(ConfigResult r)
    {
        switch (r) {
        case ConfigResult::Ok:          return ST_OK;
        case ConfigResult::BadArg:      return ST_BAD_ARG;
        case ConfigResult::NoSpace:     return ST_NO_SPACE;
        case ConfigResult::BadSequence: return ST_BAD_SEQUENCE;
        case ConfigResult::BadCrc:      return ST_BAD_CRC;
        case ConfigResult::IoError:     return ST_IO_ERROR;
        }
        return ST_BAD_ARG;
    }

    uint16_t status_of(LayoutResult r)
    {
        switch (r) {
        case LayoutResult::Ok:       return ST_OK;
        case LayoutResult::BadArg:   return ST_BAD_ARG;
        case LayoutResult::NotFound: return ST_NOT_FOUND;
        }
        return ST_BAD_ARG;
    }

    void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
    void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
}

void Protocol::Init(ImageStore *store, KeyLayout *layout, ConfigStore *config, const ProtocolHost &host)
{
    m_store  = store;
    m_layout = layout;
    m_config = config;
    m_host   = host;
}

// ===========================================================================
// Parsing
// ===========================================================================

void Protocol::Receive(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len && !m_corrupt && !m_restart) {
        uint32_t n = 0;
        switch (m_state) {
        case State::Header:
            n = PROTO_HEADER_SIZE - m_have;
            if (n > len)
                n = (uint32_t)len;
            memcpy(m_hdr + m_have, p, n);
            m_have += n;
            if (m_have == PROTO_HEADER_SIZE)
                OnHeader();
            break;

        case State::ImagePrefix:
            n = sizeof(m_prefix) - m_have;
            if (n > len)
                n = (uint32_t)len;
            memcpy(m_prefix + m_have, p, n);
            m_have += n;
            if (m_have == sizeof(m_prefix))
                OnImagePrefix();
            break;

        case State::ImageData:
            // Straight into the image's own allocation
            n = m_left < len ? m_left : (uint32_t)len;
            m_store->PutData(p, n);
            m_left -= n;
            if (!m_left) {
                Lock();
                ImageResult r = m_store->PutEnd();
                Unlock();
                Respond(status_of(r));
                EndMessage();
            }
            break;

        case State::Payload:
            n = m_len - m_have;
            if (n > len)
                n = (uint32_t)len;
            memcpy(m_buf + m_have, p, n);
            m_have += n;
            if (m_have == m_len) {
                Dispatch();
                EndMessage();
            }
            break;

        case State::Skip:
            n = m_left < len ? m_left : (uint32_t)len;
            m_left -= n;
            if (!m_left) {
                Respond(m_status);
                EndMessage();
            }
            break;
        }
        p   += n;
        len -= n;
    }
    m_host.flush(m_host.ctx);

    // A committed config: its response is out, now restart
    if (m_restart && !m_restarted) {
        m_restarted = true;
        m_host.restart(m_host.ctx);
    }
}

void Protocol::OnHeader()
{
    Reader r = { m_hdr, PROTO_HEADER_SIZE, 0 };
    uint32_t magic = r.U16();
    m_type = (uint8_t)r.U8();
    r.U8();                         // flags
    m_seq  = (uint16_t)r.U16();
    r.U16();                        // reserved
    m_len  = r.U32();
    m_have = 0;

    if (magic != PROTO_MAGIC || m_len > PROTO_MAX_PAYLOAD) {
        m_corrupt = true;           // out of step: wait for RESET_STREAM
        return;
    }
    m_host_seen = true;

    if (m_type == MSG_PUT_IMAGE && m_len >= sizeof(m_prefix)) {
        m_state = State::ImagePrefix;
    } else if (m_len == 0) {
        Dispatch();
        EndMessage();
    } else {
        m_state = State::Payload;
    }
}

// PUT_IMAGE: id, total size, offset; then the chunk's data
void Protocol::OnImagePrefix()
{
    Reader r = { m_prefix, sizeof(m_prefix), 0 };
    uint32_t id     = r.U32();
    uint32_t total  = r.U32();
    uint32_t offset = r.U32();
    uint32_t chunk  = m_len - (uint32_t)sizeof(m_prefix);

    ImageResult res = m_store->PutBegin(id, total, offset, chunk);
    m_left = chunk;
    if (res != ImageResult::Ok) {
        m_status = status_of(res);
        m_state  = State::Skip;
    } else {
        m_state = State::ImageData;
    }
    if (!m_left) {
        // No data: finish now
        if (m_state == State::ImageData) {
            Lock();
            res = m_store->PutEnd();
            Unlock();
            m_status = status_of(res);
        }
        Respond(m_status);
        EndMessage();
    }
}

void Protocol::EndMessage()
{
    m_state  = State::Header;
    m_have   = 0;
    m_left   = 0;
    m_status = ST_OK;
    Lock();
    m_store->Collect();
    Unlock();
}

void Protocol::ResetStream()
{
    m_store->AbortUpload();
    m_config->Abort();
    m_state   = State::Header;
    m_have    = 0;
    m_left    = 0;
    m_status  = ST_OK;
    m_corrupt = false;

    m_ev_lock.Enter();
    m_ev_count = 0;
    m_ev_lost  = false;
    m_ev_lock.Exit();
}

// ===========================================================================
// Writing
// ===========================================================================

void Protocol::WriteMessage(uint8_t type, uint8_t flags, uint16_t seq, const void *a, size_t alen,
                            const void *b, size_t blen)
{
    uint8_t h[PROTO_HEADER_SIZE];
    put16(h, PROTO_MAGIC);
    h[2] = type;
    h[3] = flags;
    put16(h + 4, seq);
    put16(h + 6, 0);
    put32(h + 8, (uint32_t)(alen + blen));
    m_host.write(h, sizeof(h), m_host.ctx);
    if (alen)
        m_host.write(a, alen, m_host.ctx);
    if (blen)
        m_host.write(b, blen, m_host.ctx);
}

void Protocol::Respond(uint16_t status, const void *data, size_t len)
{
    uint8_t st[4];
    put16(st, status);
    put16(st + 2, 0);
    WriteMessage(m_type, PROTO_FLAG_RESPONSE, m_seq, st, sizeof(st), data, len);
}

// ===========================================================================
// Requests
// ===========================================================================

void Protocol::Dispatch()
{
    switch (m_type) {
    case MSG_PING:           OnPing();         break;
    case MSG_GET_INFO:       OnGetInfo();      break;
    case MSG_RESET:          OnReset();        break;
    case MSG_SET_STATUS_TEXT: OnSetStatusText(); break;
    case MSG_SET_BACKLIGHT:  OnSetBacklight(); break;
    case MSG_PUT_IMAGE:      Respond(ST_BAD_LENGTH); break;     // shorter than its prefix
    case MSG_FREE_IMAGES:    OnFreeImages();   break;
    case MSG_QUERY_IMAGES:   OnQueryImages();  break;
    case MSG_SET_PAGE_COUNT: OnSetPageCount(); break;
    case MSG_SET_KEYS:       OnSetKeys();      break;
    case MSG_SET_PAGE:       OnSetPage();      break;
    case MSG_CONFIG_BEGIN:   OnConfigBegin();  break;
    case MSG_CONFIG_DATA:    OnConfigData();   break;
    case MSG_CONFIG_COMMIT:  OnConfigCommit(); break;
    default:                 Respond(ST_UNKNOWN_TYPE); break;
    }
}

void Protocol::OnPing()
{
    Respond(ST_OK, m_buf, m_len);
}

void Protocol::OnGetInfo()
{
    ImageStoreStats st;
    Lock();
    m_store->GetStats(&st);
    int pages = m_layout->PageCount();
    Unlock();

    uint8_t out[64 + 2 * 256];
    Writer w = { out, sizeof(out), 0 };
    w.U16(PROTO_VERSION);
    w.U16(m_host.backlight_on(m_host.ctx) ? INFO_FLAG_BACKLIGHT : 0);
    w.U32(PROTO_MAX_PAYLOAD);
    w.U32(st.capacity);
    w.U32(st.used);
    w.U32(st.count);
    w.U32(st.largest_free);
    w.U16(SCREEN_W);
    w.U16(SCREEN_H);
    w.U8(COMP_COLS);
    w.U8(COMP_ROWS);
    w.U8(LAYOUT_MAX_PAGES);
    w.U8(pages);
    w.U16(COMP_KEY_SIZE);
    w.U16(COMP_KEY_SIZE);
    w.U16(KEY_ICON_SIZE);
    w.U16(KEY_ICON_SIZE);
    w.U8(m_host.current_page(m_host.ctx));
    w.Str(m_host.fw_version);
    w.Str(m_host.serial);
    ConfigInfo config = {};
    m_config->Stored(&config);          // none: size and CRC stay 0
    w.U32(m_config->MaxSize());
    w.U32(config.size);
    w.U32(config.crc);
    Respond(ST_OK, out, w.len);
}

// Every key and image goes. The compositor is reset between clearing the
// layout and freeing the images, so it references none of them by then.
void Protocol::OnReset()
{
    Lock();
    m_layout->Clear();
    Unlock();
    m_host.reset_compositor(m_host.ctx);
    Lock();
    m_store->Reset();
    Unlock();
    m_host.set_page_count(1, m_host.ctx);
    m_host.set_status_text(STATUS_TEXT_LEFT, "", m_host.ctx);
    m_host.set_status_text(STATUS_TEXT_RIGHT, "", m_host.ctx);
    Respond(ST_OK);
}

// u8 field, u8 len, char text[len]
void Protocol::OnSetStatusText()
{
    Reader r = { m_buf, m_len, 0 };
    if (!r.Has(2))
        return Respond(ST_BAD_LENGTH);
    uint32_t field = r.U8();
    uint32_t len   = r.U8();
    if (!r.Has(len))
        return Respond(ST_BAD_LENGTH);
    if (field > STATUS_TEXT_RIGHT || len >= STATUS_TEXT_MAX || !valid_utf8(m_buf + 2, len))
        return Respond(ST_BAD_ARG);

    char text[STATUS_TEXT_MAX];
    memcpy(text, m_buf + 2, len);
    text[len] = 0;
    m_host.set_status_text((int)field, text, m_host.ctx);
    Respond(ST_OK);
}

void Protocol::OnSetBacklight()
{
    Reader r = { m_buf, m_len, 0 };
    if (!r.Has(1))
        return Respond(ST_BAD_LENGTH);
    m_host.set_backlight(r.U8() != 0, m_host.ctx);
    Respond(ST_OK);
}

// u16 count, u32 ids[count]
bool Protocol::ReadIds(uint16_t *count)
{
    Reader r = { m_buf, m_len, 0 };
    if (!r.Has(2))
        return false;
    *count = (uint16_t)r.U16();
    return r.Has(*count * 4u);
}

void Protocol::OnFreeImages()
{
    uint16_t count;
    if (!ReadIds(&count))
        return Respond(ST_BAD_LENGTH);
    Reader r = { m_buf, m_len, 2 };
    Lock();
    for (int i = 0; i < count; i++)
        m_store->Free(r.U32());
    Unlock();
    Respond(ST_OK);
}

void Protocol::OnQueryImages()
{
    uint16_t count;
    if (!ReadIds(&count))
        return Respond(ST_BAD_LENGTH);

    uint8_t out[2 + PROTO_MAX_PAYLOAD / 4 / 8 + 1] = {};
    put16(out, count);
    Reader r = { m_buf, m_len, 2 };
    Lock();
    for (int i = 0; i < count; i++)
        if (m_store->IsLive(r.U32()))
            out[2 + i / 8] |= (uint8_t)(1 << (i % 8));
    Unlock();
    Respond(ST_OK, out, 2 + (count + 7) / 8);
}

void Protocol::OnSetPageCount()
{
    Reader r = { m_buf, m_len, 0 };
    if (!r.Has(1))
        return Respond(ST_BAD_LENGTH);
    int count = (int)r.U8();
    Lock();
    LayoutResult res = m_layout->SetPageCount(count);
    Unlock();
    if (res == LayoutResult::Ok)
        m_host.set_page_count(count, m_host.ctx);
    Respond(status_of(res));
}

void Protocol::OnSetKeys()
{
    Reader r = { m_buf, m_len, 0 };
    if (!r.Has(2))
        return Respond(ST_BAD_LENGTH);
    uint32_t count = r.U16();
    if (!r.Has(count * PROTO_KEYDEF_SIZE))
        return Respond(ST_BAD_LENGTH);

    // m_buf + 2 is 4-byte aligned (see m_raw)
    const KeyDef *defs = (const KeyDef *)(m_buf + 2);
    Lock();
    LayoutResult res = m_layout->SetKeys(defs, (int)count);
    Unlock();
    Respond(status_of(res));
}

void Protocol::OnSetPage()
{
    Reader r = { m_buf, m_len, 0 };
    if (!r.Has(1))
        return Respond(ST_BAD_LENGTH);
    int page  = (int)r.U8();
    int flags = (int)r.U8(0);
    Lock();
    int count = m_layout->PageCount();
    Unlock();
    if (page >= count)
        return Respond(ST_BAD_ARG);
    m_host.set_page(page, (flags & 1) != 0, m_host.ctx);
    Respond(ST_OK);
}

// u32 total size, u32 CRC-32 of the blob
void Protocol::OnConfigBegin()
{
    Reader r = { m_buf, m_len, 0 };
    if (!r.Has(8))
        return Respond(ST_BAD_LENGTH);
    uint32_t size = r.U32();
    uint32_t crc  = r.U32();
    Respond(status_of(m_config->Begin(size, crc)));
}

// u32 offset, bytes
void Protocol::OnConfigData()
{
    Reader r = { m_buf, m_len, 0 };
    if (m_len <= 4)
        return Respond(ST_BAD_LENGTH);
    uint32_t offset = r.U32();
    Respond(status_of(m_config->Data(offset, m_buf + 4, m_len - 4)));
}

// Responds first: the device restarts once the response is out (Receive)
void Protocol::OnConfigCommit()
{
    // The payload is empty, so its buffer is free to read the flash back into
    ConfigResult res = m_config->Commit(m_buf, PROTO_MAX_PAYLOAD);
    Respond(status_of(res));
    if (res == ConfigResult::Ok)
        m_restart = true;
}

// ===========================================================================
// Events
// ===========================================================================

void Protocol::PostEvent(const Event &e)
{
    m_ev_lock.Enter();
    if (m_ev_count == PROTO_EVENT_QUEUE) {
        // Full: the oldest goes, and the host is told on the next KEY event
        m_ev_head = (m_ev_head + 1) % PROTO_EVENT_QUEUE;
        m_ev_count--;
        m_ev_lost = true;
    }
    m_events[(m_ev_head + m_ev_count) % PROTO_EVENT_QUEUE] = e;
    m_ev_count++;
    m_ev_lock.Exit();
}

void Protocol::PostKey(int page, int key, int action, uint32_t time_ms)
{
    Event e = { EVT_KEY, 8, { (uint8_t)page, (uint8_t)key, (uint8_t)action, 0 } };
    put32(e.data + 4, time_ms);
    PostEvent(e);
}

void Protocol::PostPageChanged(int page, int cause)
{
    Event e = { EVT_PAGE_CHANGED, 2, { (uint8_t)page, (uint8_t)cause } };
    PostEvent(e);
}

void Protocol::PostWake(uint32_t time_ms)
{
    Event e = { EVT_WAKE, 8, { 0, 0, 0, 0 } };
    put32(e.data + 4, time_ms);
    PostEvent(e);
}

bool Protocol::SendEvents()
{
    bool sent = false;
    while (1) {
        size_t space = m_host.write_space(m_host.ctx);
        m_ev_lock.Enter();
        if (!m_ev_count || space < PROTO_HEADER_SIZE + m_events[m_ev_head].len) {
            m_ev_lock.Exit();
            break;
        }
        Event e = m_events[m_ev_head];
        m_ev_head = (m_ev_head + 1) % PROTO_EVENT_QUEUE;
        m_ev_count--;
        // The lost flag rides on the next KEY or WAKE (the byte differs)
        if ((e.type == EVT_KEY || e.type == EVT_WAKE) && m_ev_lost) {
            e.data[e.type == EVT_KEY ? 3 : 0] |= 1;
            m_ev_lost = false;
        }
        m_ev_lock.Exit();

        WriteMessage(e.type, PROTO_FLAG_EVENT, m_ev_seq++, e.data, e.len);
        sent = true;
    }
    if (sent)
        m_host.flush(m_host.ctx);
    return sent;
}

bool Protocol::SendHeartbeat(uint32_t now_ms)
{
    if (m_hb_sent && now_ms - m_hb_ms < PROTO_HEARTBEAT_MS)     // unsigned: survives the clock wrapping
        return false;
    if (m_host.write_space(m_host.ctx) < PROTO_HEARTBEAT_ROOM)
        return false;                       // not due yet again: it is sent as soon as there's room

    uint8_t t[4];
    put32(t, now_ms);
    WriteMessage(EVT_HEARTBEAT, PROTO_FLAG_EVENT, m_ev_seq++, t, sizeof(t));
    m_host.flush(m_host.ctx);
    m_hb_sent = true;
    m_hb_ms   = now_ms;
    return true;
}

bool Protocol::Idle()
{
    Lock();
    m_store->Collect();
    bool waiting = m_store->HasRetired();
    Unlock();
    return waiting;
}
