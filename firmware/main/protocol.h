#pragma once

// Mode 1 host protocol: parses the byte stream from the vendor bulk OUT
// endpoint, runs each request against the image store and key layout, and
// writes responses and events for bulk IN. Platform independent: the device
// (host_link.cpp) supplies the transport and the screen through
// ProtocolHost. Spec: docs/protocol.md (wire format), docs/image-assets.md.
//
// Receive(), ResetStream(), SendEvents() and Idle() run on one task (the
// protocol task). PostKey() and PostPageChanged() are safe from any task.

#include <stddef.h>
#include <stdint.h>
#include "port.h"
#include "image_store.h"
#include "key_layout.h"

#define PROTO_VERSION        1
#define PROTO_MAGIC          0x4243         // "CB" as bytes 43 42
#define PROTO_HEADER_SIZE    12
#define PROTO_MAX_PAYLOAD    8192           // GET_INFO.max_payload: every message, images included
#define PROTO_EVENT_QUEUE    64

// Header flags
#define PROTO_FLAG_RESPONSE  0x01
#define PROTO_FLAG_EVENT     0x02

// Vendor control request on the interface (USB layer)
#define PROTO_REQ_RESET_STREAM 0x10

// Message types
enum : uint8_t
{
    MSG_PING           = 0x00,
    MSG_GET_INFO       = 0x01,
    MSG_RESET          = 0x02,
    MSG_SET_BACKLIGHT  = 0x05,
    MSG_PUT_IMAGE      = 0x10,
    MSG_FREE_IMAGES    = 0x11,
    MSG_QUERY_IMAGES   = 0x12,
    MSG_SET_PAGE_COUNT = 0x20,
    MSG_SET_KEYS       = 0x21,
    MSG_SET_PAGE       = 0x22,

    EVT_KEY            = 0x80,
    EVT_PAGE_CHANGED   = 0x81,
};

// Response status
enum : uint16_t
{
    ST_OK           = 0,
    ST_UNKNOWN_TYPE = 1,
    ST_BAD_LENGTH   = 2,
    ST_BAD_ARG      = 3,
    ST_NO_SPACE     = 4,
    ST_BAD_IMAGE    = 5,
    ST_NOT_FOUND    = 6,
    ST_BAD_SEQUENCE = 8,
};

// KEY event actions, PAGE_CHANGED causes
#define KEY_ACTION_DOWN      0
#define KEY_ACTION_UP        1
#define KEY_ACTION_CANCEL    2
#define PAGE_CAUSE_USER      0
#define PAGE_CAUSE_HOST      1

#define PROTO_KEYDEF_SIZE    20             // KeyDef on the wire

// What the protocol needs from the device
struct ProtocolHost
{
    // Bytes for the host (bulk IN), in order. May block until there's room.
    void (*write)(const void *data, size_t len, void *ctx);
    // Room write() has now without blocking. Events are sent only into
    // room, so a host that isn't reading leaves them queued.
    size_t (*write_space)(void *ctx);
    // Send whatever write() has queued
    void (*flush)(void *ctx);
    // The model lock (the compositor's get_page takes it too)
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    // comp_reset(): blocks until the compositor references no image.
    // Called without the lock.
    void (*reset_compositor)(void *ctx);
    // The screen: page count (swipe bounds, pager), go to a page, the page
    // shown now, backlight
    void (*set_page_count)(int count, void *ctx);
    void (*set_page)(int page, bool animate, void *ctx);
    int  (*current_page)(void *ctx);
    void (*set_backlight)(bool on, void *ctx);

    const char *fw_version;
    const char *serial;
    void       *ctx;
};

class Protocol
{
public:
    void Init(ImageStore *store, KeyLayout *layout, const ProtocolHost &host);

    // Bytes from the host (bulk OUT), in order, in any pieces. Complete
    // messages are handled as they arrive and their responses flushed.
    void Receive(const void *data, size_t len);

    // RESET_STREAM: drop any partial message and upload, and queued events
    void ResetStream();

    // A bad header was seen: everything is ignored until ResetStream()
    bool Corrupt() const { return m_corrupt; }

    // A valid message has arrived since boot (the demo layout stops)
    bool HostSeen() const { return m_host_seen; }

    // --- Events (any task) ---
    void PostKey(int page, int key, int action, uint32_t time_ms);
    void PostPageChanged(int page, int cause);

    // Protocol task: send queued events that fit in write_space(). Returns
    // true if any were sent.
    bool SendEvents();

    // Protocol task, now and then: free images whose sweep is done.
    // Returns true while any are still waiting.
    bool Idle();

private:
    enum class State { Header, ImagePrefix, ImageData, Payload, Skip };

    struct Event
    {
        uint8_t type;
        uint8_t len;
        uint8_t data[8];
    };

    void OnHeader();
    void OnImagePrefix();
    void Dispatch();
    void EndMessage();
    void Respond(uint16_t status, const void *data = nullptr, size_t len = 0);
    void WriteMessage(uint8_t type, uint8_t flags, uint16_t seq, const void *a, size_t alen,
                      const void *b = nullptr, size_t blen = 0);
    void Lock()   { m_host.lock(m_host.ctx); }
    void Unlock() { m_host.unlock(m_host.ctx); }

    // Handlers: the payload is m_buf[0..m_len)
    void OnPing();
    void OnGetInfo();
    void OnReset();
    void OnSetBacklight();
    void OnFreeImages();
    void OnQueryImages();
    void OnSetPageCount();
    void OnSetKeys();
    void OnSetPage();
    bool ReadIds(uint16_t *count);
    void PostEvent(const Event &e);

    ImageStore  *m_store = nullptr;
    KeyLayout   *m_layout = nullptr;
    ProtocolHost m_host = {};

    // Parser
    State    m_state = State::Header;
    bool     m_corrupt = false;
    bool     m_host_seen = false;
    uint8_t  m_hdr[PROTO_HEADER_SIZE];
    uint32_t m_have = 0;            // bytes of the current part (header, prefix, payload)
    uint8_t  m_type = 0;
    uint16_t m_seq = 0;
    uint32_t m_len = 0;             // payload length
    uint32_t m_left = 0;            // ImageData/Skip: payload bytes still to come
    uint16_t m_status = ST_OK;      // Skip: the response once skipped
    uint8_t  m_prefix[12];

    // Payloads are copied here, 2 bytes in, so SET_KEYS' KeyDefs (after a
    // u16 count) are 4-byte aligned
    alignas(4) uint8_t m_raw[2 + PROTO_MAX_PAYLOAD];
    uint8_t *const m_buf = m_raw + 2;

    // Events
    port::SpinLock m_ev_lock;
    Event    m_events[PROTO_EVENT_QUEUE];
    int      m_ev_head = 0, m_ev_count = 0;
    bool     m_ev_lost = false;
    uint16_t m_ev_seq = 0;
};
