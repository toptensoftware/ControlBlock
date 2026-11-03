#include <string.h>
#include "hid_keyboard.h"

void HidKeyboard::Init(const HidSink &sink)
{
    m_sink = sink;
    memset(m_report, 0, sizeof(m_report));
    m_mods = 0;
    memset(m_keys, 0, sizeof(m_keys));
    m_tap = false;
}

// Only a change goes out
void HidKeyboard::Send()
{
    uint8_t r[HID_KB_REPORT_BYTES] = { m_mods, 0 };
    memcpy(r + 2, m_keys, HID_KB_MAX_KEYS);
    if (memcmp(r, m_report, sizeof(r)) == 0)
        return;
    memcpy(m_report, r, sizeof(r));
    if (m_sink.send)
        m_sink.send(r, m_sink.ctx);
}

void HidKeyboard::Down(uint8_t modifiers, uint8_t key)
{
    m_mods |= modifiers;
    if (key) {
        int free_slot = -1;
        for (int i = 0; i < HID_KB_MAX_KEYS; i++) {
            if (m_keys[i] == key) { free_slot = -2; break; }
            if (!m_keys[i] && free_slot < 0) free_slot = i;
        }
        if (free_slot >= 0)
            m_keys[free_slot] = key;        // all six used: the key is dropped
    }
    Send();
}

void HidKeyboard::Up(uint8_t modifiers, uint8_t key)
{
    m_mods &= (uint8_t)~modifiers;
    if (key)
        for (int i = 0; i < HID_KB_MAX_KEYS; i++)
            if (m_keys[i] == key)
                m_keys[i] = 0;
    // Keep the key codes packed from the front so a report reads the same however it got there
    int n = 0;
    for (int i = 0; i < HID_KB_MAX_KEYS; i++)
        if (m_keys[i])
            m_keys[n++] = m_keys[i];
    for (; n < HID_KB_MAX_KEYS; n++)
        m_keys[n] = 0;
    Send();
}

uint32_t HidKeyboard::Run(uint32_t now, uint8_t mode, uint8_t modifiers, uint8_t key, bool release)
{
    switch (mode) {
    case HID_MODE_TAP:
        Poll(now + HID_TAP_GAP_MS);     // a caller that ignored the hold: finish the earlier tap first
        Down(modifiers, key);
        m_tap = true; m_tap_mods = modifiers; m_tap_key = key; m_tap_at = now + HID_TAP_GAP_MS;
        return HID_TAP_GAP_MS + HID_TAP_SETTLE_MS;
    case HID_MODE_HOLD:
        release ? Up(modifiers, key) : Down(modifiers, key);
        break;
    case HID_MODE_DOWN:
        Down(modifiers, key);
        break;
    case HID_MODE_UP:
        Up(modifiers, key);
        break;
    }
    return 0;
}

void HidKeyboard::Poll(uint32_t now)
{
    // Signed difference: right across the clock wrapping
    if (m_tap && (int32_t)(now - m_tap_at) >= 0) {
        m_tap = false;
        Up(m_tap_mods, m_tap_key);
    }
}

void HidKeyboard::ReleaseAll()
{
    m_tap = false;
    m_mods = 0;
    memset(m_keys, 0, sizeof(m_keys));
    Send();
}
