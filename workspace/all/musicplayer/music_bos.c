// The Music app's own Brick OS screens (DESIGN.md 7.2; boards design/round2/v2-Music*).
#include "music_bos.h"
#include <stdio.h>
#include <string.h>
#include "i18n.h"

static BosGlide h_x, h_y, h_w, h_h;

static void bars(SDL_Surface* s, int x, int bottom, uint32_t c, int scale) {
	static const int hh[4] = {12, 24, 16, 20};
	int bw = BOS_px(5) * scale / 100, gap = BOS_px(3) * scale / 100;
	for (int i = 0; i < 4; i++) {
		int h = BOS_px(hh[i]) * scale / 100;
		BOS_rrect(s, x + i * (bw + gap), bottom - h, bw, h, BOS_px(2), c);
	}
}

static void artOrIcon(SDL_Surface* s, SDL_Surface* art, int x, int y, int size, int r) {
	SDL_Surface* p = art ? BOS_picRounded(art, size, size, r) : NULL;
	if (p) {
		BOS_blit(p, s, x, y, 1);
		return;
	}
	BOS_rrect(s, x, y, size, size, r, 0x5E5CE6);
	int ic = size * 9 / 16;
	BOS_icon(s, "music", 0xFFFFFF, 1, x + (size - ic) / 2, y + (size - ic) / 2, ic);
}

void MusicBos_home(SDL_Surface* s, const MusicHomeView* v) {
	const BosTheme* th = BOS_theme();
	SDL_FillRect(s, NULL, BOS_map(s, th->bg));
	BOS_statusBar(s, I18N_text("Music"));
	int top = BOS_px(BOS_BAR_H), gx = BOS_px(40), gw = s->w - 2 * gx, y = top + BOS_px(8);

	// targets for the one focus (the card or a tile), so it glides between them
	SDL_Rect cardr = {gx, y, gw, BOS_px(150)};
	int ty = v->card ? y + cardr.h + BOS_px(22) : y + BOS_px(8);
	int cols = 3, gap = BOS_px(20), tw = (gw - (cols - 1) * gap) / cols, thh = BOS_px(v->card ? 180 : 210);
	SDL_Rect fr = cardr;
	if (v->sel >= 0 && v->sel < v->ntiles)
		fr = (SDL_Rect){gx + (v->sel % cols) * (tw + gap), ty + (v->sel / cols) * (thh + gap), tw, thh};
	float fx = BOS_glideTo(&h_x, (float)fr.x, 120, false), fy = BOS_glideTo(&h_y, (float)fr.y, 120, false);
	float fw = BOS_glideTo(&h_w, (float)fr.w, 120, false), fh = BOS_glideTo(&h_h, (float)fr.h, 120, false);
	int ex = (int)(fw * 0.04f / 2), ey = (int)(fh * 0.04f / 2);

	// ---- the playing card ----
	if (v->card) {
		bool hot = v->sel < 0;
		uint32_t fg = hot ? th->onfocus : BOS_C_TEXT, sub = hot ? 0x3A3A3C : BOS_C_TEXT2;
		if (hot)
			BOS_rrect(s, (int)fx - ex, (int)fy - ey, (int)fw + 2 * ex, (int)fh + 2 * ey, BOS_px(24), th->focus);
		else
			BOS_rrect(s, cardr.x, cardr.y, cardr.w, cardr.h, BOS_px(24), th->surface);
		int a = BOS_px(110), ax = cardr.x + BOS_px(20), cy = cardr.y + cardr.h / 2;
		artOrIcon(s, v->art, ax, cy - a / 2, a, BOS_px(18));
		int tx = ax + a + BOS_px(24), right = cardr.x + cardr.w - BOS_px(28), pi = BOS_px(44);
		BOS_icon(s, v->card_playing ? "pause" : "play", fg, 1, right - pi, cy - pi / 2, pi);
		int room = right - pi - BOS_px(24) - tx, ly = cardr.y + BOS_px(14);
		if (v->card_playing)
			bars(s, tx, ly + BOS_px(26), hot ? th->onfocus : th->primary, 90);
		BOS_text(s, v->card_label ? v->card_label : "", 26, 600, sub, 1, tx + (v->card_playing ? BOS_px(36) : 0), ly);
		char t[256];
		BOS_fit(v->title ? v->title : "", 40, 700, room, t, sizeof(t));
		BOS_text(s, t, 40, 700, fg, 1, tx, ly + BOS_px(28));
		if (v->line2) {
			BOS_fit(v->line2, 28, 400, room, t, sizeof(t));
			BOS_text(s, t, 28, 400, sub, 1, tx, ly + BOS_px(74));
		}
		if (v->progress >= 0) {
			int bw = room, bh = BOS_px(8), by = cardr.y + cardr.h - BOS_px(14);
			BOS_rrect(s, tx, by, bw, bh, bh / 2, hot ? 0xC8C8CE : th->surface2);
			int f = (int)(bw * (v->progress > 1 ? 1 : v->progress));
			if (f > 0)
				BOS_rrect(s, tx, by, f < bh ? bh : f, bh, bh / 2, hot ? th->onfocus : BOS_C_TEXT);
		}
	}

	// ---- the tiles ----
	for (int i = 0; i < v->ntiles; i++) {
		const MusicHomeTile* t = &v->tiles[i];
		bool hot = i == v->sel;
		int x = gx + (i % cols) * (tw + gap), yy = ty + (i / cols) * (thh + gap);
		if (hot)
			BOS_rrect(s, (int)fx - ex, (int)fy - ey, (int)fw + 2 * ex, (int)fh + 2 * ey, BOS_px(24), th->focus);
		else
			BOS_rrect(s, x, yy, tw, thh, BOS_px(24), th->surface);
		int sq = BOS_px(56), ic = BOS_px(36), px = x + BOS_px(24), py = yy + BOS_px(22);
		BOS_rrect(s, px, py, sq, sq, sq * 26 / 100, BOS_iconColor(t->colour));
		BOS_icon(s, t->icon, 0xFFFFFF, 1, px + (sq - ic) / 2, py + (sq - ic) / 2, ic);
		char nm[96];
		BOS_fit(t->name, 34, 600, tw - BOS_px(48), nm, sizeof(nm));
		int ny = yy + thh - BOS_px(22) - BOS_lineH(26) - BOS_lineH(34);
		BOS_text(s, nm, 34, hot ? 700 : 600, hot ? th->onfocus : BOS_C_TEXT, 1, px, ny);
		int sy = ny + BOS_lineH(34);
		if (t->pill && t->pill[0]) {
			int ph = BOS_px(40), pw = BOS_textW(t->pill, 24, 600) + BOS_px(32);
			BOS_rrect(s, px, sy, pw, ph, ph / 2, th->primary);
			BOS_textMid(s, t->pill, 24, 600, th->onprimary, 1, px + BOS_px(16), sy + ph / 2);
		} else if (t->sub[0])
			BOS_text(s, t->sub, 26, 400, hot ? 0x3A3A3C : BOS_C_TEXT2, 1, px, sy);
	}
	BOS_hintBar(s, v->hints, v->nhints, v->left, v->nleft);
}

bool MusicBos_homeMoving(void) {
	return BOS_glideMoving(&h_x) || BOS_glideMoving(&h_y) || BOS_glideMoving(&h_w) || BOS_glideMoving(&h_h);
}

// ---- the playing page (boards v2-MusicPlaying, -Lyrics, -Visualizer) ----
static SDL_Surface* blur_src;
static SDL_Surface* blur_bg;

// the art spread over the screen, soft and at 28 % of its light; made once per picture
static SDL_Surface* blurred(SDL_Surface* art, int w, int h) {
	if (art == blur_src && blur_bg)
		return blur_bg;
	if (blur_bg)
		SDL_FreeSurface(blur_bg);
	blur_bg = NULL;
	blur_src = art;
	if (!art)
		return NULL;
	SDL_Surface* tiny = BOS_picRounded(art, 32, 24, 0); // bilinear down to a few pixels, then up again: a blur
	SDL_Surface* big = tiny ? BOS_picRounded(tiny, w, h, 0) : NULL;
	if (!big)
		return NULL;
	blur_bg = SDL_ConvertSurfaceFormat(big, SDL_PIXELFORMAT_ARGB8888, 0);
	BOS_picForget(tiny);
	if (!blur_bg)
		return NULL;
	for (int y = 0; y < blur_bg->h; y++) {
		uint32_t* p = (uint32_t*)((uint8_t*)blur_bg->pixels + y * blur_bg->pitch);
		for (int x = 0; x < blur_bg->w; x++)
			p[x] = 0xFF000000u | ((((p[x] & 0xFF00FF) * 72) >> 8) & 0xFF00FF) | ((((p[x] & 0xFF00) * 72) >> 8) & 0xFF00);
	}
	SDL_SetSurfaceBlendMode(blur_bg, SDL_BLENDMODE_NONE);
	return blur_bg;
}

static void pillAt(SDL_Surface* s, int x, int y, const char* t) {
	int h = BOS_px(40), w = BOS_textW(t, 24, 600) + BOS_px(32);
	BOS_rrect(s, x, y, w, h, h / 2, 0x2C2C30);
	BOS_textMid(s, t, 24, 600, BOS_C_TEXT, 1, x + BOS_px(16), y + h / 2);
}

static int pillW(const char* t) {
	return BOS_textW(t, 24, 600) + BOS_px(32);
}

static void timeStr(int ms, char* out, int size) {
	int s = ms < 0 ? 0 : ms / 1000;
	if (s >= 3600)
		snprintf(out, size, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

void MusicBos_playing(SDL_Surface* s, const MusicPlayView* v, SDL_Rect* vis) {
	const BosTheme* th = BOS_theme();
	SDL_Surface* bg = blurred(v->art, s->w, s->h);
	if (bg)
		SDL_BlitSurface(bg, NULL, s, NULL);
	else
		SDL_FillRect(s, NULL, BOS_map(s, th->bg));
	BOS_statusBar(s, v->status ? v->status : "");
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);

	// the picture
	int ax = BOS_px(48), ay = top + BOS_px(16), as = BOS_px(400);
	SDL_Surface* p = v->art ? BOS_picRounded(v->art, as, as, BOS_px(28)) : NULL;
	if (p)
		BOS_blit(p, s, ax, ay, 1);
	else {
		BOS_rrect(s, ax, ay, as, as, BOS_px(28), v->icon_colour ? v->icon_colour : 0x5E5CE6);
		int ic = as / 2;
		BOS_icon(s, v->icon ? v->icon : "music", 0xFFFFFF, 0.9f, ax + (as - ic) / 2, ay + (as - ic) / 2, ic);
	}

	// the words
	int rx = ax + as + BOS_px(44), rw = s->w - BOS_px(48) - rx, y = ay;
	int px = rx;
	for (int i = 0; i < 3 && v->pills[i]; i++) {
		if (px + pillW(v->pills[i]) > rx + rw)
			break;
		pillAt(s, px, y, v->pills[i]);
		px += pillW(v->pills[i]) + BOS_px(10);
	}
	y += BOS_px(40) + BOS_px(18);
	y += BOS_textWrap(s, v->title ? v->title : "", 48, 700, BOS_C_TEXT, 1, rx, y, rw, 2);
	y += BOS_px(6);
	if (v->artist && v->artist[0]) {
		char t[256];
		BOS_fit(v->artist, 32, 400, rw, t, sizeof(t));
		BOS_text(s, t, 32, 400, BOS_C_TEXT2, 1, rx, y);
		y += BOS_lineH(32);
	}
	if (v->lyrics_on && v->lyric[1]) { // the sung line bold, the next one text2, the rest textoff
		y += BOS_px(18);
		static const int fs[4] = {30, 38, 30, 30};
		static const int fw[4] = {400, 700, 400, 400};
		uint32_t fc[4] = {BOS_C_TEXTOFF, BOS_C_TEXT, BOS_C_TEXT2, BOS_C_TEXTOFF};
		for (int i = 0; i < 4 && y < bottom - BOS_px(190); i++) {
			if (!v->lyric[i] || !v->lyric[i][0])
				continue;
			char t[256];
			BOS_fit(v->lyric[i], fs[i], fw[i], rw, t, sizeof(t));
			BOS_text(s, t, fs[i], fw[i], fc[i], 1, rx, y);
			y += BOS_lineH(fs[i]) + BOS_px(4);
		}
		if (vis)
			*vis = (SDL_Rect){0, 0, 0, 0};
	} else {
		if (v->album && v->album[0]) {
			char t[256];
			BOS_fit(v->album, 28, 400, rw, t, sizeof(t));
			BOS_text(s, t, 28, 400, BOS_C_TEXTOFF, 1, rx, y + BOS_px(4));
			y += BOS_lineH(28) + BOS_px(4);
		}
		if (v->line_extra && v->line_extra[0]) { // the radio: "เพลงตอนนี้" and the stream's title
			y += BOS_px(18);
			BOS_text(s, v->line_extra_label ? v->line_extra_label : "", 28, 400, BOS_C_TEXTOFF, 1, rx, y);
			y += BOS_lineH(28);
			char t[256];
			BOS_fit(v->line_extra, 32, 400, rw, t, sizeof(t));
			BOS_text(s, t, 32, 400, BOS_C_TEXT, 1, rx, y);
			y += BOS_lineH(32);
		}
		if (vis) { // the visualizer's place: under the words, above the seek bar
			int vy = y + BOS_px(20), vh = bottom - BOS_px(150) - vy;
			if (vh > BOS_px(110))
				vy += vh - BOS_px(110), vh = BOS_px(110);
			*vis = vh > BOS_px(30) ? (SDL_Rect){rx, vy, rw, vh} : (SDL_Rect){0, 0, 0, 0};
		}
	}

	// the seek bar, the times, the controls
	int bx = BOS_px(48), bw = s->w - 2 * bx, rowc = bottom - BOS_px(28) - BOS_px(42);
	if (v->dur_ms > 0) {
		int by = rowc - BOS_px(42) - BOS_px(12) - BOS_px(10), bh = BOS_px(10);
		float f = (float)v->pos_ms / v->dur_ms;
		if (f > 1)
			f = 1;
		if (f < 0)
			f = 0;
		BOS_rrect(s, bx, by, bw, bh, bh / 2, 0x48484A);
		int fw2 = (int)(bw * f);
		if (fw2 > 0)
			BOS_rrect(s, bx, by, fw2 < bh ? bh : fw2, bh, bh / 2, BOS_C_TEXT);
		int k = BOS_px(22);
		BOS_rrect(s, bx + fw2 - k / 2, by + bh / 2 - k / 2, k, k, k / 2, BOS_C_TEXT);
		char a[16], b[16];
		timeStr(v->pos_ms, a, sizeof(a));
		timeStr(v->dur_ms, b, sizeof(b));
		BOS_textMid(s, a, 28, 600, BOS_C_TEXT, 1, bx, rowc);
		BOS_textMid(s, b, 28, 400, BOS_C_TEXT2, 1, bx + bw - BOS_textW(b, 28, 400), rowc);
	}
	// the buttons, centred: [left mode] [back] (pause) [forward] [right mode]
	int disc = BOS_px(84), gap = BOS_px(40), big = BOS_px(40), small = BOS_px(34), cx = s->w / 2;
	BOS_rrect(s, cx - disc / 2, rowc - disc / 2, disc, disc, disc / 2, th->focus);
	BOS_icon(s, v->paused ? "play" : "pause", th->onfocus, 1, cx - big / 2, rowc - big / 2, big);
	int lx = cx - disc / 2 - gap - big, rxx = cx + disc / 2 + gap;
	BOS_icon(s, v->back_icon ? v->back_icon : "prev", BOS_C_TEXT, 1, lx, rowc - big / 2, big);
	BOS_icon(s, v->fwd_icon ? v->fwd_icon : "next", BOS_C_TEXT, 1, rxx, rowc - big / 2, big);
	if (v->modes) {
		int sx = lx - gap - small, ex = rxx + big + gap;
		BOS_icon(s, "shuffle", v->shuffle ? th->primary : BOS_C_TEXTOFF, 1, sx, rowc - small / 2, small);
		BOS_icon(s, "repeat", v->repeat ? th->primary : BOS_C_TEXTOFF, 1, ex, rowc - small / 2, small);
		int d = BOS_px(6);
		if (v->shuffle)
			BOS_rrect(s, sx + small / 2 - d / 2, rowc + small / 2 + BOS_px(8), d, d, d / 2, th->primary);
		if (v->repeat)
			BOS_rrect(s, ex + small / 2 - d / 2, rowc + small / 2 + BOS_px(8), d, d, d / 2, th->primary);
	}
	BOS_hintBar(s, v->hints, v->nhints, v->left, v->nleft);
}
