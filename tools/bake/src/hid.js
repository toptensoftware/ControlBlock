// USB HID keyboard key names -> usage ids (HID Usage Tables, keyboard page 0x07) and modifier bits.
// A chord is modifiers plus at most one key: "ctrl+shift+esc", "f5", "shift".

export const MOD = { lctrl: 0x01, lshift: 0x02, lalt: 0x04, lgui: 0x08, rctrl: 0x10, rshift: 0x20, ralt: 0x40, rgui: 0x80 };

const MODIFIERS = {
    ctrl: MOD.lctrl, control: MOD.lctrl, lctrl: MOD.lctrl,
    shift: MOD.lshift, lshift: MOD.lshift,
    alt: MOD.lalt, lalt: MOD.lalt, option: MOD.lalt,
    gui: MOD.lgui, win: MOD.lgui, windows: MOD.lgui, cmd: MOD.lgui, meta: MOD.lgui, super: MOD.lgui, lgui: MOD.lgui,
    rctrl: MOD.rctrl, rshift: MOD.rshift, ralt: MOD.ralt, altgr: MOD.ralt, rgui: MOD.rgui,
};

const KEYS = {};

// a-z, 1-9, 0
for (let i = 0; i < 26; i++) KEYS[String.fromCharCode(97 + i)] = 0x04 + i;
for (let i = 1; i <= 9; i++) KEYS[String(i)] = 0x1d + i;
KEYS["0"] = 0x27;

Object.assign(KEYS, {
    enter: 0x28, return: 0x28, esc: 0x29, escape: 0x29, backspace: 0x2a, tab: 0x2b, space: 0x2c,
    minus: 0x2d, "-": 0x2d,
    plus: 0x2e, equals: 0x2e, "=": 0x2e,            // the '=' key ('+' is the chord separator, so "plus" names it)
    lbracket: 0x2f, "[": 0x2f, rbracket: 0x30, "]": 0x30,
    backslash: 0x31, "\\": 0x31,
    semicolon: 0x33, ";": 0x33, quote: 0x34, "'": 0x34, grave: 0x35, "`": 0x35,
    comma: 0x36, ",": 0x36, period: 0x37, ".": 0x37, slash: 0x38, "/": 0x38,
    capslock: 0x39, printscreen: 0x46, scrolllock: 0x47, pause: 0x48,
    insert: 0x49, home: 0x4a, pageup: 0x4b, delete: 0x4c, end: 0x4d, pagedown: 0x4e,
    right: 0x4f, left: 0x50, down: 0x51, up: 0x52,
});

for (let i = 1; i <= 12; i++) KEYS["f" + i] = 0x39 + i;     // f1 = 0x3a
for (let i = 13; i <= 24; i++) KEYS["f" + i] = 0x68 + i - 13;

// -> { modifiers, key } (key 0 = modifiers only), or { error }
export function parseChord(text)
{
    const parts = text.split("+").map(s => s.trim().toLowerCase());
    if (parts.length === 1 && parts[0] === "")
        return { error: "empty key chord" };

    let modifiers = 0, key = 0, haveKey = false;
    for (const part of parts)
    {
        if (part === "")
            return { error: `empty name in chord "${text}"` };
        if (Object.hasOwn(MODIFIERS, part))
        {
            const bit = MODIFIERS[part];
            if (modifiers & bit)
                return { error: `modifier "${part}" repeated in chord "${text}"` };
            modifiers |= bit;
        }
        else if (Object.hasOwn(KEYS, part))
        {
            if (haveKey)
                return { error: `a chord has only one key besides modifiers ("${text}")` };
            key = KEYS[part];
            haveKey = true;
        }
        else
            return { error: `unknown key "${part}"` };
    }
    return { modifiers, key };
}
