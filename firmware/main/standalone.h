#pragma once

// The standalone owner (docs/standalone.md milestone step 2): holds the
// ConfigView, OpQueue, TriggerDispatcher and GestureBank, and is the only
// thing that touches them. The router's key events come in through Key(), the
// frame tick through Tick(), and the op task calls Poll(). While a host has
// control (SetHosted(true)) nothing fires: held keys and queued ops are
// dropped, and SetHosted(false) re-arms from the blob with no activity
// selected.
//
// Platform independent: the clock and the op executor are injected. Not
// thread safe: the device layer (standalone_task.cpp) holds one lock around
// every call.

#include <stdint.h>
#include "config_view.h"
#include "hid_keyboard.h"
#include "key_gesture.h"
#include "op_queue.h"
#include "trigger_dispatch.h"

struct StandaloneHost
{
    void *ctx;
    uint32_t (*now_ms)(void *ctx);                                                  // wrapping millisecond clock
    void (*run)(const ConfigView &cfg, const uint8_t *op, uint32_t code, void *ctx);   // an op starts (after the built-in executors; may be null)
    HidSink hid;                                                                    // where hidKey reports go
};

class Standalone
{
public:
    // Standalone mode, armed from the blob. `adjust` as for ConfigView (0 on
    // the device, the blob's address for an unrelocated blob). A null blob
    // (no config) leaves everything ignored.
    void Init(const StandaloneHost &host, const uint8_t *blob, uintptr_t adjust);

    // true: a host took control. false: back to standalone, re-armed.
    void SetHosted(bool hosted);
    bool Hosted() const { return m_hosted; }

    // The selected activity (-1 none); false for an index outside the config
    bool SelectActivity(int index);

    // A router key event (TOUCH_KEY_PRESS / RELEASE / CANCEL) at `time_ms`
    void Key(int page, int key, int action, uint32_t time_ms);

    // Once per frame: `long` presses that are due
    void Tick();

    // The op task: start the next op. True while there is work.
    bool Poll();

    int Queued() const { return m_queue.Count(); }

private:
    bool Active() const { return m_cfg.Valid() && !m_hosted; }
    void Arm();                             // gestures, queue and dispatcher fresh from the blob
    uint32_t KeyLongPressMs(int page, int key) const;

    static void RunOp(const ConfigView &cfg, const uint8_t *op, uint32_t code, void *ctx);
    static bool GestureHasBinding(const char *control, const char *event, void *ctx);
    static void GestureTrigger(const char *control, const char *event, void *ctx);

    StandaloneHost    m_host = {};
    const uint8_t    *m_blob = nullptr;
    uintptr_t         m_adjust = 0;
    ConfigView        m_cfg;
    OpQueue           m_queue;
    TriggerDispatcher m_dispatch;
    GestureBank       m_gestures;
    HidKeyboard       m_hid;
    bool              m_hosted = false;
};
