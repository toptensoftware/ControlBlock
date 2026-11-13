// The C structs generated from the binpack schema (tools/codegen/gen-types.js ->
// main/layout_types.gen.h, wrapped by layout_types.h). Checks that they read the
// sample blob the real packer wrote, both as baked (pointers are file offsets)
// and relocated (pointers are addresses). The host tests are 32-bit so the
// packed structs, with their 32-bit pointers, match the device exactly.

#include <stddef.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "layout_env.h"

using namespace SimpleLib;
using namespace layout_env;

Fact("LayoutTypes Read The Sample Blob As Baked")
{
    // Pointers are file offsets until relocated; counts and values read directly
    List<uint8_t> b = LoadSample();
    const layoutdata::layoutRoot *root = LayoutRoot(b.GetBuffer());
    Assert((const uint8_t *)root == b.GetBuffer() + CB_ROOT);
    Assert(root->longPressMs == 650);
    Assert(root->devices_count == 2);
    Assert(root->activities_count == 2);
    Assert(root->homePage.bindings_count == U32(b, CB_HOME_BINDINGS));
    Assert(root->homePage.keys_count == U32(b, CB_HOME_KEYS));
    // Home's keys are an array of structs behind a pointer (a file offset here)
    const layoutdata::key *k1 = (const layoutdata::key *)(b.GetBuffer() + (uint32_t)(uintptr_t)root->homePage.keys) + 1;
    Assert((const uint8_t *)k1 == b.GetBuffer() + HomeKey(b, 1));
    Assert(k1->normal_count == U32(b, CB_KEY_COUNT(HomeKey(b, 1), LAYOUT_KEY_NORMAL)));
    Assert(k1->id == 2);
    // Key 2 doesn't repeat (0 = off); the repeating key (id 400, `repeat: 100`) after it does
    Assert(k1->repeatDelayMs == 0 && k1->repeatMs == 0);
    const layoutdata::key *k3 = k1 + 2;
    Assert(k3->id == 400 && k3->repeatDelayMs == 100 && k3->repeatMs == 100 && k3->longPressMs == 0);
    // Watch's control timings: key1 alone
    const layoutdata::activity *a0 = (const layoutdata::activity *)(b.GetBuffer() + (uint32_t)(uintptr_t)root->activities);
    Assert(a0->controls_count == 1);
    Assert(a0[1].controls_count == 0);
}

Fact("LayoutTypes Read The Relocated Sample Through The Structs")
{
    List<uint8_t> b = LoadSample();
    const layoutdata::layoutRoot *root = Relocate(b);

    // Home's bindings: touch[1] (the key with id 1) tap runs a hidKey
    const layoutdata::binding &first = root->homePage.bindings[0];
    Assert(strcmp(first.control, "touch[1]") == 0);
    Assert(strcmp(first.event, "tap") == 0);
    Assert(first.ops_count == 1);
    Assert(first.ops[0]->op == OP_HID_KEY);
    const layoutdata::hidKeyOp *hid = (const layoutdata::hidKeyOp *)first.ops[0];
    Assert(hid->mode <= HID_MODE_MAX);

    // Keys: id, rect (x, y, w, h in quarter units) and images inside the blob
    const layoutdata::key &k0 = root->homePage.keys[0];
    Assert(k0.id == 1);
    Assert(k0.rect[0] == 0 && k0.rect[1] == 0 && k0.rect[2] == 4 && k0.rect[3] == 4);
    KeyRect r = LayoutKeyRect(k0.rect);
    Assert(r.x == 0 && r.y == 0 && r.w == 4 && r.h == 4);
    Assert(k0.normal_count > 0 && k0.pressed_count > 0);
    Assert(k0.normal >= b.GetBuffer() && k0.normal + k0.normal_count <= b.GetBuffer() + b.GetCount());

    // An activity: its name, its pages' keys and a binding
    Assert(strcmp(root->activities[0].name, "Watch") == 0);
    Assert(strcmp(root->activities[0].displayName, "Watch TV") == 0);
    Assert(root->activities[0].pages_count == 2 && root->activities[0].pages[0].keys_count == 1);
    Assert(root->activities[0].pages[0].keys[0].id == 16 && root->activities[0].pages[1].keys[0].id == 31);
    // touch[16] tap (the script), macro1 tap (a hidKey) and key1 press (a script)
    Assert(root->activities[0].bindings_count == 3);
    bool macro = false;
    for (uint32_t i = 0; i < root->activities[0].bindings_count; i++)
        macro = macro || strcmp(root->activities[0].bindings[i].control, "macro1") == 0;
    Assert(macro);
    Assert(root->devices_count == 2 && strcmp(root->devices[0].name, "tv") == 0);

    // Watch's control timing for the pad's key1: `repeat: { initialDelay: 300, period: 100 }`
    const layoutdata::controlTiming &c = root->activities[0].controls[0];
    Assert(strcmp(c.control, "key1") == 0);
    Assert(c.longPressMs == 0 && c.repeatDelayMs == 300 && c.repeatMs == 100);
}

Fact("LayoutTypes Key Images Follow The Schema")
{
    // One image per state; each count is followed by its pointer
    Assert(LayoutKeyImageOffset(LAYOUT_KEY_NORMAL) == offsetof(layoutdata::key, normal_count));
    Assert(LayoutKeyImageOffset(LAYOUT_KEY_PRESSED) == offsetof(layoutdata::key, pressed_count));
    for (int s = 0; s < LAYOUT_KEY_IMAGES; s++)
        Assert(LayoutKeyImagePtrOffset(s) == LayoutKeyImageOffset(s) + 4);
}
