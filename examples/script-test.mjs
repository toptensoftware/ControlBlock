// Hardware test layout for the Lua `script` op (wip/wip-lua-hardware-checks.md has the
// checklist). Scripts log through ESP_LOGI("lua") and talk to examples/test-server/test-server.mjs.
// After each run of the op queue the device logs a "scripts:" line with the heap peak and the
// op task's free stack. Bake and upload from the repo root:
//     node cli/cli.js upload examples/script-test.mjs

import { HTTP, SERVER, udp, flatten } from "./test-common.mjs";

const lua = source => ({ op: "script", lua: source });
const key = (label, color, source) => ({ label, color, tap: lua(source) });

// Busy for `ms` without waiting (the standalone lock is held all the while)
const busy = ms => `local t0 = millis() while millis() - t0 < ${ms} do end`;

const ping = { label: "Ping\n(op)", color: "#264653", tap: udp("ping") };

const layout = {
    version: 1,
    defaults: { color: "#22262e", pressed: { color: "#3a86ff" } },

    home: {
        keys: [
            // 0-4: the http.get reply path
            key("GET\nreply", "#2a9d8f", `
                local body, status = http.get('${HTTP}/get?from=script')
                log('get:', status, body)`),
            key("GET\n404", "#2a9d8f", `
                local body, status = http.get{ url = '${HTTP}/nothing', headers = { ['X-Test'] = 'lua' } }
                log('404:', status, body)`),
            key("GET hang\n10 s", "#e76f51", `
                log('hang: waiting (take over now to test abandon)')
                local body, err = http.get('${HTTP}/hang')
                log('hang:', body, err)`),
            key("GET big\n16 KB cap", "#2a9d8f", `
                local body, status = http.get('${HTTP}/big')
                log('big:', status, body and #body)`),
            key("Mute\ntoggle", "#8338ec", `
                local body, err = http.get('${HTTP}/mute')
                if not body then log('mute: no reply', err) return end
                local next = body:find('mute=1') and '0' or '1'
                http.post('${HTTP}/mute', 'mute=' .. next)
                log('mute: was', body, 'now', next)`),

            // 5-9: latency and the lock
            { label: "Latency\nop+script", color: "#3d405b",
              tap: [udp("latency: op"),
                    lua(`udp.send('${SERVER}', 9999, 'latency: script at ' .. millis())`)] },
            key("UDP x6", "#3d405b", `
                for i = 1, 6 do udp.send('${SERVER}', 9999, 'burst ' .. i) end
                udp.send{ host = '${SERVER}', port = 9999, data = { hex = 'de ad be ef' } }`),
            key("Busy\n10 s", "#bc4749", `
                local t0 = millis()
                while millis() - t0 < 10000 do
                    ${busy(80)}
                    delay(20)
                end
                log('busy: done')`),
            key("Too long\n(150 ms)", "#bc4749", `${busy(150)} log('too long: not stopped (bad)')`),
            ping,

            // 10-14: stack, heap, HID, switching
            key("C stack\ndeep", "#6c757d", `
                local depth = 0
                local function f(n) depth = n pcall(f, n + 1) end
                f(1)
                log('C recursion stopped at depth', depth)`),
            key("Lua stack\n2000", "#6c757d", `
                local function g(n) if n == 0 then return 0 end return 1 + g(n - 1) end
                log('Lua recursion', g(2000))`),
            key("Heap\ncap", "#6c757d", `
                local t = {}
                for i = 1, 100000 do t[i] = string.rep('y', 100) .. i end
                log('heap: not capped (bad)')`),
            key("Type\nHi!", "#e9c46a", `
                hid.key('shift+h') hid.key('i') hid.key{ keys = 'shift+1' }
                log('typed Hi!')`),
            key("Switch\nScripts", "#2a9d8f", `
                log('bad name:', switchActivity('Nope'))
                log('switch:', switchActivity('Scripts'))
                log('after the switch call (logged before the switch commits)')`),
        ],
    },

    activities: {
        Scripts: {
            hooks: { didActivate: lua(`log('Scripts didActivate')`),
                     willDeactivate: lua(`log('Scripts willDeactivate')`) },
            pages: [{
                keys: [
                    key("None\n(script)", "#555a66", `log('none:', switchActivity(nil))`),
                    ping,
                    key("GET\nreply", "#2a9d8f", `
                        local body, status = http.get('${HTTP}/get?from=activity')
                        log('get:', status, body)`),
                ],
            }],
        },
    },
};

export default flatten(layout);
