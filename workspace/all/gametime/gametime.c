#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <msettings.h>

#include "ui_draw.h"
#include "defines.h"
#include "config.h"
#include "api.h"
#include "bos_ui.h"
#include "i18n.h"
#include "ui_listview.h"
#include <time.h>
#include "image_load.h"
#include "ui_buttonhintbar.h"
#include "ui_confirmdialog.h"
#include "ui_emptystate.h"
#include "ui_menubar.h"
#include "ui_list.h"
#include "ui_list_layout.h"
#include "ui_font.h"
#include "ui_splash.h"
#include "ui_quitrequest.h"
#include "utils.h"

#include <sqlite3.h>
#include <gametimedb.h>

struct ListLayout {
	int list_display_size_x;
	int list_display_size_y;
	int list_display_start_x;
	int list_display_start_y;
	SDL_Rect list_display_rect;

	int sub_title_x;
	int sub_title_y;

	int items_per_page;
	int num_pages;

	int up_y, down_y; // arrow strip centres (UI_listBlock)
} layout = {0};


#define BIG_PILL_SIZE 40
#define IMG_MARGIN 6
#define IMG_MAX_WIDTH BIG_PILL_SIZE - IMG_MARGIN
#define IMG_MAX_HEIGHT BIG_PILL_SIZE - IMG_MARGIN

static SDL_Surface* screen;
static SDL_Surface** romImages;
static bool* romImagesTried; // romImages[i] was loaded (NULL = no image)

static PlayActivities* play_activities;

///////

int _renderText(const char* text, TTF_Font* font, SDL_Color color, SDL_Rect* rect, bool right_align) {
	int text_width = 0;
	SDL_Surface* textSurface = GFX_renderText(font, text, color);
	if (textSurface != NULL) {
		text_width = textSurface->w;
		if (right_align)
			SDL_BlitSurface(textSurface, NULL, screen, &(SDL_Rect){rect->w - textSurface->w, rect->y, rect->w, rect->h});
		else
			SDL_BlitSurface(textSurface, NULL, screen, rect);
		SDL_FreeSurface(textSurface);
	}
	return text_width;
}

int renderText(const char* text, TTF_Font* font, SDL_Color color, SDL_Rect* rect) {
	return _renderText(text, font, color, rect, false);
}

// Set a pixel color on the surface
void _setPixel(SDL_Surface* surface, int x, int y, Uint32 color) {
	if (x < 0 || x >= surface->w || y < 0 || y >= surface->h) {
		return; // Out of bounds check
	}

	// Lock surface if needed
	SDL_LockSurface(surface);

	Uint8* pixelPtr = (Uint8*)surface->pixels + y * surface->pitch + x * surface->format->BytesPerPixel;

	switch (surface->format->BytesPerPixel) {
	case 1:
		*pixelPtr = color;
		break;
	case 2:
		*(Uint16*)pixelPtr = color;
		break;
	case 3:
		if (SDL_BYTEORDER == SDL_BIG_ENDIAN) {
			pixelPtr[0] = (color >> 16) & 0xFF;
			pixelPtr[1] = (color >> 8) & 0xFF;
			pixelPtr[2] = color & 0xFF;
		} else {
			pixelPtr[0] = color & 0xFF;
			pixelPtr[1] = (color >> 8) & 0xFF;
			pixelPtr[2] = (color >> 16) & 0xFF;
		}
		break;
	case 4:
		*(Uint32*)pixelPtr = color;
		break;
	}

	SDL_UnlockSurface(surface);
}

// Draw a filled circle
void _drawFilledCircle(SDL_Surface* surface, int cx, int cy, int radius, Uint32 color) {
	for (int y = -radius; y <= radius; y++) {
		for (int x = -radius; x <= radius; x++) {
			if (x * x + y * y <= radius * radius) {
				_setPixel(surface, cx + x, cy + y, color);
			}
		}
	}
}

// Draw a filled rounded rectangle
void renderRoundedRectangle(SDL_Rect rect, Uint32 color, int radius) {
	// Fill the center and straight edges
	SDL_Rect fillRect = {rect.x + radius, rect.y, rect.w - 2 * radius, rect.h};
	SDL_FillRect(screen, &fillRect, color);

	fillRect.x = rect.x;
	fillRect.y = rect.y + radius;
	fillRect.w = rect.w;
	fillRect.h = rect.h - 2 * radius;
	SDL_FillRect(screen, &fillRect, color);

	// Draw the corner circles
	_drawFilledCircle(screen, rect.x + radius, rect.y + radius, radius, color);							  // Top-left
	_drawFilledCircle(screen, rect.x + rect.w - radius - 1, rect.y + radius, radius, color);			  // Top-right
	_drawFilledCircle(screen, rect.x + radius, rect.y + rect.h - radius - 1, radius, color);			  // Bottom-left
	_drawFilledCircle(screen, rect.x + rect.w - radius - 1, rect.y + rect.h - radius - 1, radius, color); // Bottom-right
}

// Mask s (32-bit with an alpha channel, any channel order) to the circle inscribed in it: each pixel's alpha times how
// much of it lies inside (anti-aliased over a px), the real row pitch used (GFX_ApplyRoundedCorners indexed rows by the
// scaled image's width, which left fill-scaled art's corners unmasked, and had no anti-aliasing). A surface without
// alpha can't hold the mask (SDL_MapRGBA drops it): callers convert to ARGB8888 first.
static void maskCircle(SDL_Surface* s) {
	if (!s)
		return;
	if (s->format->BytesPerPixel != 4 || s->format->Amask == 0) {
		LOG_warn("gametime: maskCircle needs a 32-bit surface with alpha (format %s)\n",
				 SDL_GetPixelFormatName(s->format->format));
		return;
	}
	float r = (s->w < s->h ? s->w : s->h) / 2.0f, cx = s->w / 2.0f, cy = s->h / 2.0f;
	if (SDL_MUSTLOCK(s))
		SDL_LockSurface(s);
	for (int y = 0; y < s->h; y++) {
		Uint32* row = (Uint32*)((Uint8*)s->pixels + y * s->pitch);
		float dy = y + 0.5f - cy;
		for (int x = 0; x < s->w; x++) {
			float dx = x + 0.5f - cx;
			float cover = r - sqrtf(dx * dx + dy * dy) + 0.5f;
			if (cover >= 1.0f)
				continue;
			Uint8 cr, cg, cb, ca;
			SDL_GetRGBA(row[x], s->format, &cr, &cg, &cb, &ca);
			ca = cover <= 0.0f ? 0 : (Uint8)(ca * cover + 0.5f);
			row[x] = SDL_MapRGBA(s->format, cr, cg, cb, ca);
		}
	}
	if (SDL_MUSTLOCK(s))
		SDL_UnlockSurface(s);
}

// The dark grey circle behind a game without a picture, built once (anti-aliased) at the thumbnail size.
static SDL_Surface* blankCircle(void) {
	static SDL_Surface* s = NULL;
	if (!s) {
		s = SDL_CreateRGBSurfaceWithFormat(0, SCALE1(IMG_MAX_WIDTH), SCALE1(IMG_MAX_HEIGHT), 32, SDL_PIXELFORMAT_RGBA32);
		if (s) {
			SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, TRIAD_DARK_GRAY, 255));
			maskCircle(s);
			SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_BLEND);
		}
	}
	return s;
}

SDL_Surface* loadRomImage(char* image_path) {
	if (!exists(image_path))
		return NULL;

	SDL_Surface* img = Image_load(image_path);
	if (!img)
		return NULL;

	// ARGB8888 whatever the file holds (24-bit PNG, JPEG, WebP, paletted): the circle mask needs an alpha channel
	if (img->format->format != SDL_PIXELFORMAT_ARGB8888) {
		SDL_Surface* argb = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ARGB8888, 0);
		SDL_FreeSurface(img);
		img = argb;
		if (!img)
			return NULL;
	}

	SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, SCALE1(IMG_MAX_WIDTH), SCALE1(IMG_MAX_HEIGHT), 32, SDL_PIXELFORMAT_ARGB8888);
	if (!dst) {
		SDL_FreeSurface(img);
		return NULL;
	}
	GFX_blitScaled(GFX_SCALE_FILL, img, dst);
	maskCircle(dst);
	SDL_SetSurfaceBlendMode(dst, SDL_BLENDMODE_BLEND);
	SDL_FreeSurface(img);

	return dst;
}

void initRomImages() {
	// Thumbnails are decoded when their row is first drawn (romImageAt), not
	// all up front: each decode reads a full-size screenshot
	romImages = calloc(play_activities->count > 0 ? play_activities->count : 1, sizeof(SDL_Surface*));
	romImagesTried = calloc(play_activities->count > 0 ? play_activities->count : 1, sizeof(bool));
}

static SDL_Surface* romImageAt(int index) {
	if (!romImages || !romImagesTried || index < 0 || index >= play_activities->count)
		return NULL;
	if (!romImagesTried[index]) {
		romImagesTried[index] = true;
		romImages[index] = loadRomImage(play_activities->play_activity[index]->rom->image_path);
	}
	return romImages[index];
}

void freeRomImages() {
	for (int i = 0; romImages && i < play_activities->count; i++) {
		SDL_FreeSurface(romImages[i]);
	}
	free(romImages);
	free(romImagesTried);
	romImages = NULL;
	romImagesTried = NULL;
}

void renderList(int count, int start, int end, int selected) {
	char rom_name[255];
	char total[25];
	char average[25];
	char plays[25];

	int num_width = 0;

	const int elemHeight = SCALE1(BIG_PILL_SIZE);
	const int thumbMargin = SCALE1(IMG_MARGIN);
	const int textHeight = (elemHeight - thumbMargin) / 2;

	// Row text (LIST-LAYOUT §10.3/§10.6): the game name is the list label (UI_TEXT_LABEL, 16 logical) and the
	// stats line its secondary text (UI_TEXT_SECONDARY, 0.8 x). Line boxes are 1.2 x their size with the
	// glyphs centred, and the pair is centred in the row (as on the rich rows).
	const int label_px = UI_textRolePx(UI_TEXT_LABEL);
	const int detail_px = UI_textRolePx(UI_TEXT_SECONDARY);
	const int title_box = (int)(label_px * 1.2f + 0.5f);
	const int detail_box = (int)(detail_px * 1.2f + 0.5f);
	const int pair_top = (elemHeight - title_box - detail_box) / 2;
	// each role font is fetched, measured and dropped before the next UIFont_get
	int title_h = label_px;
	{
		TTF_Font* f = UI_textRole(UI_TEXT_LABEL, false);
		if (f)
			title_h = TTF_FontHeight(f);
	}
	const int title_y = pair_top + (title_box - title_h) / 2;
	int detail_h = detail_px;
	{
		TTF_Font* f = UI_textRole(UI_TEXT_SECONDARY, false);
		if (f)
			detail_h = TTF_FontHeight(f);
	}
	const int detail_y = pair_top + title_box + (detail_box - detail_h) / 2;

	int selected_row = selected - start;
	for (int index = start, row = 0; index < end; index++, row++) {
		bool isSelected = selected_row == row;

		PlayActivity* entry = play_activities->play_activity[index];
		ROM* rom = entry->rom;

		renderRoundedRectangle((SDL_Rect){
								   layout.list_display_start_x,
								   layout.list_display_start_y + row * elemHeight,
								   layout.list_display_size_x,
								   elemHeight},
							   isSelected ? RGB_WHITE : RGB_BLACK, SCALE1(BIG_PILL_SIZE / 2));

		SDL_Surface* romImage = romImageAt(index);
		if (romImage) {
			SDL_Rect rectRomImage = {
				layout.list_display_start_x + num_width + thumbMargin / 2 + (SCALE1(IMG_MAX_WIDTH) - romImage->w) / 2,
				layout.list_display_start_y + elemHeight * row + thumbMargin / 2,
				SCALE1(IMG_MAX_WIDTH),
				SCALE1(IMG_MAX_HEIGHT)};
			SDL_BlitSurface(romImage, NULL, screen, &rectRomImage);
		} else {
			SDL_Rect rectRomImage = {
				layout.list_display_start_x + num_width + thumbMargin / 2,
				layout.list_display_start_y + elemHeight * row + thumbMargin / 2,
				SCALE1(IMG_MAX_WIDTH),
				SCALE1(IMG_MAX_HEIGHT)};

			SDL_Surface* blank = blankCircle();
			if (blank)
				SDL_BlitSurface(blank, NULL, screen, &(SDL_Rect){rectRomImage.x, rectRomImage.y});

			SDL_Rect rect;
			GFX_assetRect(ASSET_GAMEPAD, &rect);
			int x = rectRomImage.x;
			int y = rectRomImage.y;
			x += (SCALE1(IMG_MAX_WIDTH) - rect.w) / 2;
			y += (SCALE1(IMG_MAX_HEIGHT) - rect.h) / 2;

			GFX_blitAssetColor(ASSET_GAMEPAD, NULL, screen, &(SDL_Rect){x, y}, THEME_COLOR1);
		}

		cleanName(rom_name, rom->name);
		SDL_Color textColor = COLOR_WHITE;
		if (isSelected) {
			textColor = COLOR_BLACK;
		}
		int row_y = layout.list_display_start_y + elemHeight * row;
		// 14 dp between the thumbnail and its text (the rich list's gap, LIST-LAYOUT §10.2)
		int text_x = layout.list_display_start_x + num_width + thumbMargin / 2 + SCALE1(IMG_MAX_WIDTH) + NX_DP(14);
		// the name ends half a row before the capsule's right end, with an ellipsis when it is longer
		{
			TTF_Font* nameFont = UI_textRole(UI_TEXT_LABEL, false);
			if (!nameFont)
				nameFont = font.large;
			int name_max_w = layout.list_display_start_x + layout.list_display_size_x - elemHeight / 2 - text_x;
			char name_fit[sizeof(rom_name)];
			GFX_truncateText(nameFont, rom_name, name_fit, sizeof(name_fit), name_max_w, 0);
			renderText(name_fit, nameFont, textColor, &(SDL_Rect){text_x, row_y + title_y, layout.list_display_size_x, textHeight});
		}

		serializeTime(total, entry->play_time_total);
		serializeTime(average, entry->play_time_average);
		snprintf(plays, sizeof(plays), "%d", entry->play_count);

		// values first in the accent color, lowercase labels in muted gray
		const char* details[] = {total, " total  ·  ", average, " avg  ·  ", plays, entry->play_count == 1 ? " play" : " plays"};
		SDL_Rect detailsRect = {text_x, row_y + detail_y, layout.list_display_size_x, textHeight};
		// accent only reads well on the selected white pill; use light gray on dark rows
		SDL_Color valueCol = isSelected ? uintToColour(THEME_COLOR2_255) : COLOR_LIGHT_TEXT;
		// fetched right before use (a UIFont pointer is only valid until the next UIFont_get)
		TTF_Font* detailFont = UI_textRole(UI_TEXT_SECONDARY, false);
		if (!detailFont)
			detailFont = font.small;
		// clipped like the name: the segment that crosses the limit is ellipsised and the rest dropped
		int details_end = layout.list_display_start_x + layout.list_display_size_x - elemHeight / 2;
		for (int i = 0; i < 6; i++) {
			SDL_Color detailCol = i % 2 == 0 ? valueCol : COLOR_DARK_TEXT;
			int room = details_end - detailsRect.x;
			int seg_w = 0;
			GFX_measureText(detailFont, details[i], &seg_w, NULL);
			if (seg_w <= room) {
				detailsRect.x += renderText(details[i], detailFont, detailCol, &detailsRect);
				continue;
			}
			char seg_fit[64];
			snprintf(seg_fit, sizeof(seg_fit), "%s", details[i]);
			if (room > 0 && GFX_truncateText(detailFont, details[i], seg_fit, sizeof(seg_fit), room, 0) <= room)
				renderText(seg_fit, detailFont, detailCol, &detailsRect);
			break;
		}
	}

	if (count > layout.items_per_page)
		UI_renderScrollArrows(screen, layout.up_y, layout.down_y, start > 0, end < count);
}

// ---- the Brick OS pages (DESIGN 7.9) ----
static SDL_Surface** bosArt;	  // each game's picture, as it is (the kit rounds it)
static bool* bosArtTried;
static BosGlide gt_pill, gt_scroll;
static int gt_scroll_i = 0;

static void bosArtFree(void) {
	for (int i = 0; bosArt && i < play_activities->count; i++)
		if (bosArt[i]) {
			BOS_picForget(bosArt[i]);
			SDL_FreeSurface(bosArt[i]);
		}
	free(bosArt);
	free(bosArtTried);
	bosArt = NULL, bosArtTried = NULL;
}
static SDL_Surface* bosArtAt(int i) {
	if (!bosArt) {
		int n = play_activities->count > 0 ? play_activities->count : 1;
		bosArt = calloc(n, sizeof(SDL_Surface*));
		bosArtTried = calloc(n, sizeof(bool));
	}
	if (i < 0 || i >= play_activities->count)
		return NULL;
	if (!bosArtTried[i]) {
		bosArtTried[i] = true;
		const char* p = play_activities->play_activity[i]->rom->image_path;
		bosArt[i] = p && exists((char*)p) ? Image_load(p) : NULL;
	}
	return bosArt[i];
}

// 3 h 28 min / 17 min / 45 s
static const char* gt_dur(int sec, char* out, int n) {
	int h = sec / 3600, m = (sec % 3600) / 60;
	if (h > 0)
		snprintf(out, n, I18N_text("%d h %d min"), h, m);
	else if (m > 0)
		snprintf(out, n, I18N_text("%d min"), m);
	else
		snprintf(out, n, I18N_text("%d s"), sec);
	return out;
}

static void gt_row(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	static char name[255], sb[128], tot[48];
	char av[48];
	PlayActivity* e = play_activities->play_activity[i];
	cleanName(name, e->rom->name);
	snprintf(sb, sizeof(sb), I18N_text("Average %s · %d times"), gt_dur(e->play_time_average, av, sizeof(av)), e->play_count);
	*r = (ListViewRow){.label = name, .sub = sb, .annotation = gt_dur(e->play_time_total, tot, sizeof(tot)), .art = bosArtAt(i)};
	if (!r->art)
		r->sq_icon = "game", r->sq_color = 0x48484A, r->sq_raw = true;
}

static void render_list_bos(int selected, bool confirm) {
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("Game Time"));
	int top = BOS_px(BOS_BAR_H), gx = BOS_px(40), gw = screen->w - 2 * gx;
	char t[64], sb[96];
	snprintf(sb, sizeof(sb), I18N_text("Total play time · %d games"), play_activities->count);
	int y = top + BOS_largeTitle(screen, top, gt_dur(play_activities->play_time_total, t, sizeof(t)), sb);
	int bottom = screen->h - BOS_px(BOS_BAR_H) - BOS_px(12);
	BOS_groupRows(screen, &(SDL_Rect){gx, y, gw, bottom - y}, BOS_px(BOS_ROW_H), play_activities->count, selected, true,
				  &gt_scroll_i, &gt_pill, &gt_scroll, false, gt_row, NULL);
	BosHint h[] = {{"A", I18N_text("Details")}, {"B", I18N_text("Exit")}, {"X", I18N_text("Delete")}};
	BOS_hintBar(screen, h, 3, NULL, 0);
	if (confirm) {
		char rom_name[255];
		cleanName(rom_name, play_activities->play_activity[selected]->rom->name);
		UI_renderConfirmDialog(screen, "Delete Record?", rom_name);
	}
}

// the last 7 days of one game, minutes per day (today last)
static void gt_week(int rom_id, int mins[7]) {
	memset(mins, 0, 7 * sizeof(int));
	sqlite3* db = play_activity_db_open();
	if (!db)
		return;
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	lt.tm_hour = 0, lt.tm_min = 0, lt.tm_sec = 0;
	time_t today = mktime(&lt), from = today - 6 * 86400;
	sqlite3_stmt* st = NULL;
	if (sqlite3_prepare_v2(db, "SELECT created_at, play_time FROM play_activity WHERE rom_id = ? AND created_at >= ?", -1, &st,
						   NULL) == SQLITE_OK) {
		sqlite3_bind_int(st, 1, rom_id);
		sqlite3_bind_int64(st, 2, (sqlite3_int64)from);
		while (sqlite3_step(st) == SQLITE_ROW) {
			time_t at = (time_t)sqlite3_column_int64(st, 0);
			int d = (int)((at - from) / 86400);
			if (d >= 0 && d < 7)
				mins[d] += sqlite3_column_int(st, 1) / 60;
		}
		sqlite3_finalize(st);
	}
	play_activity_db_close(db);
}

// board v2-ToolsGameTimeDetail: the picture 300 left; the name 48/700, the console and last play, three stat cards,
// then the last 7 days as bars (today in primary)
static void render_detail_bos(int i, bool confirm) {
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("Game Time"));
	PlayActivity* e = play_activities->play_activity[i];
	int top = BOS_px(BOS_BAR_H), x = BOS_px(40), y = top + BOS_px(12), pw = BOS_px(300);
	SDL_Surface* a = bosArtAt(i);
	SDL_Surface* p = a ? BOS_picRounded(a, pw, pw, BOS_px(24)) : NULL;
	if (p)
		BOS_blit(p, screen, x, y, 1);
	else {
		BOS_rrect(screen, x, y, pw, pw, BOS_px(24), BOS_theme()->surface);
		int ic = BOS_px(120);
		BOS_icon(screen, "game", BOS_C_TEXT2, 1, x + (pw - ic) / 2, y + (pw - ic) / 2, ic);
	}
	int rx = x + pw + BOS_px(36), rw = screen->w - BOS_px(40) - rx, ry = y;
	char name[255], t[192];
	cleanName(name, e->rom->name);
	BOS_fit(name, 48, 700, rw, t, sizeof(t));
	BOS_text(screen, t, 48, 700, BOS_C_TEXT, 1, rx, ry);
	ry += BOS_lineH(48);
	if (e->last_played_at && e->last_played_at[0]) {
		snprintf(t, sizeof(t), I18N_text("Last played %.10s"), e->last_played_at);
		BOS_text(screen, t, 28, 400, BOS_C_TEXT2, 1, rx, ry);
	}
	ry += BOS_lineH(28) + BOS_px(22);
	// three cards
	char v[3][48];
	if (e->play_time_total >= 3600) // a card is narrow: 3:28 h
		snprintf(v[0], 48, I18N_text("%d:%02d h"), e->play_time_total / 3600, (e->play_time_total % 3600) / 60);
	else
		gt_dur(e->play_time_total, v[0], 48);
	gt_dur(e->play_time_average, v[1], 48);
	snprintf(v[2], 48, I18N_text("%d times"), e->play_count);
	const char* k[3] = {I18N_text("Total"), I18N_text("Average"), I18N_text("Played")};
	int cw = (rw - 2 * BOS_px(14)) / 3, ch = BOS_px(96);
	for (int c = 0; c < 3; c++) {
		int cx = rx + c * (cw + BOS_px(14));
		BOS_rrect(screen, cx, ry, cw, ch, BOS_px(20), BOS_theme()->surface);
		BOS_text(screen, k[c], 24, 400, BOS_C_TEXT2, 1, cx + BOS_px(18), ry + BOS_px(12));
		BOS_fit(v[c], 32, 700, cw - BOS_px(30), t, sizeof(t));
		BOS_text(screen, t, 32, 700, BOS_C_TEXT, 1, cx + BOS_px(18), ry + BOS_px(44));
	}
	ry += ch + BOS_px(26);
	// the week
	int mins[7];
	gt_week(e->rom->id, mins);
	int bottom = screen->h - BOS_px(BOS_BAR_H) - BOS_px(12), bh = bottom - ry;
	if (bh > BOS_px(300))
		bh = BOS_px(300);
	BOS_rrect(screen, rx, ry, rw, bh, BOS_px(24), BOS_theme()->surface);
	BOS_text(screen, I18N_text("Last 7 days"), 26, 400, BOS_C_TEXT2, 1, rx + BOS_px(24), ry + BOS_px(18));
	int mx = 1;
	for (int d = 0; d < 7; d++)
		if (mins[d] > mx)
			mx = mins[d];
	int gx = rx + BOS_px(24), gw = rw - BOS_px(48), colw = (gw - 6 * BOS_px(18)) / 7;
	int base = ry + bh - BOS_px(16) - BOS_lineH(24) - BOS_px(8), tall = base - (ry + BOS_px(18) + BOS_lineH(26) + BOS_px(12));
	time_t now = time(NULL);
	static const char* wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	for (int d = 0; d < 7; d++) {
		int h = mins[d] * tall / mx;
		if (h < BOS_px(6))
			h = BOS_px(6);
		int cx = gx + d * (colw + BOS_px(18));
		BOS_rrect(screen, cx, base - h, colw, h, BOS_px(10), d == 6 ? BOS_theme()->primary : BOS_theme()->surface2);
		time_t at = now - (6 - d) * 86400;
		struct tm lt;
		localtime_r(&at, &lt);
		const char* l = I18N_text(wd[lt.tm_wday]);
		BOS_text(screen, l, 24, 400, BOS_C_TEXT2, 1, cx + (colw - BOS_textW(l, 24, 400)) / 2, base + BOS_px(8));
	}
	BosHint h[] = {{"B", I18N_text("Back")}, {"X", I18N_text("Delete")}};
	BOS_hintBar(screen, h, 2, NULL, 0);
	if (confirm)
		UI_renderConfirmDialog(screen, "Delete Record?", name);
}

void initLayout() {
	// unscaled
	int hw = screen->w;
	int hh = screen->h;

	// the title. just leave the default padding all around
	layout.sub_title_x = SCALE1(PADDING);
	layout.sub_title_y = SCALE1(PADDING);

	// the main list (LIST-LAYOUT §10.1/§10.2): a rich list, so the thumbnail's left edge sits on the list text
	// start (14 dp, the title's x) with the capsule its image margin left of that; the row ends PADDING short
	// of the right edge. Rows sit in the standard block: equal half-row arrow strips above and below, centred
	// between the title's letters and the hint icons.
	int row_h = SCALE1(BIG_PILL_SIZE);
	UIListBlock block = UI_listBlock(UI_pageTitleBandTop(), UI_buttonHintIconTop(hh), row_h, 0);
	layout.list_display_start_x = UI_listTextX() - SCALE1(IMG_MARGIN) / 2;
	if (layout.list_display_start_x < 0)
		layout.list_display_start_x = 0;
	layout.list_display_start_y = block.top;
	layout.list_display_size_x = hw - SCALE1(PADDING) - layout.list_display_start_x;
	layout.list_display_size_y = block.rows * row_h;
	layout.up_y = block.up_y;
	layout.down_y = block.down_y;

	layout.list_display_rect.x = layout.list_display_start_x,
	layout.list_display_rect.y = layout.list_display_start_y,
	layout.list_display_rect.w = layout.list_display_size_x,
	layout.list_display_rect.h = layout.list_display_size_y;

	layout.items_per_page = block.rows > 0 ? block.rows : 1;
	layout.num_pages = (int)ceil((double)play_activities->count / (double)layout.items_per_page);
}

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;
	screen = GFX_init(MODE_MAIN);
	UI_showSplashScreen(screen, "Game Time");

	InitSettings();
	PAD_init();
	PWR_init();

	setup_signal_handlers();

	play_activities = play_activity_find_all();
	LOG_debug("found %d roms\n", play_activities->count);

	int count = play_activities->count;

	initLayout();
	initRomImages();
	int selected = 0;
	int start = 0;
	int end = MIN(count, layout.items_per_page);

	bool confirm_delete = false;
	bool detail = false;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;
	while (!app_quit) {
		GFX_startFrame();
		PAD_poll();
		// MENU + SELECT exits (same combo as in-game and the other pak tools).
		bool req_quit = false;
		UI_handleQuitRequest(screen, &req_quit, &dirty, "Exit Game Tracker?", NULL);
		if (req_quit)
			app_quit = true;

		if (confirm_delete) {
			if (PAD_justPressed(BTN_A)) {
				play_activity_delete(play_activities->play_activity[selected]->rom->id);

				// reload the list; free images before the activities they index
				bosArtFree();
				detail = false;
				freeRomImages();
				free_play_activities(play_activities);
				play_activities = play_activity_find_all();
				count = play_activities->count;
				initRomImages();
				initLayout();

				if (selected >= count)
					selected = MAX(0, count - 1);
				start = MIN(start, MAX(0, count - layout.items_per_page));
				if (selected < start)
					start = selected;
				end = MIN(count, start + layout.items_per_page);

				confirm_delete = false;
				dirty = true;
			} else if (PAD_justPressed(BTN_B)) {
				confirm_delete = false;
				dirty = true;
			}
		} else if (detail) {
			if (PAD_justPressed(BTN_B)) {
				detail = false;
				dirty = true;
			} else if (PAD_justPressed(BTN_X)) {
				confirm_delete = true;
				dirty = true;
			}
		} else if (count > 0) {
			if (PAD_justPressed(BTN_A)) {
				detail = true;
				dirty = true;
			} else if (PAD_justRepeated(BTN_UP)) {
				selected -= 1;
				if (selected < 0) {
					selected = count - 1;
					start = MAX(0, count - layout.items_per_page);
					end = count;
				} else if (selected < start) {
					start -= 1;
					end -= 1;
				}
				dirty = true;
			} else if (PAD_justRepeated(BTN_DOWN)) {
				selected += 1;
				if (selected >= count) {
					selected = 0;
					start = 0;
					end = MIN(count, layout.items_per_page);
				} else if (selected >= end) {
					start += 1;
					end += 1;
				}
				dirty = true;
			} else if (PAD_pageNav(&selected, count, layout.items_per_page) > 0) { // L2 / R2 (DESIGN 7.7)
				if (selected < start)
					start = selected;
				else if (selected >= end)
					start = selected - layout.items_per_page + 1;
				start = MAX(0, start);
				end = MIN(count, start + layout.items_per_page);
				dirty = true;
			} else if (PAD_justPressed(BTN_X)) {
				confirm_delete = true;
				dirty = true;
			} else if (PAD_justPressed(BTN_B)) {
				app_quit = BOS_askQuit(screen, "Game Tracker"); // the only page: ask first (DESIGN 7.7)
				dirty = true;
			}
		} else if (PAD_justPressed(BTN_B)) {
			app_quit = BOS_askQuit(screen, "Game Tracker");
			dirty = true;
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (UI_statusBarChanged())
			dirty = true;

		if (BOS_glideMoving(&gt_pill) || BOS_glideMoving(&gt_scroll))
			dirty = true;
		if (dirty && count > 0) {
			if (detail)
				render_detail_bos(selected, confirm_delete);
			else
				render_list_bos(selected, confirm_delete);
			GFX_flip(screen);
			dirty = false;
		} else if (dirty) {
			GFX_clear(screen);

			if (count == 0) {
				UI_renderMenuBar(screen, "Game Time");
				UI_renderEmptyStateButtons(screen, "No play activity", "Play some games to track your time",
										   (char*[]){"B", "EXIT", NULL}); // the tool's first level
			} else {
				char play_time_total_formatted[255];
				serializeTime(play_time_total_formatted, play_activities->play_time_total);
				char title[256];
				snprintf(title, sizeof(title), "Game Time: %s", play_time_total_formatted);
				UI_renderMenuBar(screen, title);

				renderList(count, start, end, selected);

				UI_renderButtonHintBar(screen, (char*[]){"B", "EXIT", "X", "DELETE", NULL});

				if (confirm_delete) {
					char rom_name[255];
					cleanName(rom_name, play_activities->play_activity[selected]->rom->name);
					UI_renderConfirmDialog(screen, "Delete Record?", rom_name);
				}
			}

			GFX_flip(screen);
			dirty = false;
		} else
			GFX_sync();
	}

	bosArtFree();
	freeRomImages();
	free_play_activities(play_activities);

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}