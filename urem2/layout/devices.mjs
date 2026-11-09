// urem's devices and activities (core.js): each device's power lists, each activity's devices, Yamaha input and
// display name, switching, and the long-press resync on Home's activity keys (turn every device on again and reset
// the input, from the same lists the switch uses).

import { setPower, setInput } from "../tables/yamaha.mjs";

// The root `devices` map, in order. urem's DVR (pana) has no power control, so it isn't a device.
export function devices({ ops })
{
    return {
        yama: { turnOn: [ops.yamaha(setPower(true))], turnOff: [ops.yamaha(setPower(false))] },
        lgtv: { turnOn: ops.irPower("lg.powerOn"), turnOff: ops.irPower("lg.powerOff") },
        // urem connect()/disconnect() on the Apple TV daemon
        atv: { turnOn: [...ops.irPower("atv.powerOn"), ops.atvPing()],
               turnOff: [...ops.irPower("atv.powerOff"), ops.atvDisconnect()] },
        hotpc: { turnOn: [ops.wake()] },
    };
}

// Activities in order: devices (turned on in this order), the Yamaha input set once they're on, the status bar name
export const ACTIVITIES = {
    atv: { devices: ["yama", "lgtv", "atv"], input: "hdmi2", displayName: "Apple TV" },
    hdd: { devices: ["yama", "lgtv"], input: "hdmi1", displayName: "TV" },
    steam: { devices: ["yama", "lgtv", "hotpc"], input: "hdmi3", displayName: "Steam" },
    playstation: { devices: ["yama", "lgtv"], input: "hdmi4", displayName: "PlayStation" },
};

function activity(name)
{
    if (!Object.hasOwn(ACTIVITIES, name))
        throw new Error(`unknown activity ${JSON.stringify(name)}`);
    return ACTIVITIES[name];
}

export const hooks = ({ ops }, name) => ({ didActivate: [ops.yamaha(setInput(activity(name).input))] });

// The activity's meta, to spread into its definition (pages and bindings are the activity module's)
export function activityMeta(ctx, name)
{
    const { displayName, devices } = activity(name);
    return { displayName, devices: [...devices], hooks: hooks(ctx, name) };
}

// name: an activity, or null for none (everything off, Home)
export function switchTo(name)
{
    if (name !== null)
        activity(name);
    return { op: "switchActivity", activity: name };
}

// A forced resync of `name`: every device's turnOn in the activity's order, then didActivate, then the turnOff of
// every device it doesn't use (in device order; those without a turnOff are skipped). null = no activity: every
// device off.
export function resync(ctx, name)
{
    const devs = devices(ctx);
    const used = name === null ? [] : activity(name).devices;
    const unused = Object.keys(devs).filter(d => !used.includes(d));
    return [
        ...used.flatMap(d => devs[d].turnOn),
        ...(name === null ? [] : hooks(ctx, name).didActivate),
        ...unused.flatMap(d => devs[d].turnOff ?? []),
    ];
}

// Home's activity keys (and the power key, name null): tap switches, long switches and then resyncs (works from
// any activity)
export const activityKeyGestures = (ctx, name) => ({ tap: [switchTo(name)], long: [switchTo(name), ...resync(ctx, name)] });
