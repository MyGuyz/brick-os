/* ===================== Brick OS player overlay (DESIGN.md 7.3; boards v2-VideoPlayer, -PlayerQuick) =====================
 * Drawn with SDL_ttf in the OS's fonts (Inter; IBM Plex Sans Thai through the kit's glyph mapping, text_thai.c), so
 * Thai names show. The app says the language in BOS_LANG ("th" / "en"). Sizes are the boards' (1024 wide), scaled. */
#include <SDL2/SDL_ttf.h>
#include <time.h>
#include "text_thai.c"

#define BOS_RES_DIR "/mnt/SDCARD/.system/res"
static int bos_th = -1;		  /* Thai words? */
static float bos_k = 1;		  /* device px per board px */
static char quick_text[160];  /* "ซับ: English 2 / 3" */
static int64_t quick_until;	  /* shown until */
static char seek_text[32];	  /* "+10 วิ" by the seek bar */
static int64_t seek_until;

static const char* T(const char* th, const char* en) {
	if (bos_th < 0) {
		const char* l = getenv("BOS_LANG");
		bos_th = l && !strcmp(l, "th");
	}
	return bos_th ? th : en;
}

static int P(float px) {
	return (int)(px * bos_k + 0.5f);
}

/* ---- fonts and drawn words (kept: the overlay draws the same words every frame) ---- */
static TTF_Font* osd_font(int px, int bold, int thai) {
	static struct {
		int px, bold, thai;
		TTF_Font* f;
	} fonts[12];
	static int n;
	for (int i = 0; i < n; i++)
		if (fonts[i].px == px && fonts[i].bold == bold && fonts[i].thai == thai)
			return fonts[i].f;
	if (!TTF_WasInit() && TTF_Init() != 0)
		return NULL;
	char path[256];
	if (thai)
		snprintf(path, sizeof(path), BOS_RES_DIR "/%s", bold ? "font1-thai-semibold.ttf" : "font1-thai.ttf");
	else
		snprintf(path, sizeof(path), BOS_RES_DIR "/bos/%s", bold ? "Inter-SemiBold.ttf" : "Inter-Regular.ttf");
	TTF_Font* f = TTF_OpenFont(path, px);
	if (!f && thai && bold) // no SemiBold: the regular
		f = TTF_OpenFont(BOS_RES_DIR "/font1-thai.ttf", px);
	if (!f || n == 12)
		return f;
	fonts[n].px = px, fonts[n].bold = bold, fonts[n].thai = thai, fonts[n].f = f;
	n++;
	return f;
}

typedef struct {
	char text[160];
	int px, bold;
	uint32_t rgb;
	SDL_Texture* tex;
	int w, h;
	int64_t used;
} OsdWord;
static OsdWord words[40];

static OsdWord* osd_word(const char* s, int px, int bold, uint32_t rgb) {
	if (!s || !s[0])
		return NULL;
	int64_t now = av_gettime_relative();
	OsdWord* old = &words[0];
	for (int i = 0; i < 40; i++) {
		OsdWord* w = &words[i];
		if (w->tex && w->px == px && w->bold == bold && w->rgb == rgb && !strcmp(w->text, s)) {
			w->used = now;
			return w;
		}
		if (w->used < old->used)
			old = w;
	}
	if (old->tex)
		SDL_DestroyTexture(old->tex);
	memset(old, 0, sizeof(*old));
	char glyphs[512];
	const char* draw = s;
	int thai = TextThai_has(s);
	if (thai && TextThai_toGlyphs(s, glyphs, sizeof(glyphs)) > 0)
		draw = glyphs;
	TTF_Font* f = osd_font(px, bold, thai);
	if (!f)
		return NULL;
	SDL_Color c = {(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255};
	SDL_Surface* surf = TTF_RenderUTF8_Blended(f, draw, c);
	if (!surf)
		return NULL;
	old->tex = SDL_CreateTextureFromSurface(renderer, surf);
	old->w = surf->w, old->h = surf->h;
	SDL_FreeSurface(surf);
	snprintf(old->text, sizeof(old->text), "%s", s);
	old->px = px, old->bold = bold, old->rgb = rgb, old->used = now;
	return old->tex ? old : NULL;
}

/* a word with its middle at cy; align 0 left of x, 1 centred on x, 2 right of x. Returns its width. */
static int osd_text(const char* s, int px, int bold, uint32_t rgb, int x, int cy, int align, int max_w) {
	OsdWord* w = osd_word(s, P(px), bold, rgb);
	if (!w)
		return 0;
	int dw = w->w, sw = w->w;
	if (max_w > 0 && dw > max_w)
		dw = max_w, sw = max_w; /* cut at the room (the name is long) */
	int dx = align == 1 ? x - dw / 2 : align == 2 ? x - dw : x;
	SDL_Rect src = {0, 0, sw, w->h}, dst = {dx, cy - w->h / 2, dw, w->h};
	SDL_RenderCopy(renderer, w->tex, &src, &dst);
	return dw;
}

static void osd_rrect(int x, int y, int w, int h, int r, uint32_t rgb, int a) {
	SDL_SetRenderDrawColor(renderer, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a);
	if (r * 2 > h)
		r = h / 2;
	if (r * 2 > w)
		r = w / 2;
	SDL_Rect mid = {x, y + r, w, h - 2 * r};
	SDL_RenderFillRect(renderer, &mid);
	for (int i = 0; i < r; i++) { /* the round ends, a line a row */
		float dy = r - i - 0.5f;
		int dx = (int)(r - sqrtf((float)r * r - dy * dy) + 0.5f);
		SDL_Rect t = {x + dx, y + i, w - 2 * dx, 1}, b = {x + dx, y + h - 1 - i, w - 2 * dx, 1};
		SDL_RenderFillRect(renderer, &t);
		SDL_RenderFillRect(renderer, &b);
	}
}

/* a dark band fading out: from alpha a0 at y0 to 0 at y1 */
static void osd_fade(int y0, int y1, int a0, int w) {
	int steps = 24, h = y1 - y0;
	for (int i = 0; i < steps; i++) {
		int ya = y0 + h * i / steps, yb = y0 + h * (i + 1) / steps;
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, a0 * (steps - i) / steps);
		SDL_Rect r = {0, ya < yb ? ya : yb, w, abs(yb - ya)};
		SDL_RenderFillRect(renderer, &r);
	}
}

static int osd_battery(int* charging) {
	static int64_t at;
	static int cap = -1, chg;
	int64_t now = av_gettime_relative();
	if (cap < 0 || now - at > 10000000) {
		at = now;
		FILE* f = fopen("/sys/class/power_supply/axp2202-battery/capacity", "r");
		if (f) {
			if (fscanf(f, "%d", &cap) != 1)
				cap = -1;
			fclose(f);
		}
		f = fopen("/sys/class/power_supply/axp2202-usb/online", "r");
		if (f) {
			if (fscanf(f, "%d", &chg) != 1)
				chg = 0;
			fclose(f);
		}
	}
	*charging = chg;
	return cap;
}

/* the subtitle in use: from its file's name ("x.th.srt" -> ไทย), "ปิด" for the empty filter */
static void osd_sub_label(VideoState* is, char* out, int size) {
	if (nb_vfilters > 1) {
		const char* v = vfilters_list[is->vfilter_idx];
		const char* p = v ? strstr(v, "subtitles='") : NULL;
		if (!p) {
			snprintf(out, size, "%s", T("ปิด", "Off"));
			return;
		}
		char path[512];
		snprintf(path, sizeof(path), "%s", p + 11);
		char* q = strchr(path, '\'');
		if (q)
			*q = 0;
		char* dot = strrchr(path, '.');
		if (dot)
			*dot = 0;
		dot = strrchr(path, '.');
		const char* lang = dot ? dot + 1 : "";
		const char* name = !strcasecmp(lang, "th") || !strcasecmp(lang, "tha") ? T("ไทย", "Thai")
						   : !strcasecmp(lang, "en") || !strcasecmp(lang, "eng") ? "English"
						   : !strcasecmp(lang, "ja") || !strcasecmp(lang, "jpn") ? T("ญี่ปุ่น", "Japanese")
						   : lang[0] && strlen(lang) <= 3 ? lang
														  : T("มี", "On");
		snprintf(out, size, "%s %d / %d", name, is->vfilter_idx + 1, nb_vfilters);
		return;
	}
	snprintf(out, size, "%s", is->subtitle_st ? T("ในไฟล์", "In file") : T("ไม่มี", "None"));
}

static void osd_audio_label(VideoState* is, char* out, int size) {
	if (!is->audio_st) {
		snprintf(out, size, "%s", T("ไม่มี", "None"));
		return;
	}
	AVDictionaryEntry* l = av_dict_get(is->audio_st->metadata, "language", NULL, 0);
	if (l && (!strcmp(l->value, "und") || !l->value[0])) // undefined: say nothing
		l = NULL;
	int ch = is->audio_st->codecpar->ch_layout.nb_channels;
	snprintf(out, size, "%s%s%s", l ? l->value : "", l ? " " : "", ch >= 6 ? "5.1" : ch == 1 ? "Mono" : "Stereo");
}

static void osd_quick(const char* text) {
	snprintf(quick_text, sizeof(quick_text), "%s", text);
	quick_until = av_gettime_relative() + 2000000;
}

static void osd_seek_note(double incr) {
	if (fabs(incr) >= 60)
		snprintf(seek_text, sizeof(seek_text), "%s%d %s", incr > 0 ? "+" : "−", (int)(fabs(incr) / 60), T("นาที", "min"));
	else
		snprintf(seek_text, sizeof(seek_text), "%s%d %s", incr > 0 ? "+" : "−", (int)fabs(incr), T("วิ", "s"));
	seek_until = av_gettime_relative() + 1500000;
}

static void osd_hint(int* x, int cy, const char* key, const char* label) {
	/* drawn right to left: A at the far right */
	int lw = osd_word(label, P(30), 0, 0xB4B4BA) ? osd_word(label, P(30), 0, 0xB4B4BA)->w : 0;
	*x -= lw;
	osd_text(label, 30, 0, 0xB4B4BA, *x, cy, 0, 0);
	*x -= P(12);
	int face = strlen(key) == 1;
	int kw = face ? P(40) : (osd_word(key, P(22), 1, 0x0B0B0D) ? osd_word(key, P(22), 1, 0x0B0B0D)->w : 0) + P(24);
	*x -= kw;
	osd_rrect(*x, cy - P(20), kw, P(40), face ? P(20) : P(10), 0xF2F2F7, 255);
	osd_text(key, face ? 24 : 22, 1, 0x0B0B0D, *x + kw / 2, cy, 1, 0);
	*x -= P(36);
}

static void osd_chip(int* x, int cy, const char* label, const char* value) {
	int pad = P(20), gap = P(10);
	OsdWord* a = osd_word(label, P(26), 0, 0xB4B4BA);
	OsdWord* b = osd_word(value, P(26), 1, 0xFFFFFF);
	int w = pad + (a ? a->w : 0) + gap + (b ? b->w : 0) + pad;
	osd_rrect(*x, cy - P(28), w, P(56), P(28), 0x2C2C30, 220);
	osd_text(label, 26, 0, 0xB4B4BA, *x + pad, cy, 0, 0);
	osd_text(value, 26, 1, 0xFFFFFF, *x + pad + (a ? a->w : 0) + gap, cy, 0, 0);
	*x += w + P(12);
}

static void osd_draw(VideoState* is) {
	int w = 0, h = 0;
	SDL_GetRendererOutputSize(renderer, &w, &h);
	if (w <= 0 || h <= 0)
		return;
	bos_k = w / 1024.0f;
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
	int64_t now = av_gettime_relative();
	double pos = get_master_clock(is), dur = is->ic && is->ic->duration > 0 ? (double)is->ic->duration / AV_TIME_BASE : 0;
	if (isnan(pos))
		pos = 0;
	int full = is->paused || (osd_visible && now - osd_last_activity <= OSD_TIMEOUT_US);
	if (!full)
		osd_visible = 0;
	if (full) {
		/* ---- top: the name, the time, the battery ---- */
		osd_fade(0, P(150), 205, w);
		char title[256], clock[16];
		if (window_title && window_title != input_filename)
			snprintf(title, sizeof(title), "%s", window_title);
		else
			osd_get_title(input_filename, title, sizeof(title));
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		snprintf(clock, sizeof(clock), "%d:%02d", tm.tm_hour, tm.tm_min);
		int chg = 0, cap = osd_battery(&chg), rx = w - P(32);
		if (cap >= 0) {
			char b[16];
			snprintf(b, sizeof(b), "%d%%", cap);
			rx -= osd_text(b, 30, 1, chg ? 0x30D158 : cap <= 10 ? 0xFF453A : cap <= 20 ? 0xFFD60A : 0xFFFFFF, rx, P(32), 2, 0);
			rx -= P(22);
		}
		rx -= osd_text(clock, 30, 1, 0xFFFFFF, rx, P(32), 2, 0) + P(32);
		osd_text(title, 30, 1, 0xFFFFFF, P(32), P(32), 0, rx - P(32));
		/* ---- bottom: chips, the seek bar, the times, the hint bar ---- */
		osd_fade(h, h - P(300), 220, w);
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
		SDL_Rect band = {0, h - P(64), w, P(64)};
		SDL_RenderFillRect(renderer, &band);
		char a[64], s[96];
		osd_audio_label(is, a, sizeof(a));
		osd_sub_label(is, s, sizeof(s));
		int x = P(40), cy = h - P(64) - P(10) - P(46) - P(22) - P(28);
		osd_chip(&x, cy, T("เสียง", "Audio"), a);
		osd_chip(&x, cy, T("ซับ", "Subtitles"), s);
		osd_chip(&x, cy, T("ภาพ", "Picture"), aspect_mode == 1 ? "16:9" : aspect_mode == 2 ? "4:3" : T("อัตโนมัติ", "Auto"));
		int bx = P(40), bw = w - 2 * bx, by = h - P(64) - P(64), bh = P(10);
		osd_rrect(bx, by, bw, bh, bh / 2, 0x5A5A5E, 200);
		if (dur > 0) {
			double f = pos / dur;
			f = f < 0 ? 0 : f > 1 ? 1 : f;
			int fw = (int)(bw * f);
			if (fw > 0)
				osd_rrect(bx, by, fw < bh ? bh : fw, bh, bh / 2, 0xFF453A, 255);
			osd_rrect(bx + fw - P(12), by + bh / 2 - P(12), P(24), P(24), P(12), 0xFFFFFF, 255);
			char c1[32], c2[40], tmp[32];
			format_time(c1, sizeof(c1), pos);
			format_time(tmp, sizeof(tmp), dur - pos);
			snprintf(c2, sizeof(c2), "−%s", tmp);
			osd_text(c1, 28, 1, 0xFFFFFF, bx, by + P(34), 0, 0);
			osd_text(c2, 28, 0, 0xB4B4BA, bx + bw, by + P(34), 2, 0);
		}
		if (seek_until > now)
			osd_text(seek_text, 26, 1, 0xFFFFFF, w / 2, by + P(34), 1, 0);
		int hx = w - P(32), hcy = h - P(32);
		osd_hint(&hx, hcy, "A", is->paused ? T("เล่น", "Play") : T("หยุด", "Pause"));
		osd_hint(&hx, hcy, "B", T("ออก", "Exit"));
		osd_hint(&hx, hcy, "X", T("ซ่อน", "Hide"));
		osd_hint(&hx, hcy, "Y", T("สัดส่วนภาพ", "Aspect"));
		{ /* left: L1 R1 */
			int lx = P(32);
			OsdWord* k = osd_word("L1 R1", P(22), 1, 0x0B0B0D);
			int kw = (k ? k->w : 0) + P(24);
			osd_rrect(lx, hcy - P(20), kw, P(40), P(10), 0xF2F2F7, 255);
			osd_text("L1 R1", 22, 1, 0x0B0B0D, lx + kw / 2, hcy, 1, 0);
			osd_text(T("±1 นาที", "±1 min"), 30, 0, 0xB4B4BA, lx + kw + P(12), hcy, 0, 0);
		}
		if (is->paused) { /* the pause disc */
			int r = P(60);
			osd_rrect(w / 2 - r, h / 2 - r, 2 * r, 2 * r, r, 0x000000, 140);
			osd_rrect(w / 2 - P(18), h / 2 - P(28), P(13), P(56), P(3), 0xFFFFFF, 255);
			osd_rrect(w / 2 + P(5), h / 2 - P(28), P(13), P(56), P(3), 0xFFFFFF, 255);
		}
	} else if (seek_until > now && dur > 0) {
		/* a seek while playing: only a thin bar, the times and how far */
		int bx = P(40), bw = w - 2 * bx, by = h - P(28) - P(8);
		double f = pos / dur;
		f = f < 0 ? 0 : f > 1 ? 1 : f;
		osd_rrect(bx, by, bw, P(8), P(4), 0x5A5A5E, 200);
		osd_rrect(bx, by, (int)(bw * f) > P(8) ? (int)(bw * f) : P(8), P(8), P(4), 0xFF453A, 255);
		char c1[32], c2[40], tmp[32];
		format_time(c1, sizeof(c1), pos);
		format_time(tmp, sizeof(tmp), dur - pos);
		snprintf(c2, sizeof(c2), "−%s", tmp);
		osd_text(c1, 26, 1, 0xFFFFFF, bx, by - P(26), 0, 0);
		osd_text(c2, 26, 0, 0xB4B4BA, bx + bw, by - P(26), 2, 0);
		OsdWord* sw = osd_word(seek_text, P(26), 1, 0xFFFFFF);
		if (sw) {
			osd_rrect(w / 2 - sw->w / 2 - P(14), by - P(26) - P(20), sw->w + P(28), P(40), P(20), 0x2C2C30, 230);
			osd_text(seek_text, 26, 1, 0xFFFFFF, w / 2, by - P(26), 1, 0);
		}
	}
	/* what just changed (audio, subtitles, picture): a pill near the top */
	if (quick_until > now && quick_text[0]) {
		OsdWord* q = osd_word(quick_text, P(32), 1, 0xFFFFFF);
		if (q) {
			int pw = q->w + P(60), py = full ? P(120) : P(56);
			osd_rrect(w / 2 - pw / 2, py, pw, P(76), P(38), 0x2C2C30, 235);
			osd_text(quick_text, 32, 1, 0xFFFFFF, w / 2, py + P(38), 1, 0);
		}
	}
}
