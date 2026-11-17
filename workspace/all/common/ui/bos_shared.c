// Brick OS kit: the shared old UI pieces (menu bar, hint bar, confirm box, MENU popup, toast, empty page, loading,
// message, splash, controls help) drawn the Brick OS way (DESIGN.md 5, 7.1-7.3). The old pieces call these when an
// app has set UI_bos (ui_draw.h); everything here translates its text (the old pieces relied on GFX_renderText).
#include "bos_ui.h"
#include "i18n.h"
#include "ui_list.h"
#include "ui_listview.h"
#include "ui_listdialog.h"
#include "ui_keyboard.h"
#include "ui_confirmdialog.h"
#include "ui_downloadprogress.h"
#include "api.h"
#include <stdio.h>
#include <string.h>

// ---- leaving an app (DESIGN 7.7): the §5.7 sheet YouTube's render_quit_prompt draws, for every app ----
// The page behind is the frame on screen, dimmed to 60 % black between the bars once; the sheet (surface, radius
// 32) slides up from the hint bar (220 ms): the question 34/700, then rows 88 (40/500, 600 focused) with an icon 32.
typedef struct {
	SDL_Surface* back;
	char head[192];
	int sel;
	uint32_t t0;
	int ms;
} QuitAsk;

static void quitRender(SDL_Surface* s, void* vctx) {
	QuitAsk* q = vctx;
	const BosTheme* th = BOS_theme();
	SDL_BlitSurface(q->back, NULL, s, NULL);
	int W = s->w, bottom = s->h - BOS_px(BOS_BAR_H), rh = BOS_px(88), padx = BOS_px(40);
	int head = BOS_px(24) + BOS_lineH(34) + BOS_px(16), sh = head + 2 * rh + BOS_px(28);
	float t = q->ms > 0 ? (SDL_GetTicks() - q->t0) / (float)q->ms : 1;
	int top = bottom - sh + (t < 1 ? (int)(sh * (1 - BOS_ease(t))) : 0);
	BOS_rrect(s, 0, top, W, sh + BOS_px(40), BOS_px(32), th->surface);
	char h[192];
	BOS_fit(q->head, 34, 700, W - 2 * padx, h, sizeof(h));
	BOS_text(s, h, 34, 700, BOS_C_TEXT, 1, padx, top + BOS_px(24));
	static const char* const words[2] = {"Leave", "Stay"};
	static const char* const icons[2] = {"power", "play"};
	for (int i = 0; i < 2; i++) {
		int x = padx - BOS_px(8), y = top + head + i * rh, w = W - 2 * padx + BOS_px(16);
		bool foc = i == q->sel;
		if (foc) {
			int dw = w / 50, dh = rh / 50;
			BOS_rrect(s, x - dw, y - dh, w + 2 * dw, rh + 2 * dh, BOS_px(22), th->focus);
		}
		uint32_t tc = foc ? th->onfocus : i == 0 ? BOS_C_DANGER : BOS_C_TEXT;
		int lx = x + BOS_px(28), cy = y + rh / 2, ic = BOS_px(32);
		BOS_icon(s, icons[i], tc, 1, lx, cy - ic / 2, ic);
		BOS_textMid(s, I18N_text(words[i]), 40, foc ? 600 : 500, tc, 1, lx + BOS_px(48), cy);
	}
	BosHint r[2] = {{"A", I18N_text("Select")}, {"B", I18N_text("Close")}};
	BOS_hintBar(s, r, 2, NULL, 0);
}

static int quitHandle(void* vctx) {
	QuitAsk* q = vctx;
	if (PAD_justPressed(BTN_A))
		return q->sel == 0 ? 1 : 0;
	if (PAD_justPressed(BTN_B))
		return 0;
	if (PAD_justRepeated(BTN_UP) || PAD_justRepeated(BTN_DOWN) || PAD_justRepeated(BTN_LEFT) ||
		PAD_justRepeated(BTN_RIGHT)) {
		q->sel ^= 1;
		return UI_MODAL_DIRTY;
	}
	return SDL_GetTicks() - q->t0 < (uint32_t)q->ms + 20 ? UI_MODAL_DIRTY : UI_MODAL_CONTINUE;
}

bool BOS_askQuit(SDL_Surface* screen, const char* app) {
	QuitAsk q = {0};
	q.back = SDL_ConvertSurface(screen, screen->format, 0);
	if (q.back) { // the page behind, 60 % black between the bars
		int top = BOS_px(BOS_BAR_H), bh = screen->h - 2 * top;
		SDL_Surface* dim = SDL_CreateRGBSurfaceWithFormat(0, screen->w, bh, 32, SDL_PIXELFORMAT_ARGB8888);
		if (dim) {
			SDL_FillRect(dim, NULL, SDL_MapRGBA(dim->format, 0, 0, 0, 153));
			SDL_SetSurfaceBlendMode(dim, SDL_BLENDMODE_BLEND);
			SDL_BlitSurface(dim, NULL, q.back, &(SDL_Rect){0, top, screen->w, bh});
			SDL_FreeSurface(dim);
		}
	}
	snprintf(q.head, sizeof(q.head), I18N_text("Leave %s?"), I18N_text(app));
	q.t0 = SDL_GetTicks();
	q.ms = BOS_motionMs(220);
	BOS_partialCancel();
	BOS_transCancel();
	BOS_sfx(BOS_SFX_OPEN);
	BOS_sfxFlush();
	int r = 0;
	if (q.back) {
		UI_ModalOpts o = {screen, quitRender, quitHandle, &q, NULL, 0, false, true};
		r = UI_modalLoop(&o);
		SDL_FreeSurface(q.back);
	} else
		r = 1; // no memory for the picture: leave as before
	return r == 1;
}

// ---- strings ----
// "Music Player | Files" -> "Files": the part after the last " | " (the old title put the app first)
static const char* lastPart(const char* t) {
	if (!t)
		return "";
	const char* p = t;
	for (const char* q = strstr(t, " | "); q; q = strstr(q + 3, " | "))
		p = q + 3;
	return p;
}

// old "BUTTON","LABEL" pairs -> the kit's hints: A B X Y on the right in that order, the rest on the left
static int pairsToHints(char** pairs, BosHint* right, int* nright, BosHint* left, int* nleft) {
	*nright = *nleft = 0;
	static const char* order[] = {"A", "B", "X", "Y"};
	for (int o = 0; o < 4; o++)
		for (int i = 0; pairs && pairs[i] && pairs[i + 1]; i += 2)
			if (!strcmp(pairs[i], order[o]) && *nright < 4)
				right[(*nright)++] = (BosHint){pairs[i], I18N_text(pairs[i + 1])};
	for (int i = 0; pairs && pairs[i] && pairs[i + 1]; i += 2) {
		bool face = false;
		for (int o = 0; o < 4; o++)
			face |= !strcmp(pairs[i], order[o]);
		if (!face && *nleft < 2) {
			const char* b = pairs[i];
			b = !strcmp(b, "LEFT/RIGHT") ? "\xE2\x86\x90\xE2\x86\x92" : !strcmp(b, "UP/DOWN") ? "\xE2\x86\x91\xE2\x86\x93" : b;
			left[(*nleft)++] = (BosHint){b, I18N_text(pairs[i + 1])};
		}
	}
	return *nright + *nleft;
}

// ---- menu bar / hint bar ----
static char last_bar_title[128];
int BOS_uiMenuBar(SDL_Surface* s, const char* title) {
	snprintf(last_bar_title, sizeof(last_bar_title), "%s", I18N_text(lastPart(title)));
	BOS_statusBar(s, I18N_text(lastPart(title)));
	return BOS_px(BOS_BAR_H);
}

int BOS_uiHintBar(SDL_Surface* s, char** pairs) {
	BosHint r[4], l[2];
	int nr, nl;
	pairsToHints(pairs, r, &nr, l, &nl);
	BOS_hintBar(s, r, nr, l, nl);
	return s->w;
}

// ---- a card in the middle: icon (optional), title, text ----
static uint32_t mixc(uint32_t a, uint32_t b, int t) {
	uint32_t o = 0;
	for (int sh = 0; sh < 24; sh += 8)
		o |= (uint32_t)(((((a >> sh) & 255) * (256 - t)) + (((b >> sh) & 255) * t)) >> 8) << sh;
	return o;
}

static void card(SDL_Surface* s, const char* icon, uint32_t icon_bg, const char* title, const char* text, int max_lines) {
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	int w = BOS_px(660), pad = BOS_px(40), inner = w - 2 * pad, circ = BOS_px(88);
	int h1 = title && title[0] ? BOS_textLines(title, 40, 700, inner, 3) * BOS_lineH(40) : 0;
	int h2 = text && text[0] ? BOS_textLines(text, 30, 400, inner, max_lines) * BOS_lineH(30) : 0;
	int h = pad + (icon ? circ + BOS_px(22) : 0) + h1 + (h1 && h2 ? BOS_px(12) : 0) + h2 + pad;
	int x = (s->w - w) / 2, y = top + (bottom - top) * 47 / 100 - h / 2;
	if (y < top + BOS_px(12))
		y = top + BOS_px(12);
	BOS_rrect(s, x, y, w, h, BOS_px(28), BOS_theme()->surface);
	int ty = y + pad;
	if (icon) {
		int ic = BOS_px(50), cx = s->w / 2;
		BOS_rrect(s, cx - circ / 2, ty, circ, circ, circ / 2, icon_bg);
		BOS_icon(s, icon, BOS_textOn(icon_bg, 0), 1, cx - ic / 2, ty + (circ - ic) / 2, ic);
		ty += circ + BOS_px(22);
	}
	if (h1) {
		BOS_textWrapCentre(s, title, 40, 700, BOS_C_TEXT, 1, x + pad, ty, inner, 3);
		ty += h1 + BOS_px(12);
	}
	if (h2)
		BOS_textWrapCentre(s, text, 30, 400, BOS_C_TEXT2, 1, x + pad, ty, inner, max_lines);
}

// the sign a question gets from its words (English, before translation)
static const char* signFor(const char* en, uint32_t* bg) {
	if (en && (strstr(en, "Delete") || strstr(en, "Remove") || strstr(en, "Clear") || strstr(en, "Unsubscribe"))) {
		*bg = BOS_C_DANGER;
		return "trash";
	}
	if (en && (strstr(en, "ail") || strstr(en, "rror") || strstr(en, "nable") || strstr(en, "ot available"))) {
		*bg = BOS_C_WARN;
		return "warn";
	}
	return NULL;
}

void BOS_resultCard(SDL_Surface* s, const char* icon, uint32_t colour, const char* title, const char* text) {
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	SDL_FillRect(s, &(SDL_Rect){0, top, s->w, bottom - top}, BOS_map(s, BOS_theme()->bg));
	card(s, icon, colour, title ? I18N_text(title) : NULL, text ? I18N_text(text) : NULL, 4);
}

void BOS_uiConfirm(SDL_Surface* s, const char* title, const char* subtitle, char** pairs) {
	static char* dflt[] = {"B", "CANCEL", "A", "CONFIRM", NULL};
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_statusBar(s, "");
	uint32_t bg = 0;
	const char* ic = signFor(title, &bg);
	card(s, ic, bg, I18N_text(title), subtitle ? I18N_text(subtitle) : NULL, 6);
	BOS_uiHintBar(s, pairs ? pairs : dflt);
}

void BOS_uiLoading(SDL_Surface* s, const char* title, const char* subtitle) {
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_statusBar(s, "");
	card(s, NULL, 0, title ? I18N_text(title) : NULL, subtitle ? I18N_text(subtitle) : NULL, 3);
}

void BOS_uiMessage(SDL_Surface* s, const char* message) {
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_statusBar(s, "");
	uint32_t bg = 0;
	const char* ic = signFor(message, &bg);
	card(s, ic, bg, I18N_text(message), NULL, 1);
}

void BOS_uiSplash(SDL_Surface* s, const char* title) {
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	const char* t = I18N_text(title);
	BOS_text(s, t, 44, 700, BOS_C_TEXT, 1, (s->w - BOS_textW(t, 44, 700)) / 2, s->h / 2 - BOS_lineH(44));
	const char* l = I18N_text("Loading...");
	BOS_text(s, l, 30, 400, BOS_C_TEXT2, 1, (s->w - BOS_textW(l, 30, 400)) / 2, s->h / 2 + BOS_px(8));
}

// ---- a long job (DESIGN 7.9, board v2-ToolsProgress): squircle 112 (radius 30) with its icon 64, the status 44/700,
// the sub 30 text2, the bar 720x16, then the detail 28 text2 on the left and the percent 28/600 on the right ----
void BOS_uiProgress(SDL_Surface* s, const UIDownloadProgress* info) {
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	SDL_FillRect(s, &(SDL_Rect){0, top, s->w, bottom - top}, BOS_map(s, BOS_theme()->bg));
	const char* st = info->status && info->status[0] ? I18N_text(info->status) : NULL;
	const char* sb = info->sub && info->sub[0] ? I18N_text(info->sub) : NULL;
	int w = BOS_px(720), x = (s->w - w) / 2, sq = BOS_px(112), cx = s->w / 2;
	int h = sq + (st ? BOS_px(26) + BOS_lineH(44) : 0) + (sb ? BOS_px(8) + BOS_lineH(30) : 0) +
			(info->show_bar ? BOS_px(34) + BOS_px(16) + BOS_px(12) + BOS_lineH(28) : 0);
	int y = top + (bottom - top) * 46 / 100 - h / 2;
	uint32_t c = info->colour ? info->colour : BOS_theme()->primary;
	BOS_rrect(s, cx - sq / 2, y, sq, sq, BOS_px(30), c);
	int ic = BOS_px(64);
	BOS_icon(s, info->icon ? info->icon : "download", BOS_textOn(c, 1), 1, cx - ic / 2, y + (sq - ic) / 2, ic);
	y += sq;
	char t[256];
	if (st) {
		y += BOS_px(26);
		BOS_fit(st, 44, 700, w, t, sizeof(t));
		BOS_text(s, t, 44, 700, BOS_C_TEXT, 1, cx - BOS_textW(t, 44, 700) / 2, y);
		y += BOS_lineH(44);
	}
	if (sb) {
		y += BOS_px(8);
		BOS_fit(sb, 30, 400, w, t, sizeof(t));
		BOS_text(s, t, 30, 400, BOS_C_TEXT2, 1, cx - BOS_textW(t, 30, 400) / 2, y);
		y += BOS_lineH(30);
	}
	if (info->show_bar) {
		int p = info->progress < 0 ? 0 : (info->progress > 100 ? 100 : info->progress), bh = BOS_px(16);
		y += BOS_px(34);
		BOS_rrect(s, x, y, w, bh, bh / 2, BOS_theme()->surface2);
		int fw = w * p / 100;
		if (fw > 0)
			BOS_rrect(s, x, y, fw < bh ? bh : fw, bh, bh / 2, BOS_theme()->primary);
		y += bh + BOS_px(12);
		char pc[16];
		snprintf(pc, sizeof(pc), "%d%%", p);
		int pw = BOS_textW(pc, 28, 600);
		BOS_text(s, pc, 28, 600, BOS_C_TEXT, 1, x + w - pw, y);
		if (info->detail && info->detail[0]) {
			BOS_fit(I18N_text(info->detail), 28, 400, w - pw - BOS_px(24), t, sizeof(t));
			BOS_text(s, t, 28, 400, BOS_C_TEXT2, 1, x, y);
		}
	}
}

// ---- empty page: a big icon card, what is missing, what to do ----
void BOS_uiEmpty(SDL_Surface* s, const char* message, const char* subtitle, char** pairs) {
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	SDL_FillRect(s, &(SDL_Rect){0, top, s->w, bottom - top}, BOS_map(s, BOS_theme()->bg));
	const char* m = message ? I18N_text(message) : "";
	const char* sub = subtitle ? I18N_text(subtitle) : NULL;
	int cardw = BOS_px(140), ic = BOS_px(76), cx = s->w / 2, tw = s->w - BOS_px(160);
	int sh = sub ? BOS_textLines(sub, 30, 400, tw, 3) * BOS_lineH(30) : 0;
	int h = cardw + BOS_px(28) + BOS_lineH(44) + (sh ? BOS_px(12) + sh : 0), y = top + (bottom - top - h) / 2 - BOS_px(20);
	BOS_rrect(s, cx - cardw / 2, y, cardw, cardw, BOS_px(36), BOS_theme()->surface);
	BOS_icon(s, "info", BOS_C_TEXT2, 1, cx - ic / 2, y + (cardw - ic) / 2, ic);
	y += cardw + BOS_px(28);
	BOS_textWrapCentre(s, m, 44, 700, BOS_C_TEXT, 1, BOS_px(80), y, tw, 1);
	if (sh)
		BOS_textWrapCentre(s, sub, 30, 400, BOS_C_TEXT2, 1, BOS_px(80), y + BOS_lineH(44) + BOS_px(12), tw, 3);
	BOS_uiHintBar(s, pairs);
}

// ---- toast: a pill above the hint bar, drawn onto its own surface (the caller puts it on its layer) ----
SDL_Surface* BOS_uiToast(const char* message, int screen_w, int screen_h, int* x, int* y) {
	const char* m = I18N_text(message);
	int h = BOS_px(76), tw = BOS_textW(m, 32, 600), w = tw + BOS_px(64);
	if (w > screen_w - BOS_px(48))
		w = screen_w - BOS_px(48);
	SDL_Surface* p = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!p)
		return NULL;
	SDL_FillRect(p, NULL, 0);
	BOS_rrect(p, 0, 0, w, h, h / 2, BOS_theme()->surface2);
	SDL_SetSurfaceBlendMode(p, SDL_BLENDMODE_BLEND);
	char t[256];
	BOS_fit(m, 32, 600, w - BOS_px(64), t, sizeof(t));
	BOS_textMid(p, t, 32, 600, BOS_C_TEXT, 1, (w - BOS_textW(t, 32, 600)) / 2, h / 2);
	*x = (screen_w - w) / 2;
	*y = screen_h - BOS_px(BOS_BAR_H) - BOS_px(24) - h;
	return p;
}

// ---- the MENU popup as a bottom sheet of tiles (board v2-MusicSheet): two columns, an icon per known action ----
typedef struct {
	const char* word;
	const char* icon;
	uint32_t colour;
} ActIcon;
static const ActIcon acts[] = {
	{"Add Folder", "folder", 0xFF375F}, {"Add to Playlist", "plus", 0xFF375F}, {"Play Next", "play", 0x30D158},
	{"Play All", "play", 0x30D158}, {"Play from Start", "update", 0x30D158}, {"Watch on", "play", 0x30D158}, {"Start over", "update", 0x64D2FF}, {"Resume", "play", 0x30D158},
	{"Rename", "pencil", 0xB4B4BA}, {"Delete", "trash", BOS_C_DANGER}, {"Remove", "trash", BOS_C_DANGER},
	{"Unsubscribe", "trash", BOS_C_DANGER}, {"Clear", "trash", BOS_C_DANGER}, {"Stop", "power", BOS_C_DANGER},
	{"Quit", "power", BOS_C_DANGER}, {"Refresh", "sync", 0x64D2FF}, {"Manage", "settings", 0x8E8E93},
	{"Mark", "check", 0x30D158}, {"Download", "upload", 0x30D158}, {"Cancel", "trash", BOS_C_DANGER},
	{"Lyrics", "lyrics", 0x64D2FF}, {"Visualizer", "wave", 0xBF5AF2}, {"Screen Off", "moon", 0xFFD60A},
	{"Background", "home", 0x30D158}, {"Playback Controls", "pad", 0x8E8E93}, {"Search", "info", 0x0A84FF},
	{"Browse", "folder", 0x0A84FF}, {"Settings", "settings", 0x8E8E93}, {NULL, NULL, 0}};

static const ActIcon* actFor(const char* label) {
	for (int i = 0; acts[i].word; i++)
		if (strstr(label, acts[i].word))
			return &acts[i];
	return NULL;
}

int BOS_uiSheetCols(int n) {
	return n >= 2 ? 2 : 1;
}

void BOS_uiSheet(SDL_Surface* s, const char* const* labels, int n, int sel) {
	const BosTheme* th = BOS_theme();
	SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 0, 0, 0, 153));
	int cols = BOS_uiSheetCols(n), rows = (n + cols - 1) / cols;
	int tile = BOS_px(88), gapx = BOS_px(16), gapy = BOS_px(14), padx = BOS_px(40);
	int h = BOS_px(32) + rows * tile + (rows - 1) * gapy + BOS_px(36);
	int bottom = s->h - BOS_px(BOS_BAR_H), y0 = bottom - h;
	BOS_rrect(s, 0, y0, s->w, h + BOS_px(40), BOS_px(32), th->surface);
	SDL_FillRect(s, &(SDL_Rect){0, bottom, s->w, s->h - bottom}, SDL_MapRGBA(s->format, 0, 0, 0, 255));
	int tw = (s->w - 2 * padx - (cols - 1) * gapx) / cols;
	for (int i = 0; i < n; i++) {
		int c = i % cols, r = i / cols, x = padx + c * (tw + gapx), y = y0 + BOS_px(32) + r * (tile + gapy);
		bool hot = i == sel;
		const ActIcon* a = actFor(labels[i]);
		bool danger = a && a->colour == BOS_C_DANGER;
		if (hot) {
			int gx = (int)(tw * BOS_ROW_SCALE) - tw, gy = (int)(tile * BOS_ROW_SCALE) - tile;
			BOS_rrect(s, x - gx / 2, y - gy / 2, tw + gx, tile + gy, BOS_px(22), th->focus);
		} else
			BOS_rrect(s, x, y, tw, tile, BOS_px(22), th->surface2);
		int tx = x + BOS_px(24), ic = BOS_px(40);
		if (a) {
			BOS_icon(s, a->icon, hot ? th->onfocus : a->colour, 1, tx, y + (tile - ic) / 2, ic);
			tx += ic + BOS_px(18);
		}
		char t[128];
		const char* l = I18N_text(labels[i]);
		BOS_fit(l, 30, 600, x + tw - BOS_px(20) - tx, t, sizeof(t));
		BOS_textMid(s, t, 30, 600, hot ? th->onfocus : danger ? BOS_C_DANGER : BOS_C_TEXT, 1, tx, y + tile / 2);
	}
	BosHint r[2] = {{"A", I18N_text("Select")}, {"B", I18N_text("Close")}};
	BOS_hintBar(s, r, 2, NULL, 0);
}

// ---- controls help: a group of key -> action rows ----
void BOS_uiControls(SDL_Surface* s, const char* title, const char* const* keys, const char* const* acts2, int n) {
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_statusBar(s, I18N_text(lastPart(title)));
	int y = BOS_px(BOS_BAR_H) + BOS_largeTitle(s, BOS_px(BOS_BAR_H), I18N_text("Controls"), NULL);
	int gx = BOS_px(40), gw = s->w - 2 * gx, rh = BOS_px(64);
	int fit = (s->h - BOS_px(BOS_BAR_H) - BOS_px(12) - y) / rh;
	if (n > fit)
		n = fit;
	BOS_rrect(s, gx, y, gw, n * rh, BOS_px(24), BOS_theme()->surface);
	for (int i = 0; i < n; i++) {
		int cy = y + i * rh + rh / 2;
		BOS_textMid(s, I18N_text(keys[i]), 30, 600, BOS_C_TEXT, 1, gx + BOS_px(28), cy);
		const char* a = I18N_text(acts2[i]);
		BOS_textMid(s, a, 30, 400, BOS_C_TEXT2, 1, gx + gw - BOS_px(24) - BOS_textW(a, 30, 400), cy);
		if (i + 1 < n)
			SDL_FillRect(s, &(SDL_Rect){gx + BOS_px(28), y + (i + 1) * rh - 1, gw - BOS_px(28), 1}, BOS_map(s, BOS_C_SEPARATOR));
	}
	BosHint r[1] = {{"B", I18N_text("Back")}};
	BOS_hintBar(s, r, 1, NULL, 0);
}

// ---- the shared settings page (UI_renderSettingsPage) as Settings' rows (DESIGN.md 6.6; board v2-MusicSettings) ----
static BosGlide sp_pill, sp_scroll;
typedef struct {
	UISettingsItem* items;
	int sel;
} SpCtx;

static void spRow(void* ctx, int i, bool hot, ListViewRow* r) {
	SpCtx* c = ctx;
	UISettingsItem* it = &c->items[i];
	r->label = I18N_text(it->label ? it->label : "");
	static char val[4][160];
	if (it->value && it->value[0]) {
		const char* v = I18N_text(it->value);
		if (hot && it->cycleable)
			snprintf(val[i & 3], sizeof(val[0]), "\xE2\x80\xB9 %s \xE2\x80\xBA", v); // ‹ value ›
		else
			snprintf(val[i & 3], sizeof(val[0]), "%s", v);
		r->annotation = val[i & 3];
	} else if (!it->cycleable && !it->custom_draw)
		r->chevron = false;
	if (it->swatch >= 0)
		r->sq_text = NULL, r->right_icon = NULL;
}

void BOS_uiSettingsPage(SDL_Surface* s, UISettingsItem* items, int count, int selected, int* scroll, const char* status_msg) {
	const BosTheme* th = BOS_theme();
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	SDL_FillRect(s, &(SDL_Rect){0, top, s->w, bottom - top}, BOS_map(s, th->bg));
	int y = top + (last_bar_title[0] ? BOS_largeTitle(s, top, last_bar_title, NULL) : BOS_px(8));
	const char* desc = selected >= 0 && selected < count && items[selected].desc ? I18N_text(items[selected].desc) : NULL;
	int foot = BOS_px(12) + 2 * BOS_lineH(26) + (status_msg ? BOS_lineH(26) : 0);
	SDL_Rect area = {BOS_px(40), y, s->w - BOS_px(80), bottom - foot - y};
	SpCtx c = {items, selected};
	int vis = BOS_groupRows(s, &area, BOS_px(96), count, selected, true, scroll, &sp_pill, &sp_scroll, false, spRow, &c);
	// swatches and custom values on top of their rows
	for (int i = scroll ? *scroll : 0, k = 0; i < count && k < vis; i++, k++) {
		int ry = y + k * BOS_px(96), right = area.x + area.w - BOS_px(24);
		if (items[i].swatch >= 0) {
			int d = BOS_px(30);
			uint32_t c2 = (uint32_t)items[i].swatch & 0xFFFFFF;
			BOS_rrect(s, right - d, ry + (BOS_px(96) - d) / 2, d, d, d / 2, c2);
		} else if (items[i].custom_draw)
			items[i].custom_draw(s, items[i].custom_draw_ctx, area.x + area.w / 2, ry, area.w / 2 - BOS_px(24), BOS_px(96),
								 i == selected);
	}
	int fy = y + vis * BOS_px(96) + BOS_px(12);
	if (desc && desc[0])
		fy += BOS_textWrap(s, desc, 26, 400, BOS_C_TEXTOFF, 1, area.x + BOS_px(28), fy, area.w - BOS_px(28), 2);
	if (status_msg && status_msg[0])
		BOS_textWrap(s, I18N_text(status_msg), 26, 600, BOS_C_TEXT2, 1, area.x + BOS_px(28), fy, area.w - BOS_px(28), 1);
}

bool BOS_uiSettingsMoving(void) {
	return BOS_glideMoving(&sp_pill) || BOS_glideMoving(&sp_scroll);
}

// ---- the shared list dialog (ListDialog: add to playlist …) as a page with a group (board v2-MusicAddTo) ----
static BosGlide ld_pill, ld_scroll;
typedef struct {
	ListDialogItem* items;
} LdCtx;
static void ldRow(void* ctx, int i, bool hot, ListViewRow* r) {
	LdCtx* c = ctx;
	(void)hot;
	const char* t = c->items[i].text;
	r->label = I18N_text(strncmp(t, "+ ", 2) == 0 ? t + 2 : t);
	if (c->items[i].detail[0])
		r->annotation = I18N_text(c->items[i].detail);
	if (strncmp(t, "New ", 4) == 0 || strncmp(t, "+ ", 2) == 0) { // the "new" row: a plus, in primary
		r->sq_icon = "plus", r->sq_color = BOS_theme()->surface2, r->sq_raw = true, r->sq_icon_colour = BOS_theme()->primary;
		r->accent = true;
	} else
		r->sq_icon = "list", r->sq_color = 0xFF375F;
}
void BOS_uiListDialog(SDL_Surface* s, const char* title, ListDialogItem* items, int count, int sel, int* scroll) {
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	const char* t = I18N_text(lastPart(title));
	BOS_statusBar(s, t);
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	int y = top + BOS_largeTitle(s, top, t, NULL);
	BosHint hr[2] = {{"A", I18N_text("Select")}, {"B", I18N_text("Back")}};
	if (count > 0) {
		SDL_Rect a = {BOS_px(40), y, s->w - BOS_px(80), bottom - BOS_px(12) - y};
		LdCtx c = {items};
		BOS_groupRows(s, &a, BOS_px(96), count, sel, true, scroll, &ld_pill, &ld_scroll, false, ldRow, &c);
	}
	BOS_hintBar(s, count > 0 ? hr : hr + 1, count > 0 ? 2 : 1, NULL, 0);
}
bool BOS_uiListDialogMoving(void) {
	return BOS_glideMoving(&ld_pill) || BOS_glideMoving(&ld_scroll) || BOS_marqueeActive();
}

// ---- the keyboard (DESIGN.md 7.6, boards v2-KeyboardThai / -English) ----
// The status bar; the label 26 text2, the field 80 (radius 20, surface, text 36, a primary caret); the key panel
// (#121214, a surface line on top) above the hint bar: keys 62 high, radius 14, surface2, gap 8, rows gap 10; the
// focused key in the focus colour x1.08; ⇧ ⌫ ก/A and the words 24/600; the confirm key primary.
static int kb_mark_shift(const char* k, int px) { // a lone Thai mark: pulled to the key's centre
	const unsigned char* u = (const unsigned char*)k;
	if (strlen(k) != 3 || u[0] != 0xE0)
		return 0;
	int lh = BOS_lineH(px);
	if (u[1] == 0xB8 && (u[2] == 0xB8 || u[2] == 0xB9 || u[2] == 0xBA))
		return -lh * 28 / 100; // below: ุ ู ฺ
	if (u[1] == 0xB8 && (u[2] == 0xB1 || (u[2] >= 0xB4 && u[2] <= 0xB7)))
		return lh * 30 / 100; // above: ั ิ ี ึ ื
	if (u[1] == 0xB9 && u[2] >= 0x87 && u[2] <= 0x8D)
		return lh * 30 / 100; // ็ ่ ้ ๊ ๋ ์ ํ
	return 0;
}

void BOS_uiKeyboard(SDL_Surface* s, const UIKeyboardView* v) {
	const BosTheme* th = BOS_theme();
	SDL_FillRect(s, NULL, BOS_map(s, th->bg));
	BOS_statusBar(s, NULL);
	int x0 = BOS_px(40), y = BOS_px(68);
	if (v->prompt && v->prompt[0]) {
		char p[256];
		BOS_fit(I18N_text(v->prompt), 26, 400, s->w - 2 * x0, p, sizeof(p));
		BOS_text(s, p, 26, 400, BOS_C_TEXT2, 1, x0, y);
	}
	y += BOS_lineH(26) + BOS_px(8);
	int fw = s->w - 2 * x0, fh = BOS_px(80);
	BOS_rrect(s, x0, y, fw, fh, BOS_px(20), th->surface);
	// the field: the part that fits, keeping the caret in view
	{
		const char* in = v->input;
		int n = (int)strlen(in), caret = v->caret, avail = fw - BOS_px(48), start = 0;
		char buf[512];
		for (;;) {
			snprintf(buf, sizeof(buf), "%.*s", caret - start, in + start);
			if (start >= caret || BOS_textW(buf, 36, 500) <= avail)
				break;
			start++;
			while (start < n && ((unsigned char)in[start] & 0xC0) == 0x80)
				start++;
		}
		int end = start;
		while (end < n) {
			int e = end + 1;
			while (e < n && ((unsigned char)in[e] & 0xC0) == 0x80)
				e++;
			snprintf(buf, sizeof(buf), "%.*s", e - start, in + start);
			if (BOS_textW(buf, 36, 500) > avail)
				break;
			end = e;
		}
		int tx = x0 + BOS_px(24), cy = y + fh / 2;
		snprintf(buf, sizeof(buf), "%.*s", end - start, in + start);
		if (buf[0])
			BOS_textMid(s, buf, 36, 500, BOS_C_TEXT, 1, tx, cy);
		snprintf(buf, sizeof(buf), "%.*s", caret - start, in + start);
		int cx = tx + (buf[0] ? BOS_textW(buf, 36, 500) : 0) + BOS_px(2);
		SDL_FillRect(s, &(SDL_Rect){cx, cy - BOS_px(22), BOS_px(3), BOS_px(44)}, BOS_map(s, th->primary));
	}
	// the panel
	int keyh = BOS_px(62), gap = BOS_px(8), rgap = BOS_px(10), bottom = s->h - BOS_px(BOS_BAR_H);
	int panel_h = BOS_px(16) * 2 + UIKB_ROWS * keyh + (UIKB_ROWS - 1) * rgap, py = bottom - panel_h;
	SDL_FillRect(s, &(SDL_Rect){0, py, s->w, panel_h}, BOS_map(s, 0x121214));
	SDL_FillRect(s, &(SDL_Rect){0, py, s->w, BOS_px(1) > 0 ? BOS_px(1) : 1}, BOS_map(s, th->surface));
	int inner = s->w - BOS_px(48);
	float unit = (inner - (v->units - 1) * gap) / (float)v->units;
	for (int pass = 0; pass < 2; pass++) // the focused key last, over its neighbours
		for (int r = 0; r < UIKB_ROWS; r++) {
			int ky = py + BOS_px(16) + r * (keyh + rgap);
			float x = BOS_px(24);
			for (int c = 0; c < UIKB_COLS && v->keys[r][c]; c++) {
				int sp = v->span[r][c];
				int kw = (int)(unit * sp + gap * (sp - 1));
				bool foc = r == v->row && c == v->col;
				if (foc != (pass == 1)) {
					x += kw + gap;
					continue;
				}
				const char* k = v->keys[r][c];
				bool go = strcmp(k, UIKB_GO) == 0, word = false;
				int kx = (int)x, kyy = ky, w = kw, h = keyh;
				if (foc) {
					kx -= kw * 4 / 100;
					kyy -= keyh * 4 / 100;
					w = kw * 108 / 100;
					h = keyh * 108 / 100;
				}
				bool caps = strcmp(k, UIKB_SHIFT) == 0 && v->shift == 2; // caps lock: the shift key filled
				BOS_rrect(s, kx, kyy, w, h, BOS_px(14), foc ? th->focus : (go || caps) ? th->primary : th->surface2);
				uint32_t tc = foc ? th->onfocus : (go || caps) ? th->onprimary : BOS_C_TEXT;
				const char* label = k;
				if (go)
					label = I18N_text(v->search ? "Search" : "OK"), word = true;
				else if (strcmp(k, UIKB_SPACE) == 0)
					label = I18N_text("Space"), word = true;
				else if (strcmp(k, UIKB_SYM) == 0)
					label = v->sym ? "abc" : "#+=", word = true;
				else if (strcmp(k, UIKB_LANG) == 0)
					word = true;
				if (strcmp(k, UIKB_SHIFT) == 0 && v->shift == 1 && !foc)
					tc = th->primary;
				int px = word ? 24 : 32, wt = word ? 600 : 400;
				int lw = BOS_textW(label, px, wt), dy = word ? 0 : kb_mark_shift(label, px);
				BOS_textMid(s, label, px, wt, tc, 1, kx + (w - (dy && lw < BOS_px(4) ? 0 : lw)) / 2, kyy + h / 2 + dy);
				x += kw + gap;
			}
		}
	BosHint right[4] = {{"A", I18N_text("Type")}, {"B", I18N_text("Delete")}, {"X", I18N_text("Space")},
						{"Y", UIKB_LANG}};
	BosHint left[1] = {{"START", I18N_text(v->search ? "Search" : "OK")}};
	BOS_hintBar(s, right, 4, left, 1);
}
