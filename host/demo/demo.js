// Device demo: connects, resets the device, uploads faces and icons (drawn
// with node-buttonImage, labels baked in) and defines two pages. Then, to
// show off the image store:
//   - page 2, key 1 flips between two stored images every 2 s (no upload)
//   - page 1's last key is a clock, replaced in place every second (same ID)
//   - page 2, key 2 uses a temporary image that's freed while still shown;
//     after 10 s the key moves on and the device releases the memory
// Logs page changes and the device's image memory. Ctrl+C to quit.
//
//   npm run demo

import { ButtonImage } from "@toptensoftware/node-buttonImage";
import { ControlBlock, keyImage, keyImageFromSvg, PageCause } from "../index.js";

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

// A key face: rounded rectangle with 4 px transparent padding
function faceSvg(size, top, bottom, border)
{
    const pad = 4, r = 14;
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}">
        <defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="${top}"/><stop offset="1" stop-color="${bottom}"/>
        </linearGradient></defs>
        <rect x="${pad + 1}" y="${pad + 1}" width="${size - 2 * pad - 2}" height="${size - 2 * pad - 2}"
              rx="${r}" fill="url(#g)" stroke="${border}" stroke-width="2"/>
    </svg>`;
}

// An icon with its label baked in, on a transparent background
async function iconImage(size, svgFile, label, color)
{
    const bi = new ButtonImage();
    bi.backColor = "transparent";
    if (svgFile) {
        bi.svgFile = "#" + svgFile;
        bi.svgForeColor = color;
        bi.svgTranslate = { y: -7 };
    }
    bi.text = label;
    bi.textColor = "white";
    bi.textAlign = "center";
    bi.textVAlign = svgFile ? "bottom" : "center";
    bi.font = svgFile ? "bold 11px sans-serif" : "bold 15px sans-serif";
    bi.textPadding = [9, 4];
    const rgba = await bi.render(size, size, 4);
    return keyImage(new Uint8Array(rgba.buffer, rgba.byteOffset, rgba.byteLength), size, size, size);
}

function clockText()
{
    return new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

const cb = await ControlBlock.open();
const info = cb.info;
const size = info.keyWidth;
console.log(`ControlBlock ${info.serial}, firmware ${info.firmwareVersion}: ${info.cols}x${info.rows} keys of ${size} px, ` +
            `${(info.imageCapacity / 1024) | 0} KB for images`);

cb.on("pageChanged", e => console.log(`page ${e.page} (${e.cause === PageCause.HOST ? "host" : "swiped"})`));
cb.on("key", e => console.log(`key ${e.page}/${e.key} action ${e.action}${e.lost ? " (events lost)" : ""}`));
cb.on("error", err => console.error("device error:", err));

// --- Images ---
console.log("rendering...");
const faces = {
    blue:    await keyImageFromSvg(faceSvg(size, "#3850a8", "#182c68", "#7090e0"), size),
    green:   await keyImageFromSvg(faceSvg(size, "#38903c", "#185a1c", "#70c070"), size),
    pressed: await keyImageFromSvg(faceSvg(size, "#e0e0e8", "#9090a0", "#ffffff"), size),
};
const icons = [];
for (const [svg, label, color] of ICONS)
    icons.push(await iconImage(size, svg, label, color));
const muteOn  = icons[5];
const muteOff = await iconImage(size, "volume-up", "Unmuted", "#40e040");
const temp    = await iconImage(size, "star", "Freed!", "#ff8000");

await cb.reset();
const t0 = performance.now();
const uploaded = await cb.syncImages([...Object.values(faces), ...icons, muteOff]);
console.log(`uploaded ${uploaded} images in ${(performance.now() - t0).toFixed(0)} ms`);

// --- Pages ---
const keysPerPage = info.cols * info.rows;
const key = (page, k, face, icon) => ({ page, key: k, bg: face.id, fg: icon?.id, pressedBg: faces.pressed.id, pressedFg: icon?.id });

const clock = await iconImage(size, null, clockText(), null);
await cb.putImage(CLOCK_ID, clock.data);
await cb.putImage(TEMP_ID, temp.data);

const keys = [];
for (let k = 0; k < keysPerPage - 1; k++)
    keys.push(key(0, k, faces.blue, icons[k % icons.length]));
keys.push({ ...key(0, keysPerPage - 1, faces.blue, null), fg: CLOCK_ID, pressedFg: CLOCK_ID });
keys.push(key(1, 0, faces.green, muteOn));
keys.push({ ...key(1, 1, faces.green, null), fg: TEMP_ID, pressedFg: TEMP_ID });
for (let k = 2; k < keysPerPage; k++)
    keys.push(key(1, k, faces.green, icons[(k + 7) % icons.length]));

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
        await cb.putImage(CLOCK_ID, (await iconImage(size, null, clockText(), null)).data);
        await cb.setStatusText({ right: new Date().toTimeString().slice(0, 8) });     // ASCII "HH:MM:SS"

        if (tick % 2 === 0) {
            muted = !muted;
            await cb.setKeys([key(1, 0, faces.green, muted ? muteOn : muteOff)]);
        }
        if (tick === 10) {
            await cb.setKeys([key(1, 1, faces.green, icons[8])]);
            setTimeout(() => logMemory("temp image's key redefined").catch(() => { }), 200);
        }
    } catch (err) {
        console.error(err.message);
    }
}, 1000);

process.on("SIGINT", async () => {
    clearInterval(timer);
    await cb.close();
    process.exit(0);
});
