// The standalone owner (docs/standalone.md milestone step 2): router key events
// go through the gestures and bindings to the op queue, and the host's
// take-over / release switches it off and re-arms it. The layout is the
// sample baked by host/src/bake (longPressMs 650). Home: touch[0,0] tap (hidKey),
// touch[1,0] tap (hidKey) + long (delay), touch[2,0] tap (switchActivity).

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
        void Key(int key, int action, int page = 0) { sa.Key(page, key, action, now); }
        void Tap(int key, int page = 0) { Key(key, TOUCH_KEY_PRESS, page); Key(key, TOUCH_KEY_RELEASE, page); }
        // Run everything, letting delays pass
        void Settle() { for (int i = 0; i < 50; i++) { Drain(); now += 100; } }
    };

    // The nth Home binding that starts with a switchActivity op (key 2 is the first: Watch, key 9 the second: none)
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
}

// ---- switchActivity (the sample: Watch uses tv + avr and has a didActivate; Listen uses avr
// and has a willActivate delay; Home key 2 selects Watch, key 9 selects none) ----

Fact("Standalone SwitchActivity From None Queues The Sequence And Commits")
{
    Rig r;
    r.Tap(2);
    r.Drain();
    // The switch op, then turnOn tv (sendWol), turnOn avr (httpGet), Watch.didActivate (udpPacket)
    Assert(r.ran == 4 && r.run[0] == SWITCH && r.run[1] == WOL && r.run[2] == GET && r.run[3] == UDP);
    Assert(r.sa.Activity() == 0 && r.sa.DevicesOn() == (TV | AVR));
    Assert(r.n_applied == 1 && r.applied[0] == 0 && r.glides == 0);
    Assert(r.sa.Errors() == 0);

    // The activity's bindings are live: page 1 key 0 is touch[5,0] (script)
    r.ran = 0;
    r.Tap(0, 1);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == SCRIPT);
}

Fact("Standalone SwitchActivity To The Current Activity Only Glides")
{
    Rig r;
    r.Tap(2);
    r.Drain();
    r.ran = 0;
    r.Tap(2);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == SWITCH);               // no device or hook ops
    Assert(r.n_applied == 1 && r.glides == 1);
    Assert(r.sa.Activity() == 0 && r.sa.DevicesOn() == (TV | AVR));
}

Fact("Standalone SwitchActivity Between Activities Keeps A Shared Device On")
{
    Rig r;
    HomeSwitch(r.root, 1)->index = 1;                       // key 9 now selects Listen
    r.Tap(2);
    r.Settle();
    r.ran = 0;
    r.Tap(9);
    r.Settle();
    // Listen.willActivate (delay), then turnOff tv (httpPost); avr stays on
    Assert(r.ran == 3 && r.run[0] == SWITCH && r.run[1] == DELAY && r.run[2] == POST);
    Assert(r.sa.Activity() == 1 && r.sa.DevicesOn() == AVR);
    Assert(r.n_applied == 2 && r.applied[1] == 1);
}

Fact("Standalone SwitchActivity To None Turns Devices Off And Unbinds The Activity")
{
    Rig r;
    r.Tap(2);
    r.Drain();
    r.ran = 0;
    r.Tap(9);
    r.Drain();
    Assert(r.ran == 2&& r.run[0] == SWITCH && r.run[1] == POST);      // tv off (avr has no turnOff list)
    Assert(r.sa.Activity() == -1 && r.sa.DevicesOn() == 0);
    Assert(r.n_applied == 2 && r.applied[1] == -1);
    r.ran = 0;
    r.Tap(0, 1);                                            // the activity's key is no longer bound
    r.Drain();
    Assert(r.ran == 0);

    // None again: nothing to do and nothing to show
    r.Tap(9);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == SWITCH);
    Assert(r.n_applied == 2 && r.glides == 0);
}

Fact("Standalone SwitchActivity Queued Back To Back Runs In Order")
{
    Rig r;
    r.Tap(2);                                               // to Watch
    r.Tap(9);                                               // then none, before anything ran
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
    // Home key 2 now runs [switchActivity Watch, hidKey (Home key 0's)]
    layoutdata::op *list[2] = { (layoutdata::op *)HomeSwitch(r.root, 0), r.root->homePage.bindings[0].ops[0] };
    Assert(list[1]->op == HID);
    layoutdata::binding *b = HomeSwitchBinding(r.root, 0);
    b->ops = list; b->ops_count = 2;
    r.Start();
    r.Tap(2);
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
    HomeSwitch(r.root, 1)->index = 1;                       // key 9 selects Listen (willActivate: a delay)
    layoutdata::op *list[2] = { (layoutdata::op *)HomeSwitch(r.root, 1), r.root->homePage.bindings[0].ops[0] };
    layoutdata::binding *b = HomeSwitchBinding(r.root, 1);
    b->ops = list; b->ops_count = 2;
    r.Start();
    r.Tap(9);
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
    // Home key 2 now runs [switchActivity Watch, switchActivity none, hidKey]
    layoutdata::op *list[3] = { (layoutdata::op *)HomeSwitch(r.root, 0), (layoutdata::op *)HomeSwitch(r.root, 1),
                                r.root->homePage.bindings[0].ops[0] };
    layoutdata::binding *b = HomeSwitchBinding(r.root, 0);
    b->ops = list; b->ops_count = 3;
    r.Start();
    r.Tap(2);
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
    r.Tap(2);                                               // the switch, first in the ring
    for (int i = 0; i < OP_QUEUE_SIZE - 1; i++)
        r.Tap(0);                                           // OP_QUEUE_SIZE - 1 more lists: the ring is full
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
    r.Tap(0, 1);
    r.Drain();
    Assert(r.ran == 0);
}

Fact("Standalone SwitchActivity Lets Go Of Held Keys")
{
    Rig r;
    r.Key(7, TOUCH_KEY_PRESS);                              // shift+a held down
    r.Tap(2);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));                  // down, then all up at the switch
    r.Key(7, TOUCH_KEY_RELEASE);                            // the release is ignored: nothing sticks, nothing doubles
    r.Drain();
    Assert(r.sent == 2);
}

Fact("Standalone SwitchActivity Inside A Hook Only Queues And Cannot Recurse")
{
    Rig r(false);
    // Watch.didActivate is replaced by Home key 9's switch-to-none op
    r.root->activities[0].didActivate[0] = (layoutdata::op *)HomeSwitch(r.root, 1);
    r.Start();
    r.Tap(2);
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
    r.Tap(2);
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
    r.Tap(2);
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
    r.Tap(2);
    r.Drain();
    Assert(r.sa.Activity() == 0 && r.sa.DevicesOn() == (TV | AVR));
}

Fact("Standalone Press Of A Home Key Runs Its Op")
{
    Rig r;
    r.Key(0, TOUCH_KEY_PRESS);              // tap with no `long` binding fires at once
    Assert(r.ran == 0);                     // queued, not run, until polled
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);
    r.Key(0, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1);                     // the release adds nothing
}

Fact("Standalone Network Ops Reach The Net Host And Their Errors Are Counted")
{
    FakeNet fake;
    Rig r(false);
    r.net = &fake.host;
    // Home's first binding (touch[0,0] tap) now runs the sample's didActivate udpPacket
    layoutdata::binding *b = &r.root->homePage.bindings[0];
    b->ops[0] = r.root->activities[0].didActivate[0];
    r.Start();
    Assert(r.sa.Errors() == 0 && r.sa.LastError() == nullptr);

    r.Key(0, TOUCH_KEY_PRESS);
    r.Drain();
    r.Key(0, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(fake.n_sent == 1 && fake.sent[0].kind == NetKind::Udp && fake.sent[0].port == 9);
    Assert(r.sa.Errors() == 0);

    // Wi-Fi down: the op is dropped, counted and says why
    fake.is_connected = false;
    r.Key(0, TOUCH_KEY_PRESS);
    r.Drain();
    r.Key(0, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(fake.n_sent == 1);
    Assert(r.sa.Errors() == 1);
    Assert(r.sa.LastError() && strcmp(r.sa.LastError(), "wi-fi not connected") == 0);
}

Fact("Standalone Key Without A Binding Does Nothing")
{
    Rig r;
    r.Key(4, TOUCH_KEY_PRESS);              // touch[4,0]: not bound
    r.Key(4, TOUCH_KEY_RELEASE);
    r.Key(0, TOUCH_KEY_PRESS, 1);           // page 1: no activity selected
    r.Key(0, TOUCH_KEY_RELEASE, 1);
    r.Drain();
    Assert(r.ran == 0);
}

Fact("Standalone Long Binding Defers Tap")
{
    Rig r;
    r.Key(1, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.ran == 0);                     // tap waits for the release

    // A short press: tap on release
    r.Advance(100);
    r.Key(1, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);

    // A long press: `long` at the threshold (the layout's 650 ms), and no tap
    r.ran = 0;
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS - 1);
    r.Drain();
    Assert(r.ran == 0);
    r.Advance(1);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);
    r.Key(1, TOUCH_KEY_RELEASE);
    r.Drain();
    r.now += 1000;                          // let the delay pass
    r.Drain();
    Assert(r.ran == 1);
}

Fact("Standalone Key's Own Long Press Time Overrides The Layout's")
{
    Rig r;
    // Home key 1's own longPressMs set to 900 (a plain value: fine in the relocated blob)
    Put32(r.bytes, CB_HOME_KEY(1) + CB_OFF(key, longPressMs), 900);
    r.Start();
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS);                     // the layout's time has passed: not long yet
    r.Drain();
    Assert(r.ran == 0);
    r.Advance(900 - LONG_MS);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);
}

Fact("Standalone Touch That Becomes A Swipe Fires No Tap And Frees The Key")
{
    Rig r;
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(100);
    r.Key(1, TOUCH_KEY_CANCEL);
    r.Advance(5000);                        // nothing is left held: no `long`
    r.Drain();
    Assert(r.ran == 0);

    r.Key(1, TOUCH_KEY_RELEASE);            // a stray release does nothing either
    r.Drain();
    Assert(r.ran == 0);

    r.Key(1, TOUCH_KEY_PRESS);              // and the key can be pressed again
    r.Key(1, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);
}

Fact("Standalone Fires Nothing While Hosted")
{
    Rig r;
    r.sa.SetHosted(true);
    Assert(r.sa.Hosted());
    r.Key(0, TOUCH_KEY_PRESS);
    r.Key(0, TOUCH_KEY_RELEASE);
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(5000);
    r.Key(1, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 0 && r.sa.Queued() == 0);
}

Fact("Standalone Takeover Cancels Held Keys And Queued Ops")
{
    Rig r;
    r.Key(1, TOUCH_KEY_PRESS);              // held, tap deferred
    r.Key(0, TOUCH_KEY_PRESS);              // an op queued, not yet run
    Assert(r.sa.Queued() == 1);

    r.sa.SetHosted(true);
    Assert(r.sa.Queued() == 0);
    r.Drain();
    Assert(r.ran == 0);

    // Back to standalone: the key that was held is not held any more
    r.sa.SetHosted(false);
    r.Advance(5000);                        // no `long`
    r.Key(1, TOUCH_KEY_RELEASE);            // no tap
    r.Drain();
    Assert(r.ran == 0);
}

Fact("Standalone Takeover Cancels A Pending Delay And The Ops Behind It")
{
    Rig r;
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS);                     // `long`: a delay op
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);    // started, and holding the queue
    r.Key(0, TOUCH_KEY_PRESS);              // hidKey queued behind the delay
    Assert(r.sa.Queued() == 1);
    Assert(r.sa.Poll());                    // the delay is still running

    r.sa.SetHosted(true);
    Assert(r.sa.Queued() == 0);

    r.sa.SetHosted(false);                  // back: the delay is gone, not just paused
    r.now += 5000;
    r.Drain();
    Assert(r.ran == 1);                     // the hidKey never ran
    Assert(!r.sa.Poll());
    r.Key(0, TOUCH_KEY_PRESS);              // and the queue works again at once
    r.Drain();
    Assert(r.ran == 2 && r.run[1] == HID);
}

Fact("Standalone Release Re-arms From The Blob")
{
    Rig r;
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    Assert(!r.sa.Hosted());

    r.Key(0, TOUCH_KEY_PRESS);              // Home's bindings work again
    r.Key(0, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);

    // The long-press time is read again too
    r.ran = 0;
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);
}

Fact("Standalone Release Selects No Activity")
{
    Rig r;
    r.sa.SelectActivity(0);                 // Watch: touch[5,0] is bound
    r.Key(0, TOUCH_KEY_PRESS, 1);
    r.Key(0, TOUCH_KEY_RELEASE, 1);
    r.Drain();
    Assert(r.ran == 1);                     // its script op

    r.ran = 0;
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    r.Key(0, TOUCH_KEY_PRESS, 1);
    r.Key(0, TOUCH_KEY_RELEASE, 1);
    r.Drain();
    Assert(r.ran == 0);
}

// Home keys: 0 space, 6 ctrl+c (tap), 7 shift+a held while pressed
Fact("Standalone Hid Key Press Sends The Chord Then Lets Go")
{
    Rig r;
    r.Key(6, TOUCH_KEY_PRESS);
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
    r.Key(0, TOUCH_KEY_PRESS);                          // space
    r.Key(6, TOUCH_KEY_PRESS);                          // ctrl+c, queued behind it
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
    r.Key(7, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.sent == 1 && r.LastIs(0x02, 0x04));        // shift+a
    r.Advance(5000);
    r.Drain();
    Assert(r.sent == 1);
    r.Key(7, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));
}

Fact("Standalone Hold Key That Becomes A Swipe Is Let Go")
{
    Rig r;
    r.Key(7, TOUCH_KEY_PRESS);
    r.Drain();
    r.Key(7, TOUCH_KEY_CANCEL);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));
}

Fact("Standalone Takeover Lets Go Of Held Keys And A Tap In Flight")
{
    Rig r;
    r.Key(7, TOUCH_KEY_PRESS);
    r.Key(6, TOUCH_KEY_PRESS);
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
    r.Key(7, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.sent == 1);
    r.sa.SetHosted(false);                              // the protocol's Release() on an unplug
    Assert(r.sent == 2 && r.LastIs(0, 0));
}

Fact("Standalone Release Of A Hold Whose Press Never Ran Sends Nothing")
{
    Rig r;
    r.Key(7, TOUCH_KEY_PRESS);
    r.sa.SetHosted(true);                               // the press was queued, then dropped
    r.sa.SetHosted(false);
    r.Key(7, TOUCH_KEY_RELEASE);
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
    r.Key(0, TOUCH_KEY_PRESS);
    r.Key(0, TOUCH_KEY_RELEASE);
    r.Advance(5000);
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    r.Key(0, TOUCH_KEY_PRESS);
    Assert(!r.sa.Poll());
    Assert(r.ran == 0);
}

Fact("Standalone Script Op Runs The Activity's Script With Its Param")
{
    Rig r(false);
    r.scripts = true;
    r.Start();
    r.Tap(2);                                               // select Watch
    r.Drain();
    r.Tap(0, 1);                                            // its page 1 key 0: script `return param`
    r.Drain();
    Assert(r.sa.Scripts().Runs() == 1 && r.sa.Scripts().Errors() == 0);
    Assert(r.sa.Errors() == 0);
}

Fact("Standalone Script Op Does Nothing Without A Lua Heap")
{
    Rig r;
    r.Tap(2);
    r.Drain();
    r.Tap(0, 1);
    r.Drain();
    Assert(r.sa.Scripts().Runs() == 0 && r.sa.Errors() == 0);
}

Fact("Standalone Takeover Frees The Lua State")
{
    Rig r(false);
    r.scripts = true;
    r.Start();
    r.Tap(2);
    r.Drain();
    r.Tap(0, 1);
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
    layoutdata::scriptOp sw = {};                           // Home key 2 runs a script that selects Watch
    const char *src = "local ok, why = switchActivity('Watch') log(tostring(ok), why)";
    sw.base.op = OP_SCRIPT; sw.source = src; sw.source_count = (uint32_t)strlen(src);
    layoutdata::op *list[1] = { &sw.base };
    layoutdata::binding *b = HomeSwitchBinding(r.root, 0);
    b->ops = list; b->ops_count = 1;
    r.Start();
    r.Tap(2);
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
    r.Tap(2);
    r.Settle();
    const uint32_t want[4] = { SWITCH, WOL, GET, SCRIPT };
    Assert(r.ran == 4);
    for (int i = 0; i < 4; i++)
        Assert(r.run[i] == want[i]);
    Assert(strcmp(r.script_log, "hook|") == 0);
    Assert(r.sa.Activity() == 0 && r.sa.Errors() == 0);
}
