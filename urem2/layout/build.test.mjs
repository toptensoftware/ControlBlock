// Tests for the whole layout: buildLayout(target) for both targets resolves clean, keeps the activity and device
// order, talks only to its own target's addresses, and the entry files are exactly buildLayout of their target.
// From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import { buildLayout } from "./build.mjs";
import { targets } from "../targets.mjs";
import { cleanLayout, UREM2_DIR } from "../test/resolve.mjs";
import { loadLayoutFile, OP } from "../../host/index.js";

// Every packed op anywhere in the resolved tree (bindings, device lists, hooks)
function* allOps(v)
{
    if (Array.isArray(v))
        for (const x of v)
            yield* allOps(x);
    else if (v && typeof v === "object" && !Buffer.isBuffer(v))
    {
        if (typeof v.op === "number")
            yield v;
        for (const x of Object.values(v))
            yield* allOps(x);
    }
}

// The hosts a target names (IR blaster, ATV daemon, Yamaha, HOT PC, slideshow)
const targetHosts = t => new Set([t.irtx.host, t.atvd.host, new URL(t.yamaha).hostname, t.hotpc.host, t.slideshow.host]);
const urlHosts = text => [...text.matchAll(/http:\/\/([^:/"'\s]+)/g)].map(m => m[1]);

for (const [name, target] of Object.entries(targets))
{
    const own = targetHosts(target);
    const others = new Set(Object.entries(targets).filter(([n]) => n !== name)
        .flatMap(([, t]) => [...targetHosts(t)]).filter(h => !own.has(h)));

    test(`${name}: buildLayout is clean, activities and devices in order`, async () => {
        const tree = await cleanLayout(buildLayout(target));
        assert.deepEqual(tree.activities.map(a => a.name), ["atv", "hdd", "steam", "playstation"]);
        assert.deepEqual(tree.devices.map(d => d.name), ["yama", "lgtv", "atv", "hotpc"]);
    });

    test(`${name}: every op talks to this target only`, async () => {
        const tree = await cleanLayout(buildLayout(target));
        const ops = [...allOps(tree)];
        const hosts = new Set();
        for (const o of ops)
        {
            if (o.op === OP.udp_packet)
                hosts.add(o.host);
            else if (o.op === OP.http_get || o.op === OP.http_post)
                hosts.add(new URL(o.url).hostname);
            else if (o.op === OP.script)
                urlHosts(o.source).forEach(h => hosts.add(h));
        }
        assert.ok(ops.some(o => o.op === OP.send_wol), "the HOT PC wake is there");
        assert.ok(hosts.size > 0);
        for (const h of hosts)
        {
            assert.ok(own.has(h), `${h} is one of ${name}'s hosts`);
            assert.ok(!others.has(h), `${h} is not another target's`);
        }
    });

    test(`${name}: three distinct Lua sources (mute toggle, 7ch toggle, volume)`, async () => {
        const tree = await cleanLayout(buildLayout(target));
        const sources = new Set([...allOps(tree)].filter(o => o.op === OP.script).map(o => o.source));
        assert.equal(sources.size, 3);
    });
}

test("the entry files are buildLayout of their target", async () => {
    assert.deepEqual(await loadLayoutFile(path.join(UREM2_DIR, "urem2.mjs")), buildLayout(targets.real));
    assert.deepEqual(await loadLayoutFile(path.join(UREM2_DIR, "urem2-test.mjs")), buildLayout(targets.test));
});
