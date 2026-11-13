// The in-game menu in the Brick OS kit (DESIGN.md 7.1; boards design/round2/v2-GameMenu*): the paused game blurred
// and dimmed behind, the status bar, the five actions in a group on the left, on the right the game as it is now or
// the chosen save slot with the eight slots under it, the hint bar.
#include "ma_menu_bos.h"
#include "bos_ui.h"
#include "i18n.h"
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define C_DANGER 0xFF453A
#define C_SEP 0x38383A
#define PIC_W 552
#define PIC_H 414

static SDL_Surface* bg;		 // the paused game, blurred and at 28 %
static SDL_Surface* now_pic; // the paused game, fitted and rounded
static SDL_Surface* slot_pic;
static char slot_pic_path[512];
static BosGlide g_pill;

// the frame scaled into w x h (bilinear), its corners cut round (radius r, 0: square)
static SDL_Surface* fitted(SDL_Surface* src, int w, int h, int r) {
	if (!src)
		return NULL;
	SDL_Surface* a = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_Surface* d = a ? SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888) : NULL;
	if (!d) {
		if (a)
			SDL_FreeSurface(a);
		return NULL;
	}
	for (int y = 0; y < h; y++) {
		float sy = (y + 0.5f) * a->h / h - 0.5f;
		int y0 = sy < 0 ? 0 : (int)sy, y1 = y0 + 1 < a->h ? y0 + 1 : y0;
		int fy = (int)((sy - y0) * 256);
		if (fy < 0)
			fy = 0;
		const uint32_t* r0 = (const uint32_t*)((uint8_t*)a->pixels + y0 * a->pitch);
		const uint32_t* r1 = (const uint32_t*)((uint8_t*)a->pixels + y1 * a->pitch);
		uint32_t* out = (uint32_t*)((uint8_t*)d->pixels + y * d->pitch);
		for (int x = 0; x < w; x++) {
			float sx = (x + 0.5f) * a->w / w - 0.5f;
			int x0 = sx < 0 ? 0 : (int)sx, x1 = x0 + 1 < a->w ? x0 + 1 : x0;
			int fx = (int)((sx - x0) * 256);
			if (fx < 0)
				fx = 0;
			uint32_t p = 0xFF000000u;
			for (int sh = 0; sh < 24; sh += 8) {
				int c00 = (r0[x0] >> sh) & 255, c01 = (r0[x1] >> sh) & 255, c10 = (r1[x0] >> sh) & 255,
					c11 = (r1[x1] >> sh) & 255;
				int top = c00 * (256 - fx) + c01 * fx, bot = c10 * (256 - fx) + c11 * fx;
				p |= (uint32_t)(((top * (256 - fy) + bot * fy) >> 16) & 255) << sh;
			}
			// round corners: coverage of the corner circle
			if (r > 0) {
				float dx = x < r ? r - x - 0.5f : x >= w - r ? x - (w - r) + 0.5f : 0;
				float dy = y < r ? r - y - 0.5f : y >= h - r ? y - (h - r) + 0.5f : 0;
				if (dx > 0 && dy > 0) {
					float dd = SDL_sqrtf(dx * dx + dy * dy);
					float cov = dd <= r - 0.5f ? 1 : dd >= r + 0.5f ? 0 : r + 0.5f - dd;
					p = (p & 0xFFFFFF) | (uint32_t)(cov * 255) << 24;
				}
			}
			out[x] = p;
		}
	}
	SDL_FreeSurface(a);
	SDL_SetSurfaceBlendMode(d, SDL_BLENDMODE_BLEND);
	return d;
}

// the mean of each k x k block (a blur that leaves no blocks once scaled back up), then one 3x3 smoothing pass
static SDL_Surface* boxDown(SDL_Surface* src, int k) {
	SDL_Surface* a = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
	if (!a)
		return NULL;
	int w = a->w / k, h = a->h / k;
	SDL_Surface* d = w > 0 && h > 0 ? SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888) : NULL;
	SDL_Surface* t = d ? SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888) : NULL;
	if (!t) {
		if (d)
			SDL_FreeSurface(d);
		SDL_FreeSurface(a);
		return NULL;
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t r = 0, g = 0, b = 0;
			for (int j = 0; j < k; j++) {
				const uint32_t* row = (const uint32_t*)((uint8_t*)a->pixels + (y * k + j) * a->pitch) + x * k;
				for (int i = 0; i < k; i++)
					r += (row[i] >> 16) & 255, g += (row[i] >> 8) & 255, b += row[i] & 255;
			}
			int n = k * k;
			((uint32_t*)((uint8_t*)t->pixels + y * t->pitch))[x] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
		}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t r = 0, g = 0, b = 0, n = 0;
			for (int j = -1; j <= 1; j++)
				for (int i = -1; i <= 1; i++) {
					int yy = y + j, xx = x + i;
					if (yy < 0 || yy >= h || xx < 0 || xx >= w)
						continue;
					uint32_t p = ((uint32_t*)((uint8_t*)t->pixels + yy * t->pitch))[xx];
					r += (p >> 16) & 255, g += (p >> 8) & 255, b += p & 255, n++;
				}
			((uint32_t*)((uint8_t*)d->pixels + y * d->pitch))[x] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
		}
	SDL_FreeSurface(t);
	SDL_FreeSurface(a);
	return d;
}

void MenuBos_open(SDL_Surface* frame, int w, int h) {
	MenuBos_close();
	if (!frame)
		return;
	// blur: averaged down to a sixteenth, smoothed, scaled back up; then 28 % of the light (the board's 72 % black)
	SDL_Surface* small = boxDown(frame, 16);
	bg = small ? fitted(small, w, h, 0) : NULL;
	if (small)
		SDL_FreeSurface(small);
	if (bg) {
		for (int y = 0; y < bg->h; y++) {
			uint32_t* px = (uint32_t*)((uint8_t*)bg->pixels + y * bg->pitch);
			for (int x = 0; x < bg->w; x++)
				px[x] = 0xFF000000u | ((((px[x] & 0xFF00FF) * 72) >> 8) & 0xFF00FF) | ((((px[x] & 0xFF00) * 72) >> 8) & 0xFF00);
		}
		SDL_SetSurfaceBlendMode(bg, SDL_BLENDMODE_NONE);
	}
	now_pic = fitted(frame, BOS_px(PIC_W), BOS_px(PIC_H), BOS_px(24));
	g_pill.set = false;
}

void MenuBos_close(void) {
	SDL_Surface** all[] = {&bg, &now_pic, &slot_pic};
	for (int i = 0; i < 3; i++)
		if (*all[i]) {
			SDL_FreeSurface(*all[i]);
			*all[i] = NULL;
		}
	slot_pic_path[0] = '\0';
}

bool MenuBos_moving(void) {
	return BOS_glideMoving(&g_pill);
}

static const char* ICON[] = {"play", "save", "update", "settings", "power"};
static const uint32_t ICON_BG[] = {0x30D158, 0x0A84FF, 0x5E5CE6, 0x8E8E93, 0xFF453A};

static void savedWhen(time_t t, char* out, int size) {
	static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	struct tm tm;
	localtime_r(&t, &tm);
	char when[64];
	snprintf(when, sizeof(when), "%d %s %02d:%02d", tm.tm_mday, I18N_text(mon[tm.tm_mon]), tm.tm_hour, tm.tm_min);
	snprintf(out, size, I18N_text("Saved %s"), when);
}

void MenuBos_render(SDL_Surface* s, const MenuBosView* v) {
	const BosTheme* th = BOS_theme();
	if (bg)
		SDL_BlitSurface(bg, NULL, s, NULL);
	else
		SDL_FillRect(s, NULL, BOS_map(s, th->bg));
	BOS_statusBar(s, v->title);

	// ---- the actions: a group of 84 px rows, 380 wide ----
	int gx = BOS_px(24), gy = BOS_px(64 + 8), gw = BOS_px(380), rh = BOS_px(84);
	BOS_rrect(s, gx, gy, gw, rh * MENU_BOS_ITEMS, BOS_px(22), th->surface);
	for (int i = 0; i + 1 < MENU_BOS_ITEMS; i++)
		if (i != v->selected && i + 1 != v->selected)
			SDL_FillRect(s, &(SDL_Rect){gx + BOS_px(96), gy + (i + 1) * rh - 1, gw - BOS_px(96), 1}, BOS_map(s, C_SEP));
	float py = BOS_glideTo(&g_pill, (float)(v->selected * rh), 120, false);
	{ // the focus: the theme's focus colour, 4 % larger
		int fw = (int)(gw * BOS_ROW_SCALE), fh = (int)(rh * BOS_ROW_SCALE);
		BOS_rrect(s, gx - (fw - gw) / 2, gy + (int)py - (fh - rh) / 2, fw, fh, BOS_px(BOS_RADIUS_ROW), th->focus);
	}
	for (int i = 0; i < MENU_BOS_ITEMS; i++) {
		bool hot = i == v->selected, off = v->item_off[i];
		float a = off ? 0.35f : 1;
		int y = gy + i * rh, cy = y + rh / 2, sq = BOS_px(56), sx = gx + BOS_px(20);
		BOS_rrect(s, sx, cy - sq / 2, sq, sq, BOS_px(15), off ? th->surface2 : ICON_BG[i]);
		int ic = BOS_px(34);
		BOS_icon(s, ICON[i], 0xFFFFFF, a, sx + (sq - ic) / 2, cy - ic / 2, ic);
		uint32_t fg = hot ? th->onfocus : (i == MENU_BOS_ITEMS - 1 ? C_DANGER : BOS_C_TEXT);
		int nx = sx + sq + BOS_px(22);
		BOS_textMid(s, v->items[i], 34, hot ? 600 : 500, fg, a, nx, cy);
		if (i == 0 && v->disc && v->disc[0]) { // more than one disc: ‹ แผ่น 2 › on the row
			int dw = BOS_textW(v->disc, BOS_FS_SMALL, 500), ar = BOS_px(26), right = gx + gw - BOS_px(16);
			uint32_t dc = hot ? th->onfocus : BOS_C_TEXT2;
			if (hot)
				BOS_icon(s, "chev", dc, 1, right - ar, cy - ar / 2, ar), right -= ar + BOS_px(4);
			BOS_textMid(s, v->disc, BOS_FS_SMALL, 500, dc, 1, right - dw, cy);
			if (hot)
				BOS_icon(s, "chevl", dc, 1, right - dw - BOS_px(4) - ar, cy - ar / 2, ar);
		}
	}

	// ---- the right side: the game as it is now, or the slot ----
	int px = BOS_px(440), pyy = gy, pw = BOS_px(PIC_W), ph = BOS_px(PIC_H);
	if (v->slots) {
		if (v->preview_path && strcmp(v->preview_path, slot_pic_path)) { // decoded once per slot
			if (slot_pic)
				SDL_FreeSurface(slot_pic), slot_pic = NULL;
			SDL_Surface* img = IMG_Load(v->preview_path);
			if (img) {
				slot_pic = fitted(img, pw, ph, BOS_px(24));
				SDL_FreeSurface(img);
			}
			snprintf(slot_pic_path, sizeof(slot_pic_path), "%s", v->preview_path);
		}
		int ring = BOS_px(4);
		BOS_rrect(s, px - ring, pyy - ring, pw + 2 * ring, ph + 2 * ring, BOS_px(24) + ring, th->focus);
		if (v->preview_path && slot_pic)
			BOS_blit(slot_pic, s, px, pyy, 1);
		else {
			BOS_rrect(s, px, pyy, pw, ph, BOS_px(24), th->surface);
			const char* t = I18N_text(v->save_exists ? "No preview" : "Empty Slot");
			BOS_textMid(s, t, 34, 500, BOS_C_TEXTOFF, 1, px + (pw - BOS_textW(t, 34, 500)) / 2,
						pyy + ph / 2);
		}
		char badge[32];
		snprintf(badge, sizeof(badge), I18N_text("Slot %d"), v->slot + 1);
		int bw = BOS_textW(badge, 24, 600) + BOS_px(32), bh = BOS_px(40);
		BOS_rrect(s, px + BOS_px(16), pyy + BOS_px(16), bw, bh, bh / 2, 0x1C1C1F);
		BOS_textMid(s, badge, 24, 600, BOS_C_TEXT, 1, px + BOS_px(32), pyy + BOS_px(16) + bh / 2);
		int y = pyy + ph + BOS_px(20);
		char when[96];
		if (v->save_exists)
			savedWhen(v->save_time, when, sizeof(when));
		else
			snprintf(when, sizeof(when), "%s", I18N_text("No save yet"));
		BOS_text(s, when, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, px, y);
		y += BOS_lineH(BOS_FS_SMALL) + BOS_px(14);
		int cs = BOS_px(44), gap = BOS_px(10);
		for (int i = 0; i < MENU_BOS_SLOTS; i++) { // the eight slots: filled ones solid, the chosen one in focus
			int x = px + i * (cs + gap);
			bool on = i == v->slot, used = v->slot_used[i];
			if (on)
				BOS_rrect(s, x, y, cs, cs, BOS_px(12), th->focus);
			else if (used)
				BOS_rrect(s, x, y, cs, cs, BOS_px(12), th->surface2);
			else {
				BOS_rrect(s, x, y, cs, cs, BOS_px(12), th->surface2);
				BOS_rrect(s, x + 2, y + 2, cs - 4, cs - 4, BOS_px(10), 0x111113);
			}
			char n[4];
			snprintf(n, sizeof(n), "%d", i + 1);
			BOS_textMid(s, n, 22, 600, on ? th->onfocus : used ? BOS_C_TEXT : BOS_C_TEXTOFF, 1,
						x + (cs - BOS_textW(n, 22, 600)) / 2, y + cs / 2);
		}
	} else {
		if (now_pic)
			BOS_blit(now_pic, s, px, pyy, 1);
		int y = pyy + ph + BOS_px(20);
		char name[256];
		BOS_fit(v->game, 34, 700, pw, name, sizeof(name));
		BOS_text(s, name, 34, 700, BOS_C_TEXT, 1, px, y);
		y += BOS_lineH(34) + BOS_px(4);
		BOS_fit(v->console, BOS_FS_SMALL, 400, pw, name, sizeof(name));
		BOS_text(s, name, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, px, y);
	}

	BOS_hintBar(s, v->hints, v->nhints, v->left_hints, v->nleft);
}

// ---- the options pages ----
static BosGlide o_pill, o_scroll, l_pill;
static const void* o_page;

static void optToggle(SDL_Surface* s, int x, int cy, bool on, bool hot) {
	int w = BOS_px(80), h = BOS_px(46), k = BOS_px(38);
	uint32_t track = on ? BOS_theme()->primary : (hot ? 0xC8C8CE : 0x48484A);
	BOS_rrect(s, x, cy - h / 2, w, h, h / 2, track);
	BOS_rrect(s, on ? x + w - k - BOS_px(4) : x + BOS_px(4), cy - k / 2, k, k, k / 2, 0xFFFFFF);
}

void MenuBos_renderOptions(SDL_Surface* s, const MenuBosOptView* v) {
	const BosTheme* th = BOS_theme();
	if (bg)
		SDL_BlitSurface(bg, NULL, s, NULL);
	else
		SDL_FillRect(s, NULL, BOS_map(s, th->bg));
	BOS_statusBar(s, I18N_text("Options"));
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);

	// ---- left: the parent page, its open row in surface2 (or the focus, at the Options page itself) ----
	int lx = BOS_px(24), lw = BOS_px(380), lrh = BOS_px(84), ly = top + BOS_px(8);
	int lmax = (bottom - ly) / lrh, n = v->nleft < lmax ? v->nleft : lmax;
	int lfirst = v->left_sel >= n ? v->left_sel - n + 1 : 0;
	BOS_rrect(s, lx, ly, lw, n * lrh, BOS_px(22), th->surface);
	float lpy = BOS_glideTo(&l_pill, (float)((v->left_sel - lfirst) * lrh), 120, false);
	if (v->left_focused) {
		int fw = (int)(lw * BOS_ROW_SCALE), fh = (int)(lrh * BOS_ROW_SCALE);
		BOS_rrect(s, lx - (fw - lw) / 2, ly + (int)lpy - (fh - lrh) / 2, fw, fh, BOS_px(BOS_RADIUS_ROW), th->focus);
	} else
		BOS_rrect(s, lx, ly + (v->left_sel - lfirst) * lrh, lw, lrh, BOS_px(22), th->surface2);
	for (int i = 0; i < n; i++) {
		int idx = lfirst + i, y = ly + i * lrh, cy = y + lrh / 2;
		bool hot = v->left_focused && idx == v->left_sel, open = idx == v->left_sel;
		float a = hot || open ? 1 : BOS_DIM;
		int nx = lx + BOS_px(24);
		if (v->left_icons && v->left_icons[idx]) {
			int sq = BOS_px(56), sx = lx + BOS_px(20), ic = BOS_px(36);
			BOS_rrect(s, sx, cy - sq / 2, sq, sq, BOS_px(15), v->left_colors ? v->left_colors[idx] : th->surface2);
			BOS_icon(s, v->left_icons[idx], 0xFFFFFF, a, sx + (sq - ic) / 2, cy - ic / 2, ic);
			nx = sx + sq + BOS_px(22);
		}
		if (i + 1 < n && idx != v->left_sel && idx + 1 != v->left_sel)
			SDL_FillRect(s, &(SDL_Rect){nx, y + lrh - 1, lx + lw - nx, 1}, BOS_map(s, C_SEP));
		char t[160];
		BOS_fit(v->left[idx], 34, hot ? 600 : 500, lx + lw - BOS_px(20) - nx, t, sizeof(t));
		BOS_textMid(s, t, 34, hot ? 600 : 500, hot ? th->onfocus : BOS_C_TEXT, a, nx, cy);
	}

	// ---- right: the page's title and rows (at the Options page: what the focused row holds) ----
	int rx = lx + lw + BOS_px(28), rw = s->w - BOS_px(32) - rx;
	char head[160];
	BOS_fit(v->head ? v->head : "", BOS_FS_TITLE, 700, rw - BOS_px(8), head, sizeof(head));
	BOS_text(s, head, BOS_FS_TITLE, 700, BOS_C_TEXT, 1, rx + BOS_px(8), top);
	int list_y = top + BOS_px(80), rh = BOS_px(BOS_ROW_H), foot = BOS_px(8) + 2 * BOS_lineH(26);
	if (v->left_focused || v->nrows <= 0) {
		if (v->head_desc && v->head_desc[0])
			BOS_textWrap(s, v->head_desc, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, rx + BOS_px(8), list_y, rw - BOS_px(16), 4);
		BOS_hintBar(s, v->hints, v->nhints, v->left_hints, v->nleft_hints);
		return;
	}
	int rows = (bottom - list_y - foot) / rh;
	if (rows > v->nrows)
		rows = v->nrows;
	int sc = v->scroll ? *v->scroll : 0;
	if (v->sel < sc)
		sc = v->sel;
	if (v->sel >= sc + rows)
		sc = v->sel - rows + 1;
	if (v->scroll)
		*v->scroll = sc;
	bool snap = o_page != (const void*)v->rows;
	o_page = v->rows;
	float fsc = BOS_glideTo(&o_scroll, (float)(sc * rh), 180, snap);
	float fsel = BOS_glideTo(&o_pill, (float)(v->sel * rh), 120, snap);
	SDL_Rect box = {rx, list_y, rw, rows * rh};
	BOS_rrect(s, rx, list_y, rw, rows * rh, BOS_px(24), th->surface);
	int gw = (int)(rw * BOS_ROW_SCALE) - rw, gh = (int)(rh * BOS_ROW_SCALE) - rh;
	SDL_Rect wide = {rx - gw, list_y - gh, rw + 2 * gw, rows * rh + 2 * gh};
	SDL_SetClipRect(s, &wide);
	BOS_rrect(s, rx - gw / 2, list_y + (int)fsel - (int)fsc - gh / 2, rw + gw, rh + gh, BOS_px(BOS_RADIUS_ROW), th->focus);
	SDL_SetClipRect(s, &box);
	for (int i = (int)fsc / rh; i < v->nrows; i++) {
		int y = list_y + i * rh - (int)fsc;
		if (y >= list_y + rows * rh)
			break;
		const MenuBosRow* r = &v->rows[i];
		bool hot = i == v->sel;
		int cy = y + rh / 2, nx = rx + BOS_px(28), right = rx + rw - BOS_px(24);
		uint32_t fg = hot ? th->onfocus : BOS_C_TEXT, sub = hot ? th->onfocus : BOS_C_TEXT2;
		if (hot)
			SDL_SetClipRect(s, &wide);
		int vw = 0, ar = BOS_px(28);
		switch (r->kind) {
		case MB_ROW_TOGGLE:
			optToggle(s, right - BOS_px(80), cy, r->on, hot);
			vw = BOS_px(80);
			break;
		case MB_ROW_SLIDER: {
			int nw = BOS_textW("100", BOS_FS_SMALL, 600), bw = BOS_px(200), bh = BOS_px(12), x = right;
			if (hot) {
				BOS_icon(s, "chev", sub, 1, x - ar, cy - ar / 2, ar);
				x -= ar + BOS_px(6);
			}
			const char* num = r->value ? r->value : "";
			BOS_textMid(s, num, BOS_FS_SMALL, 600, sub, 1, x - BOS_textW(num, BOS_FS_SMALL, 600), cy);
			x -= nw + BOS_px(18);
			BOS_rrect(s, x - bw, cy - bh / 2, bw, bh, bh / 2, hot ? 0xC8C8CE : th->surface2);
			int fw = (int)(bw * r->frac);
			if (fw > 0)
				BOS_rrect(s, x - bw, cy - bh / 2, fw < bh ? bh : fw, bh, bh / 2, hot ? th->onfocus : BOS_C_TEXT);
			x -= bw;
			if (hot) {
				BOS_icon(s, "chevl", sub, 1, x - BOS_px(6) - ar, cy - ar / 2, ar);
				x -= ar + BOS_px(6);
			}
			vw = right - x;
			break;
		}
		case MB_ROW_NAV:
			BOS_icon(s, "chev", hot ? th->onfocus : BOS_C_TEXTOFF, 1, right - ar, cy - ar / 2, ar);
			vw = ar;
			if (r->value && r->value[0]) {
				int tw = BOS_textW(r->value, BOS_FS_SMALL, 400);
				BOS_textMid(s, r->value, BOS_FS_SMALL, 400, sub, 1, right - ar - BOS_px(10) - tw, cy);
				vw += BOS_px(10) + tw;
			}
			break;
		case MB_ROW_TEXT:
		case MB_ROW_CYCLE:
		case MB_ROW_BIND: {
			if (!r->value || !r->value[0])
				break;
			char val[96];
			BOS_fit(r->value, BOS_FS_SMALL, 400, rw / 2, val, sizeof(val));
			int tw = BOS_textW(val, BOS_FS_SMALL, 400), x = right;
			bool arrows = hot && r->kind == MB_ROW_CYCLE;
			if (arrows) {
				BOS_icon(s, "chev", sub, 1, x - ar, cy - ar / 2, ar);
				x -= ar + BOS_px(6);
			}
			BOS_textMid(s, val, BOS_FS_SMALL, 400, sub, 1, x - tw, cy);
			x -= tw;
			if (arrows) {
				BOS_icon(s, "chevl", sub, 1, x - BOS_px(6) - ar, cy - ar / 2, ar);
				x -= ar + BOS_px(6);
			}
			vw = right - x;
			break;
		}
		}
		char name[160];
		int room = right - nx - vw - BOS_px(16);
		if (hot)
			BOS_textMarquee(s, r->name, 40, 600, fg, 1, nx, cy, room);
		else {
			BOS_fit(r->name, 40, 500, room, name, sizeof(name));
			BOS_textMid(s, name, 40, 500, fg, 1, nx, cy);
		}
		if (hot)
			SDL_SetClipRect(s, &box);
		else if (i + 1 != v->sel && i + 1 < v->nrows && y + rh < list_y + rows * rh)
			SDL_FillRect(s, &(SDL_Rect){nx, y + rh - 1, rx + rw - nx, 1}, BOS_map(s, C_SEP));
	}
	SDL_SetClipRect(s, NULL);
	if (v->nrows > rows && rows > 0) { // the scroll mark
		int tall = rows * rh, mh = tall * rows / v->nrows, my = list_y + (tall - mh) * sc / (v->nrows - rows);
		BOS_rrect(s, s->w - BOS_px(14), my, BOS_px(6), mh, BOS_px(3), BOS_C_TEXTOFF);
	}
	if (v->desc && v->desc[0])
		BOS_textWrap(s, v->desc, 26, 400, BOS_C_TEXTOFF, 1, rx + BOS_px(28), list_y + rows * rh + BOS_px(12),
					 rw - BOS_px(28), 2);
	BOS_hintBar(s, v->hints, v->nhints, v->left_hints, v->nleft_hints);
}

bool MenuBos_optMoving(void) {
	return BOS_glideMoving(&o_pill) || BOS_glideMoving(&o_scroll) || BOS_glideMoving(&l_pill) || BOS_marqueeActive();
}
