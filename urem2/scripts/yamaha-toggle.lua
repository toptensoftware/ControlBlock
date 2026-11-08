-- Yamaha read-modify-write toggle (urem's mute and "7ch Stereo" keys), from urem2/layout/scripts.mjs, which
-- injects STATUS_URL (getStatus), ON_PATTERN (a Lua pattern matching the "on" state in its JSON), ON_URL and OFF_URL.
-- On -> OFF_URL; otherwise, including a failed get (as urem did), -> ON_URL.
local body = http.get(STATUS_URL)
local on = type(body) == "string" and body:find(ON_PATTERN) ~= nil
http.get{ url = on and OFF_URL or ON_URL, reply = false }
