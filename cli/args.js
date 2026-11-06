// The command line of cli.js, parsed (pure, so it's tested). Every mistake throws a
// UsageError, whose message ends with the usage; cli.js exits 2 on one.

// How long `wifi set` and `wifi wait` wait for the connection by default (seconds)
export const WIFI_WAIT_S = 30;

export const USAGE = `usage: node cli/cli.js <command> ...
  bake <config.json|.js|.mjs> [-o out.bin] [--upload [--serial S] [--force]]
      bake a config; writes <config name>.bin next to it, unless -o names the file
      or --upload is given without -o
  upload <config.json|.js|.mjs|baked.bin> [--serial S] [--force]
      upload to the device (a config is baked in memory first); --force sends it
      even if the device already has it
  wifi set <ssid> [password] | get | clear | status | wait [seconds]  [--serial S]
      the device's Wi-Fi network: set stores one (no password: an open network) and
      waits for the connection; get shows it (password in clear text); clear forgets
      it; wait waits until connected (default ${WIFI_WAIT_S} s)
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
    const input = single(pos, "bake needs one config file");
    if (isBlob(input))
        throw new UsageError(`${input} is already baked: use upload`);
    if (!opts.upload && (opts.serial !== undefined || opts.force))
        throw new UsageError("--serial and --force only apply with --upload");
    return { command: "bake", input, out: opts.out, upload: !!opts.upload, serial: opts.serial, force: !!opts.force };
}

function parseUpload(argv)
{
    const { opts, pos } = scan(argv, { values: SERIAL, flags: FORCE });
    const input = single(pos, "upload needs one config or .bin file");
    return { command: "upload", input, binary: isBlob(input), serial: opts.serial, force: !!opts.force };
}

// wifi <action> ...: each action's positional arguments, checked and named
const WIFI_ACTIONS = {
    set(pos)
    {
        if (pos.length < 1 || pos.length > 2)
            throw new UsageError("wifi set needs an ssid and, unless the network is open, a password");
        return { ssid: pos[0], password: pos[1] ?? "", seconds: WIFI_WAIT_S };
    },
    get: none("get"),
    clear: none("clear"),
    status: none("status"),
    wait(pos)
    {
        const seconds = Number(pos[0] ?? WIFI_WAIT_S);
        if (pos.length > 1 || !(seconds > 0) || !Number.isFinite(seconds))
            throw new UsageError("wifi wait takes a number of seconds (more than 0)");
        return { seconds };
    },
};

function none(action)
{
    return pos => {
        if (pos.length)
            throw new UsageError(`wifi ${action} takes no arguments`);
        return {};
    };
}

function parseWifi(argv)
{
    const { opts, pos } = scan(argv, { values: SERIAL });
    const [action, ...rest] = pos;
    if (action === undefined)
        throw new UsageError("wifi needs one of set, get, clear, status, wait");
    if (!Object.hasOwn(WIFI_ACTIONS, action))
        throw new UsageError(`unknown wifi command ${action}`);
    return { command: "wifi", action, serial: opts.serial, ...WIFI_ACTIONS[action](rest) };
}

const COMMANDS = { bake: parseBake, upload: parseUpload, wifi: parseWifi };

export function parseArgs(argv)
{
    const [command, ...rest] = argv;
    if (command === undefined)
        throw new UsageError();
    if (!Object.hasOwn(COMMANDS, command))
        throw new UsageError(`unknown command ${command}`);
    return COMMANDS[command](rest);
}
