#ifndef __TEXT_THAI_H__
#define __TEXT_THAI_H__

#include <stdbool.h>

// Thai mark placement for the no-HarfBuzz SDL_ttf build (Brick OS). Thai marks
// stack on their consonant and on each other; plain SDL_ttf draws every mark at
// one default spot. TextThai_toGlyphs swaps the marks that need another spot
// for Private Use Area glyphs that res/font1-thai.ttf has pre-positioned (map:
// scripts/font/gen_thai_font.py in the Brick OS docs). Text without Thai passes
// through unchanged.

// True if s contains any Thai codepoint (U+0E00..0E7F).
bool TextThai_has(const char* utf8);

// Write the display form of utf8 into out (<= out_sz-1 bytes + NUL). Returns
// the output byte length, or 0 on empty/overflow.
int TextThai_toGlyphs(const char* utf8, char* out, int out_sz);

#endif // __TEXT_THAI_H__
