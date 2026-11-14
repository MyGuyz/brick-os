// The Video app's own Brick OS pages (DESIGN.md 7.3; boards design/round2/v2-Video*).
#include "vid_bos.h"
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "i18n.h"

// ---- a video's picture: <name>.jpg / .png beside it, loaded once (a few kept) ----
#define THUMB_SLOTS 12
static struct {
	char path[512];
	SDL_Surface* pic;
	uint32_t used;
	bool tried;
} thumbs[THUMB_SLOTS];
static uint32_t thumb_clock;

SDL_Surface* VidBos_thumb(const char* video) {
	if (!video || !video[0])
		return NULL;
	thumb_clock++;
	int old = 0;
	for (int i = 0; i < THUMB_SLOTS; i++) {
		if (thumbs[i].tried && strcmp(thumbs[i].path, video) == 0) {
			thumbs[i].used = thumb_clock;
			return thumbs[i].pic;
		}
		if (thumbs[i].used < thumbs[old].used)
			old = i;
	}
	if (thumbs[old].pic) {
		BOS_picForget(thumbs[old].pic);
		SDL_FreeSurface(thumbs[old].pic);
	}
	thumbs[old].pic = NULL;
	snprintf(thumbs[old].path, sizeof(thumbs[old].path), "%s", video);
	thumbs[old].tried = true;
	thumbs[old].used = thumb_clock;
	char base[512], p[600];
	snprintf(base, sizeof(base), "%s", video);
	char* dot = strrchr(base, '.');
	char* slash = strrchr(base, '/');
	if (dot && (!slash || dot > slash))
		*dot = '\0';
	static const char* ext[] = {".jpg", ".png", ".jpeg", "-poster.jpg", NULL};
	for (int i = 0; ext[i] && !thumbs[old].pic; i++) {
		snprintf(p, sizeof(p), "%s%s", base, ext[i]);
		if (access(p, R_OK) == 0)
			thumbs[old].pic = IMG_Load(p);
	}
	return thumbs[old].pic;
}

// ---- the home (board v2-Video): ดูต่อ, then the two places ----
static BosGlide hx, hy, hw2, hh2;
static void filmCard(SDL_Surface* s, int x, int y, int w, int h, int r) {
	BOS_rrect(s, x, y, w, h, r, BOS_theme()->surface2);
	int ic = h / 2;
	BOS_icon(s, "film", BOS_C_TEXT2, 1, x + (w - ic) / 2, y + (h - ic) / 2, ic);
}

void VidBos_home(SDL_Surface* s, const VidHomeView* v) {
	const BosTheme* th = BOS_theme();
	SDL_FillRect(s, NULL, BOS_map(s, th->bg));
	BOS_statusBar(s, I18N_text("Video"));
	int top = BOS_px(BOS_BAR_H), x0 = BOS_px(40), y = top + BOS_px(4);
	int cw = BOS_px(300), ch = BOS_px(169), gap = BOS_px(24);
	SDL_Rect focus = {0};
	int tiles_y;
	if (v->ncards > 0) {
		BOS_text(s, I18N_text("Continue watching"), 30, 600, BOS_C_TEXT2, 1, x0, y);
		y += BOS_lineH(30) + BOS_px(14);
		// the row scrolls so the focused card shows (3 fit)
		int first = v->sel < v->ncards && v->sel > 2 ? v->sel - 2 : 0;
		for (int i = first; i < v->ncards && i < first + 3; i++) {
			const VidCard* c = &v->cards[i];
			int x = x0 + (i - first) * (cw + gap);
			bool hot = i == v->sel;
			if (hot)
				focus = (SDL_Rect){x, y, cw, ch};
			SDL_Surface* raw = VidBos_thumb(c->path);
			SDL_Surface* p = raw ? BOS_picRounded(raw, cw, ch, BOS_px(18)) : NULL;
			if (hot) // the ring (§5.3)
				BOS_rrect(s, x - BOS_px(6), y - BOS_px(6), cw + BOS_px(12), ch + BOS_px(12), BOS_px(22), th->focus);
			if (p)
				BOS_blit(p, s, x, y, 1);
			else
				filmCard(s, x, y, cw, ch, BOS_px(18));
			if (c->progress >= 0) { // the red line of how far
				int bh = BOS_px(6), f = (int)(cw * (c->progress > 1 ? 1 : c->progress));
				SDL_FillRect(s, &(SDL_Rect){x + BOS_px(10), y + ch - bh - BOS_px(4), cw - BOS_px(20), bh}, BOS_map(s, 0x5A5A5E));
				SDL_FillRect(s, &(SDL_Rect){x + BOS_px(10), y + ch - bh - BOS_px(4), (cw - BOS_px(20)) * f / cw, bh},
							 BOS_map(s, BOS_C_DANGER));
			}
			char t[200];
			BOS_fit(c->name, 28, hot ? 600 : 500, cw, t, sizeof(t));
			BOS_text(s, t, 28, hot ? 600 : 500, BOS_C_TEXT, 1, x, y + ch + BOS_px(12));
			BOS_text(s, c->left, 24, 400, BOS_C_TEXT2, 1, x, y + ch + BOS_px(12) + BOS_lineH(28));
		}
		y += ch + BOS_px(12) + BOS_lineH(28) + BOS_lineH(24) + BOS_px(24);
	}
	tiles_y = y + BOS_px(6);
	int tw = (s->w - 2 * x0 - BOS_px(20)) / 2, thh = BOS_px(150);
	const char* names[2] = {I18N_text("Videos on the card"), I18N_text("Online TV")};
	const char* subs[2] = {v->files_sub, v->tv_sub};
	const char* icons[2] = {"folder", "tv"};
	uint32_t cols[2] = {0x0A84FF, 0xFF453A};
	int nt = v->tv ? 2 : 1;
	for (int i = 0; i < nt; i++) {
		int x = x0 + i * (tw + BOS_px(20));
		bool hot = v->sel == v->ncards + i;
		if (hot)
			focus = (SDL_Rect){x, tiles_y, tw, thh};
		else
			BOS_rrect(s, x, tiles_y, tw, thh, BOS_px(24), th->surface);
	}
	// the focus glides between the cards' ring and the tiles
	float fx = BOS_glideTo(&hx, (float)focus.x, 120, false), fy = BOS_glideTo(&hy, (float)focus.y, 120, false);
	float fw = BOS_glideTo(&hw2, (float)focus.w, 120, false), fh = BOS_glideTo(&hh2, (float)focus.h, 120, false);
	if (v->sel >= v->ncards) {
		int gx = (int)(fw * 0.02f), gy = (int)(fh * 0.02f);
		BOS_rrect(s, (int)fx - gx, (int)fy - gy, (int)fw + 2 * gx, (int)fh + 2 * gy, BOS_px(24), th->focus);
	}
	for (int i = 0; i < nt; i++) {
		int x = x0 + i * (tw + BOS_px(20)), cy = tiles_y + thh / 2;
		bool hot = v->sel == v->ncards + i;
		int sq = BOS_px(72), ic = BOS_px(44);
		BOS_rrect(s, x + BOS_px(24), cy - sq / 2, sq, sq, sq * 26 / 100, BOS_iconColor(cols[i]));
		BOS_icon(s, icons[i], 0xFFFFFF, 1, x + BOS_px(24) + (sq - ic) / 2, cy - ic / 2, ic);
		int tx = x + BOS_px(24) + sq + BOS_px(22);
		char t[160];
		BOS_fit(names[i], 36, 600, x + tw - BOS_px(16) - tx, t, sizeof(t));
		BOS_text(s, t, 36, 600, hot ? th->onfocus : BOS_C_TEXT, 1, tx, cy - BOS_lineH(36) + BOS_px(4));
		BOS_fit(subs[i], 26, 400, x + tw - BOS_px(16) - tx, t, sizeof(t));
		BOS_text(s, t, 26, 400, hot ? 0x3A3A3C : BOS_C_TEXT2, 1, tx, cy + BOS_px(4));
	}
	BOS_hintBar(s, v->hints, v->nhints, v->left, v->nleft);
}

bool VidBos_homeMoving(void) {
	return BOS_glideMoving(&hx) || BOS_glideMoving(&hy) || BOS_glideMoving(&hw2) || BOS_glideMoving(&hh2);
}
