// The Yamaha receiver's YamahaExtendedControl (YXC) HTTP GET API, the subset urem used.
// The builders return paths relative to the target's base URL (which ends in YAMAHA_API); setters reply
// {"response_code":0}. urem read only "mute" and "sound_program" from getStatus.

export const YAMAHA_API = "/YamahaExtendedControl/v1";

export const YAMAHA_INPUTS = ["hdmi1", "hdmi2", "hdmi3", "hdmi4"];
export const YAMAHA_PROGRAMS = ["7ch_stereo", "surr_decoder"];

function check(ok, what, value)
{
    if (!ok)
        throw new Error(`bad Yamaha ${what} ${JSON.stringify(value)}`);
}

export function setPower(on)
{
    check(typeof on === "boolean", "power", on);
    return `/main/setPower?power=${on ? "on" : "standby"}`;
}

export function setInput(input)
{
    check(YAMAHA_INPUTS.includes(input), "input", input);
    return `/main/setInput?input=${input}`;
}

// dir "up" | "down"; step a positive integer (urem used 1, 3, 5)
export function setVolume(dir, step = 1)
{
    check(dir === "up" || dir === "down", "volume direction", dir);
    check(Number.isInteger(step) && step >= 1, "volume step", step);
    return `/main/setVolume?volume=${dir}&step=${step}`;
}

export function setMute(on)
{
    check(typeof on === "boolean", "mute", on);
    return `/main/setMute?enable=${on}`;
}

export function setSoundProgram(program)
{
    check(YAMAHA_PROGRAMS.includes(program), "sound program", program);
    return `/main/setSoundProgram?program=${program}`;
}

export function getStatus()
{
    return "/main/getStatus";
}
