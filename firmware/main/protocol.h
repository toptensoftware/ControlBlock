#pragma once

// Mode 1 host protocol: parses the byte stream from the vendor bulk OUT
// endpoint, runs each request against the image store and key layout, and
// writes responses and events for bulk IN. Platform independent: the device
// (host_link.cpp) supplies the transport and the screen through
// ProtocolHost. Spec: docs/protocol.md (wire format), docs/image-assets.md.
// The standalone layout upload is LayoutStore's (layout_store.h).
//
// Receive(), ResetStream(), SendEvents() and Idle() run on one task (the
// protocol task). PostKey(), PostPageChanged(), PostWake() and PostWifiState()
// are safe from any task.

#include <stddef.h>
#include <stdint.h>
#include "port.h"
#include "image_store.h"
#include "key_layout.h"
#include "layout_store.h"
#include "layout_blob.h"
#include "layout_apply.h"
#include "wifi_credentials.h"
#include "wifi_state.h"
#include "clock_config.h"
#include "device_name.h"
#include "clock_text.h"

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
    MSG_TAKE_CONTROL   = 0x06,
    MSG_RELEASE_CONTROL = 0x07,
    MSG_PUT_IMAGE     = 0x10,
    MSG_FREE_IMAGES    = 0x11,
    MSG_QUERY_IMAGES   = 0x12,
    MSG_SET_PAGE_COUNT = 0x20,
    MSG_SET_KEYS       = 0x21,
    MSG_SET_PAGE       = 0x22,
    MSG_LAYOUT_BEGIN   = 0x30,
    MSG_LAYOUT_DATA    = 0x31,
    MSG_LAYOUT_COMMIT  = 0x32,
    MSG_WIFI_SET       = 0x40,
    MSG_WIFI_GET       = 0x41,
    MSG_WIFI_STATUS    = 0x42,
    MSG_TIME_SET       = 0x50,
    MSG_TIME_GET       = 0x51,
    MSG_CLOCK_SET      = 0x52,
    MSG_NAME_SET       = 0x60,
    MSG_NAME_GET       = 0x61,

    EVT_KEY            = 0x80,
    EVT_PAGE_CHANGED   = 0x81,
    EVT_WAKE           = 0x82,
    EVT_HEARTBEAT      = 0x83,
    EVT_WIFI_STATE     = 0x84,
};

// Heartbeat: an event about once a second, so the host's bulk IN read always
// completes before its timeout. A cancelled read leaves the IN pipe deaf on
// Windows (docs/protocol.md). Sent only while this much of the transmit FIFO
// (CONFIG_TINYUSB_VENDOR_TX_BUFSIZE, 2048) is free, so with no app reading,
// the stale ones stop at half full and never crowd out a response.
#define PROTO_HEARTBEAT_MS   1000
#define PROTO_HEARTBEAT_ROOM 1024

// GET_INFO flags (docs/backlight.md, docs/wifi.md)
#define INFO_FLAG_BACKLIGHT  0x0001         // the backlight is on
#define INFO_FLAG_WIFI_CREDENTIALS 0x0002   // Wi-Fi credentials are stored
#define INFO_FLAG_HOSTED     0x0004         // a host has taken control (else standalone)
#define INFO_FLAG_TIME_SET   0x0008         // the clock is set (reads 2025 or later)

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
    ST_NOT_HOSTED   = 11,       // a key/screen request while standalone: TAKE_CONTROL first
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

// The right text while standalone (boot, release, disconnect): the clock
#define STATUS_TEXT_STANDALONE_RIGHT  "{clock}"

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

    // Optional (may be null). The mode changed: hosted (a host took control)
    // or standalone (released or disconnected). Called without the lock, on
    // the protocol task, after the model has been rebuilt. The device layer
    // cancels whatever belongs to the old mode (held keys).
    void (*mode_changed)(bool hosted, void *ctx);

    // After a layout commit's response has been sent: restart the device.
    // Should give USB a moment to send it first; need not return.
    void (*restart)(void *ctx);

    // WIFI_STATUS: the connection state now (the credentials themselves are
    // WifiConfig's). Called without the lock.
    void (*wifi_status)(WifiStatus *out, void *ctx);

    // TIME_SET: set the clock (UTC; already checked to be 2025 or later).
    // TIME_GET, GET_INFO: the time now, where it came from (None while not
    // known) and whether it is known. Called without the lock.
    void (*set_time)(int64_t unix_ms, void *ctx);
    void (*get_time)(int64_t *unix_ms, ClockSource *source, bool *known, void *ctx);

    const char *fw_version;
    const char *serial;
    void       *ctx;
};

class Protocol
{
public:
    void Init(ImageStore *store, KeyLayout *keys, LayoutStore *layout_store, WifiConfig *wifi,
              ClockConfig *clock, DeviceName *name, const ProtocolHost &host);

    // Bytes from the host (bulk OUT), in order, in any pieces. Complete
    // messages are handled as they arrive and their responses flushed.
    void Receive(const void *data, size_t len);

    // RESET_STREAM: drop any partial message and upload, and queued events
    void ResetStream();

    // A bad header was seen: everything is ignored until ResetStream()
    bool Corrupt() const { return m_corrupt; }

    // A valid message has arrived since boot (the demo keys stop)
    bool HostSeen() const { return m_host_seen; }

    // --- Mode (docs/standalone.md "Runtime modes") ---
    // Boots standalone. TAKE_CONTROL resets the model (like RESET) and starts
    // the KEY / PAGE_CHANGED event stream; RELEASE_CONTROL and a USB
    // disconnect go through one Release(): rebuild Home, stop that stream.
    // While standalone, requests that edit the screen get ST_NOT_HOSTED.

    // The layout's Home, shown on release (null summary: an empty Home). Both
    // must stay valid. Call before the first request.
    void SetHome(const LayoutSummary *summary, const uint8_t *blob);

    // Any task: has a host taken control? The device layer routes key events
    // by this (hosted: PostKey; standalone: the activity's actions).
    bool Hosted() const { return port::AtomicLoad(&m_hosted) != 0; }

    // Any task (the USB unmount / suspend callbacks): the host is gone. Takes
    // effect when the protocol task calls ServiceDisconnect().
    void PostDisconnect() { port::AtomicStore(&m_disconnect, 1); }

    // Protocol task, each time round: if a disconnect was posted, drop the
    // stream state and release (a hosted session) or just turn the backlight
    // back on (nobody is left to wake it).
    void ServiceDisconnect();

    // --- Events (any task) ---
    // PostKey and PostPageChanged do nothing while standalone.
    void PostKey(int page, int key, int action, uint32_t time_ms);
    void PostPageChanged(int page, int cause);
    void PostWake(uint32_t time_ms);
    // The Wi-Fi state or address changed (`ip`: a.b.c.d while Connected, else zeros)
    void PostWifiState(WifiState state, const uint8_t ip[4]);

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
    void OnTakeControl();
    void OnReleaseControl();
    void OnSetBacklight();
    void OnSetStatusText();
    void OnFreeImages();
    void OnQueryImages();
    void OnSetPageCount();
    void OnSetKeys();
    void OnSetPage();
    void OnLayoutBegin();
    void OnLayoutData();
    void OnLayoutCommit();
    void OnWifiSet();
    void OnWifiGet();
    void OnWifiStatus();
    void OnTimeSet();
    void OnTimeGet();
    void OnClockSet();
    void OnNameSet();
    void OnNameGet();
    bool ReadIds(uint16_t *count);
    void PostEvent(const Event &e);
    void DropEvents(bool all);          // all, or just the hosted-only KEY and PAGE_CHANGED
    void ResetModel(bool home);         // RESET's work; `home`: then show Home
    void Release();                     // hosted -> standalone; nothing if already
    bool NeedsHost(uint8_t type) const;

    ImageStore  *m_store = nullptr;
    KeyLayout   *m_keys = nullptr;
    LayoutStore *m_layout_store = nullptr;
    WifiConfig  *m_wifi = nullptr;
    ClockConfig *m_clock = nullptr;
    DeviceName  *m_name = nullptr;
    ProtocolHost m_host = {};

    // Mode (atomics: read from any task)
    uint32_t m_hosted = 0;
    uint32_t m_disconnect = 0;
    const LayoutSummary *m_home = nullptr;
    const uint8_t       *m_home_blob = nullptr;

    // Parser
    State    m_state = State::Header;
    bool     m_corrupt = false;
    bool     m_host_seen = false;
    bool     m_restart = false;     // a layout was committed: nothing more is read, the device restarts
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
