#pragma once

// The standalone owner (docs/standalone.md milestone step 2): holds the
// layout root, OpQueue, TriggerDispatcher and GestureBank, and is the only
// thing that touches them. The router's key events come in through Key(), the
// frame tick through Tick(), and the op task calls Poll(). While a host has
// control (SetHosted(true)) nothing fires: held keys and queued ops are
// dropped (the active op is cancelled, see op_exec.h, and every param freed),
// and SetHosted(false) re-arms from the blob with no activity selected.
//
// Platform independent: the clock and the op executor are injected. Not
// thread safe: the device layer (standalone_task.cpp) holds one lock around
// every call.

#include <stdint.h>
#include "activity_switch.h"
#include "layout_types.h"
#include "hid_keyboard.h"
#include "key_gesture.h"
#include "op_exec.h"
#include "op_queue.h"
#include "op_registry.h"
#include "script_host.h"
#include "trigger_dispatch.h"

// What the device does when an activity is selected: swap the pages after Home,
// set the status text and page count, glide to page 1 (the layout, the swipe task
// and the status bar). Called under the owner's lock, after the switch's ops were
// queued and the state committed; it cannot refuse. Both may be null.
struct ActivityHost
{
    void *ctx;
    void (*apply)(int index, void *ctx);    // activity `index` (-1 none) is now selected
    void (*glide)(void *ctx);               // the selected activity was chosen again: show its first page
};

struct StandaloneHost
{
    void *ctx;
    uint32_t (*now_ms)(void *ctx);                                                  // wrapping millisecond clock
    void (*run)(const layoutdata::op *op, uint32_t code, void *ctx);   // an op started (after its executor's start; may be null)
    HidSink hid;                                                    // where hidKey reports go
    NetHost *net;                                                   // where the network ops go (null: they do nothing)
    const ActivityHost *activity;                                   // where the page swap goes (null: only the state changes)
    ScriptPlatform script;                                          // the Lua heap and log (realloc null: `script` ops do nothing)
};

class Standalone
{
public:
    // Standalone mode, armed from the layout: a validated blob, relocated
    // (LayoutRoot() of it). A null root (no layout) leaves everything ignored.
    void Init(const StandaloneHost &host, const layoutdata::layoutRoot *root);

    // true: a host took control. false: back to standalone, re-armed.
    void SetHosted(bool hosted);
    bool Hosted() const { return m_hosted; }

    // The selected activity (-1 none); false for an index outside the layout
    bool SelectActivity(int index);

    // The selected activity (-1 none) and the devices assumed on (bit = device
    // index). Re-arming clears the selection; the mask is kept (the equipment
    // keeps its state) and starts at zero.
    int      Activity() const  { return m_activity; }
    uint32_t DevicesOn() const { return m_devices_on; }

    // A router key event (TOUCH_KEY_PRESS / RELEASE / CANCEL) at `time_ms` for
    // the key with `id` (the control "touch[id]")
    void Key(uint16_t id, int action, uint32_t time_ms);

    // Once per frame: `long` presses that are due
    void Tick();

    // The op task: start the next op. True while there is work.
    bool Poll();

    int Queued() const { return m_queue.Count(); }

    // The Lua runtime (stats)
    const ScriptHost &Scripts() const { return m_script; }

    // Ops that ended in an error (a network op while Wi-Fi is down, ...), and why the last did
    uint32_t Errors() const { return m_queue.Errors(); }
    const char *LastError() const { return m_queue.LastError(); }

private:
    bool Active() const { return m_root && !m_hosted; }
    void Arm();                             // gestures, queue and dispatcher fresh from the layout

    // The switchActivity op (ExecCtx::switch_activity): null on success, else why not.
    // Queues the switch's op lists to run next (straight after the switch op, before
    // the rest of its list: OpQueue::PushListsNext), all or nothing; only if that worked
    // does the selection and device mask change, held keys go up and the host swap the pages.
    // While a script runs (it waits for the lists) a switch whose lists hold a `script` op
    // is refused: there is one Lua state.
    const char *SwitchActivity(int to);
    uint32_t KeyLongPressMs(uint16_t id) const;

    static bool GestureHasBinding(const char *control, const char *event, void *ctx);
    static void GestureTrigger(const char *control, const char *event, void *ctx);

    StandaloneHost    m_host = {};
    const layoutdata::layoutRoot *m_root = nullptr;
    ExecCtx           m_exec = {};          // the executors' hosts (clock, HID, queue)
    OpQueue           m_queue;
    TriggerDispatcher m_dispatch;
    GestureBank       m_gestures;
    HidKeyboard       m_hid;
    ScriptHost        m_script;
    bool              m_hosted = false;
    int               m_activity = -1;
    uint32_t          m_devices_on = 0;
};
