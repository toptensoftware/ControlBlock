// Upload a baked blob to the device (docs/protocol.md "Configuration upload"):
// send it, wait for the device to restart, and check what it kept. The
// protocol work is the host library's (host/src/ControlBlock.js); this is the
// command line's flow and messages.

import { ControlBlock } from "../../../host/src/ControlBlock.js";

const PROGRESS_STEP = 10;                       // log every this many percent

// blob: Uint8Array. Options (all optional):
//   serial       which device (default: the first)
//   force        send even if the device already has this exact blob
//   log(line)    progress messages
//   open(opts)   how to open a device (default ControlBlock.open); tests inject a fake
//   reconnect    { timeoutMs, intervalMs } for waiting out the restart
// Resolves { uploaded, verified, size, crc }; throws if anything fails.
export async function uploadBlob(blob, { serial, force = false, log = () => { }, open = o => ControlBlock.open(o), reconnect = {} } = {})
{
    let cb = await open({ serial });
    try
    {
        const id = cb.info.serial;
        log(`device ${id} (firmware ${cb.info.firmwareVersion})`);

        let lastStep = -1;
        const r = await cb.uploadConfig(blob, {
            force,
            onProgress: (sent, total) => {
                const step = Math.floor(sent * 100 / total / PROGRESS_STEP) * PROGRESS_STEP;
                if (step > lastStep) {
                    lastStep = step;
                    log(`  ${step}% (${sent} of ${total} bytes)`);
                }
            },
        });
        if (!r.uploaded)
        {
            log(`device already has this config (${r.size} bytes); nothing to send (--force sends it anyway)`);
            return { uploaded: false, verified: true, size: r.size, crc: r.crc };
        }

        log(`saved ${r.size} bytes; waiting for the device to restart...`);
        const old = cb;
        cb = null;                              // reconnect() closes the old one
        cb = await old.reconnect({ ...reconnect, opener: open });
        // ...and the same device again (a second one on the bus doesn't count)
        if (cb.info.serial !== id)
            throw new Error(`a different device came back (${cb.info.serial}, not ${id})`);
        if (cb.info.configSize !== r.size || cb.info.configCrc !== r.crc)
            throw new Error(`the device restarted with a different config than was sent (${cb.info.configSize} bytes, crc ${cb.info.configCrc}; sent ${r.size} bytes, crc ${r.crc})`);
        log(`device restarted and verified: ${r.size} bytes, crc 0x${r.crc.toString(16).padStart(8, "0")}`);
        return { uploaded: true, verified: true, size: r.size, crc: r.crc };
    }
    finally
    {
        await cb?.close();
    }
}
