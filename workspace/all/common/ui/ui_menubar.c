#include "ui_menubar.h"
#include "ui_draw.h"
#include "api.h"
#include "defines.h"

#include "ui_font.h"
#include "ui_list.h"
#include "../ui_title_fit.h"
#include "../text_shape.h"

#include <stdio.h>
#include <string.h>

// The page title: UI_PAGE_TITLE_SP at the UI scale.
TTF_Font* UI_pageTitleFont(void) {
	return UIFont_getPx(NX_SP(UI_PAGE_TITLE_SP), false);
}

int UI_menuBarHeight(void) {
	return BAR_HEIGHT;
}

int UI_pageTitleBaseline(void) {
	TTF_Font* f = UI_pageTitleFont();
	if (!f)
		return UI_menuBarHeight();
	// the title surface is TTF_FontHeight tall, centred in the strip (UI_renderPageTitleEx)
	return (UI_menuBarHeight() - TTF_FontHeight(f)) / 2 + TTF_FontAscent(f);
}

int UI_pageTitleBandTop(void) {
	int base = UI_pageTitleBaseline();
	TTF_Font* f = UI_pageTitleFont();
	if (!f)
		return base;
	// the deepest descender of the title's likely letters (and the separator's bar), from the glyph metrics
	int desc = 0;
	for (const char* c = "gjpqy|"; *c; c++) {
		int minx, maxx, miny, maxy, adv;
		if (TTF_GlyphMetrics(f, (Uint16)*c, &minx, &maxx, &miny, &maxy, &adv) == 0 && -miny > desc)
			desc = -miny;
	}
	return base + desc + NX_DP(2);
}

int UI_pageTitleX(void) {
	// the shared list rows' text start (LIST-LAYOUT §10.1)
	return UI_listTextX();
}

int UI_renderMenuBar(SDL_Surface* screen, const char* title) {
	return UI_renderMenuBarAt(screen, title, NULL, -1, true, false);
}

static int last_ow = 0; // the hardware group's width as the bar last drew it

int UI_statusWidth(void) {
	return last_ow;
}

int BOS_uiMenuBar(SDL_Surface* s, const char* title);
int UI_renderMenuBarAt(SDL_Surface* screen, const char* title, const char* suffix, int x, bool scrim,
					   bool title_shadow) {
	BOS_uiMenuBar(screen, title);
	last_ow = screen->w / 3;
	return last_ow;
}

const char* UI_pageTitle(char* out, size_t size, const char* parent, const char* page) {
	if (!out || size == 0)
		return out;
	if (page && page[0])
		snprintf(out, size, "%s | %s", parent ? parent : "", page);
	else
		snprintf(out, size, "%s", parent ? parent : "");
	return out;
}

int UI_renderMenuBarPage(SDL_Surface* screen, const char* tool, const char* page) {
	char title[256];
	return UI_renderMenuBar(screen, UI_pageTitle(title, sizeof(title), tool, page));
}

SDL_Surface* UI_captureMenuBar(SDL_Surface* screen) {
	int bar_h = UI_menuBarHeight();
	SDL_Surface* bar = SDL_CreateRGBSurfaceWithFormat(
		0, screen->w, bar_h, screen->format->BitsPerPixel,
		screen->format->format);
	if (!bar)
		return NULL;
	SDL_FillRect(bar, NULL, SDL_MapRGBA(bar->format, 0, 0, 0, 255));
	SDL_BlitSurface(screen, &(SDL_Rect){0, 0, screen->w, bar_h}, bar, NULL);
	return bar;
}

int UI_statusBarChanged(void) {
	static int was_online = -1;
	static int had_bt = -1;
	int is_online = PWR_isOnline();
	int has_bt = PLAT_btIsConnected();
	if (was_online == -1) {
		was_online = is_online;
		had_bt = has_bt;
		return 0;
	}
	int changed = (was_online != is_online) || (had_bt != has_bt);
	was_online = is_online;
	had_bt = has_bt;
	return changed;
}
