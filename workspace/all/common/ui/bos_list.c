// Brick OS kit: lists drawn as §5.6 groups (DESIGN.md 5.5-5.6).
//  - BOS_groupRows: a group of rows anywhere on a page, from ListViewRow data (pages that keep their own selection)
//  - BOS_listViewRender: the shared ListView drawn that way (apps that set `bos`, or UI_bos), the ListView keeping
//    its input and selection (ui_listview.c)
//  - BOS_columns: two columns, a list left and a group right (Settings' layout; radio / TV "add" pages)
#include "bos_ui.h"
#include "ui_listview.h"
#include <stdio.h>
#include <string.h>
#include "i18n.h"

int BOS_uiHintBar(SDL_Surface* s, char** pairs);

static const char* tr(const char* t) {
	return t && t[0] ? I18N_text(t) : t;
}

static const char* lastPart(const char* t) {
	const char* p = t;
	for (const char* q = t ? strstr(t, " | ") : NULL; q; q = strstr(q + 3, " | "))
		p = q + 3;
	return p;
}

static void playingBars(SDL_Surface* s, int right, int cy, uint32_t c) {
	static const int hh[4] = {12, 24, 16, 20};
	int bw = BOS_px(5), gap = BOS_px(3), x = right - 4 * bw - 3 * gap;
	for (int i = 0; i < 4; i++) {
		int h = BOS_px(hh[i]);
		BOS_rrect(s, x + i * (bw + gap), cy + BOS_px(13) - h, bw, h, BOS_px(2), c);
	}
}

// a name's colour for a logo with no picture: the same name, the same colour
static uint32_t nameColour(const char* t) {
	static const uint32_t pal[] = {0x0A84FF, 0x30D158, 0xBF5AF2, 0xFF453A, 0xFF9F0A, 0x5E5CE6, 0x30B0C7, 0xFF375F};
	unsigned h = 5381;
	for (const char* p = t ? t : ""; *p; p++)
		h = h * 33 + (unsigned char)*p;
	return pal[h % 8];
}

// ---- one row ----
static void drawRow(SDL_Surface* s, const ListViewRow* r, int gx, int gw, int ry, int rh, bool hot, bool open) {
	const BosTheme* th = BOS_theme();
	int cy = ry + rh / 2, right = gx + gw - BOS_px(24), nx = gx + BOS_px(28);
	uint32_t fg = hot ? th->onfocus : r->danger ? BOS_C_DANGER : r->accent ? th->primary : BOS_C_TEXT;
	uint32_t sub = hot ? 0x3A3A3C : BOS_C_TEXT2;
	if (r->is_header) {
		BOS_textMid(s, r->label, 26, 600, BOS_C_TEXT2, 1, nx, cy + BOS_px(14));
		return;
	}
	// lead: a picture, a squircle (icon or letters), or nothing
	int lead = 0, lx = gx + BOS_px(20);
	int big = rh >= BOS_px(90);
	if (r->art) {
		lead = big ? BOS_px(64) : BOS_px(52);
		int lw = r->art_wide ? lead * 16 / 9 : lead;
		SDL_Surface* p = BOS_picRounded(r->art, lw, lead, BOS_px(12));
		if (p)
			BOS_blit(p, s, lx, cy - lead / 2, 1);
		lead = lw;
	} else if (r->sq_icon || r->sq_text) {
		int h = big ? BOS_px(56) : BOS_px(48), w = r->sq_text && r->art_wide ? h * 3 / 2 : h;
		uint32_t c = r->sq_color ? r->sq_color : r->sq_text ? nameColour(r->sq_text) : 0x8E8E93;
		BOS_rrect(s, lx, cy - h / 2, w, h, h * 26 / 100, r->sq_icon && !r->sq_raw ? BOS_iconColor(c) : c);
		if (r->sq_icon) {
			int ic = h * 36 / 56;
			BOS_icon(s, r->sq_icon, r->sq_icon_colour ? r->sq_icon_colour : 0xFFFFFF, 1, lx + (w - ic) / 2, cy - ic / 2, ic);
		} else {
			char t[8];
			snprintf(t, sizeof(t), "%.2s", r->sq_text);
			for (char* p = t; *p; p++)
				if (*p >= 'a' && *p <= 'z')
					*p -= 32;
			int fs = h * 3 / 8 < 20 ? 20 : 22;
			BOS_textMid(s, t, fs, 800, 0xFFFFFF, 1, lx + (w - BOS_textW(t, fs, 800)) / 2, cy);
		}
		lead = w;
	}
	if (lead)
		nx = lx + lead + BOS_px(22);
	// right: chevron, a right icon, value, pill, playing bars
	int x = right;
	if (r->chevron) {
		int a = BOS_px(28);
		BOS_icon(s, "chev", hot ? th->onfocus : BOS_C_TEXTOFF, 1, x - a, cy - a / 2, a);
		x -= a + BOS_px(10);
	}
	if (r->right_icon) {
		int a = BOS_px(32);
		BOS_icon(s, r->right_icon, hot ? th->onfocus : r->right_icon_colour ? r->right_icon_colour : BOS_C_TEXT2, 1,
				 x - a, cy - a / 2, a);
		x -= a + BOS_px(14);
	}
	const char* val = r->annotation ? r->annotation : r->badge;
	if (val && val[0]) {
		char t[96];
		BOS_fit(val, BOS_FS_SMALL, 400, gw / 3, t, sizeof(t));
		int w = BOS_textW(t, BOS_FS_SMALL, 400);
		BOS_textMid(s, t, BOS_FS_SMALL, 400, sub, 1, x - w, cy);
		x -= w + BOS_px(14);
	}
	if (r->pill && r->pill[0]) {
		int ph = BOS_px(40), pw = BOS_textW(r->pill, 24, 600) + BOS_px(32) + (r->pill_dot ? BOS_px(20) : 0);
		uint32_t pb = r->pill_bg ? r->pill_bg : hot ? 0xC8C8CE : th->surface2;
		uint32_t pf = r->pill_fg ? r->pill_fg : hot ? th->onfocus : BOS_C_TEXT;
		BOS_rrect(s, x - pw, cy - ph / 2, pw, ph, ph / 2, pb);
		int tx = x - pw + BOS_px(16);
		if (r->pill_dot) {
			int d = BOS_px(12);
			BOS_rrect(s, tx, cy - d / 2, d, d, d / 2, pf);
			tx += BOS_px(20);
		}
		BOS_textMid(s, r->pill, 24, 600, pf, 1, tx, cy);
		x -= pw + BOS_px(14);
	}
	if (r->progress > 0) { // a bar for what is under way (a download)
		int bw = BOS_px(160), bh = BOS_px(8), f = (int)(bw * (r->progress > 1 ? 1 : r->progress));
		BOS_rrect(s, x - bw, cy - bh / 2, bw, bh, bh / 2, hot ? 0xC8C8CE : th->surface2);
		if (f > 0)
			BOS_rrect(s, x - bw, cy - bh / 2, f < bh ? bh : f, bh, bh / 2, hot ? th->onfocus : th->primary);
		x -= bw + BOS_px(14);
	}
	if (r->playing) {
		playingBars(s, x, cy, hot ? th->onfocus : th->primary);
		x -= BOS_px(32) + BOS_px(14);
	}
	int room = x - nx - BOS_px(8);
	bool two = r->sub && r->sub[0];
	int fs = two ? 36 : rh >= BOS_px(90) ? 40 : 34, ny = two ? cy - BOS_px(16) : cy;
	char t[256];
	if (hot)
		BOS_textMarquee(s, r->label, fs, 600, fg, 1, nx, ny, room);
	else {
		BOS_fit(r->label, fs, open ? 600 : 500, room, t, sizeof(t));
		BOS_textMid(s, t, fs, open ? 600 : 500, fg, 1, nx, ny);
	}
	if (two) {
		BOS_fit(r->sub, 26, 400, room, t, sizeof(t));
		BOS_textMid(s, t, 26, 400, sub, 1, nx, cy + BOS_px(20));
	}
}

static int leadOf(const ListViewRow* r, int rh) {
	bool big = rh >= BOS_px(90);
	if (r->art)
		return (big ? BOS_px(64) : BOS_px(52)) * (r->art_wide ? 16 : 9) / 9;
	if (r->sq_icon || r->sq_text) {
		int h = big ? BOS_px(56) : BOS_px(48);
		return r->sq_text && r->art_wide ? h * 3 / 2 : h;
	}
	return 0;
}

// ---- a group of rows in area (its height is filled with whole rows); returns the rows that show ----
int BOS_groupRows(SDL_Surface* s, const SDL_Rect* area, int rh, int count, int sel, bool focused, int* scroll,
				  BosGlide* pill, BosGlide* scr, bool snap, void (*get)(void* ctx, int i, bool hot, ListViewRow* r),
				  void* ctx) {
	const BosTheme* th = BOS_theme();
	int gx = area->x, gw = area->w, y = area->y;
	int visible = area->h / rh;
	if (visible < 1)
		visible = 1;
	if (visible > count)
		visible = count;
	if (count <= 0)
		return 0;
	int sl = sel < 0 ? 0 : sel >= count ? count - 1 : sel;
	int sc = scroll ? *scroll : 0, margin = visible >= 3 ? 1 : 0;
	if (sl - margin < sc)
		sc = sl - margin;
	if (sl + margin > sc + visible - 1)
		sc = sl + margin - visible + 1;
	if (sc > count - visible)
		sc = count - visible;
	if (sc < 0)
		sc = 0;
	if (scroll)
		*scroll = sc;
	float fsc = scr ? BOS_glideTo(scr, (float)(sc * rh), 180, snap) : sc * rh;
	float fsel = pill ? BOS_glideTo(pill, (float)(sl * rh), 120, snap) : sl * rh;
	SDL_Rect box = {gx, y, gw, visible * rh};
	BOS_rrect(s, gx, y, gw, visible * rh, BOS_px(BOS_RADIUS_GROUP), th->surface);
	int ggx = (int)(gw * BOS_ROW_SCALE) - gw, ggy = (int)(rh * BOS_ROW_SCALE) - rh;
	SDL_Rect wide = {gx - ggx, y - ggy, gw + 2 * ggx, visible * rh + 2 * ggy};
	ListViewRow hr;
	memset(&hr, 0, sizeof(hr));
	get(ctx, sl, focused, &hr);
	if (!hr.is_header && sel >= 0) {
		SDL_SetClipRect(s, &wide);
		if (focused)
			BOS_rrect(s, gx - ggx / 2, y + (int)fsel - (int)fsc - ggy / 2, gw + ggx, rh + ggy, BOS_px(BOS_RADIUS_ROW), th->focus);
		else // the open row of a parent list: surface2, no scale
			BOS_rrect(s, gx, y + (int)fsel - (int)fsc, gw, rh, BOS_px(BOS_RADIUS_ROW), th->surface2);
	}
	SDL_SetClipRect(s, &box);
	for (int i = (int)fsc / rh; i < count; i++) {
		int ry = y + i * rh - (int)fsc;
		if (ry >= y + visible * rh)
			break;
		ListViewRow r;
		memset(&r, 0, sizeof(r));
		get(ctx, i, focused && i == sel, &r);
		bool hot = focused && i == sel && !r.is_header, open = !focused && i == sel;
		if (hot)
			SDL_SetClipRect(s, &wide);
		drawRow(s, &r, gx, gw, ry, rh, hot, open);
		if (hot)
			SDL_SetClipRect(s, &box);
		else if (i + 1 != sel && i != sel && i + 1 < count && ry + rh < y + visible * rh) {
			int lead = leadOf(&r, rh), nx = lead ? gx + BOS_px(20) + lead + BOS_px(22) : gx + BOS_px(28);
			SDL_FillRect(s, &(SDL_Rect){nx, ry + rh - 1, gx + gw - nx, 1}, BOS_map(s, BOS_C_SEPARATOR));
		}
	}
	SDL_SetClipRect(s, NULL);
	if (count > visible) { // the scroll mark, just right of the group
		int tall = visible * rh, mh = tall * visible / count, my = y + (tall - mh) * sc / (count - visible);
		BOS_rrect(s, gx + gw + BOS_px(10), my, BOS_px(6), mh, BOS_px(3), BOS_C_TEXTOFF);
	}
	return visible;
}

// ---- the shared ListView ----
static void rowFromView(void* ctx, int i, bool hot, ListViewRow* r) {
	ListView* v = ctx;
	memset(r, 0, sizeof(*r));
	if (v->get_row && i >= 0 && i < v->count)
		v->get_row(v->ctx, i, hot, r);
	r->label = r->label ? tr(r->label) : "";
	r->sub = tr(r->sub);
	r->annotation = tr(r->annotation);
	r->badge = tr(r->badge);
	r->pill = tr(r->pill);
}

static void hints(SDL_Surface* s, ListView* v) {
	if (v->count <= 0 && v->empty_btn_pairs && !v->bos_hints)
		BOS_uiHintBar(s, v->empty_btn_pairs);
	else if (v->bos_hints || v->bos_left || !v->hint_pairs)
		BOS_hintBar(s, v->bos_hints, v->bos_nhints, v->bos_left, v->bos_nleft);
	else
		BOS_uiHintBar(s, v->hint_pairs);
}

static void emptyState(SDL_Surface* s, ListView* v, int top, int bottom) {
	int card = BOS_px(140), ic = BOS_px(76), cx = s->w / 2;
	const char* es = tr(v->empty_subtitle);
	int th = BOS_lineH(44), sh = es ? BOS_textLines(es, 30, 400, s->w - BOS_px(160), 3) * BOS_lineH(30) : 0;
	int h = card + BOS_px(28) + th + (sh ? BOS_px(12) + sh : 0), y = top + (bottom - top - h) / 2 - BOS_px(20);
	BOS_rrect(s, cx - card / 2, y, card, card, BOS_px(36), BOS_theme()->surface);
	BOS_icon(s, v->bos_empty_icon ? v->bos_empty_icon : "info", BOS_C_TEXT2, 1, cx - ic / 2, y + (card - ic) / 2, ic);
	y += card + BOS_px(28);
	const char* t = v->empty_title ? tr(v->empty_title) : "";
	BOS_text(s, t, 44, 700, BOS_C_TEXT, 1, cx - BOS_textW(t, 44, 700) / 2, y);
	if (sh)
		BOS_textWrapCentre(s, es, 30, 400, BOS_C_TEXT2, 1, BOS_px(80), y + th + BOS_px(12), s->w - BOS_px(160), 3);
}

void BOS_listViewRender(ListView* v, SDL_Surface* s) {
	const BosTheme* th = BOS_theme();
	bool snap = false;
	if (v->list_id != v->drawn_id || v->count != v->drawn_count) {
		v->drawn_id = v->list_id;
		v->drawn_count = v->count;
		snap = true;
	}
	v->input_pending = false;
	if (v->bos_status || v->title) {
		SDL_FillRect(s, NULL, BOS_map(s, th->bg));
		BOS_statusBar(s, v->bos_status ? v->bos_status : tr(lastPart(v->title)));
	} else // the caller drew the bar
		SDL_FillRect(s, &(SDL_Rect){0, BOS_px(BOS_BAR_H), s->w, s->h - BOS_px(BOS_BAR_H)}, BOS_map(s, th->bg));
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H), y = top;
	if (v->bos_title)
		y += BOS_largeTitle(s, y, v->bos_title, v->bos_sub);
	else
		y += BOS_px(8);
	if (v->count <= 0) {
		v->scroll = 0, v->last_visible = 0;
		emptyState(s, v, y, bottom);
		hints(s, v);
		return;
	}
	int gx = BOS_px(40), rh = BOS_px(v->bos_row_h > 0 ? v->bos_row_h : BOS_ROW_H);
	int foot = v->bos_footer ? BOS_px(12) + BOS_lineH(26) : 0;
	SDL_Rect area = {gx, y, s->w - 2 * gx, bottom - BOS_px(12) - foot - y};
	int visible = BOS_groupRows(s, &area, rh, v->count, v->selected, true, &v->scroll, &v->bos_pill, &v->bos_scroll,
								snap, rowFromView, v);
	v->last_visible = visible;
	if (v->bos_footer)
		BOS_textWrap(s, tr(v->bos_footer), 26, 400, BOS_C_TEXTOFF, 1, gx + BOS_px(28), y + visible * rh + BOS_px(12),
					 area.w - BOS_px(28), 2);
	hints(s, v);
}

bool BOS_listViewBusy(ListView* v) {
	return v->input_pending || BOS_glideMoving(&v->bos_pill) || BOS_glideMoving(&v->bos_scroll) || BOS_marqueeActive();
}

// ---- two columns (board v2-MusicRadioAdd / v2-VideoTVAdd / Settings) ----
static BosGlide col_lp, col_ls, col_rp, col_rs;
void BOS_columns(SDL_Surface* s, const BosColumnsView* v) {
	const BosTheme* th = BOS_theme();
	SDL_FillRect(s, NULL, BOS_map(s, th->bg));
	BOS_statusBar(s, v->status ? v->status : "");
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	int lx = BOS_px(24), lw = BOS_px(380), rx = lx + lw + BOS_px(28), rw = s->w - BOS_px(32) - rx;
	SDL_Rect la = {lx, top + BOS_px(8), lw, bottom - BOS_px(12) - top - BOS_px(8)};
	BOS_groupRows(s, &la, BOS_px(84), v->nleft, v->left_sel, !v->right_focused, v->left_scroll, &col_lp, &col_ls, false,
				  v->get_left, v->ctx);
	int y = top;
	if (v->title) {
		char t[160];
		BOS_fit(v->title, BOS_FS_TITLE, 700, rw - BOS_px(8), t, sizeof(t));
		BOS_text(s, t, BOS_FS_TITLE, 700, BOS_C_TEXT, 1, rx + BOS_px(8), y);
		y += (int)(BOS_FS_TITLE * 1.15f);
	}
	if (v->sub) {
		BOS_text(s, v->sub, 26, 400, BOS_C_TEXT2, 1, rx + BOS_px(8), y);
		y += BOS_lineH(26);
	}
	y += BOS_px(12);
	if (v->nright > 0) {
		SDL_Rect ra = {rx, y, rw, bottom - BOS_px(12) - y};
		BOS_groupRows(s, &ra, BOS_px(96), v->nright, v->right_sel, v->right_focused, v->right_scroll, &col_rp, &col_rs,
					  false, v->get_right, v->ctx);
	} else if (v->note)
		BOS_textWrap(s, v->note, 30, 400, BOS_C_TEXT2, 1, rx + BOS_px(8), y, rw - BOS_px(16), 6);
	BOS_hintBar(s, v->hints, v->nhints, v->lhints, v->nlhints);
}

bool BOS_columnsMoving(void) {
	return BOS_glideMoving(&col_lp) || BOS_glideMoving(&col_ls) || BOS_glideMoving(&col_rp) ||
		   BOS_glideMoving(&col_rs) || BOS_marqueeActive();
}
