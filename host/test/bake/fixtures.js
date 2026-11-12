// Shared bake fixtures: the docs/layout-format.md example layout and the script files it
// names (relative to the layout's directory). Used by the bake, files and cli tests.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

export const example = () => ({
    version: 1,
    settings: { longPressMs: 600 },
    defaults: { color: "#22262e", iconColor: "white", pressed: { color: "#3a86ff" } },
    devices: {
        tv: { turnOn: { op: "sendWol", mac: "AA:BB:CC:DD:EE:FF" },
              turnOff: { op: "httpPost", url: "http://tv.local/api/power", body: "state=off" } },
        receiver: { turnOn: { op: "httpPost", url: "http://avr.local/power", body: "on=1" },
                    turnOff: { op: "httpPost", url: "http://avr.local/power", body: "on=0" } },
    },
    actions: {
        mute: { op: "script", file: "scripts/mute.lua" },
        back: [{ op: "hidKey", keys: "esc" }],
    },
    home: { keys: [
        { icon: "#apple-tv", label: "Watch TV", color: "#8338ec", action: { op: "switchActivity", activity: "Watch TV" } },
        { icon: "#list", label: "Listen", action: { op: "switchActivity", activity: "Listen" } },
    ] },
    activities: {
        "Watch TV": {
            displayName: "TV",
            devices: ["tv", "receiver"],
            hooks: { didActivate: { op: "httpPost", url: "http://avr.local/input", body: "src=hdmi1" } },
            pages: [{ keys: [
                { icon: "#play", label: "Play", action: { op: "hidKey", keys: "space" } },
                { icon: "#next-track", label: "Next", action: { op: "hidKey", keys: "ctrl+right" } },
                null, null,
                { icon: "#volume-mute", label: "Mute", action: { use: "mute" } },
                { icon: "#arrow-left", action: { use: "back" }, long: { op: "switchActivity", activity: "Listen" } },
            ] }],
            bindings: [
                { control: "macro1", event: "tap", do: { use: "mute" } },
                { control: "volEnc", event: "turnLeft", do: { op: "script", file: "scripts/vol.lua" } },
                { control: "volEnc", event: "turnRight", do: { op: "script", file: "scripts/vol.lua" } },
            ],
        },
        Listen: {
            devices: ["receiver"],
            pages: [{ keys: [{ icon: "#play", action: { op: "hidKey", keys: "space" } }] }],
        },
    },
});

// Key placement (docs/layout-format.md "Keys"): spans, a group, fractional keys and a named id on Home
export const exampleKeys = () => ({
    version: 1,
    defaults: { color: "#22262e" },
    home: { keys: [
        // slot 0, two units wide (it covers slot 1, so entry 1 is null)
        { w: 2, icon: "#apple-tv", label: "Watch TV", action: { op: "switchActivity", activity: null } },
        null,
        { icon: "#list", label: "Listen", action: { op: "hidKey", keys: "l" } },
        null,
        // slot 4, two units tall, one picture split into two keys: + on top, - below
        { h: 2, label: "Volume", keys: [
            { icon: "#add", action: { op: "hidKey", keys: "up" } },
            { y: 1, icon: "#delete", action: { op: "hidKey", keys: "down" } },
        ] },
        // slot 5 holds two half-height keys
        { x: 0, y: 1, h: 0.5, label: "Mute", action: { op: "hidKey", keys: "m" } },
        { x: 0, y: 1.5, h: 0.5, label: "Input", action: { op: "hidKey", keys: "i" } },
        // three units wide along the bottom row, with an id of its own
        { id: 200, x: 1, y: 2, w: 3, label: "Play / Pause", action: { op: "hidKey", keys: "space" } },
    ] },
});

export const files = {
    "scripts/mute.lua": "local r = http.get('http://avr.local/mute')\nhttp.post('http://avr.local/mute', r == '1' and 'on=0' or 'on=1')\n",
    "scripts/vol.lua": "http.post('http://avr.local/vol', 'delta=' .. ((tonumber(param) or 1) // 10))\n",
};

// A temporary directory holding the example as layout.json plus its scripts. Returns
// { dir, file, cleanup }. `layout` replaces the example; `name` the file name.
export function exampleDir({ layout = example(), name = "layout.json" } = {})
{
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cbbake-"));
    fs.mkdirSync(path.join(dir, "scripts"));
    for (const [file, text] of Object.entries(files))
        fs.writeFileSync(path.join(dir, file), text);
    const file = path.join(dir, name);
    fs.writeFileSync(file, typeof layout === "string" ? layout : JSON.stringify(layout));
    return { dir, file, cleanup: () => fs.rmSync(dir, { recursive: true, force: true }) };
}
