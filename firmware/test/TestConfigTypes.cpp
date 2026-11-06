// The C structs generated from the binpack schema (tools/codegen/gen-types.js ->
// main/config_types.gen.h, wrapped by config_types.h). Checks that they read the
// sample blob the real packer wrote, both as baked (pointers are file offsets)
// and relocated (pointers are addresses). The host tests are 32-bit so the
// packed structs, with their 32-bit pointers, match the device exactly.

#include <stddef.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "config_env.h"

using namespace SimpleLib;
using namespace config_env;

Fact("ConfigTypes Read The Sample Blob As Baked")
{
    // Pointers are file offsets until relocated; counts and values read directly
    List<uint8_t> b = LoadSample();
    const cfgdata::configRoot *root = ConfigRoot(b.GetBuffer());
    Assert((const uint8_t *)root == b.GetBuffer() + CB_ROOT);
    Assert(root->longPressMs == 650);
    Assert(root->devices_count == 2);
    Assert(root->activities_count == 2);
    Assert(root->homePage.bindings_count == U32(b, CB_HOME_BINDINGS));
    Assert(root->homePage.keys[1].normalBg_count == U32(b, CB_KEY_COUNT(CB_HOME_KEY(1), KEY_SLOT_BG)));
    Assert(root->homePage.keys[1].normalBg_count == U32(b, CB_KEY_COUNT(CB_HOME_KEY(1), 0)));
}

Fact("ConfigTypes Read The Relocated Sample Through The Structs")
{
    List<uint8_t> b = LoadSample();
    const cfgdata::configRoot *root = Relocate(b);

    // Home's bindings: touch[0,0] tap runs a hidKey
    const cfgdata::binding &first = root->homePage.bindings[0];
    Assert(strcmp(first.control, "touch[0,0]") == 0);
    Assert(strcmp(first.event, "tap") == 0);
    Assert(first.ops_count == 1);
    Assert(first.ops[0]->op == OP_HID_KEY);
    const cfgdata::hidKeyOp *hid = (const cfgdata::hidKeyOp *)first.ops[0];
    Assert(hid->mode <= HID_MODE_MAX);

    // Keys point at their images inside the blob
    const cfgdata::key &k0 = root->homePage.keys[0];
    Assert(k0.normalBg_count > 0);
    Assert(k0.normalBg >= b.GetBuffer() && k0.normalBg + k0.normalBg_count <= b.GetBuffer() + b.GetCount());

    // An activity: its name and a binding
    Assert(strcmp(root->activities[0].name, "Watch") == 0);
    Assert(strcmp(root->activities[0].displayName, "Watch TV") == 0);
    // touch[5,0] tap (the script) and macro1 tap (a hidKey)
    Assert(root->activities[0].bindings_count == 2);
    bool macro = false;
    for (uint32_t i = 0; i < root->activities[0].bindings_count; i++)
        macro = macro || strcmp(root->activities[0].bindings[i].control, "macro1") == 0;
    Assert(macro);
    Assert(root->devices_count == 2 && strcmp(root->devices[0].name, "tv") == 0);
}

Fact("ConfigTypes Key Image Slots Follow The Schema Order")
{
    // The slots follow KEY_SLOT_* order; each count is followed by its pointer
    Assert(ConfigKeyImageOffset(KEY_SLOT_BG) == offsetof(cfgdata::key, normalBg_count));
    Assert(ConfigKeyImageOffset(KEY_SLOT_FG) == offsetof(cfgdata::key, normalFg_count));
    Assert(ConfigKeyImageOffset(KEY_SLOT_PRESSED_BG) == offsetof(cfgdata::key, pressedBg_count));
    Assert(ConfigKeyImageOffset(KEY_SLOT_PRESSED_FG) == offsetof(cfgdata::key, pressedFg_count));
    for (int s = 0; s < KEY_SLOTS; s++)
        Assert(ConfigKeyImagePtrOffset(s) == ConfigKeyImageOffset(s) + 4);
}
