// The Apple TV daemon's UDP text protocol (urem2/atvdaemon/atvdaemon.py, port 7777, UTF-8, one command per
// datagram; it replies to the sender with ok / pong / connecting / error: <msg>).
// atvKey / atvApp build commands for the layout (the udpPacket op's data.text); parseAtvCommand is for the test
// server's fake daemon and follows the daemon's _dispatch split rules and error messages.

export const ATV_PORT = 7777;

export const ATV_PING = "ping";
export const ATV_DISCONNECT = "disconnect";

// pyatv remote_control keys (daemon README); the daemon accepts any pyatv RemoteControl method name
export const ATV_KEYS = [
    "up", "down", "left", "right", "select", "menu", "home", "home_hold", "play_pause", "play", "pause",
    "next", "previous", "stop", "volume_up", "volume_down", "top_menu", "channel_up", "channel_down", "screensaver",
];

export const ATV_ACTIONS = ["single", "double", "hold"];

// Display name -> bundle id (urem frontend/activity-atv.js)
export const ATV_APPS = {
    "Fitness": "com.apple.Fitness",
    "10": "com.networkten.epg",
    "Podcasts": "com.apple.podcasts",
    "Movies": "com.apple.TVMovies",
    "FV": "au.com.freeview.Freeview-Plus",
    "TV": "com.apple.TVWatchList",
    "Photos": "com.apple.TVPhotos",
    "App Store": "com.apple.TVAppStore",
    "Prime Video": "com.amazon.aiv.AIVApp",
    "Stan": "au.com.stan",
    "Arcade": "com.apple.Arcade",
    "9Now": "au.com.ninemsn.jump-in",
    "VLC": "org.videolan.vlc-ios",
    "AMC+": "com.amcplus.amcn",
    "Spotify": "com.spotify.client",
    "7plus": "com.swm.live",
    "Computers": "com.apple.TVHomeSharing",
    "SBS On Demand": "au.com.sbs.ondemand",
    "BINGE": "au.com.streamotion.ares.prod",
    "YouTube": "com.google.ios.youtube",
    "Vevo": "com.vevo.appletv",
    "ABC iview": "au.net.abc.ABCiView",
    "Search": "com.apple.TVSearch",
    "TV Shows": "com.apple.TVShows",
    "Paramount+": "com.cbs.canada.app",
    "NASA": "gov.nasa.NASA",
    "Disney+": "com.disney.disneyplus",
    "Acorn TV": "com.rljentertainment.AcornTV",
    "Settings": "com.apple.TVSettings",
    "F1 TV": "com.formula1.ott",
    "Netflix": "com.netflix.Netflix",
    "Music": "com.apple.TVMusic",
};

// "menu", "hold" -> "key menu hold"; the action is left out when not given (the daemon defaults to single)
export function atvKey(name, action)
{
    if (!ATV_KEYS.includes(name))
        throw new Error(`unknown Apple TV key ${JSON.stringify(name)}`);
    if (action === undefined)
        return `key ${name}`;
    if (!ATV_ACTIONS.includes(action))
        throw new Error(`unknown Apple TV action ${JSON.stringify(action)}`);
    return `key ${name} ${action}`;
}

// "Netflix" or "com.netflix.Netflix" -> "app com.netflix.Netflix"
export function atvApp(nameOrBundle)
{
    const bundle = Object.hasOwn(ATV_APPS, nameOrBundle) ? ATV_APPS[nameOrBundle]
        : Object.values(ATV_APPS).includes(nameOrBundle) ? nameOrBundle
        : null;
    if (!bundle)
        throw new Error(`unknown Apple TV app ${JSON.stringify(nameOrBundle)}`);
    return `app ${bundle}`;
}

// Python's repr() of a str, for error messages that match the daemon's
function pyRepr(s)
{
    return s.includes("'") && !s.includes('"') ? `"${s}"` : `'${s.replaceAll("'", "\\'")}'`;
}

// A received command -> { cmd, ... } or { error } (the text after "error: " in the daemon's reply).
//   key -> { cmd, key, action }   app / text_set / text_append -> { cmd, arg }   ping / disconnect / text_clear -> { cmd }
export function parseAtvCommand(text)
{
    const msg = text.trim();
    if (!msg)
        return { error: "empty command" };
    const sp = msg.search(/\s/);
    const cmd = (sp < 0 ? msg : msg.slice(0, sp)).toLowerCase();
    const arg = sp < 0 ? null : msg.slice(sp).trim();

    switch (cmd)
    {
        case "ping":
        case "disconnect":
        case "text_clear":
            return { cmd };

        case "key":
        {
            if (!arg)
                return { error: "key requires an argument" };
            const ksp = arg.search(/\s/);
            const key = ksp < 0 ? arg : arg.slice(0, ksp);
            let action = "single";
            if (ksp >= 0)
            {
                action = arg.slice(ksp).trim().toLowerCase();
                if (!ATV_ACTIONS.includes(action))
                    return { error: `unknown action ${pyRepr(action)} (use single, double, hold)` };
            }
            if (!ATV_KEYS.includes(key))
                return { error: `Unknown key: ${pyRepr(key)}` };
            return { cmd, key, action };
        }

        case "app":
        case "text_set":
        case "text_append":
            if (!arg)
                return { error: `${cmd} requires an argument` };
            return { cmd, arg };
    }
    return { error: `unknown command: ${pyRepr(cmd)}` };
}
