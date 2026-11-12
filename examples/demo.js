// Device demo (protocol v2): connects, takes control, renders each key's normal and pressed
// images (a face, an icon and a label composed over black at the key's size, with
// node-buttonImage) and defines two pages. Then, to show off the keys and the image store:
//   - page 2 has a 2U key and two half-height keys stacked in one slot (Vol + / Vol -)
//   - page 2's first key flips between two stored images every 2 s (no upload)
//   - page 1's last key is a clock, replaced in place every second (same ID)
//   - page 2's second key uses a temporary image that's freed while still shown;
//     after 10 s the key moves on and the device releases the memory
//   - page 1's Power key turns the display off; touching it turns it back on
//     (the clock and status text keep updating while it's dark)
// Logs key events (by key id), page changes and the device's image memory. Ctrl+C to quit.
//
//   node examples/demo.js        (from the repo root)

import {
    ControlBlock, KeyAction, PageCause, keySlot, keyPixelSize, keyImage, svgLayer, buttonLayer, FACE_MARGIN,
} from "../host/index.js";

const ICONS = [
    ["home", "Home", "#ffffff"], ["settings", "Settings", "#c0c0c0"], ["play", "Play", "#40e040"],
    ["pause", "Pause", "#ffc020"], ["volume-up", "Vol +", "#40a0ff"], ["volume-mute", "Mute", "#ff4040"],
    ["power", "Power", "#ff3030"], ["heart", "Like", "#ff4080"], ["star", "Star", "#ffd700"],
    ["apple-tv", "Apple TV", "#ffffff"], ["steam", "Steam", "#80c0ff"], ["refresh", "Refresh", "#40ffc0"],
    ["next-track", "Next", "#ffffff"], ["info", "Info", "#a0a0ff"],
];

// Fixed IDs for images that change in place (content IDs for the rest)
const CLOCK_ID = 0xC10C0001;
const TEMP_ID  = 0x7E570001;

const FACES = {
    blue:    ["#3850a8", "#182c68", "#7090e0"],
    green:   ["#38903c", "#185a1c", "#70c070"],
    pressed: ["#e0e0e8", "#9090a0", "#ffffff"],
};

// A key face: a rounded rectangle filling width x height (the key minus its black margin)
function faceSvg(width, height, [top, bottom, border])
{
    const r = 14;
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}">
        <defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="${top}"/><stop offset="1" stop-color="${bottom}"/>
        </linearGradient></defs>
        <rect x="1" y="1" width="${width - 2}" height="${height - 2}" rx="${r}" fill="url(#g)" stroke="${border}" stroke-width="2"/>
    </svg>`;
}

// An icon with its label baked in (node-buttonImage settings), on a transparent background
function iconDef(svgFile, label, color)
{
    return {
        backColor: "transparent",
        ...(svgFile ? { svgFile: "#" + svgFile, svgForeColor: color, svgTranslate: { y: -7 } } : {}),
        text: label,
        textColor: "white",
        textAlign: "center",
        textVAlign: svgFile ? "bottom" : "center",
        font: svgFile ? "bold 11px sans-serif" : "bold 15px sans-serif",
        textPadding: [9, 4],
    };
}

function clockText()
{
    return new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

const cb = await ControlBlock.open();
const info = cb.info;
console.log(`ControlBlock ${info.serial}, firmware ${info.firmwareVersion}: ${info.cols}x${info.rows} grid of ${info.keyWidth} px cells, ` +
            `${(info.imageCapacity / 1024) | 0} KB for images`);

// A key's normal and pressed images: { normal, pressed } (keyImage results). `size` is the key's
// size in grid units ({ w, h }); `face` a FACES colour set; `icon` an iconDef or null.
async function keyImages(size, face, icon)
{
    const { width, height } = keyPixelSize(size, info);
    const fw = width - 2 * FACE_MARGIN, fh = height - 2 * FACE_MARGIN;
    const iconLayer = icon && await buttonLayer(icon, fw, fh);
    const look = async colors => keyImage([await svgLayer(faceSvg(fw, fh, colors), fw, fh), ...(iconLayer ? [iconLayer] : [])], width, height);
    return { normal: await look(face), pressed: await look(FACES.pressed) };
}

// Key ids: a plain grid slot's (page * 15 + slot + 1, as the bake numbers them); others from 100
const slotId = (page, slot) => page * info.cols * info.rows + slot + 1;
const slotKey = (page, slot, look) => ({ id: slotId(page, slot), page, ...keySlot(slot, info.cols), normal: look.normal, pressed: look.pressed });
const POWER_ID = slotId(0, 6);      // page 1's key with the "power" icon (index 6 in ICONS)

cb.on("pageChanged", e => console.log(`page ${e.page} (${e.cause === PageCause.HOST ? "host" : "swiped"})`));
cb.on("key", e => {
    console.log(`key ${e.id} (page ${e.page}) action ${e.action}${e.lost ? " (events lost)" : ""}`);
    // Page 1's Power key turns the display off; any touch (wake) turns it back on
    if (e.id === POWER_ID && e.action === KeyAction.RELEASE)
        setBacklight(false);
});
cb.on("wake", e => {
    console.log(`wake${e.lost ? " (events lost)" : ""}`);
    setBacklight(true);
});

async function setBacklight(on)
{
    try {
        await cb.setBacklight(on);
        console.log(`backlight ${on ? "on" : "off"}`);
    } catch (err) {
        console.error(err.message);
    }
}
cb.on("error", err => console.error("device error:", err));

// --- Images ---
console.log("rendering...");
const ONE = { w: 1, h: 1 };
const icons = { blue: [], green: [] };
for (const [svg, label, color] of ICONS) {
    icons.blue.push(await keyImages(ONE, FACES.blue, iconDef(svg, label, color)));
    icons.green.push(await keyImages(ONE, FACES.green, iconDef(svg, label, color)));
}
const muteOn  = icons.green[5];
const muteOff = await keyImages(ONE, FACES.green, iconDef("volume-up", "Unmuted", "#40e040"));
const wide    = await keyImages({ w: 2, h: 1 }, FACES.green, iconDef("apple-tv", "Apple TV (2U)", "#ffffff"));
const HALF = { w: 1, h: 0.5 };
const volUp   = await keyImages(HALF, FACES.blue, iconDef(null, "Vol +", null));
const volDown = await keyImages(HALF, FACES.blue, iconDef(null, "Vol -", null));
const clockLook = async () => (await keyImages(ONE, FACES.blue, iconDef(null, clockText(), null))).normal;
const temp    = await keyImages(ONE, FACES.green, iconDef("star", "Freed!", "#ff8000"));

const all = [...icons.blue, ...icons.green, muteOff, wide, volUp, volDown].flatMap(k => [k.normal, k.pressed]);
await cb.takeControl();      // the device boots standalone; this also clears what it showed (like reset)
const t0 = performance.now();
const uploaded = await cb.syncImages(all);
console.log(`uploaded ${uploaded} images in ${(performance.now() - t0).toFixed(0)} ms`);

// --- Pages ---
await cb.putImage(CLOCK_ID, (await clockLook()).data);
await cb.putImage(TEMP_ID, temp.normal.data);

const keys = [];
for (let s = 0; s < 14; s++)
    keys.push(slotKey(0, s, icons.blue[s % ICONS.length]));
keys.push(slotKey(0, 14, { normal: CLOCK_ID }));                           // no pressed image: no press feedback
keys.push(slotKey(1, 0, muteOn));
keys.push(slotKey(1, 1, { normal: TEMP_ID }));
for (let s = 2; s < 10; s++)
    keys.push(slotKey(1, s, icons.green[(s + 7) % ICONS.length]));
keys.push({ id: 100, page: 1, x: 0, y: 2, w: 2, h: 1, ...wide });         // a 2U key over slots 10 and 11
keys.push(slotKey(1, 12, icons.green[2]));
keys.push({ id: 101, page: 1, x: 3, y: 2, ...HALF, ...volUp });            // two half-height keys in slot 13
keys.push({ id: 102, page: 1, x: 3, y: 2.5, ...HALF, ...volDown });
keys.push(slotKey(1, 14, icons.green[3]));

await cb.setPageCount(2);
await cb.setKeys(keys);
await cb.setPage(0);
await cb.setStatusText({ left: "ControlBlock demo" });

// Free the temporary image while page 2 still shows it
await cb.freeImages([TEMP_ID]);
const [stillThere] = await cb.queryImages([TEMP_ID]);
console.log(`temp image freed: ID ${stillThere ? "still" : "no longer"} on the device; its key keeps showing it`);

const logMemory = async (when) => {
    const i = await cb.getInfo();
    console.log(`${when}: ${i.imageCount} images, ${(i.imageUsed / 1024).toFixed(1)} KB used`);
};
await logMemory("after setup");

// --- Changes ---
let muted = true, tick = 0;
const timer = setInterval(async () => {
    try {
        tick++;
        // Replace in place: same ID, new pixels
        await cb.putImage(CLOCK_ID, (await clockLook()).data);
        await cb.setStatusText({ right: new Date().toTimeString().slice(0, 8) });     // ASCII "HH:MM:SS"

        if (tick % 2 === 0) {
            muted = !muted;
            await cb.setKeys([slotKey(1, 0, muted ? muteOn : muteOff)]);       // both looks are on the device already
        }
        if (tick === 10) {
            await cb.setKeys([slotKey(1, 1, icons.green[8])]);
            setTimeout(() => logMemory("temp image's key redefined").catch(() => { }), 200);
        }
    } catch (err) {
        console.error(err.message);
    }
}, 1000);

process.on("SIGINT", async () => {
    clearInterval(timer);
    await cb.releaseControl().catch(() => { });      // the device goes back to its own Home (and its light on)
    await cb.close();
    process.exit(0);
});
