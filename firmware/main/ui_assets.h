#pragma once

// Layout of firmware/assets/ui.bin, built by tools/test-icons/gen.js.
// A blobpack of the firmware's built-in UI assets:

#define UI_ASSET_STATUS_FONT    0   // .rfnt, Inter SemiBold 20 px
#define UI_ASSET_PAGER_CURRENT  1   // rle565 pill for the page in view
#define UI_ASSET_PAGER_OTHER    2   // rle565 pill for the other pages
#define UI_ASSET_COUNT          3
