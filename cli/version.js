// The CLI's own version (its package.json's: the product version, shared with the firmware and
// the library) and the check of a device's firmware version against it. Pure but for reading
// package.json; nothing here loads the library.
import fs from "node:fs";

let cached;

// The CLI's version, e.g. "0.1.0"
export function cliVersion()
{
    cached ??= JSON.parse(fs.readFileSync(new URL("./package.json", import.meta.url), "utf8")).version;
    return cached;
}

// "1.2.3", "v1.2.3", "1.2" or any of those with a "-..." / "+..." suffix (a dev build:
// 0.1.0-dev+75ac767.dirty) -> { major, minor, patch }; anything else (a git hash from firmware
// built before versioning) -> null
export function parseVersion(text)
{
    const m = /^v?(\d+)\.(\d+)(?:\.(\d+))?(?:[-+].*)?$/.exec(text ?? "");
    return m ? { major: +m[1], minor: +m[2], patch: +(m[3] ?? 0) } : null;
}

// A line for stderr when the firmware is older (major.minor) than the CLI, or when its version
// can't be compared; null when it's fine. Never an error: protocol_version is the hard check.
export function firmwareVersionNote(firmware, cli = cliVersion())
{
    const fw = parseVersion(firmware), own = parseVersion(cli);
    if (!fw || !own)
        return `note: the device's firmware version "${firmware}" isn't a release version: can't compare it with this CLI's (${cli})`;
    if (fw.major < own.major || (fw.major === own.major && fw.minor < own.minor))
        return `warning: the device's firmware ${firmware} is older than this CLI (${cli}): update the firmware`;
    return null;
}
