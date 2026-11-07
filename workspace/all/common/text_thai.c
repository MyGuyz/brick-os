// Brick OS: Thai mark placement (text_thai.h). Built as part of text_shape.c.
#include "text_thai.h"

#include <stdint.h>
#include <string.h>

enum { BASE_NONE, BASE_PLAIN, BASE_TALL, BASE_DESC, BASE_DESC_CUT };

static int thai_base(uint32_t cp) {
	switch (cp) {
	case 0x0E1B: case 0x0E1D: case 0x0E1F: case 0x0E2C: // ป ฝ ฟ ฬ
		return BASE_TALL;
	case 0x0E0E: case 0x0E0F: // ฎ ฏ
		return BASE_DESC;
	case 0x0E0D: case 0x0E10: // ญ ฐ: the descender comes off under a lower vowel
		return BASE_DESC_CUT;
	}
	return (cp >= 0x0E01 && cp <= 0x0E2E) ? BASE_PLAIN : BASE_NONE;
}

static bool is_upper_vowel(uint32_t cp) {
	return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E37) || cp == 0x0E47 || cp == 0x0E4D;
}
static bool is_tone(uint32_t cp) { return cp >= 0x0E48 && cp <= 0x0E4C; }
static bool is_lower_vowel(uint32_t cp) { return cp >= 0x0E38 && cp <= 0x0E3A; }

// UTF-8 decode one codepoint at *p (advances *p); invalid bytes pass as themselves.
static uint32_t next_cp(const unsigned char** p) {
	const unsigned char* s = *p;
	uint32_t cp = s[0];
	int n = 1;
	if (cp >= 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		cp = ((cp & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		n = 4;
	} else if (cp >= 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		cp = ((cp & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
		n = 3;
	} else if (cp >= 0xC0 && (s[1] & 0xC0) == 0x80) {
		cp = ((cp & 0x1F) << 6) | (s[1] & 0x3F);
		n = 2;
	}
	*p = s + n;
	return cp;
}

static int put_cp(char* out, int pos, int out_sz, uint32_t cp) {
	char b[4];
	int n;
	if (cp < 0x80) {
		b[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		b[0] = (char)(0xC0 | (cp >> 6));
		b[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp < 0x10000) {
		b[0] = (char)(0xE0 | (cp >> 12));
		b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		b[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else {
		b[0] = (char)(0xF0 | (cp >> 18));
		b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		b[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	}
	if (pos < 0 || pos + n >= out_sz)
		return -1;
	memcpy(out + pos, b, n);
	return pos + n;
}

bool TextThai_has(const char* utf8) {
	// Every Thai codepoint is E0 B8 xx or E0 B9 xx in UTF-8.
	for (const unsigned char* s = (const unsigned char*)utf8; s && *s; s++)
		if (s[0] == 0xE0 && (s[1] == 0xB8 || s[1] == 0xB9))
			return true;
	return false;
}

int TextThai_toGlyphs(const char* utf8, char* out, int out_sz) {
	if (!utf8 || !*utf8 || !out || out_sz <= 0)
		return 0;
	const unsigned char* p = (const unsigned char*)utf8;
	int pos = 0;
	int base = BASE_NONE;
	bool upper = false; // an upper vowel already sits on the current consonant

	while (*p && pos >= 0) {
		uint32_t cp = next_cp(&p);
		const unsigned char* q = p;
		uint32_t next = *q ? next_cp(&q) : 0;
		int b = thai_base(cp);

		if (b != BASE_NONE) {
			base = b;
			upper = false;
			if (b == BASE_DESC_CUT && is_lower_vowel(next))
				cp = cp == 0x0E0D ? 0xF70F : 0xF700;
			else if (b == BASE_DESC && is_lower_vowel(next)) // short ฎ ฏ, the vowel drops below
				cp = cp == 0x0E0E ? 0xF71B : 0xF71C;
		} else if (is_upper_vowel(cp)) {
			if (base == BASE_TALL) {
				if (cp == 0x0E31)
					cp = 0xF710;
				else if (cp == 0x0E4D)
					cp = 0xF711;
				else if (cp == 0x0E47)
					cp = 0xF712;
				else
					cp = 0xF701 + (cp - 0x0E34);
			}
			upper = true;
		} else if (is_tone(cp)) {
			uint32_t i = cp - 0x0E48;
			bool tall = base == BASE_TALL;
			if (base == BASE_NONE)
				; // a stray mark keeps its default spot
			else if (next == 0x0E33) // above ำ's circle
				cp = (tall ? 0xF725 : 0xF720) + i;
			else if (upper) // above an upper vowel
				cp = (tall ? 0xF713 : 0xF70A) + i;
			else if (tall)
				cp = 0xF705 + i;
		} else if (is_lower_vowel(cp)) {
			if (base == BASE_DESC)
				cp = 0xF718 + (cp - 0x0E38);
		} else if (cp == 0x0E33 && base == BASE_TALL) {
			pos = put_cp(out, pos, out_sz, 0xF711); // ำ = ํ + า, circle moved off the tall stem
			cp = 0x0E32;
			base = BASE_NONE;
		} else {
			base = BASE_NONE;
			upper = false;
		}
		pos = put_cp(out, pos, out_sz, cp);
	}
	if (pos <= 0) {
		out[0] = '\0';
		return 0;
	}
	out[pos] = '\0';
	return pos;
}
