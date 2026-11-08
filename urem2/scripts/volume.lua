-- Encoder volume (urem's volume.js), from urem2/layout/scripts.mjs, which injects FAST_US, MEDIUM_US, STEP_FAST,
-- STEP_MEDIUM, STEP_SLOW, MAX_STEP and UP_URLS / DOWN_URLS (setVolume for steps 1..MAX_STEP).
-- param = "delta=<signed> interval=<us>" (the enc1 turn event). One request per event: urem's per-detent step by
-- speed, times |delta| (several detents coalesced), clamped. Fire and forget, so the queue never waits on it.
local p = param or ""
local delta = tonumber(p:match("delta=(%-?%d+)"))
if not delta or delta == 0 then return end
local interval = tonumber(p:match("interval=(%d+)"))
local per = STEP_SLOW
if interval and interval < FAST_US then per = STEP_FAST
elseif interval and interval < MEDIUM_US then per = STEP_MEDIUM end
local step = math.min(per * math.abs(delta), MAX_STEP)
http.get{ url = (delta > 0 and UP_URLS or DOWN_URLS)[step], reply = false }
