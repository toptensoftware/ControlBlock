#pragma once

// Screen geometry, shared with the host tests (no ESP-IDF dependencies).
// screen.cpp checks these against the panel driver's DISPLAY_H_RES/V_RES.

#define SCREEN_W          800
#define SCREEN_H          480

// The status bar and pager are fixed; the page area between them scrolls
// horizontally across a row of pages.
#define SCREEN_STATUS_H   40
#define SCREEN_PAGER_H    0         // pager bar disabled for now (20 with it); the key gap follows
#define SCREEN_PAGE_W     SCREEN_W
#define SCREEN_PAGE_H     (SCREEN_H - SCREEN_STATUS_H - SCREEN_PAGER_H)
#define SCREEN_PAGE_TOP   SCREEN_STATUS_H

#define SCREEN_MAX_PAGES  8         // at most COMP_MAX_PAGES

// The bounce fill sends SCREEN_BOUNCE_LINES lines at a time: SCREEN_CHUNKS
// fills a frame (screen.cpp checks this against the panel driver)
#define SCREEN_BOUNCE_LINES 15
#define SCREEN_CHUNKS     (SCREEN_H / SCREEN_BOUNCE_LINES)
