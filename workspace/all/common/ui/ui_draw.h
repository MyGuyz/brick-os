#ifndef UI_DRAW_H
#define UI_DRAW_H

#include <stdbool.h>
#include <stdint.h>
#include "sdl.h"

// Shared low-level drawing primitives used by the ui_* components.

// Fill a rounded rectangle (capsule when radius == h/2). The radius is clamped
// to half the width/height. Works on any target surface, including ARGB
// overlay surfaces (plain SDL_FillRects with the given mapped color).
void UI_fillRoundedRect(SDL_Surface* dst, int x, int y, int w, int h,
						int radius, uint32_t color);

// Return a cached full-size semi-transparent black scrim surface (alpha 178,
// blend mode enabled). `cache` is the caller's static surface slot; the
// surface is (re)created when the requested size changes. Returns NULL on
// allocation failure.
SDL_Surface* UI_getScrim(SDL_Surface** cache, int w, int h);
// The same with a chosen alpha. The cache is keyed on size only, so keep one
// static slot per alpha.
SDL_Surface* UI_getScrimAlpha(SDL_Surface** cache, int w, int h, Uint8 alpha);

// Render a horizontally centered row of button hints at `y`.
// `pairs` is a NULL-terminated {button, label} array, e.g.
// (char*[]){"B", "CANCEL", "A", "CONFIRM", NULL}.
void UI_renderCenteredButtons(SDL_Surface* dst, int y, char** pairs);

// Brick OS: the shared pieces (menu bar, hint bar, confirm, MENU popup, toast, empty page, loading, message,
// splash, controls help, list views) draw the Brick OS way. Apps that link $(BOS_UI_SRCS) set it at start.
extern bool UI_bos;

#endif // UI_DRAW_H
