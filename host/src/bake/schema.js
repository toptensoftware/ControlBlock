// The device's layout structs as binpack types (docs/standalone.md "Schema"). Importing this
// registers them. No field mappers: stage 1 already produced exactly this shape.
// `formatTypes()` (see pack.js headerText) generates the C header the firmware loader uses.

import { registerEnum, registerType } from "../../../binpack/index.js";
import { OP, HID_MODE } from "./consts.js";

export const ROOT_TYPE = "layoutRoot";

registerEnum("op", OP);
registerEnum("hid_mode", HID_MODE);

// A byte array: length + pointer. Absent (null) packs as length 0 and a null pointer.
const bytes = name => [{ name, type: "length" }, { name, type: "byte*", default: null }];
// A list of pointers to ops, or of structs
const list = (name, type) => [{ name, type: "length" }, { name, type }];
const opList = name => list(name, "op**");

const OP_TYPES = {
    delay:           "delayOp",
    switch_activity: "switchActivityOp",
    hid_key:         "hidKeyOp",
    http_get:        "httpGetOp",
    http_post:       "httpPostOp",
    udp_packet:      "udpPacketOp",
    send_wol:        "sendWolOp",
    script:          "scriptOp",
};

registerType({
    name: "op",
    fields: [{ name: "op", type: "uint" }],
    resolveVirtualType: value => OP_TYPES[Object.keys(OP).find(k => OP[k] === value.op)],
});

const op = (name, fields) => registerType({ name, extends: "op", fields });

op("delayOp",          [{ name: "duration", type: "uint" }]);
op("switchActivityOp", [{ name: "index", type: "uint" }]);
op("hidKeyOp",         [{ name: "mode", type: "byte" }, { name: "modifiers", type: "byte" }, { name: "key", type: "byte" }]);
op("httpGetOp",        [{ name: "url", type: "string" }, { name: "headers", type: "string", default: null }]);
op("httpPostOp",       [{ name: "url", type: "string" }, ...bytes("data"), { name: "contentType", type: "string" }, { name: "headers", type: "string", default: null }]);
op("udpPacketOp",      [{ name: "host", type: "string" }, { name: "port", type: "ushort" }, ...bytes("data")]);
op("sendWolOp",        [{ name: "mac", type: "byte[6]" }, { name: "broadcast", type: "string", default: null }]);
op("scriptOp",         [{ name: "source", type: "length" }, { name: "source", type: "string" }]);

// A key: its id (unique across the layout; touch bindings name it as "touch[id]"), its rect
// [x, y, w, h] in quarter units from the grid's top-left (the firmware's KeyRect), and one opaque
// image per state (absent: drawn as background)
registerType({
    name: "key",
    fields: [
        { name: "id", type: "ushort" },
        { name: "rect", type: "byte[4]" },
        ...bytes("normal"), ...bytes("pressed"),
        { name: "longPressMs", type: "uint", default: 0 },
    ],
});

registerType({
    name: "binding",
    fields: [{ name: "control", type: "string" }, { name: "event", type: "string" }, ...opList("ops")],
});

registerType({ name: "page", fields: list("keys", "key*") });

registerType({
    name: "home",
    fields: [...list("keys", "key*"), ...list("bindings", "binding*")],
});

registerType({
    name: "device",
    fields: [{ name: "name", type: "string" }, ...opList("turnOn"), ...opList("turnOff")],
});

registerType({
    name: "activity",
    fields: [
        { name: "name", type: "string" },
        { name: "displayName", type: "string", default: null },
        ...list("devices", "int*"),
        ...list("pages", "page*"),
        ...list("bindings", "binding*"),
        ...opList("willDeactivate"), ...opList("willActivate"), ...opList("didActivate"), ...opList("didDeactivate"),
    ],
});

registerType({
    name: ROOT_TYPE,
    fields: [
        { name: "version", type: "uint" },
        { name: "longPressMs", type: "uint", default: 0 },
        { name: "home", type: "home", cname: "homePage" },     // C++ rejects a member named like its type
        ...list("devices", "device*"),
        ...list("activities", "activity*"),
    ],
});
