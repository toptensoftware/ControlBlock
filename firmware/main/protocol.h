#pragma once

// Mode 1 host protocol: parses the byte stream from the vendor bulk OUT
// endpoint, runs each request against the image store and key layout, and
// writes responses and events for bulk IN. Platform independent: the device
// (host_link.cpp) supplies the transport and the screen through
// ProtocolHost. Spec: docs/protocol.md (wire format), docs/image-assets.md.
// The standalone config upload is ConfigStore's (config_store.h).
//
// Receive(), ResetStream(), SendEvents() and Idle() run on one task (the
// protocol task). PostKey(), PostPageChanged() and PostWake() are safe from any task.

#include <stddef.h>
#include <stdint.h>
#include "port.h"
#include "image_store.h"
#include "key_layout.h"
#include "config_store.h"

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
    MSG_SET_STATUS_TEXT = 0x03,
    MSG_SET_BACKLIGHT  = 0x05,
    MSG_PUT_IMAGE      = 0x10,
    MSG_FREE_IMAGES    = 0x11,
    MSG_QUERY_IMAGES   = 0x12,
    MSG_SET_PAGE_COUNT = 0x20,
    MSG_SET_KEYS       = 0x21,
    MSG_SET_PAGE       = 0x22,
    MSG_CONFIG_BEGIN   = 0x30,
    MSG_CONFIG_DATA    = 0x31,
    MSG_CONFIG_COMMIT  = 0x32,

    EVT_KEY            = 0x80,
    EVT_PAGE_CHANGED   = 0x81,
    EVT_WAKE           = 0x82,
    EVT_HEARTBEAT      = 0x83,
};

// Heartbeat: an event about once a second, so the host's bulk IN read always
// completes before its timeout. A cancelled read leaves the IN pipe deaf on
// Windows (docs/protocol.md). Sent only while this much of the transmit FIFO
// (CONFIG_TINYUSB_VENDOR_TX_BUFSIZE, 2048) is free, so with no app reading,
// the stale ones stop at half full and never crowd out a response.
#define PROTO_HEARTBEAT_MS   1000
#define PROTO_HEARTBEAT_ROOM 1024

// GET_INFO flags (docs/backlight.md)
#define INFO_FLAG_BACKLIGHT  0x0001         // the backlight is on

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
    ST_BAD_CRC      = 9,
    ST_IO_ERROR     = 10,
};

// KEY event actions, PAGE_CHANGED causes
#define KEY_ACTION_DOWN      0
#define KEY_ACTION_UP        1
#define KEY_ACTION_CANCEL    2
#define PAGE_CAUSE_USER      0
#define PAGE_CAUSE_HOST      1

#define PROTO_KEYDEF_SIZE    20             // KeyDef on the wire

// SET_STATUS_TEXT fields, and the buffer a text needs (with its NUL).
// host_link.cpp checks the size against the status bar's.
#define STATUS_TEXT_LEFT     0
#define STATUS_TEXT_RIGHT    1
#define STATUS_TEXT_MAX      48

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
    // Is it on now? (GET_INFO flags; the device layer holds the state)
    bool (*backlight_on)(void *ctx);
    // A status bar text (STATUS_TEXT_LEFT/RIGHT; "" clears): valid UTF-8, at
    // most STATUS_TEXT_MAX - 1 bytes. Called without the lock; must not block
    // for long (the bar is drawn by its own task).
    void (*set_status_text)(int field, const char *text, void *ctx);

    // After a config commit's response has been sent: restart the device.
    // Should give USB a moment to send it first; need not return.
    void (*restart)(void *ctx);

    const char *fw_version;
    const char *serial;
    void       *ctx;
};

class Protocol
{
public:
    void Init(ImageStore *store, KeyLayout *layout, ConfigStore *config, const ProtocolHost &host);

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
    void PostWake(uint32_t time_ms);

    // Protocol task: send queued events that fit in write_space(). Returns
    // true if any were sent.
    bool SendEvents();

    // Protocol task, at least every PROTO_HEARTBEAT_MS: send a HEARTBEAT
    // event if one is due (`now_ms`: the device's millisecond clock) and
    // the transmit FIFO has PROTO_HEARTBEAT_ROOM free. Not queued: it goes
    // straight out, so it never overflows the event queue. Returns true if sent.
    bool SendHeartbeat(uint32_t now_ms);

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
    void OnSetStatusText();
    void OnFreeImages();
    void OnQueryImages();
    void OnSetPageCount();
    void OnSetKeys();
    void OnSetPage();
    void OnConfigBegin();
    void OnConfigData();
    void OnConfigCommit();
    bool ReadIds(uint16_t *count);
    void PostEvent(const Event &e);

    ImageStore  *m_store = nullptr;
    KeyLayout   *m_layout = nullptr;
    ConfigStore *m_config = nullptr;
    ProtocolHost m_host = {};

    // Parser
    State    m_state = State::Header;
    bool     m_corrupt = false;
    bool     m_host_seen = false;
    bool     m_restart = false;     // a config was committed: nothing more is read, the device restarts
    bool     m_restarted = false;
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
    bool     m_hb_sent = false;     // a heartbeat has gone out (m_hb_ms is when)
    uint32_t m_hb_ms = 0;
};
