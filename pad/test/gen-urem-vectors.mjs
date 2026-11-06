// Generates data/urem_vectors.json: timestamped encoder streams with the answers urem's
// code gives, for the C++ differential test (TestPadUrem.cpp). The C++ test reads the
// committed JSON; run this only to regenerate it:  node gen-urem-vectors.js
//
// The reference is a faithful port of urem's updateEncoder (urem/frontend/gpio.js) with the
// clock and pin reads turned into parameters, plus an independent model of pigpio's glitch
// filter (see pad/common/glitch_filter.h for the exact semantics chosen).
//
// Times are microseconds. urem uses Date.now() (ms): a stream on a 1000 us grid gives
// exactly urem's numbers x 1000 (urem's interval is maSum / 4, a quarter-ms fraction, and
// 1000 / 4 is whole, so the floor below loses nothing there). Streams start at T0, far
// above the 1 s cap, as Date.now() is: urem's `timeLast = 0` makes its first elapsed
// time huge, i.e. capped.
//
// urem's alert callbacks run steady_us after the edge (pigpio delivers a filtered change
// late), and updateEncoder re-reads both pins with digitalRead(). This port takes the
// edge time (a constant delay on every edge leaves all intervals unchanged) and the
// filtered levels of both pins as of that change (see encoder_channel.h: the one
// deliberate difference).

import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const CAP = 1000000;                  // urem: 1000 ms per interval, in us
const T0 = 10000000;
const GRAY = [0, 1, 3, 2];            // A<<1|B for CW steps 00 > 01 > 11 > 10

// ---- Reference: urem's updateEncoder -----------------------------------------------

function makeUrem(a0, b0)
{
    let encoderPos = 0;
    let encoderALast = a0, encoderBLast = b0;
    let timeLast = 0;
    let ma = [CAP, CAP, CAP, CAP];
    let maSum = 4 * CAP;
    let maIndex = 0;
    let detents = 0;

    const transitions = {
        0b0001: 1, 0b0111: 1, 0b1110: 1, 0b1000: 1,
        0b0010: -1, 0b1011: -1, 0b1101: -1, 0b0100: -1
    };

    return {
        get detents() { return detents >>> 0; },
        get interval() { return Math.floor(maSum / 4); },
        update(now, a, b)
        {
            let elapsed = now - timeLast;
            timeLast = now;
            if (elapsed > CAP)
                elapsed = CAP;

            maSum = maSum - ma[maIndex % 4] + elapsed;
            ma[maIndex % 4] = elapsed;
            maIndex++;

            const current = (a << 1) | b;
            const previous = (encoderALast << 1) | encoderBLast;
            const key = (previous << 2) | current;
            const direction = transitions[key];

            let tick = null;
            if (direction)
            {
                encoderPos += direction;
                if ((encoderPos % 4) == 0)
                {
                    detents += direction;
                    tick = [direction, Math.floor(maSum / 4)];
                }
            }
            encoderALast = a;
            encoderBLast = b;
            return tick;
        }
    };
}

// ---- Reference: glitch filter model ---------------------------------------------------
// raw: [[t, level]] changes (each flips the level), init: level before the first.
// A change is accepted iff the level then holds for >= steady (until the next change or
// forever) and it differs from the accepted level.

function filterModel(init, steady, raw)
{
    const out = [];
    let lvl = init;
    for (let i = 0; i < raw.length; i++)
    {
        const [t, level] = raw[i];
        const next = i + 1 < raw.length ? raw[i + 1][0] : Infinity;
        if (next - t >= steady && level !== lvl)
        {
            out.push([t, level]);
            lvl = level;
        }
    }
    return out;
}

// ---- Random -----------------------------------------------------------------------------

function mulberry32(seed)
{
    let a = seed >>> 0;
    return function ()
    {
        a = (a + 0x6D2B79F5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

const cases = [];

function pick(rng, arr) { return arr[Math.floor(rng() * arr.length)]; }
function between(rng, lo, hi) { return lo + Math.floor(rng() * (hi - lo + 1)); }

// ---- quad cases: calls (t, a, b) straight into updateEncoder ------------------------------

function quadCase(name, a0, b0, events)
{
    const u = makeUrem(a0, b0);
    const ticks = [];
    events.forEach(([t, a, b], i) =>
    {
        const tick = u.update(t, a, b);
        if (tick)
            ticks.push([i, tick[0], tick[1]]);
    });
    cases.push({ kind: 'quad', name, a0, b0, events, ticks, detents: u.detents, interval_us: u.interval });
}

// steps: array of +1/-1 moves along the Gray cycle, gap(i) the time before step i
function walkEvents(startIdx, steps, gap)
{
    const ev = [];
    let idx = startIdx, t = T0;
    steps.forEach((d, i) =>
    {
        idx += d;
        t += gap(i);
        const s = GRAY[((idx % 4) + 4) % 4];
        ev.push([t, s >> 1, s & 1]);
    });
    return ev;
}

function startLevels(idx) { const s = GRAY[idx]; return [s >> 1, s & 1]; }

function addQuadCases()
{
    const rng = mulberry32(12345);

    // Clean detents at several speeds (per-edge gap), both directions, ms and us grids
    for (const gapUs of [1000, 2000, 3000, 5000, 8000, 20000, 150000, 1200000])
        for (const dir of [1, -1])
            for (const start of [0, 2])
            {
                const steps = new Array(48).fill(dir);
                const [a0, b0] = startLevels(start);
                quadCase(`clean dir=${dir} gap=${gapUs}us start=${start}`, a0, b0, walkEvents(start, steps, () => gapUs));
            }
    for (const dir of [1, -1])
    {
        const steps = new Array(64).fill(dir);
        const [a0, b0] = startLevels(0);
        quadCase(`clean us-jitter dir=${dir}`, a0, b0, walkEvents(0, steps, () => between(rng, 700, 9000)));
    }

    // Reversals in the middle of a detent (urem's multiple-of-4 rule)
    const reversals = [
        [1, 1, -1, -1],                 // out and back: a tick of -1 at the return to 0
        [1, 1, 1, -1, -1, -1],
        [1, 1, 1, 1, -1, -1, -1, -1],
        [1, 1, 1, 1, 1, -1, -1, 1, 1, 1, 1],
        [-1, -1, 1, 1, 1, 1, 1, 1, 1, 1],
        [1, -1, 1, -1, 1, -1, 1, -1, 1, 1, 1, 1],
        [1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1],
        [-1, -1, -1, -1, -1, -1, -1, -1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1],
        [-1, 1, -1, 1, -1, 1, -1, -1, -1, -1],
    ];
    reversals.forEach((steps, n) =>
    {
        const [a0, b0] = startLevels(0);
        quadCase(`reversal ${n}`, a0, b0, walkEvents(0, steps, () => 3000));
    });

    // Long idle gaps (cap at 1 s) in the middle of a spin
    {
        const steps = new Array(40).fill(1);
        const [a0, b0] = startLevels(1);
        quadCase('idle gaps', a0, b0, walkEvents(1, steps, (i) => i % 7 === 3 ? 3500000 : 2000));
    }

    // Random walks: mostly continuing, sometimes reversing, random speeds
    for (let n = 0; n < 12; n++)
    {
        const grid = n % 2 === 0;
        const start = between(rng, 0, 3);
        const steps = [];
        let dir = rng() < 0.5 ? 1 : -1;
        const len = between(rng, 100, 600);
        const pRev = pick(rng, [0.05, 0.15, 0.4]);
        for (let i = 0; i < len; i++)
        {
            if (rng() < pRev) dir = -dir;
            steps.push(dir);
        }
        const gaps = [];
        let g = between(rng, 1, 30);
        for (let i = 0; i < len; i++)
        {
            if (rng() < 0.1) g = pick(rng, [1, 1, 2, 3, 5, 8, 12, 20, 40, 150, 1100, 2500]);
            gaps.push(grid ? g * 1000 : g * 1000 + between(rng, -500, 500) + (g === 1 ? 600 : 0));
        }
        const [a0, b0] = startLevels(start);
        quadCase(`random walk ${n} ${grid ? 'ms-grid' : 'us'}`, a0, b0, walkEvents(start, steps, (i) => gaps[i]));
    }

    // Garbage: arbitrary (a, b) per call, including no change and double changes
    for (let n = 0; n < 8; n++)
    {
        const grid = n % 2 === 0;
        const len = between(rng, 200, 800);
        const ev = [];
        let t = T0;
        for (let i = 0; i < len; i++)
        {
            t += grid ? between(rng, 1, 15) * 1000 : between(rng, 50, 15000);
            ev.push([t, rng() < 0.5 ? 1 : 0, rng() < 0.5 ? 1 : 0]);
        }
        quadCase(`garbage ${n} ${grid ? 'ms-grid' : 'us'}`, between(rng, 0, 1), between(rng, 0, 1), ev);
    }
}

// ---- pipe cases: raw pin edges -> filters (1 ms) -> updateEncoder ---------------------------

function pipeCase(name, a0, b0, steady, raw)
{
    // raw: [[t, pin, level]]
    const perPin = [[], []];
    raw.forEach(([t, pin, level]) => perPin[pin].push([t, level]));
    const fa = filterModel(a0, steady, perPin[0]).map(([t, l]) => [t, 0, l]);
    const fb = filterModel(b0, steady, perPin[1]).map(([t, l]) => [t, 1, l]);
    // Accepted changes in edge-time order, A first on a tie
    const merged = fa.concat(fb).sort((x, y) => x[0] - y[0] || x[1] - y[1]);

    const u = makeUrem(a0, b0);
    let la = a0, lb = b0;
    const ticks = [];
    for (const [t, pin, level] of merged)
    {
        if (pin === 0) la = level; else lb = level;
        const tick = u.update(t, la, lb);
        if (tick)
            ticks.push([t, tick[0], tick[1]]);
    }
    cases.push({ kind: 'pipe', name, a0, b0, filter_us: steady, events: raw, ticks, detents: u.detents });
}

// Edges of a spin along the Gray cycle with contact bounce. 'bounce(rng)' returns the list
// of gaps (alternating: level away, level back, ...) appended after an edge, in us.
function genPipe(rng, { len, start, edgeGap, pReverse, pBounce, bounceGap, grid })
{
    const raw = [];
    let idx = start, t = T0;
    let dir = rng() < 0.5 ? 1 : -1;
    let [a, b] = startLevels(start);
    const level = [a, b];
    for (let i = 0; i < len; i++)
    {
        if (rng() < pReverse) dir = -dir;
        const prev = GRAY[((idx % 4) + 4) % 4];
        idx += dir;
        const s = GRAY[((idx % 4) + 4) % 4];
        const pin = (prev ^ s) === 2 ? 0 : 1;
        const newLevel = pin === 0 ? s >> 1 : s & 1;

        t += edgeGap(rng);
        raw.push([t, pin, newLevel]);
        level[pin] = newLevel;

        if (rng() < pBounce)
        {
            const pairs = between(rng, 1, 3);
            for (let k = 0; k < pairs; k++)
            {
                t += bounceGap(rng);
                raw.push([t, pin, 1 - newLevel]);
                t += bounceGap(rng);
                raw.push([t, pin, newLevel]);
            }
        }
    }
    const [a0, b0] = startLevels(start);
    return { a0, b0, raw };
}

function addPipeCases()
{
    const rng = mulberry32(777);

    const gapsMs = (rng) => pick(rng, [1, 2, 2, 3, 4, 6, 10, 25]) * 1000;
    const gapsUs = (rng) => between(rng, 400, 12000);

    // Clean, no bounce, several speeds
    for (const g of [1500, 2500, 6000, 15000])
    {
        const { a0, b0, raw } = genPipe(rng, { len: 80, start: 0, edgeGap: () => g, pReverse: 0, pBounce: 0, bounceGap: () => 0 });
        pipeCase(`pipe clean gap=${g}us`, a0, b0, 1000, raw);
    }

    // Bounce: sub-ms glitches (filtered out) and some slower ones (taken as real reversals)
    for (let n = 0; n < 14; n++)
    {
        const grid = n % 3 === 0;
        const sub = n % 3 !== 2;
        const { a0, b0, raw } = genPipe(rng, {
            len: between(rng, 150, 700),
            start: between(rng, 0, 3),
            edgeGap: grid ? gapsMs : gapsUs,
            pReverse: pick(rng, [0, 0.02, 0.1]),
            pBounce: pick(rng, [0.1, 0.4, 0.8]),
            bounceGap: grid ? () => pick(rng, [1000, 1000, 2000]) : (sub ? (r) => between(r, 30, 990) : (r) => between(r, 200, 3000)),
            grid,
        });
        pipeCase(`pipe bounce ${n} ${grid ? 'ms-grid' : sub ? 'us sub-ms' : 'us mixed'}`, a0, b0, 1000, raw);
    }

    // Random raw edges on both pins (pulses around the filter time)
    for (let n = 0; n < 8; n++)
    {
        const grid = n % 2 === 0;
        const len = between(rng, 200, 700);
        const [a0, b0] = [between(rng, 0, 1), between(rng, 0, 1)];
        const level = [a0, b0];
        const raw = [];
        let t = T0;
        for (let i = 0; i < len; i++)
        {
            t += grid ? between(rng, 1, 6) * 1000 : pick(rng, [between(rng, 20, 999), 1000, 1001, between(rng, 1000, 5000)]);
            const pin = rng() < 0.5 ? 0 : 1;
            level[pin] ^= 1;
            raw.push([t, pin, level[pin]]);
        }
        pipeCase(`pipe random ${n} ${grid ? 'ms-grid' : 'us'}`, a0, b0, 1000, raw);
    }
}

// ---- filter cases: one pin -----------------------------------------------------------------

function filterCase(name, init, steady, raw)
{
    cases.push({ kind: 'filter', name, init, steady_us: steady, events: raw, accepted: filterModel(init, steady, raw) });
}

function addFilterCases()
{
    const rng = mulberry32(4242);
    for (const steady of [1000, 5000])
        for (const init of [0, 1])
            for (let n = 0; n < 4; n++)
            {
                const raw = [];
                let t = T0, level = init;
                for (let i = 0; i < 400; i++)
                {
                    const w = pick(rng, [steady - 1, steady, steady + 1, between(rng, 1, steady - 1), between(rng, steady, 6 * steady), steady * 40]);
                    t += n === 0 ? Math.max(1, Math.round(w / 1000)) * 1000 : w;
                    level ^= 1;
                    raw.push([t, level]);
                }
                filterCase(`filter steady=${steady} init=${init} run=${n}${n === 0 ? ' ms-grid' : ''}`, init, steady, raw);
            }
    // A single edge, and none
    filterCase('filter single edge', 0, 5000, [[T0, 1]]);
    filterCase('filter pulse exactly steady', 1, 5000, [[T0, 0], [T0 + 5000, 1]]);
    filterCase('filter pulse just short', 1, 5000, [[T0, 0], [T0 + 4999, 1]]);
}

addQuadCases();
addPipeCases();
addFilterCases();

const here = dirname(fileURLToPath(import.meta.url));
mkdirSync(join(here, 'data'), { recursive: true });
const lines = cases.map(c => JSON.stringify(c));
// One case per line (still plain JSON): the C++ test reads it line by line
const text = '{"format":"urem-vectors-1","cases":[\n' + lines.join(',\n') + '\n]}\n';
writeFileSync(join(here, 'data', 'urem_vectors.json'), text);
const nTicks = cases.reduce((s, c) => s + (c.ticks ? c.ticks.length : 0), 0);
console.log(`${cases.length} cases, ${nTicks} ticks, ${text.length} bytes`);
