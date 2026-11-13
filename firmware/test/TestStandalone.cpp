// The standalone owner (docs/standalone.md milestone step 2): router key events
// go through the gestures and bindings to the op queue, and the host's
// take-over / release switches it off and re-arms it. The layout is the
// sample baked by host/src/bake (longPressMs 650). Keys go by id (the control
// "touch[id]"; the sample's plain grid keys have the bake's ids page * 15 + slot + 1):
// Home: touch[1] tap (hidKey), touch[2] tap (hidKey) + long (delay), touch[3] tap
// (switchActivity), touch[400] repeating tap (script). Watch's `key1` press (script)
// repeats too (its `pad` entry: delay 300, period 100).

#include "UnitTesting.h"
#include "Core.h"
#include "layout_env.h"
#include "standalone.h"
#include "hid_keyboard.h"
#include "touch_router.h"       // TOUCH_KEY_*
#include "net_env.h"

using namespace SimpleLib;
using namespace layout_env;

namespace
{
    const uint32_t HID = OP_HID_KEY, DELAY = OP_DELAY, SWITCH = OP_SWITCH_ACTIVITY;
    const uint32_t LONG_MS = 650;                       // the sample's settings.longPressMs

    // The sample's key ids (data/sample_layout.json)
    const uint16_t K_SPACE = 1;         // Home slot 0: tap space
    const uint16_t K_NEXT  = 2;         // slot 1: tap ctrl+right, long a delay
    const uint16_t K_WATCH = 3;         // slot 2: switchActivity Watch
    const uint16_t K_RED   = 5;         // slot 4: no bindings
    const uint16_t K_COPY  = 7;         // slot 6: tap ctrl+c
    const uint16_t K_HOLD  = 8;         // slot 7: shift+a held while pressed
    const uint16_t K_OFF   = 10;        // slot 9: switchActivity none
    const uint16_t K_WIDE  = 11;        // slots 10-11 (2U): tap w, its own longPressMs 900
    const uint16_t K_R     = 61;        // a 0.5U key off the whole-unit corners: an assigned id, tap r
    const uint16_t K_GO    = 16;        // Watch's page 0 (the layout's page 1), slot 0: script
    const uint16_t K_RPT   = 400;       // Home slot 3: repeat 100 (delay and period), tap script `return param`

    struct Rig
    {
        List<uint8_t> bytes = LoadSample();
        const layoutdata::layoutRoot *root = nullptr;      // `bytes`, relocated once
        Standalone    sa;
        uint32_t      now = 1000;
        uint32_t      run[64];                  // op codes in the order they ran
        int           ran = 0;
        uint8_t       reports[32][8];           // HID reports sent
        int           sent = 0;
        NetHost      *net = nullptr;            // the network ops' host (null: they do nothing)
        bool          scripts = false;          // give the Lua runtime a heap
        int           script_logs = 0;
        char          script_log[512] = {};     // every script log line, each followed by '|'
        ActivityHost  ah = {};
        int           applied[16];              // ApplyActivity(index) calls, in order
        int           n_applied = 0;
        int           glides = 0;

        explicit Rig(bool start = true)
        {
            root = Relocate(bytes);
            if (start)
                Start();
        }

        void Start()
        {
            StandaloneHost h = {};
            h.ctx    = this;
            h.now_ms = [](void *c) { return ((Rig *)c)->now; };
            h.run    = [](const layoutdata::op *, uint32_t code, void *c) {
                Rig *r = (Rig *)c;
                if (r->ran < 64)
                    r->run[r->ran++] = code;
            };
            h.net      = net;
            if (scripts) {
                h.script.ctx     = this;
                h.script.realloc = [](void *p, size_t n, void *) -> void * { if (!n) { free(p); return nullptr; } return realloc(p, n); };
                h.script.log     = [](const char *line, void *c) {
                    Rig *r = (Rig *)c;
                    r->script_logs++;
                    strncat(r->script_log, line, sizeof(r->script_log) - strlen(r->script_log) - 2);
                    strcat(r->script_log, "|");
                };
            }
            ah.ctx     = this;
            ah.apply   = [](int index, void *c) {
                Rig *r = (Rig *)c;
                if (r->n_applied < 16)
                    r->applied[r->n_applied++] = index;
            };
            ah.glide   = [](void *c) { ((Rig *)c)->glides++; };
            h.activity = &ah;
            h.hid.ctx  = this;
            h.hid.send = [](const uint8_t *rep, void *c) {
                Rig *r = (Rig *)c;
                if (r->sent < 32)
                    memcpy(r->reports[r->sent++], rep, 8);
            };
            sa.Init(h, root);
        }

        void Drain() { for (int i = 0; i < 1000 && sa.Poll(); i++) {} }
        // The last report's [modifiers, first key]
        bool LastIs(uint8_t mods, uint8_t key) const { return sent && reports[sent - 1][0] == mods && reports[sent - 1][2] == key; }
        void Advance(uint32_t ms) { now += ms; sa.Tick(); }
        void Key(uint16_t id, int action) { sa.Key(id, action, now); }
        void Tap(uint16_t id) { Key(id, TOUCH_KEY_PRESS); Key(id, TOUCH_KEY_RELEASE); }
        // Run everything, letting delays pass
        void Settle() { for (int i = 0; i < 50; i++) { Drain(); now += 100; } }
    };

    // The first binding of `control` in a list (relocated blob), or null
    layoutdata::binding *FindBinding(layoutdata::binding *list, uint32_t count, const char *control)
    {
        for (uint32_t i = 0; i < count; i++)
            if (strcmp(list[i].control, control) == 0)
                return &list[i];
        return nullptr;
    }

    // The nth Home binding that starts with a switchActivity op (K_WATCH's is the first, K_OFF's the second)
    layoutdata::binding *HomeSwitchBinding(const layoutdata::layoutRoot *root, int nth)
    {
        for (uint32_t i = 0; i < root->homePage.bindings_count; i++) {
            layoutdata::binding &b = root->homePage.bindings[i];
            if (b.ops_count && b.ops[0]->op == SWITCH && nth-- == 0)
                return &b;
        }
        return nullptr;
    }
    // Its switchActivity op
    layoutdata::switchActivityOp *HomeSwitch(const layoutdata::layoutRoot *root, int nth)
    {
        layoutdata::binding *b = HomeSwitchBinding(root, nth);
        return b ? (layoutdata::switchActivityOp *)b->ops[0] : nullptr;
    }

    const uint32_t WOL = OP_SEND_WOL, GET = OP_HTTP_GET, POST = OP_HTTP_POST, UDP = OP_UDP_PACKET, SCRIPT = OP_SCRIPT;
    const uint32_t TV = 1, AVR = 2;

    layoutdata::scriptOp ScriptOf(const char *src)
    {
        layoutdata::scriptOp s = {};
        s.base.op       = OP_SCRIPT;
        s.source        = src;
        s.source_count  = (uint32_t)strlen(src);
        return s;
    }

    // Ops of `code` that ran
    int Ran(const Rig &r, uint32_t code)
    {
        int n = 0;
        for (int i = 0; i < r.ran; i++)
            n += r.run[i] == code;
        return n;
    }

    // K_RPT's tap logs its param, and touch[1]'s binding becomes K_RPT's release, logging its own
    struct RepeatRig : Rig
    {
        layoutdata::scriptOp tap = ScriptOf("log('tap ' .. param)");
        layoutdata::scriptOp rel = ScriptOf("log('rel ' .. param)");
        layoutdata::op *tap_list[1] = { &tap.base };
        layoutdata::op *rel_list[1] = { &rel.base };
        bool bound = false;

        RepeatRig() : Rig(false)
        {
            scripts = true;
            layoutdata::binding *t = FindBinding(root->homePage.bindings, root->homePage.bindings_count, "touch[400]");
            layoutdata::binding *r = &root->homePage.bindings[0];
            bound = t && strcmp(t->event, "tap") == 0 && strcmp(r->control, "touch[1]") == 0;
            if (t) { t->ops = tap_list; t->ops_count = 1; }
            r->control = "touch[400]"; r->event = "release"; r->ops = rel_list; r->ops_count = 1;
            Start();
        }
    };

    bool IsZero(const GestureTiming &t)
    {
        return !t.long_ms && !t.repeat_delay_ms && !t.repeat_ms && !t.not_before_ms;
    }
}

// ---- auto repeat (K_RPT: repeat 100 = first repeat 100 ms after the press, then every 100 ms;
// a touch key's first repeat waits for the swipe window) ----

Fact("Standalone Repeating Key Runs Its Ops At The Press And Each Period With The Repeat Param")
{
    RepeatRig r;
    Assert(r.bound);
    r.Key(K_RPT, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(strcmp(r.script_log, "tap repeat=0|") == 0);
    r.Advance(100);                                     // the delay has passed, the swipe window hasn't
    r.Drain();
    r.Advance(TOUCH_SWIPE_WINDOW_MS - 100 - 1);
    r.Drain();
    Assert(strcmp(r.script_log, "tap repeat=0|") == 0);
    r.Advance(1);                                       // 150 ms: the first repeat, n from the schedule
    r.Drain();
    Assert(strcmp(r.script_log, "tap repeat=0|tap repeat=1|") == 0);
    r.Advance(50);                                      // 200 ms
    r.Drain();
    r.Advance(100);                                     // 300 ms
    r.Drain();
    Assert(strcmp(r.script_log, "tap repeat=0|tap repeat=1|tap repeat=2|tap repeat=3|") == 0);

    // The release runs `release` once, with no param, and the repeats stop
    r.Key(K_RPT, TOUCH_KEY_RELEASE);
    r.Drain();
    for (int i = 0; i < 10; i++) { r.Advance(100); r.Drain(); }
    Assert(strcmp(r.script_log, "tap repeat=0|tap repeat=1|tap repeat=2|tap repeat=3|rel |") == 0);
    Assert(r.sa.Scripts().Errors() == 0 && r.sa.Errors() == 0);
}

Fact("Standalone Repeating Key Cancelled By A Swipe Releases Once And Stops")
{
    RepeatRig r;
    r.Key(K_RPT, TOUCH_KEY_PRESS);
    r.Drain();                                          // (undrained, the press's list would make the repeat skip)
    r.Advance(TOUCH_SWIPE_WINDOW_MS);
    r.Drain();
    r.Key(K_RPT, TOUCH_KEY_CANCEL);
    r.Drain();
    for (int i = 0; i < 10; i++) { r.Advance(100); r.Drain(); }
    Assert(strcmp(r.script_log, "tap repeat=0|tap repeat=1|rel |") == 0);
}

Fact("Standalone Repeats Are Skipped While The Op Queue Has Lists Waiting")
{
    Rig r(false);
    layoutdata::delayOp slow = {};
    slow.base.op  = OP_DELAY;
    slow.duration = 1000;
    layoutdata::op *list[1] = { &slow.base };
    layoutdata::binding *b = FindBinding(r.root->homePage.bindings, r.root->homePage.bindings_count, "touch[400]");
    Assert(b);
    b->ops = list; b->ops_count = 1;
    r.Start();

    r.Key(K_RPT, TOUCH_KEY_PRESS);                      // at 1000: the first delay starts
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY && r.sa.Queued() == 0);
    r.Advance(TOUCH_SWIPE_WINDOW_MS);                   // the first repeat waits behind it
    r.Drain();
    Assert(r.sa.Queued() == 1);
    for (int i = 0; i < 16; i++) {                      // to 1950: every repeat skipped, the queue doesn't grow
        r.Advance(50);
        r.Drain();
        Assert(r.sa.Queued() == 1 && r.ran == 1);
    }
    r.Advance(50);                                      // 2000: the first delay ends, the waiting one starts
    r.Drain();
    Assert(r.ran == 2 && r.sa.Queued() == 0);
    r.Advance(50);                                      // nothing waits: the next repeat fires
    r.Drain();
    Assert(r.sa.Queued() == 1);
    r.Key(K_RPT, TOUCH_KEY_RELEASE);
    r.Settle();
    Assert(r.ran == 3 && r.sa.Queued() == 0);           // nothing ran after the last repeat
}

Fact("Standalone Activity Switch Stops A Repeating Key")
{
    Rig r;
    r.Key(K_RPT, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(Ran(r, SCRIPT) == 1);
    r.Tap(K_WATCH);
    r.Drain();
    for (int i = 0; i < 10; i++) { r.Advance(100); r.Drain(); }
    r.Key(K_RPT, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(Ran(r, SCRIPT) == 1 && r.sa.Activity() == 0);
}

Fact("Standalone Takeover Stops A Repeating Key")
{
    Rig r;
    r.Key(K_RPT, TOUCH_KEY_PRESS);
    r.Drain();
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    for (int i = 0; i < 10; i++) { r.Advance(100); r.Drain(); }
    Assert(Ran(r, SCRIPT) == 1);
}

Fact("Standalone Repeat Of An Activity Key Is Read From The Activity's Pages")
{
    Rig r(false);
    layoutdata::key &go = r.root->activities[0].pages[0].keys[0];
    Assert(go.id == K_GO && go.repeatMs == 0);
    go.repeatDelayMs = 300;
    go.repeatMs      = 200;
    r.Start();
    Assert(r.sa.SelectActivity(0));
    r.Key(K_GO, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(Ran(r, SCRIPT) == 1);
    r.Advance(299);
    r.Drain();
    Assert(Ran(r, SCRIPT) == 1);
    r.Advance(1);
    r.Drain();
    Assert(Ran(r, SCRIPT) == 2);
    r.Advance(200);
    r.Drain();
    Assert(Ran(r, SCRIPT) == 3);
    r.Key(K_GO, TOUCH_KEY_RELEASE);
    r.Advance(1000);
    r.Drain();
    Assert(Ran(r, SCRIPT) == 3);
}

Fact("Standalone ControlTiming Reads The Selected Activity's Controls")
{
    Rig r;
    Assert(IsZero(r.sa.ControlTiming("key1")));         // no activity (Home has no controls table)
    Assert(r.sa.SelectActivity(0));                     // Watch: key1 repeats, delay 300, period 100
    GestureTiming t = r.sa.ControlTiming("key1");
    Assert(t.long_ms == 0 && t.repeat_delay_ms == 300 && t.repeat_ms == 100 && t.not_before_ms == 0);
    Assert(IsZero(r.sa.ControlTiming("key2")));
    Assert(IsZero(r.sa.ControlTiming("macro1")));
    Assert(IsZero(r.sa.ControlTiming("touch[400]")));
    Assert(r.sa.SelectActivity(1));                     // Listen: no controls
    Assert(IsZero(r.sa.ControlTiming("key1")));
    r.sa.SetHosted(true);                               // hosted: nothing
    Assert(IsZero(r.sa.ControlTiming("key1")));
}

// ---- switchActivity (the sample: Watch uses tv + avr and has a didActivate; Listen uses avr
// and has a willActivate delay; K_WATCH selects Watch, K_OFF selects none) ----

Fact("Standalone SwitchActivity From None Queues The Sequence And Commits")
{
    Rig r;
    r.Tap(K_WATCH);
    r.Drain();
    // The switch op, then turnOn tv (sendWol), turnOn avr (httpGet), Watch.didActivate (udpPacket)
    Assert(r.ran == 4 && r.run[0] == SWITCH && r.run[1] == WOL && r.run[2] == GET && r.run[3] == UDP);
    Assert(r.sa.Activity() == 0 && r.sa.DevicesOn() == (TV | AVR));
    Assert(r.n_applied == 1 && r.applied[0] == 0 && r.glides == 0);
    Assert(r.sa.Errors() == 0);

    // The activity's bindings are live: touch[16] (K_GO) runs a script
    r.ran = 0;
    r.Tap(K_GO);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == SCRIPT);
}

Fact("Standalone SwitchActivity To The Current Activity Only Glides")
{
    Rig r;
    r.Tap(K_WATCH);
    r.Drain();
    r.ran = 0;
    r.Tap(K_WATCH);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == SWITCH);               // no device or hook ops
    Assert(r.n_applied == 1 && r.glides == 1);
    Assert(r.sa.Activity() == 0 && r.sa.DevicesOn() == (TV | AVR));
}

Fact("Standalone SwitchActivity Between Activities Keeps A Shared Device On")
{
    Rig r;
    HomeSwitch(r.root, 1)->index = 1;                       // K_OFF now selects Listen
    r.Tap(K_WATCH);
    r.Settle();
    r.ran = 0;
    r.Tap(K_OFF);
    r.Settle();
    // Listen.willActivate (delay), then turnOff tv (httpPost); avr stays on
    Assert(r.ran == 3 && r.run[0] == SWITCH && r.run[1] == DELAY && r.run[2] == POST);
    Assert(r.sa.Activity() == 1 && r.sa.DevicesOn() == AVR);
    Assert(r.n_applied == 2 && r.applied[1] == 1);
}

Fact("Standalone SwitchActivity To None Turns Devices Off And Unbinds The Activity")
{
    Rig r;
    r.Tap(K_WATCH);
    r.Drain();
    r.ran = 0;
    r.Tap(K_OFF);
    r.Drain();
    Assert(r.ran == 2&& r.run[0] == SWITCH && r.run[1] == POST);      // tv off (avr has no turnOff list)
    Assert(r.sa.Activity() == -1 && r.sa.DevicesOn() == 0);
    Assert(r.n_applied == 2 && r.applied[1] == -1);
    r.ran = 0;
    r.Tap(K_GO);                                            // the activity's key is no longer bound
    r.Drain();
    Assert(r.ran == 0);

    // None again: nothing to do and nothing to show
    r.Tap(K_OFF);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == SWITCH);
    Assert(r.n_applied == 2 && r.glides == 0);
}

Fact("Standalone SwitchActivity Queued Back To Back Runs In Order")
{
    Rig r;
    r.Tap(K_WATCH);                                               // to Watch
    r.Tap(K_OFF);                                               // then none, before anything ran
    Assert(r.sa.Queued() == 2);
    r.Drain();
    Assert(r.ran == 6);
    // Each switch's ops run straight after it, so the first's are done before the
    // second switch starts; the second is planned from the state the first left
    const uint32_t want[6] = { SWITCH, WOL, GET, UDP, SWITCH, POST };
    for (int i = 0; i < 6; i++)
        Assert(r.run[i] == want[i]);
    Assert(r.sa.Activity() == -1 && r.sa.DevicesOn() == 0);
    Assert(r.n_applied == 2 && r.applied[0] == 0 && r.applied[1] == -1);
}

Fact("Standalone SwitchActivity Runs Its Ops Before The Rest Of Its List")
{
    Rig r(false);
    // K_WATCH now runs [switchActivity Watch, hidKey (K_SPACE's)]
    layoutdata::op *list[2] = { (layoutdata::op *)HomeSwitch(r.root, 0), r.root->homePage.bindings[0].ops[0] };
    Assert(list[1]->op == HID);
    layoutdata::binding *b = HomeSwitchBinding(r.root, 0);
    b->ops = list; b->ops_count = 2;
    r.Start();
    r.Tap(K_WATCH);
    r.Drain();
    // tv on, avr on, Watch.didActivate, and only then the op after the switch
    const uint32_t want[5] = { SWITCH, WOL, GET, UDP, HID };
    Assert(r.ran == 5);
    for (int i = 0; i < 5; i++)
        Assert(r.run[i] == want[i]);
    Assert(r.sa.Activity() == 0 && r.sa.Errors() == 0);
}

Fact("Standalone SwitchActivity Hook Delay Holds Back The Rest Of The List")
{
    Rig r(false);
    HomeSwitch(r.root, 1)->index = 1;                       // K_OFF selects Listen (willActivate: a delay)
    layoutdata::op *list[2] = { (layoutdata::op *)HomeSwitch(r.root, 1), r.root->homePage.bindings[0].ops[0] };
    layoutdata::binding *b = HomeSwitchBinding(r.root, 1);
    b->ops = list; b->ops_count = 2;
    r.Start();
    r.Tap(K_OFF);
    r.Drain();
    Assert(r.ran == 2 && r.run[0] == SWITCH && r.run[1] == DELAY);     // the hidKey waits for the delay
    r.Settle();
    const uint32_t want[4] = { SWITCH, DELAY, GET, HID };  // then avr on, then the hidKey
    Assert(r.ran == 4);
    for (int i = 0; i < 4; i++)
        Assert(r.run[i] == want[i]);
    Assert(r.sa.Activity() == 1 && r.sa.DevicesOn() == AVR);
}

Fact("Standalone Two Switches In One List Each Run Their Ops Before The Next")
{
    Rig r(false);
    // K_WATCH now runs [switchActivity Watch, switchActivity none, hidKey]
    layoutdata::op *list[3] = { (layoutdata::op *)HomeSwitch(r.root, 0), (layoutdata::op *)HomeSwitch(r.root, 1),
                                r.root->homePage.bindings[0].ops[0] };
    layoutdata::binding *b = HomeSwitchBinding(r.root, 0);
    b->ops = list; b->ops_count = 3;
    r.Start();
    r.Tap(K_WATCH);
    r.Drain();
    const uint32_t want[7] = { SWITCH, WOL, GET, UDP, SWITCH, POST, HID };
    Assert(r.ran == 7);
    for (int i = 0; i < 7; i++)
        Assert(r.run[i] == want[i]);
    Assert(r.sa.Activity() == -1 && r.sa.DevicesOn() == 0);
    Assert(r.n_applied == 2 && r.applied[0] == 0 && r.applied[1] == -1);
}

Fact("Standalone SwitchActivity Refused By A Full Queue Changes Nothing")
{
    Rig r;
    r.Tap(K_WATCH);                                               // the switch, first in the ring
    for (int i = 0; i < OP_QUEUE_SIZE - 1; i++)
        r.Tap(K_SPACE);                                           // OP_QUEUE_SIZE - 1 more lists: the ring is full
    r.Drain();
    // Watch from none needs 3 lists; the switch's own entry is out of the ring, OP_QUEUE_SIZE - 1 remain
    Assert(r.sa.Errors() == 1);
    Assert(r.sa.LastError() && strlen(r.sa.LastError()) > 0);
    Assert(r.sa.Activity() == -1 && r.sa.DevicesOn() == 0);
    Assert(r.n_applied == 0 && r.glides == 0);
    // The queued hid taps still ran, and none of the switch's lists did
    for (int i = 0; i < r.ran; i++)
        Assert(r.run[i] == SWITCH || r.run[i] == HID);
    // Activity bindings stayed off
    r.ran = 0;
    r.Tap(K_GO);
    r.Drain();
    Assert(r.ran == 0);
}

Fact("Standalone SwitchActivity Lets Go Of Held Keys")
{
    Rig r;
    r.Key(K_HOLD, TOUCH_KEY_PRESS);                              // shift+a held down
    r.Tap(K_WATCH);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));                  // down, then all up at the switch
    r.Key(K_HOLD, TOUCH_KEY_RELEASE);                            // the release is ignored: nothing sticks, nothing doubles
    r.Drain();
    Assert(r.sent == 2);
}

Fact("Standalone SwitchActivity Inside A Hook Only Queues And Cannot Recurse")
{
    Rig r(false);
    // Watch.didActivate is replaced by K_OFF's switch-to-none op
    r.root->activities[0].didActivate[0] = (layoutdata::op *)HomeSwitch(r.root, 1);
    r.Start();
    r.Tap(K_WATCH);
    r.Drain();
    // Watch: tv on, avr on, then the hook's switch (planned when it runs: Watch -> none, tv off),
    // whose ops run straight after it
    const uint32_t want[5] = { SWITCH, WOL, GET, SWITCH, POST };
    Assert(r.ran == 5);
    for (int i = 0; i < 5; i++)
        Assert(r.run[i] == want[i]);
    Assert(r.sa.Activity() == -1 && r.sa.DevicesOn() == 0);
    Assert(r.n_applied == 2);
}

Fact("Standalone Takeover Keeps The Device Mask But Not The Selection Or Queued Switch Ops")
{
    Rig r;
    r.Tap(K_WATCH);
    r.sa.Poll();                                            // the switch op runs; its device ops are queued
    Assert(r.sa.Activity() == 0 && r.sa.Queued() == 3);
    r.sa.SetHosted(true);
    Assert(r.sa.Queued() == 0 && r.sa.Activity() == -1);
    r.sa.SetHosted(false);
    Assert(r.sa.Activity() == -1 && r.sa.DevicesOn() == (TV | AVR));
    r.Drain();
    Assert(r.ran == 1);

    // Selecting Watch again only runs what is needed: both devices are assumed on
    r.ran = 0;
    r.Tap(K_WATCH);
    r.Drain();
    Assert(r.ran == 2 && r.run[0] == SWITCH && r.run[1] == UDP);
}

Fact("Standalone SwitchActivity Without An Activity Host Still Switches")
{
    Rig r(false);
    StandaloneHost h = {};
    h.ctx    = &r;
    h.now_ms = [](void *c) { return ((Rig *)c)->now; };
    r.sa.Init(h, r.root);
    r.Tap(K_WATCH);
    r.Drain();
    Assert(r.sa.Activity() == 0 && r.sa.DevicesOn() == (TV | AVR));
}

Fact("Standalone Press Of A Home Key Runs Its Op")
{
    Rig r;
    r.Key(K_SPACE, TOUCH_KEY_PRESS);              // tap with no `long` binding fires at once
    Assert(r.ran == 0);                     // queued, not run, until polled
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);
    r.Key(K_SPACE, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1);                     // the release adds nothing
}

Fact("Standalone Network Ops Reach The Net Host And Their Errors Are Counted")
{
    FakeNet fake;
    Rig r(false);
    r.net = &fake.host;
    // Home's first binding (touch[1] tap) now runs the sample's didActivate udpPacket
    layoutdata::binding *b = &r.root->homePage.bindings[0];
    b->ops[0] = r.root->activities[0].didActivate[0];
    r.Start();
    Assert(r.sa.Errors() == 0 && r.sa.LastError() == nullptr);

    r.Key(K_SPACE, TOUCH_KEY_PRESS);
    r.Drain();
    r.Key(K_SPACE, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(fake.n_sent == 1 && fake.sent[0].kind == NetKind::Udp && fake.sent[0].port == 9);
    Assert(r.sa.Errors() == 0);

    // Wi-Fi down: the op is dropped, counted and says why
    fake.is_connected = false;
    r.Key(K_SPACE, TOUCH_KEY_PRESS);
    r.Drain();
    r.Key(K_SPACE, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(fake.n_sent == 1);
    Assert(r.sa.Errors() == 1);
    Assert(r.sa.LastError() && strcmp(r.sa.LastError(), "wi-fi not connected") == 0);
}

Fact("Standalone Key Without A Binding Does Nothing")
{
    Rig r;
    r.Key(K_RED, TOUCH_KEY_PRESS);              // touch[5]: not bound
    r.Key(K_RED, TOUCH_KEY_RELEASE);
    r.Key(K_GO, TOUCH_KEY_PRESS);           // an activity key: no activity selected
    r.Key(K_GO, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 0);
}

Fact("Standalone Long Binding Defers Tap")
{
    Rig r;
    r.Key(K_NEXT, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.ran == 0);                     // tap waits for the release

    // A short press: tap on release
    r.Advance(100);
    r.Key(K_NEXT, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);

    // A long press: `long` at the threshold (the layout's 650 ms), and no tap
    r.ran = 0;
    r.Key(K_NEXT, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS - 1);
    r.Drain();
    Assert(r.ran == 0);
    r.Advance(1);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);
    r.Key(K_NEXT, TOUCH_KEY_RELEASE);
    r.Drain();
    r.now += 1000;                          // let the delay pass
    r.Drain();
    Assert(r.ran == 1);
}

Fact("Standalone Key's Own Long Press Time Overrides The Layout's")
{
    Rig r;
    // K_WIDE has its own longPressMs (900) in the sample; its tap becomes a long binding
    // (a plain value: fine in the relocated blob)
    layoutdata::binding *b = FindBinding(r.root->homePage.bindings, r.root->homePage.bindings_count, "touch[11]");
    Assert(b && strcmp(b->event, "tap") == 0);
    b->event = "long";
    bool wide = false;
    for (uint32_t i = 0; i < r.root->homePage.keys_count; i++)
        wide = wide || (r.root->homePage.keys[i].id == K_WIDE && r.root->homePage.keys[i].longPressMs == 900);
    Assert(wide);
    r.Start();
    r.Key(K_WIDE, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS);                     // the layout's time has passed: not long yet
    r.Drain();
    Assert(r.ran == 0);
    r.Advance(900 - LONG_MS);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);
}

Fact("Standalone Key's Own Long Press Time Is Found On The Selected Activity's Pages")
{
    Rig r(false);
    // K_GO (Watch's page) gets its own 900 ms and its tap becomes a long binding
    layoutdata::activity &watch = r.root->activities[0];
    layoutdata::binding *b = FindBinding(watch.bindings, watch.bindings_count, "touch[16]");
    Assert(b);
    b->event = "long";
    Assert(watch.pages[0].keys[0].id == K_GO);
    watch.pages[0].keys[0].longPressMs = 900;
    r.Start();
    Assert(r.sa.SelectActivity(0));
    r.Key(K_GO, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS);
    r.Drain();
    Assert(r.ran == 0);
    r.Advance(900 - LONG_MS);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == SCRIPT);
}

Fact("Standalone Span And Fractional Keys Run Their Bindings By Id")
{
    Rig r;
    r.Tap(K_WIDE);                          // 2U
    r.Tap(K_R);                             // 0.5U at a fractional corner: an assigned id
    r.Tap(14);                              // the Vol group's members: touch[14] up, touch[15] down
    r.Tap(15);
    r.Settle();                             // each hid tap holds the queue for its gap
    Assert(r.ran == 4);
    for (int i = 0; i < 4; i++)
        Assert(r.run[i] == HID);
    // An id that isn't in the layout does nothing
    r.ran = 0;
    r.Tap(999);
    r.Drain();
    Assert(r.ran == 0);
}

Fact("Standalone Touch That Becomes A Swipe Fires No Tap And Frees The Key")
{
    Rig r;
    r.Key(K_NEXT, TOUCH_KEY_PRESS);
    r.Advance(100);
    r.Key(K_NEXT, TOUCH_KEY_CANCEL);
    r.Advance(5000);                        // nothing is left held: no `long`
    r.Drain();
    Assert(r.ran == 0);

    r.Key(K_NEXT, TOUCH_KEY_RELEASE);            // a stray release does nothing either
    r.Drain();
    Assert(r.ran == 0);

    r.Key(K_NEXT, TOUCH_KEY_PRESS);              // and the key can be pressed again
    r.Key(K_NEXT, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);
}

Fact("Standalone Fires Nothing While Hosted")
{
    Rig r;
    r.sa.SetHosted(true);
    Assert(r.sa.Hosted());
    r.Key(K_SPACE, TOUCH_KEY_PRESS);
    r.Key(K_SPACE, TOUCH_KEY_RELEASE);
    r.Key(K_NEXT, TOUCH_KEY_PRESS);
    r.Advance(5000);
    r.Key(K_NEXT, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 0 && r.sa.Queued() == 0);
}

Fact("Standalone Takeover Cancels Held Keys And Queued Ops")
{
    Rig r;
    r.Key(K_NEXT, TOUCH_KEY_PRESS);              // held, tap deferred
    r.Key(K_SPACE, TOUCH_KEY_PRESS);              // an op queued, not yet run
    Assert(r.sa.Queued() == 1);

    r.sa.SetHosted(true);
    Assert(r.sa.Queued() == 0);
    r.Drain();
    Assert(r.ran == 0);

    // Back to standalone: the key that was held is not held any more
    r.sa.SetHosted(false);
    r.Advance(5000);                        // no `long`
    r.Key(K_NEXT, TOUCH_KEY_RELEASE);            // no tap
    r.Drain();
    Assert(r.ran == 0);
}

Fact("Standalone Takeover Cancels A Pending Delay And The Ops Behind It")
{
    Rig r;
    r.Key(K_NEXT, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS);                     // `long`: a delay op
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);    // started, and holding the queue
    r.Key(K_SPACE, TOUCH_KEY_PRESS);              // hidKey queued behind the delay
    Assert(r.sa.Queued() == 1);
    Assert(r.sa.Poll());                    // the delay is still running

    r.sa.SetHosted(true);
    Assert(r.sa.Queued() == 0);

    r.sa.SetHosted(false);                  // back: the delay is gone, not just paused
    r.now += 5000;
    r.Drain();
    Assert(r.ran == 1);                     // the hidKey never ran
    Assert(!r.sa.Poll());
    r.Key(K_SPACE, TOUCH_KEY_PRESS);              // and the queue works again at once
    r.Drain();
    Assert(r.ran == 2 && r.run[1] == HID);
}

Fact("Standalone Release Re-arms From The Blob")
{
    Rig r;
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    Assert(!r.sa.Hosted());

    r.Key(K_SPACE, TOUCH_KEY_PRESS);              // Home's bindings work again
    r.Key(K_SPACE, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);

    // The long-press time is read again too
    r.ran = 0;
    r.Key(K_NEXT, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);
}

Fact("Standalone Release Selects No Activity")
{
    Rig r;
    r.sa.SelectActivity(0);                 // Watch: touch[16] is bound
    r.Key(K_GO, TOUCH_KEY_PRESS);
    r.Key(K_GO, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1);                     // its script op

    r.ran = 0;
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    r.Key(K_GO, TOUCH_KEY_PRESS);
    r.Key(K_GO, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 0);
}

// Home keys: K_SPACE space, K_COPY ctrl+c (tap), K_HOLD shift+a held while pressed
Fact("Standalone Hid Key Press Sends The Chord Then Lets Go")
{
    Rig r;
    r.Key(K_COPY, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.sent == 1 && r.LastIs(0x01, 0x06));        // ctrl+c down
    r.Advance(HID_TAP_GAP_MS - 1);
    r.Drain();
    Assert(r.sent == 1);
    r.Advance(1);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));              // up, from Poll
}

Fact("Standalone Taps Complete In Turn With A Settle Gap")
{
    Rig r;
    r.Key(K_SPACE, TOUCH_KEY_PRESS);                          // space
    r.Key(K_COPY, TOUCH_KEY_PRESS);                          // ctrl+c, queued behind it
    r.Drain();
    Assert(r.sent == 1 && r.sa.Queued() == 1);             // the second waits
    r.Advance(HID_TAP_GAP_MS + HID_TAP_SETTLE_MS - 1);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));              // up sent, next not started
    r.Advance(1);
    r.Drain();
    Assert(r.sent == 3 && r.LastIs(0x01, 0x06));
    r.Advance(HID_TAP_GAP_MS);
    r.Drain();
    Assert(r.sent == 4 && r.LastIs(0, 0));
}

Fact("Standalone Hold Key Is Down While Pressed And Up On Release")
{
    Rig r;
    r.Key(K_HOLD, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.sent == 1 && r.LastIs(0x02, 0x04));        // shift+a
    r.Advance(5000);
    r.Drain();
    Assert(r.sent == 1);
    r.Key(K_HOLD, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));
}

Fact("Standalone Hold Key That Becomes A Swipe Is Let Go")
{
    Rig r;
    r.Key(K_HOLD, TOUCH_KEY_PRESS);
    r.Drain();
    r.Key(K_HOLD, TOUCH_KEY_CANCEL);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));
}

Fact("Standalone Takeover Lets Go Of Held Keys And A Tap In Flight")
{
    Rig r;
    r.Key(K_HOLD, TOUCH_KEY_PRESS);
    r.Key(K_COPY, TOUCH_KEY_PRESS);
    r.Drain();                                          // shift+a down, then ctrl+c down too
    Assert(r.sent == 2);
    r.sa.SetHosted(true);
    Assert(r.sent == 3 && r.LastIs(0, 0));              // all up at once
    r.Advance(5000);
    r.Drain();
    Assert(r.sent == 3);                                // nothing more, not even the tap's up
}

Fact("Standalone Disconnect While Already Standalone Lets Go Of Held Keys")
{
    Rig r;
    r.Key(K_HOLD, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.sent == 1);
    r.sa.SetHosted(false);                              // the protocol's Release() on an unplug
    Assert(r.sent == 2 && r.LastIs(0, 0));
}

Fact("Standalone Release Of A Hold Whose Press Never Ran Sends Nothing")
{
    Rig r;
    r.Key(K_HOLD, TOUCH_KEY_PRESS);
    r.sa.SetHosted(true);                               // the press was queued, then dropped
    r.sa.SetHosted(false);
    r.Key(K_HOLD, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.sent == 0);
}

Fact("Standalone Without A Layout Ignores Everything")
{
    Rig r(false);
    StandaloneHost h = {};
    h.ctx    = &r;
    h.now_ms = [](void *c) { return ((Rig *)c)->now; };
    h.run    = [](const layoutdata::op *, uint32_t, void *c) { ((Rig *)c)->ran++; };
    r.sa.Init(h, nullptr);
    r.Key(K_SPACE, TOUCH_KEY_PRESS);
    r.Key(K_SPACE, TOUCH_KEY_RELEASE);
    r.Advance(5000);
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    r.Key(K_SPACE, TOUCH_KEY_PRESS);
    Assert(!r.sa.Poll());
    Assert(r.ran == 0);
}

Fact("Standalone Script Op Runs The Activity's Script With Its Param")
{
    Rig r(false);
    r.scripts = true;
    r.Start();
    r.Tap(K_WATCH);                                               // select Watch
    r.Drain();
    r.Tap(K_GO);                                            // its key: script `return param`
    r.Drain();
    Assert(r.sa.Scripts().Runs() == 1 && r.sa.Scripts().Errors() == 0);
    Assert(r.sa.Errors() == 0);
}

Fact("Standalone Script Op Does Nothing Without A Lua Heap")
{
    Rig r;
    r.Tap(K_WATCH);
    r.Drain();
    r.Tap(K_GO);
    r.Drain();
    Assert(r.sa.Scripts().Runs() == 0 && r.sa.Errors() == 0);
}

Fact("Standalone Takeover Frees The Lua State")
{
    Rig r(false);
    r.scripts = true;
    r.Start();
    r.Tap(K_WATCH);
    r.Drain();
    r.Tap(K_GO);
    r.Drain();
    Assert(r.sa.Scripts().HeapUsed() > 0);
    r.sa.SetHosted(true);
    Assert(r.sa.Scripts().HeapUsed() == 0);
}

// A script waits for its switch's ops, so a switch that would run a script (a second
// one: there is one Lua state) is refused from a script; from an op it is fine
Fact("Standalone SwitchActivity From A Script Is Refused If The Switch Runs A Script")
{
    Rig r(false);
    r.scripts = true;
    layoutdata::scriptOp hook = {};                         // Watch.didActivate becomes a script
    hook.base.op = OP_SCRIPT; hook.source = "log('hook')"; hook.source_count = 11;
    r.root->activities[0].didActivate[0] = &hook.base;
    layoutdata::scriptOp sw = {};                           // K_WATCH runs a script that selects Watch
    const char *src = "local ok, why = switchActivity('Watch') log(tostring(ok), why)";
    sw.base.op = OP_SCRIPT; sw.source = src; sw.source_count = (uint32_t)strlen(src);
    layoutdata::op *list[1] = { &sw.base };
    layoutdata::binding *b = HomeSwitchBinding(r.root, 0);
    b->ops = list; b->ops_count = 1;
    r.Start();
    r.Tap(K_WATCH);
    r.Settle();
    Assert(strstr(r.script_log, "script: switchActivity refused: the switch to Watch runs a script|") != nullptr);
    Assert(strstr(r.script_log, "nil\tswitchActivity: the switch runs a script|") != nullptr);
    Assert(!strstr(r.script_log, "hook"));
    // Nothing changed or ran but the script and its switch call (reported before the script,
    // which runs it from its start)
    Assert(r.ran == 2 && r.run[0] == SWITCH && r.run[1] == SCRIPT);
    Assert(r.sa.Activity() == -1 && r.sa.DevicesOn() == 0 && r.n_applied == 0);
    Assert(r.sa.Errors() == 0 && r.sa.Scripts().Errors() == 0);
}

Fact("Standalone SwitchActivity From An Op May Run A Script")
{
    Rig r(false);
    r.scripts = true;
    layoutdata::scriptOp hook = {};
    hook.base.op = OP_SCRIPT; hook.source = "log('hook')"; hook.source_count = 11;
    r.root->activities[0].didActivate[0] = &hook.base;
    r.Start();
    r.Tap(K_WATCH);
    r.Settle();
    const uint32_t want[4] = { SWITCH, WOL, GET, SCRIPT };
    Assert(r.ran == 4);
    for (int i = 0; i < 4; i++)
        Assert(r.run[i] == want[i]);
    Assert(strcmp(r.script_log, "hook|") == 0);
    Assert(r.sa.Activity() == 0 && r.sa.Errors() == 0);
}
