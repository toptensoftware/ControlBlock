#pragma once

// Layout of firmware/assets/render_lab.bin, built by tools/test-icons/gen.js.
// The render lab's assets (wip/wip-on-device-render-feasibility.md), embedded
// in the firmware only with -DCB_RENDER_LAB=ON. A blobpack of:

#define RENDER_LAB_ASSET_FONT_20        0   // .rfnt, Inter SemiBold 20 px, ASCII, subpixel 1
#define RENDER_LAB_ASSET_FONT_27        1   // .rfnt, Inter SemiBold 27 px, ASCII, subpixel 1
#define RENDER_LAB_ASSET_FONT_27_SP4    2   // .rfnt, Inter SemiBold 27 px, ASCII, subpixel 4
#define RENDER_LAB_ASSET_FACE           3   // rle565, shaded key face with alpha (RENDER_LAB_FACE_SIZE square)
#define RENDER_LAB_ASSET_ICON           4   // rle565, icon with alpha (RENDER_LAB_ICON_SIZE square)
#define RENDER_LAB_ASSET_COUNT          5

#define RENDER_LAB_FONTS                3   // the fonts are blobs 0..RENDER_LAB_FONTS-1
#define RENDER_LAB_FACE_SIZE            128
#define RENDER_LAB_ICON_SIZE            80
