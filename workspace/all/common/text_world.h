#ifndef __TEXT_WORLD_H__
#define __TEXT_WORLD_H__

#include <stdbool.h>
#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#else
#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#endif

// The chain's font i (0 the string's own face, 1 Inter, 2 font1.ttf, 3 font1-world.ttf) at the size wanted, or NULL.
typedef TTF_Font* (*TextWorldFont)(int i, void* ctx);

// s is ready to draw: Thai already in glyphs (TextThai_toGlyphs), Arabic already visual (TextShape_toVisual).
int TextWorld_width(const char* s, TextWorldFont get, void* ctx);
SDL_Surface* TextWorld_render(const char* s, SDL_Color color, int* ascent, TextWorldFont get, void* ctx);

#endif // __TEXT_WORLD_H__
