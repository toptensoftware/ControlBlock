// Wi-Fi settings on a ControlBlock (docs/wifi.md).
//
//   node demo/wifi.js set <ssid> [password]   store a network and connect ("" for an empty password)
//   node demo/wifi.js get                     show the stored network (password in clear text)
//   node demo/wifi.js clear                   forget it and turn Wi-Fi off
//   node demo/wifi.js status                  show the connection state
//   node demo/wifi.js wait [seconds]          wait until connected (default 30)
//
// `set` also waits for the connection and prints the result.

import { ControlBlock, WifiState } from "../index.js";

const USAGE = "usage: node demo/wifi.js set <ssid> [password] | get | clear | status | wait [seconds]";
const WAIT_DEFAULT_S = 30;

function show(st)
{
    const parts = [st.stateName];
    if (st.ip)
        parts.push(st.ip, `${st.rssi} dBm`);
    if (st.state === WifiState.RETRYING)
        parts.push(`last disconnect reason ${st.reason}`);
    if (st.state !== WifiState.NO_CREDENTIALS)
        parts.push(`MAC ${st.mac}`);
    return parts.join(", ");
}

// Poll until connected or the time is up; prints each new state
async function waitConnected(cb, seconds)
{
    const deadline = Date.now() + seconds * 1000;
    let last = "";
    while (true) {
        const st = await cb.wifiStatus();
        const text = show(st);
        if (text !== last)
            console.log(text);
        last = text;
        if (st.state === WifiState.CONNECTED)
            return true;
        if (st.state === WifiState.NO_CREDENTIALS || Date.now() >= deadline)
            return false;
        await new Promise(resolve => setTimeout(resolve, 500));
    }
}

const [command, ...args] = process.argv.slice(2);
if (!["set", "get", "clear", "status", "wait"].includes(command)) {
    console.error(USAGE);
    process.exit(2);
}

const cb = await ControlBlock.open();
let code = 0;
try {
    switch (command) {
    case "set":
        if (!args.length || args.length > 2) {
            console.error(USAGE);
            code = 2;
            break;
        }
        await cb.setWifi(args[0], args[1] ?? "");
        console.log(`stored "${args[0]}"`);
        code = await waitConnected(cb, WAIT_DEFAULT_S) ? 0 : 1;
        break;
    case "get": {
        const w = await cb.getWifi();
        console.log(w.ssid ? `ssid: ${w.ssid}\npassword: ${w.password || "(none: open network)"}` : "no network stored");
        break;
    }
    case "clear":
        await cb.clearWifi();
        console.log("cleared");
        break;
    case "status":
        console.log(show(await cb.wifiStatus()));
        break;
    case "wait":
        code = await waitConnected(cb, Number(args[0] ?? WAIT_DEFAULT_S)) ? 0 : 1;
        break;
    }
} catch (err) {
    console.error(err.message);
    code = 1;
} finally {
    await cb.close();
}
process.exit(code);
