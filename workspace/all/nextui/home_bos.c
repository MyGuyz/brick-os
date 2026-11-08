// Brick OS Home (DESIGN.md 6.1, the mockup "Home" artboard at 1024x768): status bar, the เล่นต่อ row of up to three
// recent games, a 3x2 grid of tool tiles and the hint bar. Drawn whole by this file when Layouts > Home layout is
// Grid (the Brick OS default); home.c hands over its render and input. Every size, colour and font here is the
// mockup's; px are the 1024x768 panel's (FIXED_WIDTH/HEIGHT), scaled by the screen's width when it differs.

#include "home_bos.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "api.h"
#include "config.h"
#include "defines.h"
#include "i18n.h"
#include "text_thai.h"
#include "utils.h"
#include "ui/ui_draw.h"

#include "../musicplayer/music_service_client.h"
#include "../musicplayer/music_service_protocol.h"
#include "content.h"
#include "gameinfo.h"
#include "gameinfo_text.h"
#include "gamelist.h"
#include "home.h"
#include "homeart.h"
#include "launcher.h"
#include "menutabs.h"
#include "recents.h"
#include "types.h"

// ---- tokens (DESIGN.md 2) ----
#define C_BG 0x000000
#define C_SURFACE1 0x1C1C1E
#define C_SURFACE2 0x2C2C2E
#define C_TEXT 0xF2F2F7
#define C_TEXT2 0xA1A1A6
#define C_TEXT4 0x6E6E73
#define C_BATTERY 0x8E8E93
#define C_ACCENT 0x0A84FF

#define BOS_RES RES_PATH "/bos"
#define MAX_CARDS 3
#define NTOOLS 6

static float k = 1.0f; // screen width / 1024
static int P(float v) {
	return (int)(v * k + 0.5f);
}

// ---- fonts: Inter at four weights, IBM Plex Sans Thai (font1-thai.ttf) for any string with Thai ----
typedef struct {
	int px, weight;
	bool thai;
	TTF_Font* f;
} FontSlot;
static FontSlot fonts[32];
static int nfonts;

static TTF_Font* fontFor(int px, int weight, bool thai) {
	for (int i = 0; i < nfonts; i++)
		if (fonts[i].px == px && fonts[i].weight == weight && fonts[i].thai == thai)
			return fonts[i].f;
	if (nfonts == (int)(sizeof(fonts) / sizeof(fonts[0])))
		return NULL;
	char path[MAX_PATH];
	TTF_Font* f;
	if (thai) {
		f = TTF_OpenFont(GFX_getThaiFontPath(), px);
		if (f && weight >= 600)
			TTF_SetFontStyle(f, TTF_STYLE_BOLD); // the Thai font has one weight: 600/700 as synthetic bold
	} else {
		const char* name = weight >= 700 ? "Inter-Bold" : weight >= 600 ? "Inter-SemiBold"
													: weight >= 500	  ? "Inter-Medium"
																	  : "Inter-Regular";
		snprintf(path, sizeof(path), "%s/%s.ttf", BOS_RES, name);
		f = TTF_OpenFont(path, px);
	}
	if (!f)
		return NULL;
	fonts[nfonts++] = (FontSlot){px, weight, thai, f};
	return f;
}

static SDL_Color rgb(uint32_t c, float a) {
	// a < 1: the colour over the black page (the mockup's opacity on a black ground)
	return (SDL_Color){(Uint8)(((c >> 16) & 255) * a), (Uint8)(((c >> 8) & 255) * a), (Uint8)((c & 255) * a), 255};
}
static uint32_t mapc(SDL_Surface* s, uint32_t c, float a) {
	SDL_Color q = rgb(c, a);
	return SDL_MapRGB(s->format, q.r, q.g, q.b);
}

// Text whose top of line is `y` (the Inter line box at px: its ascent sets the baseline for both scripts).
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
static void text(SDL_Surface* dst, const char* s, int px, int weight, uint32_t c, float a, int x, int y) {
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
	SDL_Surface* t = TTF_RenderUTF8_Blended(f, draw, rgb(c, a));
	if (!t)
		return;
	SDL_BlitSurface(t, NULL, dst, &(SDL_Rect){x, baseline - TTF_FontAscent(f)});
	SDL_FreeSurface(t);
}
// `s` cut with "…" to max_w
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

static void rrect(SDL_Surface* s, int x, int y, int w, int h, int r, uint32_t c, float a) {
	UI_fillRoundedRect(s, x, y, w, h, r, mapc(s, c, a));
}

// ---- icons: white stroke PNGs (design/icons, rendered at the mockup's px), tinted when drawn ----
static SDL_Surface* icons[NTOOLS + 1]; // the six tools, then WiFi
static const char* icon_names[NTOOLS + 1] = {"games", "youtube", "music", "video", "ports", "settings", "wifi"};
static void drawIcon(SDL_Surface* dst, int i, uint32_t c, int x, int y, int w, int h) {
	if (!icons[i]) {
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s/icon_%s.png", BOS_RES, icon_names[i]);
		SDL_Surface* raw = IMG_Load(path);
		if (!raw)
			return;
		icons[i] = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
		SDL_FreeSurface(raw);
		if (!icons[i])
			return;
		SDL_SetSurfaceBlendMode(icons[i], SDL_BLENDMODE_BLEND);
	}
	SDL_SetSurfaceColorMod(icons[i], (c >> 16) & 255, (c >> 8) & 255, c & 255);
	SDL_BlitScaled(icons[i], NULL, dst, &(SDL_Rect){x, y, w, h});
}

// ---- data ----
typedef struct {
	Entry* entry;
	char tag[32];
} Card;
static Card cards[MAX_CARDS];
static int ncards;

typedef struct {
	const char* name;  // English key (I18N_text)
	uint32_t color;	   // icon colour (DESIGN.md 2)
	char sub[96];	   // subtitle
	char path[MAX_PATH];
} Tool;
static Tool tools[NTOOLS] = {
	{"Games", C_ACCENT, "", ""},
	{"YouTube", 0xFF6B5E, "", "/.system/paks/Tools/YouTube.pak"},
	{"Music", 0xFF7AB6, "", "/.system/paks/Tools/Music Player.pak"},
	{"Video", 0xFFB340, "", "/.system/paks/Tools/Media Player.pak"},
	{"Ports", 0x5EE0A0, "", ""},
	{"Settings", 0xC7C7CC, "", "/.system/paks/Tools/Settings.pak"},
};

static bool built;
static int focus; // 0..2 the cards, 3..8 the tools (row by row)

static void groupDigits(int n, char* out, size_t size) { // 1248 -> "1,248"
	char raw[32];
	snprintf(raw, sizeof(raw), "%d", n);
	int len = (int)strlen(raw), o = 0;
	for (int i = 0; i < len && o < (int)size - 2; i++) {
		if (i > 0 && (len - i) % 3 == 0)
			out[o++] = ',';
		out[o++] = raw[i];
	}
	out[o] = '\0';
}

static int countLines(const char* path) { // lines that aren't the "#fp=" header
	FILE* f = fopen(path, "r");
	if (!f)
		return 0;
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof(line), f))
		if (line[0] && line[0] != '#' && line[0] != '\n')
			n++;
	fclose(f);
	return n;
}

static int countFiles(const char* dir, bool scripts_only) {
	DIR* d = opendir(dir);
	if (!d)
		return 0;
	int n = 0;
	struct dirent* e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		if (scripts_only) {
			const char* ext = strrchr(e->d_name, '.');
			if (!ext || strcasecmp(ext, ".sh"))
				continue;
		} else if (e->d_type == DT_DIR)
			continue;
		n++;
	}
	closedir(d);
	return n;
}

static int countChannels(void) { // YTC's favourite channels: one "id" per channel in channels.json
	FILE* f = fopen(SDCARD_PATH "/.userdata/shared/ytc/config/channels.json", "r");
	if (!f)
		return 0;
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, "\"id\""))
			n++;
	fclose(f);
	return n;
}

static void portsDir(char* out, size_t size) {
	out[0] = '\0';
	DIR* d = opendir(ROMS_PATH);
	if (!d)
		return;
	struct dirent* e;
	while ((e = readdir(d)))
		if (strstr(e->d_name, "(PORTS)")) {
			snprintf(out, size, "%s/%s", ROMS_PATH, e->d_name);
			break;
		}
	closedir(d);
}

static void nowPlaying(char* out, size_t size) {
	out[0] = '\0';
	int fd = MusicService_connect(NULL, 20);
	if (fd < 0)
		return;
	MusicResponseWire r;
	if (MusicService_request(fd, MUSIC_CMD_SNAPSHOT, NULL, 0, &r, 100) == MUSIC_STATUS_OK &&
		r.snapshot.state == MUSIC_STATE_PLAYING && r.snapshot.title[0])
		snprintf(out, size, I18N_text("Playing · %s"), r.snapshot.title);
	MusicService_disconnect(fd);
}

static void tagOf(const char* path, char* out, size_t size) { // "Roms/Super Nintendo ES (SFC)/x.sfc" -> "SFC"
	out[0] = '\0';
	const char* slash = strrchr(path, '/');
	if (!slash)
		return;
	const char* close = NULL;
	for (const char* p = slash; p > path; p--)
		if (*p == ')') {
			close = p;
			break;
		}
	if (!close)
		return;
	const char* open = close;
	while (open > path && *open != '(')
		open--;
	if (*open == '(' && close - open - 1 > 0)
		snprintf(out, size, "%.*s", (int)(close - open - 1), open + 1);
}

static void freeCards(void) {
	for (int i = 0; i < ncards; i++)
		Entry_free(cards[i].entry);
	ncards = 0;
}

static void build(void) {
	k = FIXED_WIDTH / 1024.0f;
	freeCards();
	Recents_load();
	for (int i = 0; i < Recents_count() && ncards < MAX_CARDS; i++) {
		Recent* r = Recents_at(i);
		if (!r || !r->available || suffixMatch((char*)".pak", r->path))
			continue;
		Entry* e = Recents_entryFromRecent(r);
		if (!e)
			continue;
		cards[ncards].entry = e;
		tagOf(e->path, cards[ncards].tag, sizeof(cards[ncards].tag));
		ncards++;
	}
	char n[32];
	groupDigits(countLines(USERDATA_PATH "/romindex_cache.txt"), n, sizeof(n));
	snprintf(tools[0].sub, sizeof(tools[0].sub), I18N_text("%s games"), n);
	snprintf(tools[1].sub, sizeof(tools[1].sub), I18N_text("Following %d channels"), countChannels());
	nowPlaying(tools[2].sub, sizeof(tools[2].sub));
	snprintf(tools[3].sub, sizeof(tools[3].sub), I18N_text("%d files"), countFiles(SDCARD_PATH "/Videos", false));
	portsDir(tools[4].path, sizeof(tools[4].path));
	snprintf(tools[4].sub, sizeof(tools[4].sub), I18N_text("%d PC games"),
			 tools[4].path[0] ? countFiles(tools[4].path, true) : 0);
	snprintf(tools[5].sub, sizeof(tools[5].sub), "WiFi · Bluetooth");
	if (focus < 3 && focus >= ncards)
		focus = ncards > 0 ? 0 : 3;
	built = true;
}

bool HomeBos_active(void) {
	return Home_active() && CFG_getHomeStyle() == HOME_STYLE_GRID;
}

void HomeBos_reset(void) {
	built = false;
}

static bool pending; // a picture or a game's info still loading: keep drawing

// ---- drawing ----
static void statusBar(SDL_Surface* s) { // DESIGN.md 5.1, Home's
	int h = P(56), cy = h / 2;
	char clock[16];
	time_t now = time(NULL);
	strftime(clock, sizeof(clock), "%H:%M", localtime(&now));
	text(s, clock, P(21), 600, C_TEXT, 1, P(32), cy - lineH(P(21)) / 2);

	int charging = 0, pct = 0;
	PLAT_getBatteryStatusFine(&charging, &pct);
	int bw = P(35), bh = P(18), bx = s->w - P(32) - bw, by = cy - bh / 2;
	rrect(s, bx, by, bw, bh, P(5), C_BATTERY, 1);
	rrect(s, bx + P(2), by + P(2), bw - P(4), bh - P(4), P(3), C_BG, 1);
	int iw = bw - P(8), fill = iw * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100;
	if (fill > 0)
		rrect(s, bx + P(4), by + P(4), fill, bh - P(8), P(1), C_TEXT, 1);
	char p[16];
	snprintf(p, sizeof(p), "%d%%", pct);
	int pw = textW(p, P(19), 600), px = bx - P(8) - pw;
	text(s, p, P(19), 600, C_TEXT, 1, px, cy - lineH(P(19)) / 2);
	if (PLAT_wifiConnected())
		drawIcon(s, NTOOLS, C_TEXT, px - P(16) - P(26), cy - P(19) / 2, P(26), P(19));
}

static void hintBar(SDL_Surface* s) { // DESIGN.md 5.2
	int top = s->h - P(64), cy = top + P(32);
	SDL_FillRect(s, &(SDL_Rect){0, top, s->w, P(1) > 0 ? P(1) : 1}, mapc(s, C_SURFACE1, 1));
	struct {
		const char* btn;
		const char* label;
	} hints[] = {{"A", "Continue"}, {"Y", "Search"}, {"MENU", "Switch game"}};
	int x = P(40);
	for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
		bool wide = strlen(hints[i].btn) > 1;
		int bpx = wide ? P(15) : P(16);
		int bw = wide ? textW(hints[i].btn, bpx, 700) + 2 * P(12) : P(32);
		rrect(s, x, cy - P(16), bw, P(32), P(16), C_SURFACE2, 1);
		text(s, hints[i].btn, bpx, 700, C_TEXT, 1, x + (bw - textW(hints[i].btn, bpx, 700)) / 2, cy - lineH(bpx) / 2);
		x += bw + P(10);
		const char* l = I18N_text(hints[i].label);
		text(s, l, P(19), 400, C_TEXT2, 1, x, cy - lineH(P(19)) / 2);
		x += textW(l, P(19), 400) + P(32);
	}
	const char* note = "Brick OS";
	text(s, note, P(19), 400, C_TEXT4, 1, s->w - P(40) - textW(note, P(19), 400), cy - lineH(P(19)) / 2);
}

static void ring(SDL_Surface* s, int x, int y, int w, int h, int r, int off) { // 5px accent outline at `off`
	int o = off + P(5);
	rrect(s, x - o, y - o, w + 2 * o, h + 2 * o, r + o, C_ACCENT, 1);
	rrect(s, x - off, y - off, w + 2 * off, h + 2 * off, r + off, C_BG, 1);
}

static void continueRow(SDL_Surface* s) {
	int top = P(56) + P(8);
	const char* title = I18N_text("Continue");
	text(s, title, P(35), 700, C_TEXT, 1, P(40), top);
	char count[32];
	snprintf(count, sizeof(count), I18N_text("%d games"), ncards);
	int title_h = lineH(P(35));
	// the count shares the title's baseline
	TTF_Font* tf = fontFor(P(35), 700, false);
	TTF_Font* cf = fontFor(P(19), 400, false);
	int base = top + (tf ? TTF_FontAscent(tf) : P(35));
	text(s, count, P(19), 400, C_TEXT2, 1, s->w - P(40) - textW(count, P(19), 400), base - (cf ? TTF_FontAscent(cf) : P(19)));

	int cy = top + title_h + P(14), cw = P(298), ch = P(224), gap = P(24), r = P(22);
	time_t now = time(NULL);
	for (int i = 0; i < ncards; i++) {
		int x = P(40) + i * (cw + gap);
		bool sel = focus == i;
		float a = sel ? 1.0f : 0.72f;
		if (sel)
			ring(s, x, cy, cw, ch, r, P(4));
		SDL_Surface* pic = NULL;
		HomeArtState st = HomeArt_continue(cards[i].entry->path, NULL, cw, ch, r, &pic);
		if (st == HOMEART_LOADING)
			pending = true;
		if (st == HOMEART_READY && pic) {
			SDL_SetSurfaceAlphaMod(pic, (Uint8)(255 * a));
			SDL_BlitSurface(pic, NULL, s, &(SDL_Rect){x, cy});
			SDL_SetSurfaceAlphaMod(pic, 255);
		} else
			rrect(s, x, cy, cw, ch, r, C_SURFACE1, a);
		if (cards[i].tag[0]) { // console badge: 16/600, black pill, radius 10, padding 4 10, 14 in from the corner
			int bw = textW(cards[i].tag, P(16), 600) + 2 * P(10), bh = lineH(P(16)) + 2 * P(4);
			int bx = x + P(14), by = cy + ch - P(14) - bh;
			rrect(s, bx, by, bw, bh, P(10), C_BG, 1);
			text(s, cards[i].tag, P(16), 600, C_TEXT, a, bx + P(10), by + P(4));
		}
		char name[256];
		fit(cards[i].entry->name, P(21), 600, cw, name, sizeof(name));
		int ny = cy + ch + P(10);
		text(s, name, P(21), 600, C_TEXT, a, x, ny);
		GameInfo gi;
		if (GameInfo_get(cards[i].entry->path, &gi)) {
			if (gi.has_time && gi.last_played > 0) {
				char when[64];
				GameInfo_whenText(now, gi.last_played, when, sizeof(when));
				text(s, when, P(18), 400, C_TEXT2, a, x, ny + lineH(P(21)) + P(2));
			}
		} else
			pending = true;
	}
}

static int toolsTop(void) {
	return P(56) + P(8) + lineH(P(35)) + P(14) + P(224) + P(10) + lineH(P(21)) + P(2) + lineH(P(18)) + P(24);
}

static void toolGrid(SDL_Surface* s) {
	int top = toolsTop(), gap = P(16), tw = (s->w - 2 * P(40) - 2 * gap) / 3, th = P(120), r = P(24);
	for (int i = 0; i < NTOOLS; i++) {
		int x = P(40) + (i % 3) * (tw + gap), y = top + (i / 3) * (th + gap);
		if (focus == 3 + i)
			ring(s, x, y, tw, th, r, P(4));
		rrect(s, x, y, tw, th, r, C_SURFACE1, 1);
		int well = P(62), wx = x + P(20), wy = y + (th - well) / 2;
		rrect(s, wx, wy, well, well, P(18), C_SURFACE2, 1);
		drawIcon(s, i, tools[i].color, wx + (well - P(34)) / 2, wy + (well - P(34)) / 2, P(34), P(34));
		int tx = wx + well + P(16), maxw = x + tw - P(20) - tx;
		int nh = lineH(P(24)), sh = tools[i].sub[0] ? lineH(P(18)) + P(2) : 0, ty = y + (th - nh - sh) / 2;
		char buf[128];
		fit(I18N_text(tools[i].name), P(24), 600, maxw, buf, sizeof(buf));
		text(s, buf, P(24), 600, C_TEXT, 1, tx, ty);
		if (tools[i].sub[0]) {
			fit(tools[i].sub, P(18), 400, maxw, buf, sizeof(buf));
			text(s, buf, P(18), 400, C_TEXT2, 1, tx, ty + nh + P(2));
		}
	}
}

void HomeBos_render(SDL_Surface* s) {
	if (!built)
		build();
	pending = false;
	SDL_FillRect(s, NULL, mapc(s, C_BG, 1));
	statusBar(s);
	continueRow(s);
	toolGrid(s);
	hintBar(s);
}

bool HomeBos_animating(void) {
	return pending || HomeArt_checkAsyncLoaded() || GameInfo_checkAsyncLoaded();
}

// ---- input ----
static void openTool(int i, bool* dirty) {
	if (i == 0) {
		GameList_openTab(MENU_TAB_CONSOLES, dirty);
		return;
	}
	if (!tools[i].path[0])
		return;
	char full[MAX_PATH];
	bool dir = i == 4;
	if (dir)
		snprintf(full, sizeof(full), "%s", tools[i].path);
	else
		snprintf(full, sizeof(full), "%s%s", SDCARD_PATH, tools[i].path);
	if (!exists(full))
		return;
	Entry* e = Entry_new(full, dir ? ENTRY_DIR : ENTRY_PAK);
	if (!e)
		return;
	if (dir || GameList_settingsPinAllows(e)) {
		if (!dir)
			MenuTabs_markHomeLaunch();
		Entry_open(e);
	}
	Entry_free(e);
	*dirty = true;
}

HomeBosInput HomeBos_handleInput(unsigned long now, bool* dirty, bool tap_menu) {
	(void)now;
	if (!built)
		build();
	if (tap_menu)
		return HOMEBOS_SWITCHER;
	if (PAD_justPressed(BTN_Y))
		return HOMEBOS_SEARCH;
	if (PAD_justPressed(BTN_L1) || PAD_justPressed(BTN_R1))
		return HOMEBOS_HANDLED; // no tabs on Home (the mockup)
	int f = focus;
	if (PAD_justRepeated(BTN_LEFT)) {
		if (f < 3 ? f > 0 : (f - 3) % 3 > 0)
			f--;
	} else if (PAD_justRepeated(BTN_RIGHT)) {
		if (f < 3 ? f < ncards - 1 : (f - 3) % 3 < 2)
			f++;
	} else if (PAD_justRepeated(BTN_UP)) {
		if (f >= 6)
			f -= 3;
		else if (f >= 3 && ncards > 0)
			f = (f - 3) < ncards ? f - 3 : ncards - 1;
	} else if (PAD_justRepeated(BTN_DOWN)) {
		if (f < 3)
			f += 3;
		else if (f < 6)
			f += 3;
	} else if (PAD_justPressed(BTN_A)) {
		if (focus < 3 && focus < ncards) {
			Entry* e = cards[focus].entry;
			readyResume(e);
			resume.should_resume = resume.can_resume;
			MenuTabs_markHomeLaunch();
			Entry_open(e);
		} else if (focus >= 3)
			openTool(focus - 3, dirty);
		*dirty = true;
		return HOMEBOS_HANDLED;
	} else
		return HOMEBOS_UNHANDLED;
	if (f != focus) {
		focus = f;
		*dirty = true;
	}
	return HOMEBOS_HANDLED;
}

void HomeBos_quit(void) {
	freeCards();
	for (int i = 0; i < nfonts; i++)
		TTF_CloseFont(fonts[i].f);
	nfonts = 0;
	for (int i = 0; i < NTOOLS + 1; i++)
		if (icons[i]) {
			SDL_FreeSurface(icons[i]);
			icons[i] = NULL;
		}
	built = false;
}

// ---- Game Switcher (DESIGN.md 6.2) ----
#define C_BG_SWITCHER 0x0B0B0D
#define C_DOT_OFF 0x48484A

static void durationText(int seconds, char* out, size_t size) { // "3 ชม. 12 นาที"
	int h = seconds / 3600, m = seconds % 3600 / 60;
	if (h > 0)
		snprintf(out, size, I18N_text("%d hr %d min"), h, m);
	else
		snprintf(out, size, I18N_text("%d min"), m);
}

static void switcherCard(SDL_Surface* s, const BosSwitchCard* c, int x, int y, int w, int h, int r, float a) {
	SDL_Surface* pic = NULL;
	HomeArtState st = HomeArt_continue(c->path, c->preview[0] ? c->preview : NULL, w, h, r, &pic);
	if (st == HOMEART_LOADING)
		pending = true;
	if (st == HOMEART_READY && pic) {
		SDL_SetSurfaceAlphaMod(pic, (Uint8)(255 * a));
		SDL_BlitSurface(pic, NULL, s, &(SDL_Rect){x, y});
		SDL_SetSurfaceAlphaMod(pic, 255);
	} else
		rrect(s, x, y, w, h, r, C_SURFACE1, a);
}

void BosSwitcher_render(SDL_Surface* s, const BosSwitchCard* prev, const BosSwitchCard* cur,
						const BosSwitchCard* next, int sel, int count, bool playing, time_t saved_at) {
	k = FIXED_WIDTH / 1024.0f;
	pending = false;
	SDL_FillRect(s, NULL, mapc(s, C_BG_SWITCHER, 1));
	int bar = P(56), cy = bar / 2;
	text(s, I18N_text("Switch game"), P(21), 600, C_TEXT, 1, P(32), cy - lineH(P(21)) / 2);
	if (saved_at > 0) {
		char t[16], line[128];
		strftime(t, sizeof(t), "%H:%M", localtime(&saved_at));
		snprintf(line, sizeof(line), I18N_text("Saved · %s"), t);
		text(s, line, P(19), 400, C_TEXT2, 1, s->w - P(32) - textW(line, P(19), 400), cy - lineH(P(19)) / 2);
	}

	// the centre row: side 180x135 | focused 528x396 | side, gap 28, in the band between the bar and the dots
	int hint_top = s->h - P(64), dots_h = P(10), dots_y = hint_top - P(20) - dots_h;
	int band_top = bar, band_h = dots_y - band_top;
	int fw = P(528), fh = P(396), sw = P(180), sh = P(135), gap = P(28);
	int name_h = lineH(P(30)), meta_h = lineH(P(19));
	int col_h = fh + P(18) + name_h + P(4) + meta_h;
	int fy = band_top + (band_h - col_h) / 2, fx = (s->w - fw) / 2;
	int sy = band_top + (band_h - sh) / 2;
	if (prev)
		switcherCard(s, prev, fx - gap - sw, sy, sw, sh, P(20), 0.55f);
	if (next)
		switcherCard(s, next, fx + fw + gap, sy, sw, sh, P(20), 0.55f);
	if (cur) {
		int off = P(5), o = off + P(5);
		rrect(s, fx - o, fy - o, fw + 2 * o, fh + 2 * o, P(30) + o, C_ACCENT, 1);
		rrect(s, fx - off, fy - off, fw + 2 * off, fh + 2 * off, P(30) + off, C_BG_SWITCHER, 1);
		switcherCard(s, cur, fx, fy, fw, fh, P(30), 1);
		if (playing) { // "กำลังเล่น": 18/600, black pill, radius 11, padding 5 12, 20 in from the corner
			const char* l = I18N_text("Playing");
			int bw = textW(l, P(18), 600) + 2 * P(12), bh = lineH(P(18)) + 2 * P(5);
			int bx = fx + P(20), by = fy + fh - P(20) - bh;
			rrect(s, bx, by, bw, bh, P(11), C_BG, 1);
			text(s, l, P(18), 600, C_TEXT, 1, bx + P(12), by + P(5));
		}
		char name[256];
		fit(cur->name, P(30), 700, s->w - 2 * P(40), name, sizeof(name));
		int ny = fy + fh + P(18);
		text(s, name, P(30), 700, C_TEXT, 1, (s->w - textW(name, P(30), 700)) / 2, ny);
		char meta[192] = "", dur[64];
		char tag[32];
		tagOf(cur->path, tag, sizeof(tag));
		if (cur->seconds > 0) {
			char played[96];
			durationText(cur->seconds, dur, sizeof(dur));
			snprintf(played, sizeof(played), I18N_text("Played %s"), dur);
			snprintf(meta, sizeof(meta), tag[0] ? "%s · %s" : "%s%s", tag, played);
		} else
			snprintf(meta, sizeof(meta), "%s", tag);
		text(s, meta, P(19), 400, C_TEXT2, 1, (s->w - textW(meta, P(19), 400)) / 2, ny + name_h + P(4));
	}

	// page dots: 10x10 #48484A, the current 28x10 text, gap 12
	if (count > 1) {
		int total = (count - 1) * P(10) + P(28) + (count - 1) * P(12), x = (s->w - total) / 2;
		for (int i = 0; i < count; i++) {
			int w = i == sel ? P(28) : P(10);
			rrect(s, x, dots_y, w, dots_h, dots_h / 2, i == sel ? C_TEXT : C_DOT_OFF, 1);
			x += w + P(12);
		}
	}

	SDL_FillRect(s, &(SDL_Rect){0, hint_top, s->w, P(1) > 0 ? P(1) : 1}, mapc(s, C_SURFACE1, 1));
	int hy = hint_top + P(32), x = P(40);
	const char* hints[][2] = {{"A", "Play this game"}, {"B", "Back to game"}, {"X", "Close game"}};
	for (int i = 0; i < 3; i++) {
		rrect(s, x, hy - P(16), P(32), P(32), P(16), C_SURFACE2, 1);
		text(s, hints[i][0], P(16), 700, C_TEXT, 1, x + (P(32) - textW(hints[i][0], P(16), 700)) / 2, hy - lineH(P(16)) / 2);
		x += P(32) + P(10);
		const char* l = I18N_text(hints[i][1]);
		text(s, l, P(19), 400, C_TEXT2, 1, x, hy - lineH(P(19)) / 2);
		x += textW(l, P(19), 400) + P(32);
	}
	const char* note = I18N_text("Hold MENU to open");
	text(s, note, P(19), 400, C_TEXT4, 1, s->w - P(40) - textW(note, P(19), 400), hy - lineH(P(19)) / 2);
}

bool BosSwitcher_pending(void) {
	return pending;
}
