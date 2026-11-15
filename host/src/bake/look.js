// Look properties: validation, inheritance, the four layer definitions per key (face and
// icon/label, normal and pressed) and the key's two image specs built from them. A layer
// definition is a button-face definition ({ virtualSize?, layers: [...] }, drawn on a transparent
// background); stage 2 renders each image spec (its layers composed over black at the key's pixel
// size). See docs/layout-format.md "Button definition" and "Inheritance and defaults".

import { FACE_SIZE, FACE_MARGIN, LONG_PRESS_MIN, LONG_PRESS_MAX } from "./consts.js";
import { childPath } from "./diag.js";
import { t, required, checkObject } from "./check.js";

const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);

// ------------------------------------------------------------ validation

const color = t.string({ nonEmpty: true });
const xy = t.object({ x: t.number(), y: t.number() });
// > 0: button-face rejects a zero or negative scale and line spacing
const positive = (v, p, d) => t.number()(v, p, d) && (v > 0 || (d.error(p, "must be greater than 0"), false));
const numbers = (...counts) => (v, p, d) =>
    Array.isArray(v) && counts.includes(v.length) && v.every(n => typeof n === "number")
        ? true : (d.error(p, `expected a number or an array of ${counts.join(" or ")} numbers`), false);
const padding = (v, p, d) => typeof v === "number" ? true : numbers(2, 4)(v, p, d);

const FACE_NAMES = ["rounded", "square", "none"];
const FACE_OBJECT = { top: color, bottom: color, border: color, radius: t.number({ min: 0 }), padding: t.number({ min: 0 }) };
const face = (v, p, d) =>
    typeof v === "string" ? t.oneOf(...FACE_NAMES)(v, p, d)
    : isObject(v) ? checkObject(v, FACE_OBJECT, p, d)
    : (d.error(p, `expected ${FACE_NAMES.join(", ")} or an object`), false);

const LAYERS = ["normalBg", "normalFg", "pressedBg", "pressedFg"];
// A button-face definition, checked only for its shape ({ layers: [{ type, ... }] }): button-face
// validates the rest when it renders
const definition = (v, p, d) => {
    if (!Array.isArray(v.layers))
        return d.error(Object.hasOwn(v, "layers") ? childPath(p, "layers") : p,
            "expected a button-face definition: { virtualSize?, layers: [{ type, ... }] }"), false;
    let good = true;
    v.layers.forEach((l, i) => {
        if (!isObject(l) || typeof l.type !== "string")
            good = (d.error(childPath(childPath(p, "layers"), i), "expected a layer object with a type"), false);
    });
    return good;
};
const layer = (v, p, d) =>
    typeof v === "string" ? (v.toLowerCase().endsWith(".svg") ? true : (d.error(p, "only .svg files are supported for now"), false))
    : isObject(v) ? definition(v, p, d)
    : (d.error(p, "expected an .svg path, a button-face definition or null"), false);
const image = t.object(Object.fromEntries(LAYERS.map(n => [n, t.nullable(layer)])));

// Every look property may be null: that clears an inherited value
const nullable = spec => Object.fromEntries(Object.entries(spec).map(([k, c]) => [k, t.nullable(c)]));

const LOOK = nullable({
    face, color,
    icon: t.string({ nonEmpty: true }), iconColor: color, iconScale: positive, iconOffset: xy, iconText: t.string(),
    labelColor: color, labelAlign: t.oneOf("left", "center", "right"), labelVAlign: t.oneOf("top", "center", "bottom"),
    font: t.string({ nonEmpty: true }), labelPadding: padding, labelOffset: xy, labelLineSpacing: positive,
});
const LABEL = nullable({ label: t.string() });
const PRESSED = t.object({ ...LOOK, ...LABEL });
const PRESSED_NO_LABEL = t.object(LOOK);

// What a button may carry besides gestures and its label
const LOOK_AND_TIMING = (pressed) => ({
    ...LOOK, pressed: t.nullable(pressed), image: t.nullable(image),
    longPressMs: t.nullable(t.integer(LONG_PRESS_MIN, LONG_PRESS_MAX)),
});
export const BUTTON_LOOK_SPEC = { ...LABEL, ...LOOK_AND_TIMING(PRESSED) };
export const DEFAULTS_SPEC = LOOK_AND_TIMING(PRESSED_NO_LABEL);

// A group's keys: the group draws the face, so a key in it sets only its icon and label
const groupFace = (v, p, d) => (d.error(p, "a group's keys take their face from the group: set it on the group"), false);
const NO_FACE = { face: groupFace, color: groupFace };
export const MEMBER_LOOK_SPEC = { ...BUTTON_LOOK_SPEC, ...NO_FACE, image: groupFace, pressed: t.nullable(t.object({ ...LOOK, ...LABEL, ...NO_FACE })) };

// The part of a group's look its keys inherit: not what the group draws itself (its icon and label)
export function groupLookForKeys(look)
{
    const { icon, iconText, label, image, pressed, ...rest } = look;
    if (pressed === undefined || pressed === null)
        return pressed === null ? { ...rest, pressed } : rest;
    const { icon: _i, iconText: _t, label: _l, ...pressedRest } = pressed;
    return { ...rest, pressed: pressedRest };
}

// ------------------------------------------------------------ inheritance

// Recursive merge; `over` wins, objects merge, anything else (including arrays and null) replaces
export function mergeLook(base, over)
{
    const out = { ...base };
    for (const [k, v] of Object.entries(over ?? {}))
        out[k] = isObject(v) && isObject(out[k]) ? mergeLook(out[k], v) : v;
    return out;
}

// Remove nulls (cleared values) and empty objects left by clearing
function stripNulls(value)
{
    if (!isObject(value))
        return value;
    const out = {};
    for (const [k, v] of Object.entries(value))
    {
        // an `image` layer set to null means "no layer": keep it
        const s = k === "image" ? v : stripNulls(v);
        if (s !== null && s !== undefined)
            out[k] = s;
    }
    return out;
}

// chain: defaults objects from the root down; own: the button's look part. -> resolved look
export function inheritLook(chain, own)
{
    let look = {};
    for (const level of chain)
        look = mergeLook(look, level);
    return stripNulls(mergeLook(look, own));
}

// ------------------------------------------------------------ images
const DEFAULT_FACE_COLOR = "#2b2f38";
const FACE_PADDING = 4, FACE_RADIUS = 14;
const VIRTUAL_SIZE = 72;      // button-face's default virtual size, for a FACE_SIZE square
const ICON_GREY = "#848484";  // what button-face's built-in icons are drawn in: mapped to the icon's colour

// The face area of a key (or group) `width` x `height` px whose image starts `left`, `top` px into
// it: the rect less FACE_MARGIN of black each side, in the image's coordinates
export const faceBox = (width, height, left = 0, top = 0) =>
    ({ x: FACE_MARGIN - left, y: FACE_MARGIN - top, width: width - 2 * FACE_MARGIN, height: height - 2 * FACE_MARGIN });

// The key face as SVG, `width` x `height` px. `overlay` darkens it (the default pressed look).
function faceSvg({ top, bottom, border, radius, padding: pad, shade, overlay }, { width, height })
{
    const rect = (extra) => `<rect x="${pad}" y="${pad}" width="${width - 2 * pad}" height="${height - 2 * pad}" rx="${radius}" ${extra}/>`;
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}">` +
        `<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${top}"/><stop offset="1" stop-color="${bottom}"/></linearGradient>` +
        `<linearGradient id="s" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity=".3"/></linearGradient></defs>` +
        rect(`fill="url(#g)"` + (border ? ` stroke="${border}" stroke-width="2"` : "")) +
        (shade ? rect(`fill="url(#s)"`) : "") +
        (overlay ? rect(`class="pressed-overlay" fill="#000" fill-opacity=".35"`) : "") +
        `</svg>`;
}

function faceDef(look, overlay, size)
{
    const shape = typeof look.face === "string" ? look.face : "rounded";
    if (shape === "none" || (look.face === undefined && look.color === undefined))
        return null;
    const f = isObject(look.face) ? look.face : {};
    const base = look.color ?? DEFAULT_FACE_COLOR;
    return {
        layers: [{
            type: "svg",
            svg: faceSvg({
                top: f.top ?? base,
                bottom: f.bottom ?? base,
                border: f.border,
                radius: shape === "square" ? 0 : f.radius ?? FACE_RADIUS,
                padding: f.padding ?? FACE_PADDING,
                shade: f.top === undefined && f.bottom === undefined,   // a plain colour gets a gentle top-to-bottom shade
                overlay,
            }, size),
        }],
    };
}

// Icon (an svg layer) plus label (a text layer over it) on a transparent background, drawn
// `size` px. Virtual pixels stay the same size as on a 1U key's FACE_SIZE face, so text and
// offsets don't stretch on wider or shorter keys (button-face sizes the icon from the height).
function foregroundDef(look, rc, size)
{
    if (look.icon === undefined && look.label === undefined)
        return null;
    const hasIcon = look.icon !== undefined, hasLabel = look.label !== undefined;
    const def = {};
    if (size.width !== FACE_SIZE || size.height !== FACE_SIZE)
        def.virtualSize = { width: VIRTUAL_SIZE * size.width / FACE_SIZE, height: VIRTUAL_SIZE * size.height / FACE_SIZE };
    def.layers = [];
    if (hasIcon)
    {
        const svg = {
            type: "svg",
            src: look.icon.startsWith("#") ? look.icon : rc.host.resolvePath(look.icon),
            colors: { [ICON_GREY]: look.iconColor ?? "white" },
        };
        if (look.iconScale !== undefined) svg.scale = look.iconScale;
        if (look.iconText !== undefined) svg.text = look.iconText;
        svg.translate = { x: look.iconOffset?.x ?? 0, y: look.iconOffset?.y ?? (hasLabel ? -7 : 0) };
        def.layers.push(svg);
    }
    if (hasLabel)
    {
        const text = {
            type: "text",
            text: look.label,
            color: look.labelColor ?? "white",
            align: look.labelAlign ?? "center",
            valign: look.labelVAlign ?? (hasIcon ? "bottom" : "center"),
            font: look.font ?? (hasIcon ? "bold 11px sans-serif" : "bold 15px sans-serif"),
            padding: look.labelPadding ?? [9, 4],
            translate: { x: look.labelOffset?.x ?? 0, y: look.labelOffset?.y ?? 0 },
        };
        if (look.labelLineSpacing !== undefined) text.lineSpacing = look.labelLineSpacing;
        def.layers.push(text);
    }
    return def;
}

// A file path the layout gives (not a #built-in or @material icon) -> resolved against the
// layout's folder; reports a missing file at `path`
function resolveFile(src, rc, path)
{
    const file = rc.host.resolvePath(src);
    if (!rc.host.exists(file))
        rc.diag.error(path, `file not found: ${src}`);
    return file;
}
const isFileSrc = src => typeof src === "string" && !src.startsWith("#") && !src.startsWith("@");

// An `image` override replaces the layer outright: an .svg path (one svg layer) or a button-face
// definition, its svg layers' file `src`s resolved (a copy: the layout's object is left alone)
function overrideLayer(value, rc, path)
{
    if (typeof value === "string")
        return { layers: [{ type: "svg", src: resolveFile(value, rc, path) }] };
    if (!Array.isArray(value.layers))
        return null;    // already reported by the `image` validator
    const layersPath = childPath(path, "layers");
    return {
        ...value,
        layers: value.layers.map((l, i) => isObject(l) && l.type === "svg" && isFileSrc(l.src)
            ? { ...l, src: resolveFile(l.src, rc, childPath(childPath(layersPath, i), "src")) }
            : l),
    };
}

function checkIcon(look, rc, path)
{
    const icon = look.icon;
    if (icon === undefined)
        return;
    if (icon.startsWith("#"))
    {
        // hasIcon is optional: a host that can't list its built-in icons accepts any name
        if (rc.host.hasIcon && !rc.host.hasIcon(icon.slice(1)))
            rc.diag.error(childPath(path, "icon"), `unknown built-in icon ${icon}`);
    }
    else
        resolveFile(icon, rc, childPath(path, "icon"));
}

// A resolved look -> { normalBg, normalFg, pressedBg, pressedFg }, each a layer definition (drawn
// `size` px: the face box) or null
export function lookImages(look, rc, path, size)
{
    const { pressed, image: override, ...normal } = look;
    checkIcon(normal, rc, path);

    const pressedLook = stripNulls(mergeLook(normal, pressed));
    if (pressed?.icon) checkIcon(pressedLook, rc, childPath(path, "pressed"));
    const explicitFace = pressed !== undefined && (pressed.face !== undefined || pressed.color !== undefined);

    const out = {
        normalBg: faceDef(normal, false, size),
        normalFg: foregroundDef(normal, rc, size),
        pressedBg: faceDef(pressedLook, !explicitFace, size),
        pressedFg: foregroundDef(pressedLook, rc, size),
    };
    for (const name of LAYERS)
        if (override && Object.hasOwn(override, name))
            out[name] = override[name] === null ? null : overrideLayer(override[name], rc, childPath(childPath(path, "image"), name));
    return out;
}

// A key's two images from layer definitions placed in it: `placed` is [{ defs: lookImages(),
// x, y, width, height }] (face boxes), drawn in order, each face then its icon and label.
// -> { normal, pressed }, each { width, height, layers: [{ layer: "bg" | "fg", def, x, y, width,
// height }] } (the key's image, `width` x `height` px, drawn over black) or null if nothing is drawn
export function imageSpecs(placed, width, height)
{
    const spec = (bg, fg) => {
        const layers = placed.flatMap(({ defs, ...box }) =>
            [["bg", defs[bg]], ["fg", defs[fg]]].filter(([, def]) => def).map(([layer, def]) => ({ layer, def, ...box })));
        return layers.length ? { width, height, layers } : null;
    };
    return { normal: spec("normalBg", "normalFg"), pressed: spec("pressedBg", "pressedFg") };
}
