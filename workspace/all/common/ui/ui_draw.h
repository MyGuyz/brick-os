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

// The same with a chosen alpha. The cache is keyed on size only, so keep one
// static slot per alpha.
SDL_Surface* UI_getScrimAlpha(SDL_Surface** cache, int w, int h, Uint8 alpha);

#endif // UI_DRAW_H
