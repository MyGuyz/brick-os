// Brick OS UI 2.0 shared kit — see bos_ui.h. Every number here is a token (bos_tokens.h) or a DESIGN.md §5 size.

#include "bos_ui.h"

#include <math.h>
#include <msettings.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "api.h"
#include "config.h"
#include "defines.h"
#include "text_thai.h"
#include "ui_draw.h"

#define BOS_RES RES_PATH "/bos"

void BOS_benchLap(const char* name) {
	static int on = -1;
	static Uint64 last;
	if (on < 0)
		on = getenv("BOS_BENCH") != NULL;
	if (!on)
		return;
	Uint64 now = SDL_GetPerformanceCounter();
	if (name)
		fprintf(stderr, "%s=%.1f ", name, (double)(now - last) * 1000.0 / SDL_GetPerformanceFrequency());
	last = now;
}

int BOS_px(float v) {
	return (int)(v * (FIXED_WIDTH / 1024.0f) + 0.5f);
}

// ---- theme: ธีมสี (DESIGN.md §6.8), read from the shared config on every call so a change shows at once ----
static double lum(uint32_t c) { // WCAG relative luminance
	double v[3];
	for (int i = 0; i < 3; i++) {
		double x = ((c >> (16 - 8 * i)) & 255) / 255.0;
		v[i] = x <= 0.03928 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
	}
	return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
}

double BOS_contrast(uint32_t a, uint32_t b) {
	double la = lum(a), lb = lum(b);
	return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

uint32_t BOS_textOn(uint32_t bg, int mode) {
	if (mode & THEME_TEXT_RGB)
		return (uint32_t)mode & 0xFFFFFF;
	if (mode == 1)
		return 0xFFFFFF;
	if (mode == 2)
		return BOS_C_ONFOCUS;
	return BOS_contrast(bg, BOS_C_ONFOCUS) >= BOS_contrast(bg, 0xFFFFFF) ? BOS_C_ONFOCUS : 0xFFFFFF;
}

// ธีมสำเร็จรูป: {focus (also the hint keys), primary}; 0 is ค่าเริ่มต้น
static const uint32_t PRESETS[6][2] = {{BOS_C_FOCUS, BOS_C_PRIMARY}, {0x64B5FF, 0x64B5FF}, {0xFF8FC0, 0xFF6FAE},
									   {0x5EE08A, 0x30D158}, {0xFFB04A, 0xFF9F0A}, {0xC79BFF, 0xBF5AF2}};
void BOS_preset(int i, uint32_t* focus, uint32_t* primary) {
	if (i < 0 || i > 5)
		i = 0;
	*focus = PRESETS[i][0];
	*primary = PRESETS[i][1];
}

static BosTheme theme;
const BosTheme* BOS_theme(void) {
	// กำหนดเอง off: the preset card's colours with auto text; on: the colour slots (-1 = the default colour)
	uint32_t pf, pp;
	BOS_preset(CFG_getTheme(THEME_PRESET), &pf, &pp);
	if (CFG_getTheme(THEME_CUSTOM)) {
		int f = CFG_getTheme(THEME_FOCUS), p = CFG_getTheme(THEME_PRIMARY), h = CFG_getTheme(THEME_HINT);
		theme.focus = f < 0 ? BOS_C_FOCUS : (uint32_t)f & 0xFFFFFF;
		theme.onfocus = BOS_textOn(theme.focus, CFG_getTheme(THEME_FOCUS_TEXT));
		theme.primary = p < 0 ? BOS_C_PRIMARY : (uint32_t)p & 0xFFFFFF;
		theme.onprimary = BOS_textOn(theme.primary, CFG_getTheme(THEME_PRIMARY_TEXT));
		theme.hint_cap = h < 0 ? BOS_C_FOCUS : (uint32_t)h & 0xFFFFFF;
		theme.onhint = BOS_textOn(theme.hint_cap, CFG_getTheme(THEME_HINT_TEXT));
	} else {
		theme.focus = theme.hint_cap = pf;
		theme.primary = pp;
		theme.onfocus = theme.onhint = BOS_textOn(pf, 0);
		theme.onprimary = BOS_textOn(pp, 0);
	}
	theme.icon_mode = CFG_getTheme(THEME_ICON);
	// พื้นหลังเทาเข้ม lifts the groups and their controls one step, so they still stand off the page
	bool grey = CFG_getTheme(THEME_BG) == 1;
	theme.bg = grey ? BOS_C_SURFACE : BOS_C_BG;
	theme.surface = grey ? BOS_C_SURFACE2 : BOS_C_SURFACE;
	theme.surface2 = grey ? 0x3A3A3E : BOS_C_SURFACE2;
	return &theme;
}

uint32_t BOS_iconColor(uint32_t own) {
	int m = BOS_theme()->icon_mode;
	return m == 1 ? 0xFFFFFF : m == 2 ? theme.primary : own;
}

// ---- fonts ----
typedef struct {
	int px, weight;
	bool thai;
	TTF_Font* f;
} FontSlot;
static FontSlot fonts[40];
static int nfonts;

static TTF_Font* fontFor(int px, int weight, bool thai) {
	for (int i = 0; i < nfonts; i++)
		if (fonts[i].px == px && fonts[i].weight == weight && fonts[i].thai == thai)
			return fonts[i].f;
	if (nfonts == (int)(sizeof(fonts) / sizeof(fonts[0])))
		return NULL;
	TTF_Font* f;
	if (thai) {
		// a real SemiBold for 600/700 (scripts/font/gen_thai_font.py on IBM Plex Sans Thai SemiBold): SDL_ttf's
		// synthetic bold blurs Thai and grows the strokes past the glyph box, cutting their tops and bottoms
		f = NULL;
		if (weight >= 600)
			f = TTF_OpenFont(RES_PATH "/font1-thai-semibold.ttf", px);
		if (!f)
			f = TTF_OpenFont(GFX_getThaiFontPath(), px);
	} else {
		const char* name = weight >= 700 ? "Inter-Bold" : weight >= 600 ? "Inter-SemiBold"
													: weight >= 500	  ? "Inter-Medium"
																	  : "Inter-Regular";
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s/%s.ttf", BOS_RES, name);
		f = TTF_OpenFont(path, px);
	}
	if (!f)
		return NULL;
	fonts[nfonts++] = (FontSlot){px, weight, thai, f};
	return f;
}

// The font and the string to hand it: Thai strings are reshaped into the font's glyph positions first.
static TTF_Font* prepare(const char* s, int px, int weight, char* glyphs, int size, const char** draw) {
	*draw = s;
	if (TextThai_has(s)) {
		TTF_Font* f = fontFor(px, weight, true);
		if (!f || TextThai_toGlyphs(s, glyphs, size) <= 0)
			return NULL;
		*draw = glyphs;
		return f;
	}
	return fontFor(px, weight, false);
}

// ---- text caches ----
// A page draws the same strings every frame, so both the width and the drawn picture of a string are kept: by
// (size, weight, string) and (size, weight, colour, string). Shaping and drawing one with FreeType costs about a
// millisecond on the Brick Pro: a page's first frame (30-50 ms) shows what every frame would cost without this.
// The oldest entry is replaced; a string longer than an entry's room is drawn uncached.
#define BOS_TC_TEXT 160
#define BOS_WC_SIZE 512
#define BOS_TC_SIZE 192
static unsigned strHash(const char* s) {
	unsigned h = 2166136261u;
	for (const unsigned char* p = (const unsigned char*)s; *p; p++)
		h = (h ^ *p) * 16777619u;
	return h;
}
typedef struct {
	unsigned hash;
	short px, weight;
	int w;
	uint32_t lru;
	char text[BOS_TC_TEXT];
} WidthEntry;
static WidthEntry wcache[BOS_WC_SIZE];
typedef struct {
	unsigned hash;
	short px, weight;
	uint32_t rgb, lru;
	SDL_Surface* surf;
	int ascent; // of the font it was drawn with
	char text[BOS_TC_TEXT];
} TextEntry;
static TextEntry tcache[BOS_TC_SIZE];
static uint32_t cache_lru;

int BOS_textW(const char* s, int px, int weight) {
	if (!s || !s[0])
		return 0;
	size_t len = strlen(s);
	WidthEntry* slot = NULL;
	unsigned hash = 0;
	if (len < BOS_TC_TEXT) {
		hash = strHash(s);
		// a few probes from the hash: a full table walk per measure would cost what it saves
		for (int i = 0; i < 4; i++) {
			WidthEntry* e = &wcache[(hash + i) % BOS_WC_SIZE];
			if (e->hash == hash && e->px == px && e->weight == weight && !strcmp(e->text, s)) {
				e->lru = ++cache_lru;
				return e->w;
			}
			if (!slot || e->lru < slot->lru)
				slot = e;
		}
	}
	char g[1024];
	const char* d;
	TTF_Font* f = prepare(s, px, weight, g, sizeof(g), &d);
	int w = 0;
	if (f)
		TTF_SizeUTF8(f, d, &w, NULL);
	if (slot) {
		slot->hash = hash, slot->px = (short)px, slot->weight = (short)weight, slot->w = w, slot->lru = ++cache_lru;
		memcpy(slot->text, s, len + 1);
	}
	return w;
}

int BOS_lineH(int px) {
	TTF_Font* f = fontFor(px, 400, false);
	return f ? TTF_FontHeight(f) : px;
}

int BOS_text(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int y) {
	if (!s || !s[0])
		return 0;
	TTF_Font* latin = fontFor(px, weight, false);
	int baseline = y + (latin ? TTF_FontAscent(latin) : px); // Inter's line box sets the baseline for both scripts
	rgb &= 0xFFFFFF;
	Uint8 mod = alpha < 1.0f ? (Uint8)(alpha * 255 + 0.5f) : 255;
	size_t len = strlen(s);
	TextEntry* slot = NULL;
	unsigned hash = 0;
	if (len < BOS_TC_TEXT) {
		hash = strHash(s);
		for (int i = 0; i < 4; i++) {
			TextEntry* e = &tcache[(hash + i) % BOS_TC_SIZE];
			if (e->surf && e->hash == hash && e->px == px && e->weight == weight && e->rgb == rgb &&
				!strcmp(e->text, s)) {
				e->lru = ++cache_lru;
				BOS_blit(e->surf, dst, x, baseline - e->ascent, mod / 255.0f);
				return e->surf->w;
			}
			if (!slot || e->lru < slot->lru)
				slot = e;
		}
	}
	char g[1024];
	const char* d;
	TTF_Font* f = prepare(s, px, weight, g, sizeof(g), &d);
	if (!f)
		return 0;
	SDL_Color c = {(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255};
	SDL_Surface* t = TTF_RenderUTF8_Blended(f, d, c);
	if (!t)
		return 0;
	BOS_blit(t, dst, x, baseline - TTF_FontAscent(f), mod / 255.0f);
	int w = t->w;
	if (slot) {
		if (slot->surf)
			SDL_FreeSurface(slot->surf);
		slot->hash = hash, slot->px = (short)px, slot->weight = (short)weight, slot->rgb = rgb;
		slot->surf = t, slot->ascent = TTF_FontAscent(f), slot->lru = ++cache_lru;
		memcpy(slot->text, s, len + 1);
	} else
		SDL_FreeSurface(t);
	return w;
}

int BOS_textMid(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int cy) {
	return BOS_text(dst, s, px, weight, rgb, alpha, x, cy - BOS_lineH(px) / 2);
}

// the bytes of s that make whole UTF-8 characters, at most max bytes
static size_t utf8Whole(const char* s, size_t max) {
	size_t n = strlen(s);
	if (n > max)
		n = max;
	size_t i = n;
	while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) // back over continuation bytes
		i--;
	if (i > 0) { // s[i-1] is a lead byte (or ASCII): keep it only if its whole sequence fits
		unsigned char c = (unsigned char)s[i - 1];
		size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
		if (i - 1 + len <= n)
			return i - 1 + len;
		return i - 1;
	}
	return 0;
}

// s cut with "…" to max_w. Cuts only between whole characters (a cut through a Thai letter made text that
// measured as fitting and then drew nothing), and finds the length by halving, not one letter at a time.
void BOS_fit(const char* s, int px, int weight, int max_w, char* out, int size) {
	if (size <= 0)
		return;
	s = s ? s : "";
	size_t whole = utf8Whole(s, (size_t)size - 1);
	memcpy(out, s, whole);
	out[whole] = '\0';
	if (whole == strlen(s) && BOS_textW(out, px, weight) <= max_w)
		return;
	// the longest head (whole characters) + "…" that fits
	char buf[1024];
	size_t lo = 0, hi = whole < sizeof(buf) - 4 ? whole : sizeof(buf) - 4;
	while (lo < hi) {
		size_t mid = utf8Whole(out, (lo + hi + 1) / 2);
		if (mid <= lo) { // the character at the middle is wider than the gap: step a whole one
			mid = lo + 1;
			while (mid < hi && ((unsigned char)out[mid] & 0xC0) == 0x80)
				mid++;
		}
		memcpy(buf, out, mid);
		memcpy(buf + mid, "\xE2\x80\xA6", 4);
		if (BOS_textW(buf, px, weight) <= max_w)
			lo = mid;
		else
			hi = mid - 1 > lo ? utf8Whole(out, mid - 1) : lo;
	}
	if (lo == 0) {
		out[0] = '\0';
		return;
	}
	memcpy(buf, out, lo);
	memcpy(buf + lo, "\xE2\x80\xA6", 4);
	snprintf(out, size, "%s", buf);
}

// ---- marquee: a focused text too long for its room scrolls instead of being cut (§5.3) ----
// hold 0.5 s at the start, scroll at 120 px/s to its end, hold 1 s, jump back. Up to 4 at once (a row's name and value).
typedef struct {
	unsigned key;
	uint32_t start, seen;
} MarqueeSlot;
static MarqueeSlot marquees[4];
static uint32_t marquee_at; // the last time one was drawn scrolling

bool BOS_marqueeActive(void) {
	return marquee_at && SDL_GetTicks() - marquee_at < 300;
}

int BOS_textMarquee(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int cy,
					int max_w) {
	if (!s || !s[0] || max_w <= 0)
		return 0;
	int w = BOS_textW(s, px, weight);
	if (w <= max_w)
		return BOS_textMid(dst, s, px, weight, rgb, alpha, x, cy);
	uint32_t now = SDL_GetTicks();
	unsigned key = 2166136261u ^ (unsigned)px;
	for (const char* p = s; *p; p++)
		key = (key ^ (unsigned char)*p) * 16777619u;
	MarqueeSlot* sl = &marquees[0];
	for (int i = 0; i < 4; i++) {
		if (marquees[i].key == key) {
			sl = &marquees[i];
			break;
		}
		if (marquees[i].seen < sl->seen)
			sl = &marquees[i];
	}
	if (sl->key != key || now - sl->seen > 300) // new, or not on screen a moment ago: from the start
		sl->key = key, sl->start = now;
	sl->seen = now;
	int over = w - max_w, speed = BOS_px(120), hold = 500, end_hold = 1000; // user: start at 0.5 s, faster
	int run = over * 1000 / (speed > 0 ? speed : 1);
	int t = (int)((now - sl->start) % (uint32_t)(hold + run + end_hold));
	int off = t < hold ? 0 : t < hold + run ? (t - hold) * speed / 1000 : over;
	SDL_Rect old, r = {x, cy - BOS_lineH(px), max_w, 2 * BOS_lineH(px)}, clip;
	SDL_GetClipRect(dst, &old);
	if (!SDL_IntersectRect(&r, &old, &clip))
		return max_w;
	SDL_SetClipRect(dst, &clip);
	BOS_textMid(dst, s, px, weight, rgb, alpha, x - off, cy);
	SDL_SetClipRect(dst, &old);
	marquee_at = now ? now : 1;
	return max_w;
}

// ---- wrapped text: up to max_lines lines max_w wide; a longer last line scrolls ----
static unsigned cpAt(const char* p, int* len) { // the UTF-8 code point at p
	const unsigned char* u = (const unsigned char*)p;
	if (u[0] < 0x80)
		return *len = 1, u[0];
	if ((u[0] & 0xE0) == 0xC0 && u[1])
		return *len = 2, ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
	if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2])
		return *len = 3, ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
	return *len = 1, u[0];
}
// Thai has no spaces between words: a line may break between two code points unless the next one belongs to the
// letter before it (an above/below mark, ะ า ำ ๆ) or this one is a leading vowel (เ แ โ ใ ไ) that needs its letter.
static bool breakBefore(unsigned prev, unsigned next) {
	if (next == 0x0E30 || next == 0x0E31 || (next >= 0x0E32 && next <= 0x0E3A) || next == 0x0E45 || next == 0x0E46 ||
		(next >= 0x0E47 && next <= 0x0E4E))
		return false;
	if (prev >= 0x0E40 && prev <= 0x0E44)
		return false;
	return true;
}

// dst NULL: only counts. centre: each line centred in max_w. A "\n" ends a line.
static int wrapImpl(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int y,
					int max_w, int max_lines, bool centre) {
	if (!s || !s[0] || max_lines <= 0)
		return 0;
	int lh = BOS_lineH(px), lines = 0;
	const char* p = s;
	char line[512];
	while (*p && lines < max_lines) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		int seg = (int)strcspn(p, "\n"); // this paragraph
		if (lines == max_lines - 1) { // the last line takes the rest (paragraphs joined), scrolling
			int len = 0;
			for (const char* q = p; *q && len < (int)sizeof(line) - 1; q++)
				line[len++] = *q == '\n' ? ' ' : *q;
			line[len] = '\0';
		} else {
			int len = seg < (int)sizeof(line) - 1 ? seg : (int)sizeof(line) - 1;
			memcpy(line, p, len), line[len] = '\0';
		}
		if (lines == max_lines - 1 || BOS_textW(line, px, weight) <= max_w) {
			if (dst) {
				int w = BOS_textW(line, px, weight);
				int lx = centre && w < max_w ? x + (max_w - w) / 2 : x;
				BOS_textMarquee(dst, line, px, weight, rgb, alpha, lx, y + lines * lh + lh / 2, max_w);
			}
			lines++;
			if (lines >= max_lines)
				break;
			p += seg;
			if (*p == '\n')
				p++;
			continue;
		}
		// the longest head that fits, ending at a space if there is one, else at a legal Thai break
		const char *q = p, *fit = NULL, *space = NULL, *stop = p + seg;
		unsigned prev = 0;
		while (q < stop) {
			int n;
			unsigned cp = cpAt(q, &n);
			if (q > p && (prev == ' ' || breakBefore(prev, cp))) {
				int len = (int)(q - p);
				if (len >= (int)sizeof(line))
					break;
				memcpy(line, p, len), line[len] = '\0';
				if (BOS_textW(line, px, weight) > max_w)
					break;
				fit = q;
				if (prev == ' ')
					space = q;
			}
			prev = cp;
			q += n;
		}
		const char* end = space ? space : fit ? fit : stop;
		if (end == p)
			end = stop;
		int len = (int)(end - p) < (int)sizeof(line) ? (int)(end - p) : (int)sizeof(line) - 1;
		memcpy(line, p, len), line[len] = '\0';
		if (dst) {
			int w = BOS_textW(line, px, weight);
			BOS_text(dst, line, px, weight, rgb, alpha, centre && w < max_w ? x + (max_w - w) / 2 : x, y + lines * lh);
		}
		lines++;
		p = end;
		if (*p == '\n')
			p++;
	}
	return lines * lh;
}

int BOS_textWrap(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int y, int max_w,
				 int max_lines) {
	return dst ? wrapImpl(dst, s, px, weight, rgb, alpha, x, y, max_w, max_lines, false) : 0;
}

int BOS_textWrapCentre(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int y,
					   int max_w, int max_lines) {
	return dst ? wrapImpl(dst, s, px, weight, rgb, alpha, x, y, max_w, max_lines, true) : 0;
}

int BOS_textLines(const char* s, int px, int weight, int max_w, int max_lines) {
	return wrapImpl(NULL, s, px, weight, 0, 1, 0, 0, max_w, max_lines, false) / BOS_lineH(px);
}

// ---- shapes ----
uint32_t BOS_map(SDL_Surface* s, uint32_t rgb) {
	return SDL_MapRGB(s->format, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

void BOS_rrect(SDL_Surface* s, int x, int y, int w, int h, int r, uint32_t rgb) {
	UI_fillRoundedRect(s, x, y, w, h, r, BOS_map(s, rgb));
}

void BOS_ring(SDL_Surface* s, int x, int y, int w, int h, int r, int th, uint32_t rgb) {
	if (w <= 0 || h <= 0 || th <= 0)
		return;
	if (s->format->BytesPerPixel != 4) { // not expected: the old way
		BOS_rrect(s, x, y, w, h, r, rgb);
		BOS_rrect(s, x + th, y + th, w - 2 * th, h - 2 * th, r > th ? r - th : 0, BOS_theme()->bg);
		return;
	}
	float hw = w / 2.0f, hh = h / 2.0f, rad = r < hw && r < hh ? r : (hw < hh ? hw : hh);
	float cx = x + hw, cy = y + hh;
	int cr = (rgb >> 16) & 255, cg = (rgb >> 8) & 255, cb = rgb & 255;
	SDL_Rect clip;
	SDL_GetClipRect(s, &clip);
	int y0 = y > clip.y ? y : clip.y, y1 = y + h < clip.y + clip.h ? y + h : clip.y + clip.h;
	int xa = x > clip.x ? x : clip.x, xb = x + w < clip.x + clip.w ? x + w : clip.x + clip.w;
	if (SDL_MUSTLOCK(s))
		SDL_LockSurface(s);
	for (int py = y0; py < y1; py++) {
		uint32_t* row = (uint32_t*)((uint8_t*)s->pixels + py * s->pitch);
		float dy = (py + 0.5f) - cy;
		bool straight = (dy < 0 ? -dy : dy) < hh - rad - 1; // between the corners: only the two side bands
		for (int px = xa; px < xb; px++) {
			if (straight && px >= x + th + 1 && px < x + w - th - 1) {
				px = x + w - th - 2;
				continue;
			}
			float dx = (px + 0.5f) - cx;
			float qx = (dx < 0 ? -dx : dx) - hw + rad, qy = (dy < 0 ? -dy : dy) - hh + rad;
			float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
			float in = qx > qy ? qx : qy;
			float d = sqrtf(mx * mx + my * my) + (in < 0 ? in : 0) - rad; // < 0 inside the rounded box
			float a = 0.5f - d, b = 0.5f - d - th;
			a = a < 0 ? 0 : a > 1 ? 1 : a;
			b = b < 0 ? 0 : b > 1 ? 1 : b;
			float k = a - b;
			if (k <= 0)
				continue;
			Uint8 r8, g8, b8;
			SDL_GetRGB(row[px], s->format, &r8, &g8, &b8);
			row[px] = SDL_MapRGB(s->format, (Uint8)(r8 + (cr - r8) * k + 0.5f), (Uint8)(g8 + (cg - g8) * k + 0.5f),
								 (Uint8)(b8 + (cb - b8) * k + 0.5f));
		}
	}
	if (SDL_MUSTLOCK(s))
		SDL_UnlockSurface(s);
}

void BOS_focusRing(SDL_Surface* s, int x, int y, int w, int h, int r) {
	int q = 2 * BOS_px(BOS_RING);
	BOS_ring(s, x - q, y - q, w + 2 * q, h + 2 * q, r + q, BOS_px(BOS_RING), BOS_theme()->focus);
}

// ---- icons ----
typedef struct {
	char name[24];
	SDL_Surface* src; // 128 px white
} IconSlot;
static IconSlot icons[64];
static int nicons;

static SDL_Surface* iconSrc(const char* name) {
	for (int i = 0; i < nicons; i++)
		if (!strcmp(icons[i].name, name))
			return icons[i].src;
	if (nicons == (int)(sizeof(icons) / sizeof(icons[0])))
		return NULL;
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s/icons/%s.png", BOS_RES, name);
	SDL_Surface* raw = IMG_Load(path);
	SDL_Surface* s = raw ? SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0) : NULL;
	if (raw)
		SDL_FreeSurface(raw);
	if (s)
		SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_BLEND);
	snprintf(icons[nicons].name, sizeof(icons[nicons].name), "%s", name);
	icons[nicons++].src = s; // NULL is cached too: a missing icon is looked up once
	return s;
}

// An icon at one size in one colour, made once: scaling the 128 px source and tinting it on every draw was a
// software stretch per icon per frame. Each pixel is the average of the source pixels it covers (smoother than the
// nearest-pixel stretch it replaces).
typedef struct {
	SDL_Surface* src; // which icon
	int size;
	uint32_t rgb, lru;
	SDL_Surface* surf;
} IconDrawn;
static IconDrawn drawn[96];
static uint32_t drawn_lru;

static SDL_Surface* iconScaled(SDL_Surface* src, int size, uint32_t rgb) {
	SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!out)
		return NULL;
	int tr = (rgb >> 16) & 255, tg = (rgb >> 8) & 255, tb = rgb & 255;
	for (int y = 0; y < size; y++) {
		int y0 = y * src->h / size, y1 = (y + 1) * src->h / size;
		if (y1 <= y0)
			y1 = y0 + 1;
		uint32_t* o = (uint32_t*)((uint8_t*)out->pixels + y * out->pitch);
		for (int x = 0; x < size; x++) {
			int x0 = x * src->w / size, x1 = (x + 1) * src->w / size;
			if (x1 <= x0)
				x1 = x0 + 1;
			uint32_t a = 0, r = 0, g = 0, b = 0;
			for (int sy = y0; sy < y1; sy++) {
				const uint32_t* p = (const uint32_t*)((const uint8_t*)src->pixels + sy * src->pitch) + x0;
				for (int sx = x0; sx < x1; sx++, p++) {
					uint32_t pa = *p >> 24;
					a += pa, r += ((*p >> 16) & 255) * pa, g += ((*p >> 8) & 255) * pa, b += (*p & 255) * pa;
				}
			}
			uint32_t n = (uint32_t)((y1 - y0) * (x1 - x0));
			if (a) // colour weighted by coverage, then the tint
				r = r / a * tr / 255, g = g / a * tg / 255, b = b / a * tb / 255;
			o[x] = (a / n) << 24 | r << 16 | g << 8 | b;
		}
	}
	SDL_SetSurfaceBlendMode(out, SDL_BLENDMODE_BLEND);
	return out;
}

void BOS_icon(SDL_Surface* dst, const char* name, uint32_t rgb, float alpha, int x, int y, int size) {
	SDL_Surface* s = iconSrc(name);
	if (!s || size <= 0)
		return;
	rgb &= 0xFFFFFF;
	IconDrawn* slot = &drawn[0];
	SDL_Surface* got = NULL;
	for (int i = 0; i < (int)(sizeof(drawn) / sizeof(drawn[0])); i++) {
		IconDrawn* e = &drawn[i];
		if (e->surf && e->src == s && e->size == size && e->rgb == rgb) {
			e->lru = ++drawn_lru;
			got = e->surf;
			break;
		}
		if (e->lru < slot->lru)
			slot = e;
	}
	if (!got) {
		got = iconScaled(s, size, rgb);
		if (!got)
			return;
		if (slot->surf)
			SDL_FreeSurface(slot->surf);
		*slot = (IconDrawn){s, size, rgb, ++drawn_lru, got};
	}
	BOS_blit(got, dst, x, y, alpha);
}

// ---- status bar (§5.1) ----
static bool music_playing;
void BOS_setMusicPlaying(bool playing) {
	music_playing = playing;
}

static int battery(SDL_Surface* s, int right, int cy, bool percent) { // draws right-aligned at `right`, returns its left edge
	int charging = 0, pct = 0;
	PWR_getBattery(&charging, &pct); // the power thread's reading: asking the hardware here cost 6 ms a frame
	pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
	uint32_t col = charging ? BOS_C_OK : pct <= BOS_BATT_DANGER ? BOS_C_DANGER : pct <= BOS_BATT_WARN ? BOS_C_WARN : BOS_C_TEXT;
	// the glyph: 46x24 outline (3 px, radius 7), a 4x8 nub 4 px out, a fill bar inset 3 px
	int bw = BOS_px(46), bh = BOS_px(24), b = BOS_px(3), nub = BOS_px(4);
	int bx = right - nub - bw, by = cy - bh / 2;
	BOS_rrect(s, bx, by, bw, bh, BOS_px(7), col);
	BOS_rrect(s, bx + b, by + b, bw - 2 * b, bh - 2 * b, BOS_px(4), BOS_C_BG);
	int inner = bw - 4 * b, fill = inner * pct / 100;
	if (fill > 0)
		BOS_rrect(s, bx + 2 * b, by + 2 * b, fill, bh - 4 * b, BOS_px(2), col);
	BOS_rrect(s, bx + bw, cy - BOS_px(4), nub, BOS_px(8), BOS_px(2), col);
	int x = bx;
	if (percent) {
		char p[8];
		snprintf(p, sizeof(p), "%d%%", pct);
		x -= BOS_px(8) + BOS_textW(p, BOS_FS_SMALL, 600);
		BOS_textMid(s, p, BOS_FS_SMALL, 600, col, 1, x, cy);
	}
	if (charging) { // never colour alone: the bolt says "charging"
		x -= BOS_px(8) + BOS_px(26);
		BOS_icon(s, "bolt", col, 1, x, cy - BOS_px(13), BOS_px(26));
	}
	return x;
}

void BOS_statusBar(SDL_Surface* s, const char* title) {
	int h = BOS_px(BOS_BAR_H), cy = h / 2, pad = BOS_px(BOS_BAR_PAD);
	SDL_FillRect(s, &(SDL_Rect){0, 0, s->w, h}, BOS_map(s, BOS_theme()->bg));
	if (title)
		BOS_textMid(s, title, BOS_FS_SMALL, 600, BOS_C_TEXT, 1, pad, cy);

	int bits = CFG_getStatusBar(); // ตั้งค่า › หน้าตา › แถบสถานะ; the battery icon always shows
	int x = battery(s, s->w - pad, cy, bits & STATUSBAR_PERCENT);
	if (bits & STATUSBAR_CLOCK) {
		char clock[16];
		time_t now = time(NULL);
		strftime(clock, sizeof(clock), CFG_getClock24H() ? "%H:%M" : "%I:%M", localtime(&now));
		x -= BOS_px(22) + BOS_textW(clock, BOS_FS_SMALL, 600);
		BOS_textMid(s, clock, BOS_FS_SMALL, 600, BOS_C_TEXT, 1, x, cy);
	}

	// right to left: WiFi, mute, BT, music — each only while it is true (§5.1)
	const char* shown[4];
	int n = 0;
	// PWR_isOnline is the power thread's reading. PLAT_wifiConnected runs wpa_cli in a shell: 19 ms, and this is
	// drawn every frame — it was half of every frame of every page.
	if ((bits & STATUSBAR_WIFI) && PWR_isOnline())
		shown[n++] = "wifi";
	if ((bits & STATUSBAR_MUTE) && (GetSpeakerMute() || GetVolume() == 0))
		shown[n++] = "mute";
	if ((bits & STATUSBAR_BT) && PLAT_btIsConnected())
		shown[n++] = "bt";
	if ((bits & STATUSBAR_MUSIC) && music_playing)
		shown[n++] = "music";
	int is = BOS_px(BOS_STATUS_ICON);
	for (int i = 0; i < n; i++) {
		x -= BOS_px(22) + is;
		BOS_icon(s, shown[i], BOS_C_TEXT, 1, x, cy - is / 2, is);
	}
}

// ---- motion (§5.7): ภาพเคลื่อนไหว เต็ม 180 ms / น้อย 120 ms / ปิด (a cut) ----
int BOS_tweenMs(void) {
	if (!CFG_getMenuAnimations())
		return 1; // t reaches 1 on the next frame
	return CFG_getMenuTransitions() ? 180 : 120;
}

// ---- hint bar (§5.2) ----
static bool isFace(const char* b) {
	return b[0] && !b[1] && strchr("ABXY", b[0]);
}

static int hintW(const BosHint* h) {
	int cap = isFace(h->btn) ? BOS_px(BOS_HINT_CAP) : BOS_textW(h->btn, BOS_FS_HINT_WORD, 700) + 2 * BOS_px(12);
	return cap + BOS_px(12) + BOS_textW(h->label, BOS_FS_SMALL, 400);
}

static void hint(SDL_Surface* s, const BosHint* h, int x, int cy) {
	int ch = BOS_px(BOS_HINT_CAP);
	int cw;
	if (isFace(h->btn)) {
		cw = ch;
		BOS_rrect(s, x, cy - ch / 2, cw, ch, ch / 2, theme.hint_cap);
		int tw = BOS_textW(h->btn, BOS_FS_HINT_FACE, 700);
		BOS_textMid(s, h->btn, BOS_FS_HINT_FACE, 700, theme.onhint, 1, x + (cw - tw) / 2, cy);
	} else {
		int tw = BOS_textW(h->btn, BOS_FS_HINT_WORD, 700);
		cw = tw + 2 * BOS_px(12);
		BOS_rrect(s, x, cy - ch / 2, cw, ch, BOS_px(12), theme.hint_cap);
		BOS_textMid(s, h->btn, BOS_FS_HINT_WORD, 700, theme.onhint, 1, x + BOS_px(12), cy);
	}
	BOS_textMid(s, h->label, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, x + cw + BOS_px(12), cy);
}

void BOS_hintBar(SDL_Surface* s, const BosHint* right, int nright, const BosHint* left, int nleft) {
	int h = BOS_px(BOS_BAR_H), top = s->h - h, cy = top + h / 2, pad = BOS_px(BOS_BAR_PAD);
	SDL_FillRect(s, &(SDL_Rect){0, top, s->w, h}, BOS_map(s, BOS_theme()->bg));
	int x = pad;
	for (int i = 0; i < nleft && i < 2; i++) {
		hint(s, &left[i], x, cy);
		x += hintW(&left[i]) + BOS_px(28);
	}
	x = s->w - pad;
	for (int i = 0; i < nright && i < 4; i++) { // A first, drawn rightmost
		x -= hintW(&right[i]);
		hint(s, &right[i], x, cy);
		x -= BOS_px(36);
	}
}

// ---- large title (§5.5) ----
int BOS_largeTitle(SDL_Surface* s, int y, const char* title, const char* sub) {
	int x = BOS_px(BOS_PAGE_PAD), top = y + BOS_px(4);
	BOS_text(s, title, BOS_FS_TITLE, 700, BOS_C_TEXT, 1, x, top);
	int h = BOS_px(4) + (int)(BOS_FS_TITLE * 1.15f);
	if (sub && sub[0]) {
		BOS_text(s, sub, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, x, y + h + BOS_px(2));
		h += BOS_px(2) + BOS_lineH(BOS_FS_SMALL);
	}
	return h + BOS_px(12);
}

void BOS_quit(void) {
	for (int i = 0; i < nfonts; i++)
		TTF_CloseFont(fonts[i].f);
	nfonts = 0;
	for (int i = 0; i < nicons; i++)
		if (icons[i].src)
			SDL_FreeSurface(icons[i].src);
	nicons = 0;
	for (int i = 0; i < (int)(sizeof(drawn) / sizeof(drawn[0])); i++)
		if (drawn[i].surf)
			SDL_FreeSurface(drawn[i].surf);
	memset(drawn, 0, sizeof(drawn));
	for (int i = 0; i < BOS_TC_SIZE; i++)
		if (tcache[i].surf)
			SDL_FreeSurface(tcache[i].surf);
	memset(tcache, 0, sizeof(tcache));
	memset(wcache, 0, sizeof(wcache));
}
