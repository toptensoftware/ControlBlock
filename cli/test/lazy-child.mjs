// Run by cli.test.js under block-image-stack.mjs: the commands that don't bake or render, against
// a FakeDevice, each through main(). Prints { commands: [{ argv, code, err }], blocked } as JSON,
// where `blocked` is the error from importing the whole library (proves the hook works).
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { main } from "../cli.js";
import { FakeDevice, fakeOpener, layoutBlobOf } from "../../host/test/FakeDevice.js";

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cb-lazy-"));
const commands = [];
try
{
    const bin = path.join(dir, "baked.bin");
    fs.writeFileSync(bin, layoutBlobOf(3000));
    const dev = new FakeDevice({ name: {}, clock: {}, wifi: {}, layout: { restartMs: 30 } });
    const io = { open: fakeOpener(dev), reconnect: { timeoutMs: 3000, intervalMs: 10 } };
    for (const argv of [["--version"], ["info"], ["wifi", "status"], ["time", "get"], ["name"], ["upload", bin]])
    {
        const err = [];
        const code = await main(argv, { ...io, stdout: () => { }, stderr: l => err.push(l) });
        commands.push({ argv, code, err });
    }
}
finally
{
    fs.rmSync(dir, { recursive: true, force: true });
}

let blocked = null;
try
{
    await import("../../host/index.js");
}
catch (err)
{
    blocked = err.message;
}

console.log(JSON.stringify({ commands, blocked }));
