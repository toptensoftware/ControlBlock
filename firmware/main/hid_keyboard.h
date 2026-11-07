#pragma once

// HID keyboard report building (docs/standalone.md milestone step 3). Tracks
// which keys are down and sends a boot-protocol report (8 bytes: modifiers,
// reserved, 6 key codes) through an injected sink whenever that changes.
// Platform independent and single threaded: the device layer serialises.
//
// `tap` is down now and up HID_TAP_GAP_MS later (from Poll), and asks the
// caller to hold its queue for gap + settle so the next op can't merge with it.
// `hold` is down for a `press` trigger and up for a `release` one.

#include <stdint.h>
#include "layout_types.h"           // HID_MODE_*

#define HID_KB_REPORT_BYTES     8
#define HID_KB_MAX_KEYS        6
#define HID_TAP_GAP_MS      20      // down to up in a tap (tune on the hardware)
#define HID_TAP_SETTLE_MS   20      // after the up, before the next op: two taps of one key stay two taps

struct HidSink
{
    void *ctx;
    void (*send)(const uint8_t *report, void *ctx);     // HID_KB_REPORT_BYTES bytes
};

class HidKeyboard
{
public:
    void Init(const HidSink &sink);

    // One hidKey op at `now` (wrapping ms). `release`: it came from a `release`
    // trigger (only `hold` cares). Returns how long the caller should hold its
    // queue, in ms (0 for none).
    uint32_t Run(uint32_t now, uint8_t mode, uint8_t modifiers, uint8_t key, bool release);

    // Sends the up of a tap that is due
    void Poll(uint32_t now);

    // Everything up (takeover, disconnect, release); a tap in flight is dropped
    void ReleaseAll();

private:
    void Down(uint8_t modifiers, uint8_t key);
    void Up(uint8_t modifiers, uint8_t key);
    void Send();

    HidSink  m_sink = {};
    uint8_t  m_report[HID_KB_REPORT_BYTES] = {};    // what the PC has been told
    uint8_t  m_mods = 0;
    uint8_t  m_keys[HID_KB_MAX_KEYS] = {};
    bool     m_tap = false;                     // an up is due
    uint8_t  m_tap_mods = 0, m_tap_key = 0;
    uint32_t m_tap_at = 0;
};
