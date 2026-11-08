// Brick OS ตั้งค่า (DESIGN.md 6.6): categories on the left (the root page), the highlighted category's page on the
// right (the page open now once one is pushed). Only the drawing changes: input, pages and items are the menu's own.

#include "settings_bos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "api.h"
#include "defines.h"
#include "i18n.h"
#include "text_thai.h"
#include "ui/ui_draw.h"

#define C_BG 0x000000
#define C_SURFACE1 0x1C1C1E
#define C_SURFACE2 0x2C2C2E
#define C_SURFACE3 0x3A3A3C
#define C_TEXT 0xF2F2F7
#define C_TEXT2 0xA1A1A6
#define C_TEXT3 0xC7C7CC
#define C_TEXT4 0x6E6E73
#define C_ACCENT 0x0A84FF

static float k = 1.0f;
static int P(float v) {
	return (int)(v * k + 0.5f);
}

typedef struct {
	int px, weight;
	bool thai;
	TTF_Font* f;
} FontSlot;
static FontSlot fonts[24];
static int nfonts;

static TTF_Font* fontFor(int px, int weight, bool thai) {
	for (int i = 0; i < nfonts; i++)
		if (fonts[i].px == px && fonts[i].weight == weight && fonts[i].thai == thai)
			return fonts[i].f;
	if (nfonts == (int)(sizeof(fonts) / sizeof(fonts[0])))
		return NULL;
	TTF_Font* f;
	if (thai) {
		f = TTF_OpenFont(GFX_getThaiFontPath(), px);
		if (f && weight >= 600)
			TTF_SetFontStyle(f, TTF_STYLE_BOLD);
	} else {
		char path[MAX_PATH];
		const char* name = weight >= 700 ? "Inter-Bold" : weight >= 600 ? "Inter-SemiBold"
													: weight >= 500	  ? "Inter-Medium"
																	  : "Inter-Regular";
		snprintf(path, sizeof(path), "%s/bos/%s.ttf", RES_PATH, name);
		f = TTF_OpenFont(path, px);
	}
	if (!f)
		return NULL;
	fonts[nfonts++] = (FontSlot){px, weight, thai, f};
	return f;
}

static SDL_Color rgb(uint32_t c) {
	return (SDL_Color){(c >> 16) & 255, (c >> 8) & 255, c & 255, 255};
}
static uint32_t mapc(SDL_Surface* s, uint32_t c) {
	return SDL_MapRGB(s->format, (c >> 16) & 255, (c >> 8) & 255, c & 255);
}
static void rrect(SDL_Surface* s, int x, int y, int w, int h, int r, uint32_t c) {
	UI_fillRoundedRect(s, x, y, w, h, r, mapc(s, c));
}
static int textW(const char* s, int px, int weight) {
	if (!s || !s[0])
		return 0;
	int w = 0;
	if (TextThai_has(s)) {
		char g[1024];
		TTF_Font* f = fontFor(px, weight, true);
		if (f && TextThai_toGlyphs(s, g, sizeof(g)) > 0)
			TTF_SizeUTF8(f, g, &w, NULL);
	} else {
		TTF_Font* f = fontFor(px, weight, false);
		if (f)
			TTF_SizeUTF8(f, s, &w, NULL);
	}
	return w;
}
static int lineH(int px) {
	TTF_Font* f = fontFor(px, 400, false);
	return f ? TTF_FontHeight(f) : px;
}
static void text(SDL_Surface* dst, const char* s, int px, int weight, uint32_t c, int x, int y) {
	if (!s || !s[0])
		return;
	TTF_Font* latin = fontFor(px, weight, false);
	int baseline = y + (latin ? TTF_FontAscent(latin) : px);
	TTF_Font* f = latin;
	const char* draw = s;
	char g[1024];
	if (TextThai_has(s)) {
		f = fontFor(px, weight, true);
		if (!f || TextThai_toGlyphs(s, g, sizeof(g)) <= 0)
			return;
		draw = g;
	}
	if (!f)
		return;
	SDL_Surface* t = TTF_RenderUTF8_Blended(f, draw, rgb(c));
	if (!t)
		return;
	SDL_BlitSurface(t, NULL, dst, &(SDL_Rect){x, baseline - TTF_FontAscent(f)});
	SDL_FreeSurface(t);
}
static void fit(const char* s, int px, int weight, int max_w, char* out, size_t size) {
	snprintf(out, size, "%s", s ? s : "");
	if (textW(out, px, weight) <= max_w)
		return;
	size_t n = strlen(out);
	while (n > 0) {
		do
			n--;
		while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80);
		char buf[512];
		snprintf(buf, sizeof(buf), "%.*s\xE2\x80\xA6", (int)n, out);
		if (textW(buf, px, weight) <= max_w) {
			snprintf(out, size, "%s", buf);
			return;
		}
	}
	out[0] = '\0';
}

// category chips (DESIGN.md 2): the mockup's categories by their English names; the rest the About grey
static void chipFor(const char* name, char* mark, size_t size, uint32_t* color) {
	static const struct {
		const char* name;
		const char* mark;
		uint32_t color;
	} map[] = {
		{"Network", "W", 0x0A6CD6},	   {"Bluetooth", "B", 0x0A6CD6},	{"Display", "\xE0\xB8\x88", 0x5E5CE6},
		{"Appearance", "\xE0\xB8\x88", 0x5E5CE6}, {"Layouts", "\xE0\xB8\x88", 0x5E5CE6},
		{"Audio", "\xE0\xB8\xAA", 0xD63A6B},	 {"Emulator Shortcuts", "\xE0\xB8\x81", 0x3A8A4F},
		{"FN switch", "\xE0\xB8\x81", 0x3A8A4F}, {"F1 / F2 keys", "\xE0\xB8\x81", 0x3A8A4F},
		{"Input Tester", "\xE0\xB8\x81", 0x3A8A4F}, {"System", "\xE0\xB8\x9E", 0x2F8F4E},
		{"About", "i", 0x636366},
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcmp(name, map[i].name)) {
			snprintf(mark, size, "%s", map[i].mark);
			*color = map[i].color;
			return;
		}
	snprintf(mark, size, "%c", name[0] ? name[0] : '?');
	*color = 0x636366;
}

static const char* itemValue(SettingItem* it) {
	if ((it->type == ITEM_CYCLE || it->type == ITEM_COLOR) && it->labels && it->current_idx >= 0 &&
		it->current_idx < it->label_count)
		return it->labels[it->current_idx];
	if (it->type == ITEM_STATIC)
		return it->get_display ? it->get_display() : it->display_text;
	if (it->type == ITEM_TEXT_INPUT)
		return it->get_text ? it->get_text() : it->text_value;
	return "";
}

static bool numericRange(SettingItem* it) { // a 0..N cycle drawn as the mockup's slider
	if (it->type != ITEM_CYCLE || !it->labels || it->label_count < 5)
		return false;
	for (int i = 0; i < it->label_count; i++) {
		const char* l = it->labels[i];
		if (!l || !l[0])
			return false;
		for (const char* p = l; *p; p++)
			if ((*p < '0' || *p > '9') && *p != '%')
				return false;
	}
	return true;
}

static void statusBar(SDL_Surface* s) {
	int bar = P(56), cy = bar / 2;
	char clock[16];
	time_t now = time(NULL);
	strftime(clock, sizeof(clock), "%H:%M", localtime(&now));
	text(s, clock, P(21), 600, C_TEXT, P(32), cy - lineH(P(21)) / 2);
	int charging = 0, pct = 0;
	PLAT_getBatteryStatusFine(&charging, &pct);
	char p[16];
	snprintf(p, sizeof(p), "%d%%", pct);
	text(s, p, P(19), 600, C_TEXT, s->w - P(32) - textW(p, P(19), 600), cy - lineH(P(19)) / 2);
}

static void hintBar(SDL_Surface* s) {
	int top = s->h - P(64), cy = top + P(32);
	SDL_FillRect(s, &(SDL_Rect){0, top, s->w, P(1) > 0 ? P(1) : 1}, mapc(s, C_SURFACE1));
	const char* hints[][2] = {{"A", "Select"}, {"B", "Back"}};
	int x = P(40);
	for (int i = 0; i < 2; i++) {
		rrect(s, x, cy - P(16), P(32), P(32), P(16), C_SURFACE2);
		text(s, hints[i][0], P(16), 700, C_TEXT, x + (P(32) - textW(hints[i][0], P(16), 700)) / 2, cy - lineH(P(16)) / 2);
		x += P(32) + P(10);
		const char* l = I18N_text(hints[i][1]);
		text(s, l, P(19), 400, C_TEXT2, x, cy - lineH(P(19)) / 2);
		x += textW(l, P(19), 400) + P(32);
	}
	const char* note = I18N_text("Brick OS · based on NX Redux");
	text(s, note, P(19), 400, C_TEXT4, s->w - P(40) - textW(note, P(19), 400), cy - lineH(P(19)) / 2);
}

void SettingsBos_render(SDL_Surface* s, SettingsPage** stack, int depth) {
	if (depth <= 0)
		return;
	k = FIXED_WIDTH / 1024.0f;
	SettingsPage* root = stack[0];
	SDL_FillRect(s, NULL, mapc(s, C_BG));
	statusBar(s);

	// left (400 wide): "ตั้งค่า", the categories (rows 62, padding 7, radius 22; the highlighted one in the accent)
	int lx = P(32), lw = P(400), top = P(56) + P(4);
	text(s, I18N_text("Settings"), P(35), 700, C_TEXT, lx + P(10), top);
	int gy = top + lineH(P(35)) + P(14);
	int bottom = s->h - P(64) - P(16);
	int nvis = settings_page_visible_count(root);
	int fit_rows = (bottom - gy - 2 * P(7)) / P(62);
	if (fit_rows > nvis)
		fit_rows = nvis;
	if (root->selected < root->scroll)
		root->scroll = root->selected;
	if (fit_rows > 0 && root->selected >= root->scroll + fit_rows)
		root->scroll = root->selected - fit_rows + 1;
	if (fit_rows > 0) {
		rrect(s, lx, gy, lw, fit_rows * P(62) + 2 * P(7), P(22), C_SURFACE1);
		for (int r = 0; r < fit_rows; r++) {
			int i = root->scroll + r, ry = gy + P(7) + r * P(62);
			SettingItem* it = settings_page_visible_item(root, i);
			if (!it)
				continue;
			bool sel = i == root->selected;
			if (sel)
				rrect(s, lx + P(7), ry, lw - 2 * P(7), P(62), P(15), C_ACCENT);
			char mark[8];
			uint32_t chip;
			chipFor(it->name, mark, sizeof(mark), &chip);
			int cx = lx + P(7) + P(14), cyy = ry + (P(62) - P(38)) / 2;
			rrect(s, cx, cyy, P(38), P(38), P(10), chip);
			text(s, mark, P(16), 700, 0xFFFFFF, cx + (P(38) - textW(mark, P(16), 700)) / 2, cyy + (P(38) - lineH(P(16))) / 2);
			const char* val = itemValue(it);
			int vw = textW(val, P(18), 400);
			char name[128];
			int nx = cx + P(38) + P(14);
			fit(I18N_text(it->name), P(21), 500, lx + lw - P(7) - P(14) - vw - P(10) - nx, name, sizeof(name));
			text(s, name, P(21), 500, C_TEXT, nx, ry + (P(62) - lineH(P(21))) / 2);
			if (val && val[0])
				text(s, val, P(18), 400, sel ? 0xFFFFFF : C_TEXT2, lx + lw - P(7) - P(14) - vw, ry + (P(62) - lineH(P(18))) / 2);
		}
	}

	// right: the page open now (depth > 1), else a preview of the highlighted category's page
	SettingItem* cat = settings_page_visible_item(root, root->selected);
	SettingsPage* page = depth > 1 ? stack[depth - 1] : (cat && cat->type == ITEM_SUBMENU ? cat->submenu : NULL);
	bool focused = depth > 1;
	int rx = lx + lw + P(24), rw = s->w - P(32) - rx, ry0 = top + P(8);
	const char* title = page && page->title ? page->title : (cat ? cat->name : "");
	const char* bar = strrchr(title, '|'); // "Settings | Display": the page's own name
	if (bar) {
		title = bar + 1;
		while (*title == ' ')
			title++;
	}
	char head[160];
	fit(I18N_text(title), P(28), 700, rw - P(20), head, sizeof(head));
	text(s, head, P(28), 700, C_TEXT, rx + P(10), ry0);
	int list_y = ry0 + lineH(P(28)) + P(14);
	const char* foot = NULL;
	if (page) {
		int has_lock = page->dynamic_start >= 0;
		if (has_lock)
			pthread_rwlock_rdlock(&page->lock);
		int n = settings_page_visible_count(page);
		int rows = (bottom - list_y - lineH(P(18)) - P(14)) / P(70);
		if (rows > n)
			rows = n;
		int sel = focused ? page->selected : -1;
		int first = 0;
		if (focused) {
			if (page->selected < page->scroll)
				page->scroll = page->selected;
			if (rows > 0 && page->selected >= page->scroll + rows)
				page->scroll = page->selected - rows + 1;
			first = page->scroll;
		}
		if (rows > 0) {
			rrect(s, rx, list_y, rw, rows * P(70), P(22), C_SURFACE1);
			for (int r = 0; r < rows; r++) {
				int i = first + r, y = list_y + r * P(70);
				SettingItem* it = settings_page_visible_item(page, i);
				if (!it)
					continue;
				if (it->type == ITEM_CYCLE || it->type == ITEM_COLOR)
					settings_item_sync(it);
				bool hot = i == sel;
				if (hot) {
					int rt = r == 0 ? P(22) : 0, rb = r == rows - 1 ? P(22) : 0;
					rrect(s, rx, y, rw, P(70), rt > rb ? rt : rb, C_SURFACE2);
					if (rt && !rb)
						SDL_FillRect(s, &(SDL_Rect){rx, y + P(35), rw, P(35)}, mapc(s, C_SURFACE2));
					if (rb && !rt)
						SDL_FillRect(s, &(SDL_Rect){rx, y, rw, P(35)}, mapc(s, C_SURFACE2));
				}
				if (r < rows - 1)
					SDL_FillRect(s, &(SDL_Rect){rx, y + P(70) - 1, rw, 1}, mapc(s, C_SURFACE2));
				// the value: a slider (a 0..N cycle), the colour's dot, else the text with › on a page that opens
				int right = rx + rw - P(22), vy = y + P(35);
				int used = 0;
				if (numericRange(it)) {
					int tw = P(240), tx = right - tw;
					rrect(s, tx, vy - P(4), tw, P(8), P(4), C_SURFACE3);
					int fw = it->label_count > 1 ? tw * it->current_idx / (it->label_count - 1) : 0;
					if (fw > 0)
						rrect(s, tx, vy - P(4), fw, P(8), P(4), C_TEXT);
					used = tw;
				} else if (it->type == ITEM_COLOR && it->values && it->current_idx >= 0 && it->current_idx < it->label_count) {
					uint32_t c = (uint32_t)it->values[it->current_idx] & 0xFFFFFF;
					rrect(s, right - P(28), vy - P(14), P(28), P(28), P(14), c);
					used = P(28);
				} else {
					char val[160];
					const char* v = itemValue(it);
					snprintf(val, sizeof(val), "%s%s", v ? I18N_text(v) : "",
							 it->type == ITEM_SUBMENU ? " \xE2\x80\xBA" : "");
					if (val[0]) {
						int vw = textW(val, P(20), 400);
						if (vw > rw / 2)
							fit(val, P(20), 400, rw / 2, val, sizeof(val)), vw = textW(val, P(20), 400);
						text(s, val, P(20), 400, hot ? C_TEXT3 : C_TEXT2, right - vw, vy - lineH(P(20)) / 2);
						used = vw;
					}
				}
				char label[160];
				fit(I18N_text(it->name), P(21), 400, right - used - P(16) - (rx + P(22)), label, sizeof(label));
				text(s, label, P(21), 400, C_TEXT, rx + P(22), vy - lineH(P(21)) / 2);
			}
			SettingItem* fi = focused ? settings_page_visible_item(page, page->selected) : cat;
			if (page->status_msg)
				foot = page->status_msg;
			else if (fi && fi->desc && fi->desc[0])
				foot = fi->desc;
		}
		if (has_lock)
			pthread_rwlock_unlock(&page->lock);
		if (foot) {
			char f[256];
			fit(I18N_text(foot), P(18), 400, rw - P(20), f, sizeof(f));
			text(s, f, P(18), 400, C_TEXT2, rx + P(10), list_y + rows * P(70) + P(14));
		}
	} else if (cat && cat->desc && cat->desc[0]) {
		char f[256];
		fit(I18N_text(cat->desc), P(18), 400, rw - P(20), f, sizeof(f));
		text(s, f, P(18), 400, C_TEXT2, rx + P(10), list_y);
	}
	hintBar(s);
}
