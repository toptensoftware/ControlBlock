// Shared bake fixtures: the docs/config-format.md example config and the script files it
// names (relative to the config's directory). Used by the bake, files and cli tests.

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

export const files = {
    "scripts/mute.lua": "local r = http.get('http://avr.local/mute')\nhttp.post('http://avr.local/mute', r == '1' and 'on=0' or 'on=1')\n",
    "scripts/vol.lua": "http.post('http://avr.local/vol', 'delta=' .. ((tonumber(param) or 1) // 10))\n",
};

// A temporary directory holding the example as config.json plus its scripts. Returns
// { dir, file, cleanup }. `config` replaces the example; `name` the file name.
export function exampleDir({ config = example(), name = "config.json" } = {})
{
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cbbake-"));
    fs.mkdirSync(path.join(dir, "scripts"));
    for (const [file, text] of Object.entries(files))
        fs.writeFileSync(path.join(dir, file), text);
    const file = path.join(dir, name);
    fs.writeFileSync(file, typeof config === "string" ? config : JSON.stringify(config));
    return { dir, file, cleanup: () => fs.rmSync(dir, { recursive: true, force: true }) };
}
