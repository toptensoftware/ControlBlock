// The command line of cli.js, parsed (pure, so it's tested). Every mistake throws a
// UsageError, whose message ends with the usage; cli.js exits 2 on one.

// How long `wifi set` and `wifi wait` wait for the connection by default (seconds)
export const WIFI_WAIT_S = 30;

export const USAGE = `usage: node cli/cli.js <command> ...
  bake <layout.json|.js|.mjs> [-o out.bin] [--upload [--serial S] [--force]]
      bake a layout; writes <layout name>.bin next to it, unless -o names the file
      or --upload is given without -o
  upload <layout.json|.js|.mjs|baked.bin> [--serial S] [--force]
      upload to the device (a layout is baked in memory first); --force sends it
      even if the device already has it
  wifi set <ssid> [password] | get | clear | status | wait [seconds]  [--serial S]
      the device's Wi-Fi network: set stores one (no password: an open network) and
      waits for the connection; get shows it (password in clear text); clear forgets
      it; wait waits until connected (default ${WIFI_WAIT_S} s)
  time [set] | get | zone <posix TZ>|--default | server <host>|--default  [--serial S]
      the device's clock: set (the default) sets it from this computer's clock; get
      shows it; zone and server store the time zone (a POSIX TZ string, default UTC0)
      and the NTP server (default pool.ntp.org); --default puts one back
  --serial S picks the device by serial number (default: the first one found)`;

export class UsageError extends Error
{
    constructor(message)
    {
        super(message ? `${message}\n${USAGE}` : USAGE);
        this.name = "UsageError";
    }
}

const SERIAL = { "--serial": "serial" };
const FORCE = { "--force": "force" };

// Splits argv into positionals and options. `values` and `flags` map an option to its
// name in `opts` (a value option takes the next argument). "--" ends the options.
function scan(argv, { values = {}, flags = {} } = {})
{
    const opts = {}, pos = [];
    for (let i = 0; i < argv.length; i++)
    {
        const arg = argv[i];
        if (arg === "--")
        {
            pos.push(...argv.slice(i + 1));
            break;
        }
        if (Object.hasOwn(values, arg))
        {
            if (i + 1 >= argv.length || argv[i + 1].startsWith("-"))
                throw new UsageError(`${arg} needs a value`);
            opts[values[arg]] = argv[++i];
        }
        else if (Object.hasOwn(flags, arg))
            opts[flags[arg]] = true;
        else if (arg.startsWith("-"))
            throw new UsageError(`unknown option ${arg}`);
        else
            pos.push(arg);
    }
    return { opts, pos };
}

// The single positional argument, or a usage error saying what's wanted
function single(pos, what)
{
    if (pos.length !== 1)
        throw new UsageError(what);
    return pos[0];
}

const isBlob = file => /\.bin$/i.test(file);

function parseBake(argv)
{
    const { opts, pos } = scan(argv, { values: { "-o": "out", "--out": "out", ...SERIAL }, flags: { "--upload": "upload", ...FORCE } });
    const input = single(pos, "bake needs one layout file");
    if (isBlob(input))
        throw new UsageError(`${input} is already baked: use upload`);
    if (!opts.upload && (opts.serial !== undefined || opts.force))
        throw new UsageError("--serial and --force only apply with --upload");
    return { command: "bake", input, out: opts.out, upload: !!opts.upload, serial: opts.serial, force: !!opts.force };
}

function parseUpload(argv)
{
    const { opts, pos } = scan(argv, { values: SERIAL, flags: FORCE });
    const input = single(pos, "upload needs one layout or .bin file");
    return { command: "upload", input, binary: isBlob(input), serial: opts.serial, force: !!opts.force };
}

// An action that takes no arguments (or options beyond --serial)
function none(name)
{
    return (pos, opts) => {
        if (pos.length)
            throw new UsageError(`${name} takes no arguments`);
        if (opts.default)
            throw new UsageError(`--default only applies to time zone and time server`);
        return {};
    };
}

// A command made of actions (`wifi set ...`): `actions` maps each action to a function of
// its positional arguments and the options, which checks them and returns them named.
// `scanOptions` are scan()'s beyond --serial; `defaultAction` is used when none is given.
function actionCommand(command, actions, { scanOptions = {}, defaultAction } = {})
{
    return argv => {
        const { opts, pos } = scan(argv, { ...scanOptions, values: { ...SERIAL, ...scanOptions.values } });
        const [action = defaultAction, ...rest] = pos;
        if (action === undefined)
            throw new UsageError(`${command} needs one of ${Object.keys(actions).join(", ")}`);
        if (!Object.hasOwn(actions, action))
            throw new UsageError(`unknown ${command} command ${action}`);
        return { command, action, serial: opts.serial, ...actions[action](rest, opts) };
    };
}

// wifi <action> ...
const parseWifi = actionCommand("wifi", {
    set(pos)
    {
        if (pos.length < 1 || pos.length > 2)
            throw new UsageError("wifi set needs an ssid and, unless the network is open, a password");
        return { ssid: pos[0], password: pos[1] ?? "", seconds: WIFI_WAIT_S };
    },
    get: none("wifi get"),
    clear: none("wifi clear"),
    status: none("wifi status"),
    wait(pos)
    {
        const seconds = Number(pos[0] ?? WIFI_WAIT_S);
        if (pos.length > 1 || !(seconds > 0) || !Number.isFinite(seconds))
            throw new UsageError("wifi wait takes a number of seconds (more than 0)");
        return { seconds };
    },
});

// time zone/server: one value, or --default ("": the device's default)
function setting(name, what)
{
    return (pos, opts) => {
        if (pos.length !== (opts.default ? 0 : 1))
            throw new UsageError(`${name} needs ${what} or --default`);
        return { value: opts.default ? "" : pos[0] };
    };
}

// time [<action>] ...
const parseTime = actionCommand("time", {
    set: none("time set"),
    get: none("time get"),
    zone: setting("time zone", "a POSIX TZ string"),
    server: setting("time server", "a host name"),
}, { scanOptions: { flags: { "--default": "default" } }, defaultAction: "set" });

const COMMANDS = { bake: parseBake, upload: parseUpload, wifi: parseWifi, time: parseTime };

export function parseArgs(argv)
{
    const [command, ...rest] = argv;
    if (command === undefined)
        throw new UsageError();
    if (!Object.hasOwn(COMMANDS, command))
        throw new UsageError(`unknown command ${command}`);
    return COMMANDS[command](rest);
}
