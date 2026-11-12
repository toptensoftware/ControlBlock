// Stage 1, touch keys: a page's `keys` list -> the device's keys (id, rect in quarter units, one
// image spec per state) and their touch bindings (docs/layout-format.md "Keys").
//
// Placement: entry i of a page's list goes in slot i of the 5x3 grid (row-major), 1U, unless it
// gives x and y (grid units from the top-left, multiples of 0.25); w and h (default 1) size it.
// A group (an entry with its own `keys`) is one picture, its face and label drawn across its
// rect, split into keys placed relative to the group; each key presses on its own.
//
// Ids are u16 and unique across the layout: a key whose top-left corner is on a whole unit gets
// that slot's plain grid id, page * 15 + slot + 1, with pages numbered across the whole layout
// (Home 0, then each activity's pages in file order); a key may name its own `id`; any other key
// gets the lowest free id above every page's plain grid ids. Touch bindings name the id: touch[id].

import { KEY_COLS, KEY_ROWS, GRID_SLOTS, KEY_QUARTER, MAX_KEYS_PER_PAGE, MAX_KEY_ID, GESTURES } from "./consts.js";
import { childPath } from "./diag.js";
import { t, required, checkProps } from "./check.js";
import { resolveOpList } from "./ops.js";
import { BUTTON_LOOK_SPEC, MEMBER_LOOK_SPEC, inheritLook, groupLookForKeys, lookImages, imageSpecs, faceBox } from "./look.js";

const GRID_QW = KEY_COLS * 4, GRID_QH = KEY_ROWS * 4;      // the grid in quarter units

// A position or size in grid units: a whole number of quarters, at least `min`
const units = min => (v, p, d) =>
    typeof v !== "number" || !Number.isFinite(v) ? (d.error(p, `expected a number of grid units, got ${JSON.stringify(v)}`), false)
    : v < min ? (d.error(p, `must be at least ${min}`), false)
    : !Number.isInteger(v * 4) ? (d.error(p, `must be a multiple of 0.25 (got ${v})`), false)
    : true;
const RECT_SPEC = { x: units(0), y: units(0), w: units(0.25), h: units(0.25) };
const GESTURE_SPEC = Object.fromEntries([...GESTURES, "action"].map(g => [g, t.any]));
const ID_SPEC = { id: t.integer(1, MAX_KEY_ID) };

const BUTTON_SPEC = { ...BUTTON_LOOK_SPEC, ...GESTURE_SPEC, ...ID_SPEC, ...RECT_SPEC };
const GROUP_SPEC = { ...BUTTON_LOOK_SPEC, ...RECT_SPEC, keys: required(t.array(t.any, { min: 1 })) };
const MEMBER_SPEC = { ...MEMBER_LOOK_SPEC, ...GESTURE_SPEC, ...ID_SPEC, ...RECT_SPEC };
const NOT_LOOK = [...GESTURES, "action", "id", "x", "y", "w", "h", "keys"];

const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);
const lookOf = b => Object.fromEntries(Object.entries(b).filter(([k]) => !NOT_LOOK.includes(k)));
const overlaps = (a, b) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
const describe = r => `x ${r.x / 4}, y ${r.y / 4}, w ${r.w / 4}, h ${r.h / 4}`;

// The rect (quarter units) a checked entry asks for. `raw` is the entry as written (a bad x is
// dropped from the checked one, but it was still given); `slot` its default place, or -1 for a
// group's key (x and y each default to 0, the group's corner).
function rectOf(b, raw, slot, path, diag)
{
    const hasX = Object.hasOwn(raw, "x"), hasY = Object.hasOwn(raw, "y");
    const r = { x: 0, y: 0, w: (b.w ?? 1) * 4, h: (b.h ?? 1) * 4 };
    if (slot < 0)
        Object.assign(r, { x: (b.x ?? 0) * 4, y: (b.y ?? 0) * 4 });
    else if (hasX !== hasY)
        diag.error(path, "x and y go together: give both, or neither to use the entry's slot");
    else if (hasX)
        Object.assign(r, { x: (b.x ?? 0) * 4, y: (b.y ?? 0) * 4 });
    else if (slot >= GRID_SLOTS)
        diag.error(path, `has no slot (only the first ${GRID_SLOTS} entries do): give it x and y`);
    else
        Object.assign(r, { x: (slot % KEY_COLS) * 4, y: Math.floor(slot / KEY_COLS) * 4 });
    return r;
}

// Each rect must not overlap an earlier one: items are { rect, path }
function checkOverlaps(items, diag)
{
    items.forEach((a, i) => {
        const b = items.slice(0, i).find(b => overlaps(a.rect, b.rect));
        if (b)
            diag.error(a.path, `overlaps ${b.path}`);
    });
}

// Gestures -> [{ event, ops }] (op lists resolved now; the bindings are made once ids are known)
function gesturesOf(b, path, rc)
{
    if (b.action !== undefined && b.tap !== undefined)
        rc.diag.error(path, "action and tap are the same gesture; set only one");
    if (b.long !== undefined && b.tap === undefined && b.action === undefined)
        rc.diag.warn(path, "has a long gesture but no tap");
    const gestures = { ...b, tap: b.tap ?? b.action };
    const tapName = b.tap !== undefined ? "tap" : "action";
    return GESTURES.map(g => ({ event: g, ops: resolveOpList(gestures[g], childPath(path, g === "tap" ? tapName : g), rc, g) }))
                   .filter(g => g.ops.length);
}

// A key: { key, path, gestures, explicitId, slotId }. `rect` is absolute (quarter units); `placed`
// adds layers drawn under the key's own (a group's picture, in the key's coordinates).
function makeKey(b, rect, look, path, page, rc, placed = [], ownFace = true)
{
    const width = rect.w * KEY_QUARTER, height = rect.h * KEY_QUARTER;
    const box = faceBox(width, height);
    const defs = lookImages(look, rc, path, box);
    const own = ownFace ? defs : { ...defs, normalBg: null, pressedBg: null };
    const { normal, pressed } = imageSpecs([...placed, { defs: own, ...box }], width, height);
    const onSlot = rect.x % 4 === 0 && rect.y % 4 === 0;
    return {
        key: { id: 0, rect: [rect.x, rect.y, rect.w, rect.h], normal, pressed, longPressMs: look.longPressMs ?? 0 },
        path,
        gestures: gesturesOf(b, path, rc),
        explicitId: b.id ?? null,
        slotId: onSlot ? page * GRID_SLOTS + (rect.y / 4) * KEY_COLS + rect.x / 4 + 1 : null,
    };
}

// A group: its keys, with the group's face and label drawn across them
function resolveGroup(g, raw, slot, path, chain, page, rc)
{
    const { diag } = rc;
    const rect = rectOf(g, raw, slot, path, diag);
    const own = lookOf(g);
    const look = inheritLook(chain, own);
    const width = rect.w * KEY_QUARTER, height = rect.h * KEY_QUARTER;
    const groupDefs = lookImages(look, rc, path, faceBox(width, height));
    const memberChain = [...chain, groupLookForKeys(own)];

    const members = [], keys = [];
    let area = 0;
    (g.keys ?? []).forEach((m, i) => {
        const mp = childPath(childPath(path, "keys"), i);
        const b = checkProps(m, MEMBER_SPEC, mp, diag).value;
        if (!b)
            return;
        const r = rectOf(b, m, -1, mp, diag);
        if (r.x + r.w > rect.w || r.y + r.h > rect.h)
            diag.error(mp, `${describe(r)} is outside its group (w ${rect.w / 4}, h ${rect.h / 4})`);
        members.push({ rect: r, path: mp });
        area += r.w * r.h;
        // the group's picture, placed so the part under this key shows
        const placed = [{ defs: groupDefs, ...faceBox(width, height, r.x * KEY_QUARTER, r.y * KEY_QUARTER) }];
        const abs = { ...r, x: rect.x + r.x, y: rect.y + r.y };
        keys.push(makeKey(b, abs, inheritLook(memberChain, lookOf(b)), mp, page, rc, placed, false));
    });
    checkOverlaps(members, diag);
    if (members.length && area < rect.w * rect.h)
        diag.warn(path, "part of the group is not covered by its keys (that part shows black)");
    return { rect, keys };
}

// One page's keys. `chain`: defaults blocks from the root down; `page`: the page's number across
// the whole layout (Home 0). -> [{ key, path, gestures, explicitId, slotId }] in layout order.
export function resolvePage(list, path, chain, page, rc)
{
    const { diag } = rc;
    const entries = list === undefined ? [] : Array.isArray(list) ? list : (diag.error(path, "expected an array"), []);
    const items = [], keys = [];
    entries.forEach((entry, i) => {
        const p = childPath(path, i);
        if (entry === null)
            return;
        if (isObject(entry) && Object.hasOwn(entry, "keys"))
        {
            const g = checkProps(entry, GROUP_SPEC, p, diag).value;
            const r = resolveGroup(g, entry, i, p, chain, page, rc);
            items.push({ rect: r.rect, path: p });
            keys.push(...r.keys);
            return;
        }
        const b = checkProps(entry, BUTTON_SPEC, p, diag).value;
        if (!b)
            return;
        const rect = rectOf(b, entry, i, p, diag);
        items.push({ rect, path: p });
        keys.push(makeKey(b, rect, inheritLook(chain, lookOf(b)), p, page, rc));
    });

    for (const { rect, path: p } of items)
        if (rect.x + rect.w > GRID_QW || rect.y + rect.h > GRID_QH)
            diag.error(p, `${describe(rect)} is outside the ${KEY_COLS}x${KEY_ROWS} grid`);
    checkOverlaps(items, diag);
    if (keys.length > MAX_KEYS_PER_PAGE)
        diag.error(path, `at most ${MAX_KEYS_PER_PAGE} keys a page, got ${keys.length}`);
    warnDuplicates(keys, diag);
    return keys;
}

// Two keys on a page with identical images and actions: a likely copy-paste
function warnDuplicates(keys, diag)
{
    const seen = new Map();
    for (const { key, path, gestures } of keys)
    {
        if (!key.normal && !key.pressed && gestures.length === 0)
            continue;
        const signature = JSON.stringify([key.normal, key.pressed, key.longPressMs, gestures]);
        if (seen.has(signature))
            diag.warn(path, `is the same as ${seen.get(signature)}`);
        else
            seen.set(signature, path);
    }
}

// Gives every key of every page its id. `pages`: each page's resolvePage result, pages in layout order.
export function assignIds(pages, diag)
{
    const used = new Map();     // id -> the path of the key that has it
    const all = pages.flat();
    for (const k of all)
    {
        const id = k.explicitId ?? k.slotId;
        if (id === null)
            continue;
        if (used.has(id))
            k.explicitId !== null
                ? diag.error(childPath(k.path, "id"), `id ${id} is already used by ${used.get(id)}`)
                : diag.error(k.path, `its slot's id ${id} is already used by ${used.get(id)}: give one of them another id`);
        else
            used.set(id, k.path);
        k.key.id = id;
    }
    let next = pages.length * GRID_SLOTS + 1;
    for (const k of all.filter(k => k.key.id === 0))
    {
        while (used.has(next))
            next++;
        if (next > MAX_KEY_ID)
            diag.error(k.path, `no key id left (ids are 1..${MAX_KEY_ID})`);
        used.set(next, k.path);
        k.key.id = next;
    }
}

// The touch bindings of resolved keys (ids assigned): page, key, then gesture order
export const touchBindings = keys =>
    keys.flatMap(({ key, gestures }) => gestures.map(({ event, ops }) => ({ control: `touch[${key.id}]`, event, ops })));
