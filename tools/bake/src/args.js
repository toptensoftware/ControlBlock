// Command line of cli.js, parsed (pure, so it's tested).

export const USAGE = `usage: node cli.js <config.json|config.js> [-o out.bin] [--header types.h] [--upload [--serial S] [--force]]
       node cli.js <baked.bin> [--serial S] [--force]       (uploads a blob that is already baked)`;

export function parseArgs(argv)
{
    const a = { input: undefined, out: undefined, header: undefined, upload: false, serial: undefined, force: false, binary: false };
    const value = i => {
        if (i + 1 >= argv.length || argv[i + 1].startsWith("-"))
            throw new Error(`${argv[i]} needs a value\n${USAGE}`);
        return argv[i + 1];
    };
    for (let i = 0; i < argv.length; i++)
    {
        const arg = argv[i];
        if (arg === "-o" || arg === "--out") a.out = value(i++);
        else if (arg === "--header") a.header = value(i++);
        else if (arg === "--serial") a.serial = value(i++);
        else if (arg === "--upload") a.upload = true;
        else if (arg === "--force") a.force = true;
        else if (arg.startsWith("-")) throw new Error(`unknown option ${arg}\n${USAGE}`);
        else if (a.input === undefined) a.input = arg;
        else throw new Error(USAGE);
    }
    if (a.input === undefined)
        throw new Error(USAGE);

    // A baked blob has one use
    if (/\.bin$/i.test(a.input))
    {
        if (a.out !== undefined || a.header !== undefined)
            throw new Error(`${a.input} is already baked: -o and --header don't apply\n${USAGE}`);
        a.binary = true;
        a.upload = true;
    }
    if (!a.upload && (a.serial !== undefined || a.force))
        throw new Error(`--serial and --force only apply with --upload\n${USAGE}`);
    return a;
}
