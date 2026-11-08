// Tests for appletv.mjs. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { ATV_PORT, ATV_KEYS, ATV_ACTIONS, ATV_APPS, ATV_PING, ATV_DISCONNECT, atvKey, atvApp, parseAtvCommand }
    from "./appletv.mjs";

test("constants", () => {
    assert.equal(ATV_PORT, 7777);
    assert.equal(ATV_PING, "ping");
    assert.equal(ATV_DISCONNECT, "disconnect");
    assert.deepEqual(ATV_ACTIONS, ["single", "double", "hold"]);
    for (const k of ["up", "down", "left", "right", "select", "menu", "home", "play", "pause", "next", "previous"])
        assert.ok(ATV_KEYS.includes(k), k);
});

test("atvKey builds the key command", () => {
    assert.equal(atvKey("menu", "hold"), "key menu hold");
    assert.equal(atvKey("up"), "key up");
    assert.equal(atvKey("select", "double"), "key select double");
});

test("atvKey throws on an unknown key or action", () => {
    assert.throws(() => atvKey("nope"), /unknown Apple TV key/);
    assert.throws(() => atvKey("menu", "triple"), /unknown Apple TV action/);
});

test("atvApp takes a display name or a bundle id", () => {
    assert.equal(atvApp("Netflix"), "app com.netflix.Netflix");
    assert.equal(atvApp("com.netflix.Netflix"), "app com.netflix.Netflix");
    assert.equal(atvApp("ABC iview"), "app au.net.abc.ABCiView");
    assert.throws(() => atvApp("Hulu"), /unknown Apple TV app/);
    assert.throws(() => atvApp("toString"), /unknown Apple TV app/);    // not inherited properties
});

test("all 32 apps from urem are present", () => {
    assert.deepEqual(ATV_APPS, {
        "Fitness": "com.apple.Fitness", "10": "com.networkten.epg", "Podcasts": "com.apple.podcasts",
        "Movies": "com.apple.TVMovies", "FV": "au.com.freeview.Freeview-Plus", "TV": "com.apple.TVWatchList",
        "Photos": "com.apple.TVPhotos", "App Store": "com.apple.TVAppStore", "Prime Video": "com.amazon.aiv.AIVApp",
        "Stan": "au.com.stan", "Arcade": "com.apple.Arcade", "9Now": "au.com.ninemsn.jump-in",
        "VLC": "org.videolan.vlc-ios", "AMC+": "com.amcplus.amcn", "Spotify": "com.spotify.client",
        "7plus": "com.swm.live", "Computers": "com.apple.TVHomeSharing", "SBS On Demand": "au.com.sbs.ondemand",
        "BINGE": "au.com.streamotion.ares.prod", "YouTube": "com.google.ios.youtube", "Vevo": "com.vevo.appletv",
        "ABC iview": "au.net.abc.ABCiView", "Search": "com.apple.TVSearch", "TV Shows": "com.apple.TVShows",
        "Paramount+": "com.cbs.canada.app", "NASA": "gov.nasa.NASA", "Disney+": "com.disney.disneyplus",
        "Acorn TV": "com.rljentertainment.AcornTV", "Settings": "com.apple.TVSettings", "F1 TV": "com.formula1.ott",
        "Netflix": "com.netflix.Netflix", "Music": "com.apple.TVMusic",
    });
    assert.equal(Object.keys(ATV_APPS).length, 32);
});

test("parseAtvCommand: key with and without an action", () => {
    assert.deepEqual(parseAtvCommand("key menu hold"), { cmd: "key", key: "menu", action: "hold" });
    assert.deepEqual(parseAtvCommand("key up"), { cmd: "key", key: "up", action: "single" });
    assert.deepEqual(parseAtvCommand("  KEY   select   HOLD \n"), { cmd: "key", key: "select", action: "hold" });
});

test("parseAtvCommand: the other commands", () => {
    assert.deepEqual(parseAtvCommand("ping"), { cmd: "ping" });
    assert.deepEqual(parseAtvCommand("Ping\n"), { cmd: "ping" });
    assert.deepEqual(parseAtvCommand("disconnect"), { cmd: "disconnect" });
    assert.deepEqual(parseAtvCommand("app  com.netflix.Netflix "), { cmd: "app", arg: "com.netflix.Netflix" });
    assert.deepEqual(parseAtvCommand("text_set hello  world"), { cmd: "text_set", arg: "hello  world" });
    assert.deepEqual(parseAtvCommand("text_append x"), { cmd: "text_append", arg: "x" });
    assert.deepEqual(parseAtvCommand("text_clear"), { cmd: "text_clear" });
});

test("parseAtvCommand: errors use the daemon's messages", () => {
    assert.deepEqual(parseAtvCommand(""), { error: "empty command" });
    assert.deepEqual(parseAtvCommand("   "), { error: "empty command" });
    assert.deepEqual(parseAtvCommand("key"), { error: "key requires an argument" });
    assert.deepEqual(parseAtvCommand("app "), { error: "app requires an argument" });
    assert.deepEqual(parseAtvCommand("text_set"), { error: "text_set requires an argument" });
    assert.deepEqual(parseAtvCommand("text_append"), { error: "text_append requires an argument" });
    assert.deepEqual(parseAtvCommand("key menu triple"),
        { error: "unknown action 'triple' (use single, double, hold)" });
    assert.deepEqual(parseAtvCommand("key nope"), { error: "Unknown key: 'nope'" });
    assert.deepEqual(parseAtvCommand("Launch x"), { error: "unknown command: 'launch'" });
});
