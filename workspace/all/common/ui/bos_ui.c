// Brick OS UI 2.0 shared kit — see bos_ui.h. Every number here is a token (bos_tokens.h) or a DESIGN.md §5 size.

#include "bos_ui.h"

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

int BOS_px(float v) {
	return (int)(v * (FIXED_WIDTH / 1024.0f) + 0.5f);
}

static BosTheme theme = {BOS_C_FOCUS, BOS_C_ONFOCUS, BOS_C_PRIMARY, BOS_C_FOCUS, BOS_C_BG};
const BosTheme* BOS_theme(void) {
	return &theme; // ธีมสี (DESIGN.md §6.8) will fill this from the shared config
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

int BOS_textW(const char* s, int px, int weight) {
	if (!s || !s[0])
		return 0;
	char g[1024];
	const char* d;
	TTF_Font* f = prepare(s, px, weight, g, sizeof(g), &d);
	int w = 0;
	if (f)
		TTF_SizeUTF8(f, d, &w, NULL);
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
	char g[1024];
	const char* d;
	TTF_Font* f = prepare(s, px, weight, g, sizeof(g), &d);
	if (!f)
		return 0;
	SDL_Color c = {(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255};
	SDL_Surface* t = TTF_RenderUTF8_Blended(f, d, c);
	if (!t)
		return 0;
	if (alpha < 1.0f)
		SDL_SetSurfaceAlphaMod(t, (Uint8)(alpha * 255 + 0.5f));
	SDL_BlitSurface(t, NULL, dst, &(SDL_Rect){x, baseline - TTF_FontAscent(f)});
	int w = t->w;
	SDL_FreeSurface(t);
	return w;
}

int BOS_textMid(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int cy) {
	return BOS_text(dst, s, px, weight, rgb, alpha, x, cy - BOS_lineH(px) / 2);
}

void BOS_fit(const char* s, int px, int weight, int max_w, char* out, int size) {
	snprintf(out, size, "%s", s ? s : "");
	if (BOS_textW(out, px, weight) <= max_w)
		return;
	size_t n = strlen(out);
	while (n > 0) {
		do
			n--;
		while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80); // back to a UTF-8 lead byte
		char buf[512];
		snprintf(buf, sizeof(buf), "%.*s\xE2\x80\xA6", (int)n, out);
		if (BOS_textW(buf, px, weight) <= max_w) {
			snprintf(out, size, "%s", buf);
			return;
		}
	}
	out[0] = '\0';
}

// ---- marquee: a focused text too long for its room scrolls instead of being cut (§5.3) ----
// hold 1.2 s at the start, scroll at 60 px/s to its end, hold 1.2 s, jump back. Up to 4 at once (a row's name and value).
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
	int over = w - max_w, speed = BOS_px(60), hold = 1200;
	int run = over * 1000 / (speed > 0 ? speed : 1);
	int t = (int)((now - sl->start) % (uint32_t)(hold + run + hold));
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

int BOS_textWrap(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int y, int max_w,
				 int max_lines) {
	if (!s || !s[0] || max_lines <= 0)
		return 0;
	int lh = BOS_lineH(px), lines = 0;
	const char* p = s;
	char line[512];
	while (*p && lines < max_lines) {
		while (*p == ' ')
			p++;
		if (lines == max_lines - 1 || BOS_textW(p, px, weight) <= max_w) { // the last line takes the rest, scrolling
			BOS_textMarquee(dst, p, px, weight, rgb, alpha, x, y + lines * lh + lh / 2, max_w);
			return (lines + 1) * lh;
		}
		// the longest head that fits, ending at a space if there is one, else at a legal Thai break
		const char *q = p, *fit = NULL, *space = NULL;
		unsigned prev = 0;
		while (*q) {
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
		const char* end = space ? space : fit ? fit : q;
		if (end == p) // not even one cluster fits: give up on wrapping
			end = p + strlen(p);
		int len = (int)(end - p) < (int)sizeof(line) ? (int)(end - p) : (int)sizeof(line) - 1;
		memcpy(line, p, len), line[len] = '\0';
		BOS_text(dst, line, px, weight, rgb, alpha, x, y + lines * lh);
		lines++;
		p = end;
	}
	return lines * lh;
}

// ---- shapes ----
uint32_t BOS_map(SDL_Surface* s, uint32_t rgb) {
	return SDL_MapRGB(s->format, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

void BOS_rrect(SDL_Surface* s, int x, int y, int w, int h, int r, uint32_t rgb) {
	UI_fillRoundedRect(s, x, y, w, h, r, BOS_map(s, rgb));
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

void BOS_icon(SDL_Surface* dst, const char* name, uint32_t rgb, float alpha, int x, int y, int size) {
	SDL_Surface* s = iconSrc(name);
	if (!s)
		return;
	SDL_SetSurfaceColorMod(s, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
	SDL_SetSurfaceAlphaMod(s, (Uint8)(alpha * 255 + 0.5f));
	SDL_BlitScaled(s, NULL, dst, &(SDL_Rect){x, y, size, size});
}

// ---- status bar (§5.1) ----
static bool music_playing;
void BOS_setMusicPlaying(bool playing) {
	music_playing = playing;
}

static int battery(SDL_Surface* s, int right, int cy, bool percent) { // draws right-aligned at `right`, returns its left edge
	int charging = 0, pct = 0;
	PLAT_getBatteryStatusFine(&charging, &pct);
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
	SDL_FillRect(s, &(SDL_Rect){0, 0, s->w, h}, BOS_map(s, theme.bg));
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
	if ((bits & STATUSBAR_WIFI) && PLAT_wifiConnected())
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
		BOS_textMid(s, h->btn, BOS_FS_HINT_FACE, 700, theme.onfocus, 1, x + (cw - tw) / 2, cy);
	} else {
		int tw = BOS_textW(h->btn, BOS_FS_HINT_WORD, 700);
		cw = tw + 2 * BOS_px(12);
		BOS_rrect(s, x, cy - ch / 2, cw, ch, BOS_px(12), theme.hint_cap);
		BOS_textMid(s, h->btn, BOS_FS_HINT_WORD, 700, theme.onfocus, 1, x + BOS_px(12), cy);
	}
	BOS_textMid(s, h->label, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, x + cw + BOS_px(12), cy);
}

void BOS_hintBar(SDL_Surface* s, const BosHint* right, int nright, const BosHint* left, int nleft) {
	int h = BOS_px(BOS_BAR_H), top = s->h - h, cy = top + h / 2, pad = BOS_px(BOS_BAR_PAD);
	SDL_FillRect(s, &(SDL_Rect){0, top, s->w, h}, BOS_map(s, theme.bg));
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
}
