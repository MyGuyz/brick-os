// Brick OS: text in any language. A string is drawn in runs, each with the first font of a chain that has the glyphs:
// the UI's own face (Inter, or BrickOS Thai for a Thai string), Inter, font1.ttf (MiSans: Chinese, Japanese), then
// font1-world.ttf (Hangul, the other scripts, symbols, emoji; scripts/font-world/gen_world_font.py in the Brick OS
// docs). SDL_ttf here draws the BMP only, so emoji and the maths letters are drawn from the world font's private use
// area. Included by the two programs that draw with it (bos_ui.c, osdmusic.c).
#include "text_world.h"

#include <stdint.h>
#include <string.h>

#define TW_WORLD 3 // the last font of the chain
#define TW_RUN 512

static const unsigned char* tw_decode(const unsigned char* s, uint32_t* cp) {
	int n = s[0] < 0x80 ? 0 : (s[0] & 0xE0) == 0xC0 ? 1 : (s[0] & 0xF0) == 0xE0 ? 2 : (s[0] & 0xF8) == 0xF0 ? 3 : -1;
	if (n <= 0) {
		*cp = n < 0 ? '?' : s[0];
		return s + 1;
	}
	uint32_t c = s[0] & (0x3F >> n);
	for (int i = 1; i <= n; i++) {
		if ((s[i] & 0xC0) != 0x80) {
			*cp = '?';
			return s + 1;
		}
		c = c << 6 | (s[i] & 0x3F);
	}
	*cp = c;
	return s + n + 1;
}

// The BMP code point to draw cp with (0 = draw nothing: joiners, variation selectors, skin tones, unknown planes)
static uint32_t tw_map(uint32_t cp, bool* world) {
	*world = false;
	if ((cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x200B && cp <= 0x200F) || cp == 0x20E3 || cp == 0xFEFF ||
		(cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) || (cp >= 0x1F3FB && cp <= 0x1F3FF))
		return 0;
	if (cp < 0x10000)
		return cp;
	*world = true;
	if (cp >= 0x1F000 && cp <= 0x1FAFF)
		return 0xE000 + (cp - 0x1F000);
	if (cp >= 0x1D400 && cp <= 0x1D7FF)
		return 0xEB00 + (cp - 0x1D400);
	return 0;
}

static int tw_fontOf(uint32_t c, bool world, TextWorldFont get, void* ctx) {
	for (int i = world ? TW_WORLD : 0; i <= TW_WORLD; i++) {
		TTF_Font* f = get(i, ctx);
		if (f && TTF_GlyphIsProvided(f, (Uint16)c))
			return i;
	}
	return world ? -1 : 0;
}

// The next run of s: its text (as drawn) in run, its font's place in the chain in *font. Returns the rest, or NULL at
// the end.
static const unsigned char* tw_run(const unsigned char* s, char* run, int* font, TextWorldFont get, void* ctx) {
	int n = 0;
	*font = -1;
	while (*s) {
		uint32_t cp;
		bool world;
		const unsigned char* next = tw_decode(s, &cp);
		uint32_t c = tw_map(cp, &world);
		int f = c ? tw_fontOf(c, world, get, ctx) : -1;
		if (f < 0) {
			s = next;
			continue;
		}
		if ((*font >= 0 && f != *font) || n > TW_RUN - 5)
			break;
		*font = f;
		if (c < 0x80)
			run[n++] = (char)c;
		else if (c < 0x800) {
			run[n++] = (char)(0xC0 | c >> 6);
			run[n++] = (char)(0x80 | (c & 0x3F));
		} else {
			run[n++] = (char)(0xE0 | c >> 12);
			run[n++] = (char)(0x80 | (c >> 6 & 0x3F));
			run[n++] = (char)(0x80 | (c & 0x3F));
		}
		s = next;
	}
	run[n] = '\0';
	return n ? s : NULL;
}

int TextWorld_width(const char* s, TextWorldFont get, void* ctx) {
	char run[TW_RUN];
	int font, total = 0;
	const unsigned char* p = (const unsigned char*)s;
	while (*p && (p = tw_run(p, run, &font, get, ctx))) {
		int w = 0;
		TTF_SizeUTF8(get(font, ctx), run, &w, NULL);
		total += w;
	}
	return total;
}

SDL_Surface* TextWorld_render(const char* s, SDL_Color color, int* ascent, TextWorldFont get, void* ctx) {
	SDL_Surface* runs[64];
	int asc[64], n = 0, w = 0, up = 0, down = 0, font;
	char run[TW_RUN];
	const unsigned char* p = (const unsigned char*)s;
	while (n < 64 && *p && (p = tw_run(p, run, &font, get, ctx))) {
		TTF_Font* f = get(font, ctx);
		SDL_Surface* t = TTF_RenderUTF8_Blended(f, run, color);
		if (!t)
			continue;
		asc[n] = TTF_FontAscent(f);
		w += t->w;
		if (asc[n] > up)
			up = asc[n];
		if (t->h - asc[n] > down)
			down = t->h - asc[n];
		runs[n++] = t;
	}
	*ascent = up;
	if (n == 0)
		return NULL;
	if (n == 1)
		return runs[0];
	SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, w, up + down, 32, SDL_PIXELFORMAT_ARGB8888);
	int x = 0;
	if (out)
		SDL_FillRect(out, NULL, SDL_MapRGBA(out->format, color.r, color.g, color.b, 0));
	for (int i = 0; i < n; i++) {
		if (out) {
			SDL_SetSurfaceBlendMode(runs[i], SDL_BLENDMODE_NONE);
			SDL_BlitSurface(runs[i], NULL, out, &(SDL_Rect){x, up - asc[i], 0, 0});
		}
		x += runs[i]->w;
		SDL_FreeSurface(runs[i]);
	}
	if (out)
		SDL_SetSurfaceBlendMode(out, SDL_BLENDMODE_BLEND);
	return out;
}
