// Hardware test layout for activity switching and devices
// (wip/wip-activity-device-management.md, step 9). Every hook and device op sends a short
// UDP message (or an HTTP GET) to examples/test-server/test-server.mjs, so its console shows what ran
// and in what order. Bake and upload from the repo root:
//     node cli/cli.js upload examples/activity-test.mjs

import { udp, get, flatten } from "./test-common.mjs";

// Plain key faces instead of the default gradient shade (see flatten)
const FLAT = true;

// All four hooks of an activity, each announcing itself
const hooks = name => ({
    willActivate:   udp(`${name} willActivate`),
    didActivate:    udp(`${name} didActivate`),
    willDeactivate: udp(`${name} willDeactivate`),
    didDeactivate:  udp(`${name} didDeactivate`),
});

// "Big" uses 28 devices whose lists are a 1 ms delay (with the 4 real ones that's the
// firmware's limit of 32): switching to it from no activity queues 30 lists (2 hooks +
// 28 turnOn), close to the 40-entry ring
const BIG_DEVICES = 28;
const bigNames = Array.from({ length: BIG_DEVICES }, (_, i) => `big${i}`);
const bigDevices = Object.fromEntries(bigNames.map(n => [n, {
    turnOn: { op: "delay", ms: 1 }, turnOff: { op: "delay", ms: 1 } }]));

const go = (label, activity, color) => ({ label, color, tap: { op: "switchActivity", activity } });

// The same switch row at the bottom of every activity's first page, so A->B->C can be tapped quickly
const switchRow = [
    go("Watch", "Watch", "#8338ec"),
    go("Listen", "Listen", "#2a9d8f"),
    go("Lights", "Lights", "#e76f51"),
    go("None", null, "#555a66"),
    go("Big", "Big", "#6c757d"),
];

// A page of numbered keys, so a stale or blank key is easy to spot. `overrides` maps a key
// index to the key to put there instead.
function page(tag, p, color, overrides = {})
{
    const keys = [];
    for (let k = 0; k < 15; k++)
        keys.push(overrides[k] ?? { label: `${tag} p${p}\nk${k}`, color });
    return { keys };
}

const ping = (name) => ({ label: `Ping\n${name}`, color: "#264653", tap: udp(`ping from ${name}`) });

const layout = {
    version: 1,
    defaults: { color: "#22262e", pressed: { color: "#3a86ff" } },

    devices: {
        tv:        { turnOn: udp("tv on"), turnOff: udp("tv off") },
        avr:       { turnOn: get("avr=on"), turnOff: get("avr=off") },
        light:     { turnOn: udp("light on"), turnOff: udp("light off") },
        // A long warm-up: the delay holds the whole queue, so touches wait behind it
        projector: { turnOn: [udp("projector on"), { op: "delay", ms: 6000 }, udp("projector warm")],
                     turnOff: udp("projector off") },
        ...bigDevices,
    },

    home: {
        keys: [
            ...switchRow,
            { label: "Watch\nListen\nLights", color: "#3d405b",
              tap: [{ op: "switchActivity", activity: "Watch" },
                    { op: "switchActivity", activity: "Listen" },
                    { op: "switchActivity", activity: "Lights" }] },
            { label: "Hold a\n(no switch)", color: "#bc4749",
              press: { op: "hidKey", keys: "a", mode: "hold" },
              release: { op: "hidKey", keys: "a", mode: "hold" } },
            { label: "Hold a\nlong: Listen", color: "#bc4749", longPressMs: 1500,
              press: { op: "hidKey", keys: "a", mode: "hold" },
              release: { op: "hidKey", keys: "a", mode: "hold" },
              long: { op: "switchActivity", activity: "Listen" } },
            { label: "Big\nin 5 s", color: "#6c757d",
              tap: [{ op: "delay", ms: 5000 }, { op: "switchActivity", activity: "Big" }] },
            { label: "Noop", color: "#22262e", tap: { op: "delay", ms: 1 } },
            ping("Home"),
        ],
    },

    activities: {
        Watch: {
            displayName: "Watch TV",
            devices: ["tv", "avr"],
            hooks: hooks("Watch"),
            pages: [
                page("W", 1, "#4a2c7a", {
                    8: ping("Watch"),
                    9: { label: "Hold a\nlong: Lights", color: "#bc4749", longPressMs: 1500,
                         press: { op: "hidKey", keys: "a", mode: "hold" },
                         release: { op: "hidKey", keys: "a", mode: "hold" },
                         long: { op: "switchActivity", activity: "Lights" } },
                    10: switchRow[0], 11: switchRow[1], 12: switchRow[2], 13: switchRow[3], 14: switchRow[4] }),
                page("W", 2, "#4a2c7a"),
                page("W", 3, "#4a2c7a"),
            ],
        },
        Listen: {
            devices: ["avr", "light"],
            hooks: hooks("Listen"),
            pages: [
                page("L", 1, "#1d5c55", {
                    8: ping("Listen"),
                    10: switchRow[0], 11: switchRow[1], 12: switchRow[2], 13: switchRow[3], 14: switchRow[4] }),
            ],
        },
        Lights: {
            devices: ["light", "projector"],
            hooks: hooks("Lights"),
            pages: [
                page("G", 1, "#7a3a2a", {
                    8: ping("Lights"),
                    // Under Watch's "Hold a, long: Lights" key: lifting that finger after the switch must not tap this
                    9: { label: "Must not\nfire", color: "#7a3a2a", tap: udp("Lights k9 tapped (bad if from a hold)") },
                    10: switchRow[0], 11: switchRow[1], 12: switchRow[2], 13: switchRow[3], 14: switchRow[4] }),
                page("G", 2, "#7a3a2a"),
            ],
        },
        Big: {
            devices: bigNames,
            hooks: { willActivate: { op: "delay", ms: 1 }, didActivate: { op: "delay", ms: 1 },
                     willDeactivate: { op: "delay", ms: 1 }, didDeactivate: { op: "delay", ms: 1 } },
            pages: [
                page("B", 1, "#3a3f47", {
                    10: switchRow[0], 11: switchRow[1], 12: switchRow[2], 13: switchRow[3], 14: switchRow[4] }),
            ],
        },
    },
};

export default FLAT ? flatten(layout) : layout;
