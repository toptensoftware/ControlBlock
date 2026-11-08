// urem2's look: the root defaults (a bordered black face, a grey pressed fill) and the icon/label helpers that turn
// urem's image props (colour, scale, offset, font, alignment) into ControlBlock look properties.

export const BORDER = "#a0a0a0";
export const PRESSED = "#606060";

// Opaque black with a border; an explicit pressed face (no 35% overlay) in mid grey, the border kept
export const ROOT_DEFAULTS = {
    face: { top: "#000000", bottom: "#000000", border: BORDER, radius: 14 },
    pressed: { face: { top: PRESSED, bottom: PRESSED } },
};

// urem's colours (CSS names kept as written)
export const COLORS = {
    white: "white", orange: "orange", purple: "purple", green: "green", red: "red", yellow: "yellow", blue: "blue",
    grey: "#606060",
};

// Built-in icons fill ~99 px of the 128 px key at scale 1, over the 80-96 px target: every icon scale is multiplied
// by this (inheritance replaces iconScale, so it can't be a default). Tune on the hardware.
export const ICON_SCALE = 0.9;

const VALIGNS = ["top", "center", "bottom"];

function checkOptions(opts, allowed, what)
{
    for (const k of Object.keys(opts))
        if (!allowed.includes(k))
            throw new Error(`unknown ${what} option ${JSON.stringify(k)}`);
}

function colour(c)
{
    if (!Object.values(COLORS).includes(c))
        throw new Error(`unknown colour ${JSON.stringify(c)}`);
    return c;
}

const offset = ({ x = 0, y = 0 }) => ({ x, y });

// Round away float noise (0.9 * 0.7 = 0.6299999...)
const round = n => Math.round(n * 1e6) / 1e6;

// name: "#builtin" or an .svg path relative to urem2/. opts: { color, scale, offset: { x, y } }
export function icon(name, opts = {})
{
    if (typeof name !== "string" || !(name.startsWith("#") || name.toLowerCase().endsWith(".svg")))
        throw new Error(`icon must be "#name" or an .svg path, got ${JSON.stringify(name)}`);
    checkOptions(opts, ["color", "scale", "offset"], "icon");
    return {
        icon: name,
        ...(opts.color !== undefined && { iconColor: colour(opts.color) }),
        iconScale: round(ICON_SCALE * (opts.scale ?? 1)),
        ...(opts.offset !== undefined && { iconOffset: offset(opts.offset) }),
    };
}

// opts: { color, font, valign, offset: { x, y } }
export function label(text, opts = {})
{
    if (typeof text !== "string" || text === "")
        throw new Error("label must be a non-empty string");
    checkOptions(opts, ["color", "font", "valign", "offset"], "label");
    if (opts.valign !== undefined && !VALIGNS.includes(opts.valign))
        throw new Error(`label valign must be one of ${VALIGNS.join(", ")}`);
    return {
        label: text,
        ...(opts.color !== undefined && { labelColor: colour(opts.color) }),
        ...(opts.font !== undefined && { font: opts.font }),
        ...(opts.valign !== undefined && { labelVAlign: opts.valign }),
        ...(opts.offset !== undefined && { labelOffset: offset(opts.offset) }),
    };
}
