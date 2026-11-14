// Brick OS kit: the shared ListView drawn as a §5.6 group (DESIGN.md 5.5-5.6), for apps that set `bos` on the view.
// The ListView keeps owning input and selection (ui_listview.c); this only draws, and keeps its scroll and page size.
#include "bos_ui.h"
#include "ui_listview.h"
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

static void hints(SDL_Surface* s, ListView* v) {
	if (v->bos_hints || v->bos_left || !v->hint_pairs)
		BOS_hintBar(s, v->bos_hints, v->bos_nhints, v->bos_left, v->bos_nleft);
	else
		BOS_uiHintBar(s, v->hint_pairs);
}

static void rowSafe(ListView* v, int i, bool sel, ListViewRow* r) {
	memset(r, 0, sizeof(*r));
	if (v->get_row && i >= 0 && i < v->count)
		v->get_row(v->ctx, i, sel, r);
	r->label = r->label ? tr(r->label) : "";
	r->sub = tr(r->sub);
	r->annotation = tr(r->annotation);
	r->badge = tr(r->badge);
	r->pill = tr(r->pill);
}

static void playingBars(SDL_Surface* s, int right, int cy, uint32_t c) {
	static const int hh[4] = {12, 24, 16, 20};
	int bw = BOS_px(5), gap = BOS_px(3), x = right - 4 * bw - 3 * gap;
	for (int i = 0; i < 4; i++) {
		int h = BOS_px(hh[i]);
		BOS_rrect(s, x + i * (bw + gap), cy + BOS_px(13) - h, bw, h, BOS_px(2), c);
	}
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

	int gx = BOS_px(40), gw = s->w - 2 * gx, rh = BOS_px(v->bos_row_h > 0 ? v->bos_row_h : BOS_ROW_H);
	int foot = v->bos_footer ? BOS_px(12) + BOS_lineH(26) : 0;
	int visible = (bottom - BOS_px(12) - foot - y) / rh;
	if (visible < 1)
		visible = 1;
	if (visible > v->count)
		visible = v->count;
	int sel = v->selected < 0 ? 0 : v->selected >= v->count ? v->count - 1 : v->selected;
	// the focused row keeps one row between it and the edge while there is more
	int sc = v->scroll, margin = visible >= 3 ? 1 : 0;
	if (sel - margin < sc)
		sc = sel - margin;
	if (sel + margin > sc + visible - 1)
		sc = sel + margin - visible + 1;
	if (sc > v->count - visible)
		sc = v->count - visible;
	if (sc < 0)
		sc = 0;
	v->scroll = sc, v->last_visible = visible;
	float fsc = BOS_glideTo(&v->bos_scroll, (float)(sc * rh), 180, snap);
	float fsel = BOS_glideTo(&v->bos_pill, (float)(sel * rh), 120, snap);

	SDL_Rect box = {gx, y, gw, visible * rh};
	BOS_rrect(s, gx, y, gw, visible * rh, BOS_px(BOS_RADIUS_GROUP), th->surface);
	int ggx = (int)(gw * BOS_ROW_SCALE) - gw, ggy = (int)(rh * BOS_ROW_SCALE) - rh;
	SDL_Rect wide = {gx - ggx, y - ggy, gw + 2 * ggx, visible * rh + 2 * ggy};
	ListViewRow hot_row;
	rowSafe(v, sel, true, &hot_row);
	if (!hot_row.is_header) {
		SDL_SetClipRect(s, &wide);
		BOS_rrect(s, gx - ggx / 2, y + (int)fsel - (int)fsc - ggy / 2, gw + ggx, rh + ggy, BOS_px(BOS_RADIUS_ROW), th->focus);
	}
	SDL_SetClipRect(s, &box);
	for (int i = (int)fsc / rh; i < v->count; i++) {
		int ry = y + i * rh - (int)fsc;
		if (ry >= y + visible * rh)
			break;
		bool hot = i == sel;
		ListViewRow r;
		rowSafe(v, i, hot, &r);
		hot = hot && !r.is_header;
		if (hot)
			SDL_SetClipRect(s, &wide);
		int cy = ry + rh / 2, right = gx + gw - BOS_px(24), nx = gx + BOS_px(28);
		uint32_t fg = hot ? th->onfocus : r.danger ? BOS_C_DANGER : BOS_C_TEXT, sub = hot ? 0x3A3A3C : BOS_C_TEXT2;
		if (r.is_header) {
			BOS_textMid(s, r.label, 26, 600, BOS_C_TEXT2, 1, nx, cy + BOS_px(14));
			continue;
		}
		// lead: a picture, a squircle icon, or the row's own small icon
		int lead = 0;
		if (r.art) {
			lead = BOS_px(64);
			SDL_Surface* p = BOS_picRounded(r.art, lead, lead, BOS_px(12));
			if (p)
				BOS_blit(p, s, gx + BOS_px(20), cy - lead / 2, 1);
		} else if (r.sq_icon) {
			lead = BOS_px(56);
			int ic = BOS_px(36);
			BOS_rrect(s, gx + BOS_px(20), cy - lead / 2, lead, lead, lead * 26 / 100, BOS_iconColor(r.sq_color ? r.sq_color : 0x8E8E93));
			BOS_icon(s, r.sq_icon, 0xFFFFFF, 1, gx + BOS_px(20) + (lead - ic) / 2, cy - ic / 2, ic);
		}
		if (lead)
			nx = gx + BOS_px(20) + lead + BOS_px(22);
		// right: chevron, value, pill, playing bars (from the right edge in)
		int x = right;
		if (r.chevron) {
			int a = BOS_px(28);
			BOS_icon(s, "chev", hot ? th->onfocus : BOS_C_TEXTOFF, 1, x - a, cy - a / 2, a);
			x -= a + BOS_px(10);
		}
		const char* val = r.annotation ? r.annotation : r.badge;
		if (val && val[0]) {
			char t[96];
			BOS_fit(val, BOS_FS_SMALL, 400, gw / 3, t, sizeof(t));
			int w = BOS_textW(t, BOS_FS_SMALL, 400);
			BOS_textMid(s, t, BOS_FS_SMALL, 400, sub, 1, x - w, cy);
			x -= w + BOS_px(14);
		}
		if (r.pill && r.pill[0]) {
			int ph = BOS_px(40), pw = BOS_textW(r.pill, 24, 600) + BOS_px(32);
			uint32_t pb = r.pill_bg ? r.pill_bg : hot ? 0xC8C8CE : th->surface2;
			BOS_rrect(s, x - pw, cy - ph / 2, pw, ph, ph / 2, pb);
			BOS_textMid(s, r.pill, 24, 600, r.pill_fg ? r.pill_fg : hot ? th->onfocus : BOS_C_TEXT, 1, x - pw + BOS_px(16), cy);
			x -= pw + BOS_px(14);
		}
		if (r.playing) {
			playingBars(s, x, cy, hot ? th->onfocus : th->primary);
			x -= BOS_px(32) + BOS_px(14);
		}
		int room = x - nx - BOS_px(8);
		bool two = r.sub && r.sub[0];
		int fs = two ? 36 : 40, ny = two ? cy - BOS_px(16) : cy;
		char t[256];
		if (hot)
			BOS_textMarquee(s, r.label, fs, 600, fg, 1, nx, ny, room);
		else {
			BOS_fit(r.label, fs, 500, room, t, sizeof(t));
			BOS_textMid(s, t, fs, 500, fg, 1, nx, ny);
		}
		if (two) {
			BOS_fit(r.sub, 26, 400, room, t, sizeof(t));
			BOS_textMid(s, t, 26, 400, sub, 1, nx, cy + BOS_px(20));
		}
		if (hot)
			SDL_SetClipRect(s, &box);
		else if (i + 1 != sel && i + 1 < v->count && ry + rh < y + visible * rh)
			SDL_FillRect(s, &(SDL_Rect){nx, ry + rh - 1, gx + gw - nx, 1}, BOS_map(s, BOS_C_SEPARATOR));
	}
	SDL_SetClipRect(s, NULL);
	if (v->count > visible) { // the scroll mark
		int tall = visible * rh, mh = tall * visible / v->count, my = y + (tall - mh) * sc / (v->count - visible);
		BOS_rrect(s, s->w - BOS_px(26), my, BOS_px(6), mh, BOS_px(3), BOS_C_TEXTOFF);
	}
	if (v->bos_footer)
		BOS_textWrap(s, v->bos_footer, 26, 400, BOS_C_TEXTOFF, 1, gx + BOS_px(28), y + visible * rh + BOS_px(12),
					 gw - BOS_px(28), 1);
	hints(s, v);
}

bool BOS_listViewBusy(ListView* v) {
	return v->input_pending || BOS_glideMoving(&v->bos_pill) || BOS_glideMoving(&v->bos_scroll) || BOS_marqueeActive();
}
