// Brick OS UI 2.0 design tokens (DESIGN.md §2–§4). The ONLY place UI colours and sizes are written down:
// every Brick OS screen (C apps through common/ui/bos_ui.h, YTC in C++) includes this file. Values are the
// mockup's px on the 1024x768 panel; BOS_px() scales them for any other width.
// Keep in step with scripts/design/mockup_v2.py (T, FS, ROW_H …).
#ifndef BOS_TOKENS_H
#define BOS_TOKENS_H

// ---- colours (0xRRGGBB) ----
#define BOS_C_BG 0x000000
#define BOS_C_SURFACE 0x1C1C1F
#define BOS_C_SURFACE2 0x2C2C30
#define BOS_C_SEPARATOR 0x38383A
#define BOS_C_TEXT 0xFFFFFF
#define BOS_C_TEXT2 0xB4B4BA
#define BOS_C_TEXTOFF 0x6E6E73
#define BOS_C_FOCUS 0xF2F2F7 // default; ธีมสี can change it (BOS_theme())
#define BOS_C_ONFOCUS 0x0B0B0D
#define BOS_C_PRIMARY 0x4A9BFF // default; ธีมสี can change it
#define BOS_C_SECONDARY 0x8E8E93
#define BOS_C_OK 0x30D158
#define BOS_C_WARN 0xFFD60A
#define BOS_C_DANGER 0xFF453A
#define BOS_C_YOUTUBE 0xFF0033

// ---- type (px) ----
#define BOS_FS_TITLE 56
#define BOS_FS_BODY 40
#define BOS_FS_SMALL 30
#define BOS_FS_BADGE 26
#define BOS_FS_HINT_FACE 24 // A B X Y inside the circle
#define BOS_FS_HINT_WORD 22 // MENU, SELECT … inside the pill

// ---- sizes (px) ----
#define BOS_BAR_H 64	 // status bar and hint bar
#define BOS_BAR_PAD 32	 // their side padding
#define BOS_PAGE_PAD 40	 // page side padding
#define BOS_ROW_H 96
#define BOS_ICON 64
#define BOS_STATUS_ICON 32
#define BOS_RING 4		 // picture focus ring, offset 4
#define BOS_RADIUS_ROW 20
#define BOS_RADIUS_GROUP 24
#define BOS_HINT_CAP 40	 // hint circle / pill height
#define BOS_TILE 112	 // app tile (radius 26), label box 128
#define BOS_TILE_RADIUS 26

// ---- focus (DESIGN.md §4) ----
#define BOS_FOCUS_SCALE 1.12f // tiles, cards
#define BOS_ROW_SCALE 1.04f	  // full-width rows, sliders, wide tiles (>= 400 px)
#define BOS_DIM 0.6f		  // every other focusable item on the screen

// ---- battery thresholds (§5.1) ----
#define BOS_BATT_WARN 20
#define BOS_BATT_DANGER 10

// ---- app identity colours (icon colour on the app tiles, DESIGN.md 6.1b) ----
#define BOS_APP_GAMES 0x4A9BFF
#define BOS_APP_YOUTUBE 0xFF4D5E
#define BOS_APP_MUSIC 0xFF7AB6
#define BOS_APP_VIDEO 0xFFB340
#define BOS_APP_PORTS 0x5EE0A0
#define BOS_APP_SETTINGS 0xC7C7CC
#define BOS_APP_FILES 0x64D2FF
#define BOS_APP_IMAGES 0xFFD60A
#define BOS_APP_TROPHIES 0xFF9F0A
#define BOS_APP_STATS 0x30D158
#define BOS_APP_SYNC 0x0A84FF
#define BOS_APP_XTRAS 0xFF6482
#define BOS_APP_EMUSET 0xAC8E68
#define BOS_APP_ARTWORK 0xBF5AF2
#define BOS_APP_PORTMASTER 0x5AC8FA

#endif
