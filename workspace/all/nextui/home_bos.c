// Brick OS Home (DESIGN.md 6.1, the mockup "Home" artboard at 1024x768): status bar, the เล่นต่อ row of up to three
// recent games, a 3x2 grid of tool tiles and the hint bar. Drawn whole by this file when Layouts > Home layout is
// Grid (the Brick OS default); home.c hands over its render and input. Every size, colour and font here is the
// mockup's; px are the 1024x768 panel's (FIXED_WIDTH/HEIGHT), scaled by the screen's width when it differs.

#include "home_bos.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "api.h"
#include "config.h"
#include "defines.h"
#include "i18n.h"
#include "text_thai.h"
#include "utils.h"
#include "ui/ui_draw.h"
#include "bos_ui.h"

#include "../musicplayer/music_service_client.h"
#include "../musicplayer/music_service_protocol.h"
#include "content.h"
#include "controller_art_model.h"
#include <math.h>
#include "gameinfo.h"
#include "gameinfo_text.h"
#include "gamelist.h"
#include "gameswitcher.h"
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

// ==== Home v2 (DESIGN.md 6.1): status bar, เล่นต่อ carousel, pinned app row + ทั้งหมด, hint bar — all from bos_ui ====

#define MAX_CARDS 12
#define MAX_APPS 40

static bool pending; // a picture or a game's info still loading: keep drawing

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

static bool musicPlaying(void) {
	int fd = MusicService_connect(NULL, 20);
	if (fd < 0)
		return false;
	MusicResponseWire r;
	bool playing = MusicService_request(fd, MUSIC_CMD_SNAPSHOT, NULL, 0, &r, 100) == MUSIC_STATUS_OK &&
				   r.snapshot.state == MUSIC_STATE_PLAYING;
	MusicService_disconnect(fd);
	return playing;
}

// ---- เล่นต่อ cards ----
typedef struct {
	Entry* entry;
	char tag[32];
	bool resume; // has a resume state (the เล่นต่อ pill)
	char preview[MAX_PATH]; // its resume frame, "" for none (the picture, as สลับเกม shows it)
	int recent;				// its index in Recents (X ปิด)
} Card;
static Card cards[MAX_CARDS];
static int ncards;

static void freeCards(void) {
	for (int i = 0; i < ncards; i++)
		Entry_free(cards[i].entry);
	ncards = 0;
}

// ---- apps: every Tools pak (shipped + installed), plus เกม and Ports ----
typedef enum { APP_GAMES, APP_PORTS, APP_PAK } AppKind;
typedef struct {
	char id[64];	 // "games", "ports", or the pak's folder name ("Music Player")
	char name[64];	 // English key for I18N_text, or the pak name as is
	const char* icon;
	uint32_t color;
	AppKind kind;
	char path[MAX_PATH]; // the pak or the Ports folder
	bool fresh;			 // installed since the user last opened แอปทั้งหมด (the ใหม่ pill)
} App;
static App apps[MAX_APPS];
static int napps;
static int pins[MAX_APPS]; // indexes into apps, in the user's order
static int npins;

static const struct {
	const char* pak;
	const char* name;
	const char* icon;
	uint32_t color;
} KNOWN[] = {
	{"YouTube", "YouTube", "youtube", BOS_APP_YOUTUBE},
	{"Music Player", "Music", "music", BOS_APP_MUSIC},
	{"Media Player", "Video", "video", BOS_APP_VIDEO},
	{"Settings", "Settings", "settings", BOS_APP_SETTINGS},
	{"Files", "Files", "folder", BOS_APP_FILES},
	{"Image Viewer", "Images", "image", BOS_APP_IMAGES},
	{"RetroAchievements", "Trophies", "trophy", BOS_APP_TROPHIES},
	{"Game Tracker", "Stats", "chart", BOS_APP_STATS},
	{"Device Sync", "Sync", "sync", BOS_APP_SYNC},
	{"Xtras", "Xtras", "spark", BOS_APP_XTRAS},
	{"Emulator Settings", "Emulator Settings", "sliders", BOS_APP_EMUSET},
	{"Artwork Manager", "Artwork", "palette", BOS_APP_ARTWORK},
	{"PortMaster", "PortMaster", "box", BOS_APP_PORTMASTER},
};

#define PINS_PATH USERDATA_PATH "/bos_home_pins.txt"
#define SEEN_PATH USERDATA_PATH "/bos_apps_seen.txt"
static const char* DEFAULT_PINS[] = {"games", "YouTube", "Music Player", "Media Player", "ports", "Settings"};

static int findApp(const char* id) {
	for (int i = 0; i < napps; i++)
		if (!strcmp(apps[i].id, id))
			return i;
	return -1;
}

static bool lineInFile(const char* path, const char* s) {
	FILE* f = fopen(path, "r");
	if (!f)
		return false;
	char line[128];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		found = !strcmp(line, s);
	}
	fclose(f);
	return found;
}

static void addPak(const char* dir, const char* file, bool seen_known) {
	if (napps >= MAX_APPS)
		return;
	size_t n = strlen(file);
	if (n < 5 || strcasecmp(file + n - 4, ".pak"))
		return;
	char id[64];
	snprintf(id, sizeof(id), "%.*s", (int)(n - 4), file);
	if (findApp(id) >= 0)
		return;
	App* a = &apps[napps];
	memset(a, 0, sizeof(*a));
	snprintf(a->id, sizeof(a->id), "%s", id);
	snprintf(a->path, sizeof(a->path), "%s/%s", dir, file);
	a->kind = APP_PAK;
	a->icon = "box";
	a->color = BOS_C_SECONDARY;
	snprintf(a->name, sizeof(a->name), "%s", id);
	for (size_t k2 = 0; k2 < sizeof(KNOWN) / sizeof(KNOWN[0]); k2++)
		if (!strcmp(KNOWN[k2].pak, id)) {
			snprintf(a->name, sizeof(a->name), "%s", KNOWN[k2].name);
			a->icon = KNOWN[k2].icon;
			a->color = KNOWN[k2].color;
		}
	a->fresh = seen_known && !lineInFile(SEEN_PATH, id);
	napps++;
}

static void scanPaks(const char* dir, bool seen_known) {
	DIR* d = opendir(dir);
	if (!d)
		return;
	struct dirent* e;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.')
			addPak(dir, e->d_name, seen_known);
	closedir(d);
}

static void buildApps(void) {
	napps = 0;
	App* g = &apps[napps++];
	memset(g, 0, sizeof(*g));
	snprintf(g->id, sizeof(g->id), "games");
	snprintf(g->name, sizeof(g->name), "Games");
	g->icon = "game";
	g->color = BOS_APP_GAMES;
	g->kind = APP_GAMES;

	App* p = &apps[napps++];
	memset(p, 0, sizeof(*p));
	snprintf(p->id, sizeof(p->id), "ports");
	snprintf(p->name, sizeof(p->name), "Ports");
	p->icon = "ports";
	p->color = BOS_APP_PORTS;
	p->kind = APP_PORTS;
	DIR* d = opendir(ROMS_PATH);
	if (d) {
		struct dirent* e;
		while ((e = readdir(d)))
			if (strstr(e->d_name, "(PORTS)")) {
				snprintf(p->path, sizeof(p->path), "%s/%s", ROMS_PATH, e->d_name);
				break;
			}
		closedir(d);
	}

	bool seen_known = exists((char*)SEEN_PATH); // the very first boot marks nothing as new
	scanPaks(SDCARD_PATH "/.system/paks/Tools", seen_known);
	scanPaks(TOOLS_PATH, seen_known); // user-installed (PortMaster …), as the Tools tab reads them

	npins = 0;
	FILE* f = fopen(PINS_PATH, "r");
	if (f) {
		char line[128];
		while (npins < MAX_APPS && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			int i = findApp(line);
			if (i >= 0)
				pins[npins++] = i;
		}
		fclose(f);
	} else
		for (size_t i = 0; i < sizeof(DEFAULT_PINS) / sizeof(DEFAULT_PINS[0]); i++) {
			int a = findApp(DEFAULT_PINS[i]);
			if (a >= 0)
				pins[npins++] = a;
		}
}

static float gdim = 1.0f;		 // the เล่นต่อ block's alpha (0.35 while reordering)
static bool moving;				 // จัดเรียง: the focused pin is picked up
static int saved_pins[MAX_APPS]; // the order to restore on B
static int nsaved;
static bool lib;				 // แอปทั้งหมด is showing
static int lorder[MAX_APPS], nlorder, lfocus, ltop;

static void savePins(void) {
	FILE* f = fopen(PINS_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < npins; i++)
		fprintf(f, "%s\n", apps[pins[i]].id);
	fclose(f);
}

static void markSeen(void) { // every app now on the card is no longer "ใหม่"
	FILE* f = fopen(SEEN_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < napps; i++) {
		fprintf(f, "%s\n", apps[i].id);
		apps[i].fresh = false;
	}
	fclose(f);
}

static int pinIndex(int app) {
	for (int i = 0; i < npins; i++)
		if (pins[i] == app)
			return i;
	return -1;
}

static int byName(const void* a, const void* b) {
	return strcasecmp(I18N_text(apps[*(const int*)a].name), I18N_text(apps[*(const int*)b].name));
}

static void buildLibrary(void) { // pinned apps in pin order, then the rest A–Z
	nlorder = 0;
	for (int i = 0; i < npins; i++)
		lorder[nlorder++] = pins[i];
	int rest = nlorder;
	for (int i = 0; i < napps; i++)
		if (pinIndex(i) < 0)
			lorder[nlorder++] = i;
	qsort(lorder + rest, nlorder - rest, sizeof(int), byName);
}

// ---- state ----
static bool built;
static int row;		 // 0 the carousel, 1 the app row
static int cfocus;	 // carousel card
static int afocus;	 // app row tile (npins = ทั้งหมด)
static float cpos;	 // carousel position shown (tweens to cfocus)
static float cfrom;	 // tween start
static uint32_t cstart; // tween start time (ms), 0 = idle
static uint32_t menu_down; // when MENU went down (hold = switcher)
#define TWEEN_MS 180

static void build(void) {
	freeCards();
	Recents_load();
	for (int i = 0; i < Recents_count() && ncards < MAX_CARDS; i++) {
		Recent* r = Recents_at(i);
		if (!r || !r->available || suffixMatch((char*)".pak", r->path))
			continue;
		Entry* e = Recents_entryFromRecent(r);
		if (!e)
			continue;
		Card* c = &cards[ncards++];
		c->entry = e;
		c->recent = i;
		tagOf(e->path, c->tag, sizeof(c->tag));
		readyResume(e);
		c->resume = resume.can_resume;
		c->preview[0] = '\0';
		if (resume.can_resume && resume.has_preview)
			snprintf(c->preview, sizeof(c->preview), "%s", resume.preview_path);
	}
	buildApps();
	if (cfocus >= ncards)
		cfocus = ncards > 0 ? ncards - 1 : 0;
	cpos = cfrom = (float)cfocus;
	if (afocus > npins)
		afocus = npins;
	if (ncards == 0)
		row = 1;
	BOS_setMusicPlaying(musicPlaying());
	built = true;
}

bool HomeBos_active(void) {
	return Home_active() && CFG_getHomeStyle() == HOME_STYLE_GRID;
}

void HomeBos_reset(void) {
	built = false;
}

// ---- drawing ----
static float easeOut(float t) {
	return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// a colour drawn at `a` alpha over the black page
static uint32_t over(uint32_t c, float a) {
	return ((uint32_t)(((c >> 16) & 255) * a) << 16) | ((uint32_t)(((c >> 8) & 255) * a) << 8) | (uint32_t)((c & 255) * a);
}

static void cover(SDL_Surface* s, const Card* c, int x, int y, int w, int h, int r, float a, bool ring) {
	if (ring) { // picture focus: 4 px ring, offset 4 (§4)
		int o = BOS_px(BOS_RING), q = 2 * o;
		BOS_rrect(s, x - q, y - q, w + 2 * q, h + 2 * q, r + q, BOS_theme()->focus);
		BOS_rrect(s, x - o, y - o, w + 2 * o, h + 2 * o, r + o, BOS_theme()->bg);
	}
	SDL_Surface* pic = NULL;
	HomeArtState st = HomeArt_continue(c->entry->path, c->preview[0] ? c->preview : NULL, w, h, r, &pic);
	if (st == HOMEART_LOADING)
		pending = true;
	if (st == HOMEART_READY && pic) {
		SDL_SetSurfaceAlphaMod(pic, (Uint8)(255 * a * gdim));
		SDL_BlitSurface(pic, NULL, s, &(SDL_Rect){x, y});
		SDL_SetSurfaceAlphaMod(pic, 255);
	} else
		BOS_rrect(s, x, y, w, h, r, over(BOS_C_SURFACE, a * gdim));
}

static int sectionLabel(SDL_Surface* s, int y, const char* t) { // 30/600 text2, padding 8 40 10
	BOS_text(s, t, BOS_FS_SMALL, 600, BOS_C_TEXT2, 1, BOS_px(BOS_PAGE_PAD), y + BOS_px(8));
	return BOS_px(8) + BOS_lineH(BOS_FS_SMALL) + BOS_px(10);
}

static void pill(SDL_Surface* s, int x, int cy, const char* label, uint32_t dot) { // §5.4: 44 high, padding 0 18
	int h = BOS_px(44), d = dot ? BOS_px(12) : 0, gap = dot ? BOS_px(8) : 0;
	int w = 2 * BOS_px(18) + d + gap + BOS_textW(label, BOS_FS_BADGE, 600);
	BOS_rrect(s, x, cy - h / 2, w, h, h / 2, BOS_C_SURFACE2);
	if (dot)
		BOS_rrect(s, x + BOS_px(18), cy - d / 2, d, d, d / 2, dot);
	BOS_textMid(s, label, BOS_FS_BADGE, 600, BOS_C_TEXT, 1, x + BOS_px(18) + d + gap, cy);
}

static int pillW(const char* label, bool dot) {
	return 2 * BOS_px(18) + (dot ? BOS_px(20) : 0) + BOS_textW(label, BOS_FS_BADGE, 600);
}

// Home's carousel (§5.3 "home"): centre 346x260, sides 240x180 at .55, outer 180x135 at .25, gap 24
static int carousel(SDL_Surface* s, int top) {
	int cw = BOS_px(346), ch = BOS_px(260), sw = BOS_px(240), sh = BOS_px(180);
	int ow = BOS_px(180), oh = BOS_px(135), gap = BOS_px(24);
	float c1 = cw / 2.0f + gap + sw / 2.0f, c2 = cw / 2.0f + gap + sw + gap + ow / 2.0f; // slot centre offsets
	bool focused = row == 0;
	if (ncards == 0) {
		BOS_rrect(s, (s->w - cw) / 2, top, cw, ch, BOS_px(24), BOS_C_SURFACE);
		const char* t = I18N_text("Nothing to continue"); // ว่าง, large in the box
		BOS_textMid(s, t, BOS_FS_TITLE, 600, BOS_C_TEXTOFF, 1, (s->w - BOS_textW(t, BOS_FS_TITLE, 600)) / 2, top + ch / 2);
		return ch;
	}
	// back to front: the outer cards first so the centre draws on top
	for (int pass = 2; pass >= 0; pass--)
		for (int i = 0; i < ncards; i++) {
			float d = i - cpos;
			int slot = (int)(d < 0 ? -d + 0.5f : d + 0.5f);
			if (slot != pass)
				continue;
			float ad = d < 0 ? -d : d;
			float off = ad <= 1 ? ad * c1 : ad <= 2 ? c1 + (ad - 1) * (c2 - c1) : c2;
			if (ad > 2.6f)
				continue;
			int w = slot == 0 ? cw : slot == 1 ? sw : ow, h = slot == 0 ? ch : slot == 1 ? sh : oh;
			float a = slot == 0 ? 1.0f : slot == 1 ? 0.55f : 0.25f;
			int r = BOS_px(slot == 0 ? 24 : slot == 1 ? 20 : 18);
			int cx = s->w / 2 + (int)(d < 0 ? -off : off);
			cover(s, &cards[i], cx - w / 2, top + (ch - h) / 2, w, h, r, a, focused && slot == 0 && cstart == 0);
		}
	return ch;
}

#define CAPTION_H (BOS_px(14) + BOS_lineH(BOS_FS_BODY) + BOS_px(4) + BOS_px(44))
static int caption(SDL_Surface* s, int top) { // name 40/600, then "<console> · <when>" 30 text2 + pill
	if (ncards == 0)
		return CAPTION_H; // its space stays: the app row never moves
	const Card* c = &cards[cfocus];
	int y = top + BOS_px(14);
	char name[256];
	BOS_fit(c->entry->name, BOS_FS_BODY, 600, s->w - 2 * BOS_px(BOS_PAGE_PAD), name, sizeof(name));
	BOS_text(s, name, BOS_FS_BODY, 600, BOS_C_TEXT, gdim, (s->w - BOS_textW(name, BOS_FS_BODY, 600)) / 2, y);
	y += BOS_lineH(BOS_FS_BODY) + BOS_px(4);
	char meta[160];
	snprintf(meta, sizeof(meta), "%s", c->tag);
	GameInfo gi;
	if (GameInfo_get(c->entry->path, &gi)) {
		if (gi.has_time && gi.last_played > 0) {
			char when[64];
			GameInfo_whenText(time(NULL), gi.last_played, when, sizeof(when));
			snprintf(meta, sizeof(meta), c->tag[0] ? "%s · %s" : "%s%s", c->tag, when);
		}
	} else
		pending = true;
	const char* rl = I18N_text("Continue");
	int mw = BOS_textW(meta, BOS_FS_SMALL, 400), pw = c->resume ? BOS_px(16) + pillW(rl, true) : 0;
	int x = (s->w - mw - pw) / 2, cy = y + BOS_px(22);
	BOS_textMid(s, meta, BOS_FS_SMALL, 400, BOS_C_TEXT2, gdim, x, cy);
	if (c->resume && gdim == 1.0f)
		pill(s, x + mw + BOS_px(16), cy, rl, BOS_C_OK);
	return CAPTION_H;
}

// one app tile (§6.1): 112 radius 26, icon 64, name 30 under it in a 128 box; focused: white, x1.12; others at 60 %
static SDL_Rect appTile(SDL_Surface* s, int cx, int top, const char* icon, const char* label, uint32_t col, bool focused,
						bool dim, int label_box) {
	float k2 = focused ? BOS_FOCUS_SCALE : 1.0f;
	float a = dim ? BOS_DIM : 1.0f;
	int size = (int)(BOS_px(BOS_TILE) * k2 + 0.5f), r = (int)(BOS_px(BOS_TILE_RADIUS) * k2 + 0.5f);
	int ic = (int)(BOS_px(BOS_ICON) * k2 + 0.5f), box = (int)(label_box * k2 + 0.5f);
	int y = top + (BOS_px(BOS_TILE) - size) / 2; // grow about the tile's centre
	BOS_rrect(s, cx - size / 2, y, size, size, r, focused ? BOS_theme()->focus : over(BOS_C_SURFACE, a));
	BOS_icon(s, icon, focused ? BOS_theme()->onfocus : col, a, cx - ic / 2, y + (size - ic) / 2, ic);
	char t[96];
	BOS_fit(label, BOS_FS_SMALL, focused ? 600 : 500, box, t, sizeof(t));
	BOS_text(s, t, BOS_FS_SMALL, focused ? 600 : 500, BOS_C_TEXT, a, cx - BOS_textW(t, BOS_FS_SMALL, focused ? 600 : 500) / 2,
			 y + size + BOS_px(8));
	return (SDL_Rect){cx - size / 2, y, size, size};
}

static void appRow(SDL_Surface* s, int top) {
	int n = npins + (moving ? 0 : 1), cell = BOS_px(BOS_TILE + 16), gap = BOS_px(12);
	int total = n * cell + (n - 1) * gap, x0 = (s->w - total) / 2;
	int avail = s->w - 2 * BOS_px(BOS_PAGE_PAD);
	if (total > avail) { // more pins than fit: scroll so the focused tile stays on screen
		int fx = afocus * (cell + gap) + cell / 2;
		x0 = s->w / 2 - fx;
		int minx = s->w - BOS_px(BOS_PAGE_PAD) - total, maxx = BOS_px(BOS_PAGE_PAD);
		x0 = x0 < minx ? minx : x0 > maxx ? maxx : x0;
	}
	for (int i = 0; i < n; i++) {
		int cx = x0 + i * (cell + gap) + cell / 2;
		bool f = row == 1 && afocus == i;
		int box = BOS_px(BOS_TILE + 16 + 10); // the cell plus most of the gap
		if (i < npins) {
			const App* a = &apps[pins[i]];
			if (moving && f) { // picked up: lifted 12 px, < > beside it, nothing else moves (§6.1 จัดเรียง)
				SDL_Rect t = appTile(s, cx, top - BOS_px(12), a->icon, I18N_text(a->name), a->color, true, false, box);
				int ar = BOS_px(40), ay = t.y + t.h / 2 - ar / 2;
				BOS_icon(s, "chevl", BOS_C_TEXT, 1, t.x - ar + BOS_px(4), ay, ar);
				BOS_icon(s, "chev", BOS_C_TEXT, 1, t.x + t.w - BOS_px(4), ay, ar);
			} else
				appTile(s, cx, top, a->icon, I18N_text(a->name), a->color, f, !f, box);
		} else
			appTile(s, cx, top, "grid", I18N_text("All"), BOS_C_TEXT, f, !f, box);
	}
}

// แอปทั้งหมด (§6.1b): 5 columns (cell 176, tile 112), row gap 18, scrolls by rows
static void libraryRender(SDL_Surface* s) {
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_statusBar(s, I18N_text("Home"));
	char sub[128];
	snprintf(sub, sizeof(sub), I18N_text("%d apps · blue dot = pinned to Home"), napps);
	int y = BOS_px(BOS_BAR_H);
	y += BOS_largeTitle(s, y, I18N_text("All apps"), sub);
	int cols = 5, cell = BOS_px(176), pad = BOS_px(BOS_PAGE_PAD);
	int pitch = (s->w - 2 * pad - cell) / (cols - 1);
	int rowh = BOS_px(BOS_TILE + 8) + BOS_lineH(BOS_FS_SMALL) + BOS_px(18);
	int bottom = s->h - BOS_px(BOS_BAR_H), avail = bottom - y;
	int vis = avail / rowh > 0 ? avail / rowh : 1, frow = lfocus / cols;
	if (frow < ltop)
		ltop = frow;
	if (frow >= ltop + vis)
		ltop = frow - vis + 1;
	SDL_SetClipRect(s, &(SDL_Rect){0, y, s->w, avail});
	for (int i = 0; i < nlorder; i++) {
		int r = i / cols - ltop;
		if (r < 0 || r > vis)
			continue;
		const App* a = &apps[lorder[i]];
		bool f = i == lfocus;
		int cx = pad + cell / 2 + (i % cols) * pitch, top = y + BOS_px(10) + r * rowh;
		SDL_Rect t = appTile(s, cx, top, a->icon, I18N_text(a->name), a->color, f, !f, cell);
		if (pinIndex(lorder[i]) >= 0) { // pinned: a 14 px primary dot, 10 in from the top-right corner
			int d = BOS_px(14);
			BOS_rrect(s, t.x + t.w - BOS_px(10) - d, t.y + BOS_px(10), d, d, d / 2, BOS_theme()->primary);
		}
		if (a->fresh) { // ใหม่: 32 high, padding 0 10, primary, 22/700 white, 10 above / 14 past the corner
			const char* nl = I18N_text("New");
			int ph = BOS_px(32), pw = BOS_textW(nl, BOS_FS_HINT_WORD, 700) + 2 * BOS_px(10);
			int px = t.x + t.w + BOS_px(14) - pw, py = t.y - BOS_px(10);
			BOS_rrect(s, px, py, pw, ph, ph / 2, BOS_theme()->primary);
			BOS_textMid(s, nl, BOS_FS_HINT_WORD, 700, 0xFFFFFF, 1, px + BOS_px(10), py + ph / 2);
		}
	}
	SDL_SetClipRect(s, NULL);
	bool pinned = lfocus < nlorder && pinIndex(lorder[lfocus]) >= 0;
	BosHint r[] = {{"A", I18N_text("Open")}, {"B", I18N_text("Back")}, {"X", I18N_text(pinned ? "Unpin" : "Pin")}};
	BOS_hintBar(s, r, 3, NULL, 0);
}

void HomeBos_render(SDL_Surface* s) {
	if (!built)
		build();
	pending = false;
	if (lib) {
		libraryRender(s);
		return;
	}
	gdim = moving ? 0.35f : 1.0f;
	if (cstart) { // the carousel tween
		float t = (SDL_GetTicks() - cstart) / (float)TWEEN_MS;
		if (t >= 1) {
			t = 1;
			cstart = 0;
		}
		cpos = cfrom + (cfocus - cfrom) * easeOut(t);
	}
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_statusBar(s, I18N_text("Home"));
	int y = BOS_px(BOS_BAR_H);
	y += sectionLabel(s, y, I18N_text("Continue"));
	y += carousel(s, y);
	y += caption(s, y);
	appRow(s, y + BOS_px(14));
	if (row == 0) {
		BosHint r[] = {{"A", I18N_text("Continue")}, {"X", I18N_text("Close")}, {"Y", I18N_text("Search")}};
		BosHint l[] = {{"MENU", I18N_text("Options")}};
		BOS_hintBar(s, r, 3, l, ncards > 0 ? 1 : 0);
	} else if (moving) {
		BosHint r[] = {{"A", I18N_text("Done")}, {"B", I18N_text("Cancel")}};
		BosHint l[] = {{"\xE2\x86\x90 \xE2\x86\x92", I18N_text("Move")}};
		BOS_hintBar(s, r, 2, l, 1);
	} else if (afocus < npins) {
		BosHint r[] = {{"A", I18N_text("Open")}, {"X", I18N_text("Arrange")}, {"Y", I18N_text("Search")}};
		BOS_hintBar(s, r, 3, NULL, 0);
	} else {
		BosHint r[] = {{"A", I18N_text("Open")}, {"Y", I18N_text("Search")}};
		BOS_hintBar(s, r, 2, NULL, 0);
	}
}

bool HomeBos_animating(void) {
	return cstart != 0 || pending || HomeArt_checkAsyncLoaded() || GameInfo_checkAsyncLoaded();
}

// ---- input ----
static void openApp(int i, bool* dirty) {
	const App* a = &apps[i];
	if (a->kind == APP_GAMES) {
		GameList_openTab(MENU_TAB_CONSOLES, dirty);
		return;
	}
	if (!a->path[0] || !exists((char*)a->path))
		return;
	bool dir = a->kind == APP_PORTS;
	Entry* e = Entry_new((char*)a->path, dir ? ENTRY_DIR : ENTRY_PAK);
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

static void moveCarousel(int to) {
	if (to < 0 || to >= ncards || to == cfocus)
		return;
	cfrom = cpos;
	cfocus = to;
	cstart = SDL_GetTicks();
	if (!cstart)
		cstart = 1;
}

Entry* HomeBos_focusedGame(void) {
	return row == 0 && cfocus < ncards ? cards[cfocus].entry : NULL;
}

static HomeBosInput libraryInput(bool* dirty) {
	int cols = 5, f = lfocus;
	if (PAD_justRepeated(BTN_LEFT) && f > 0)
		f--;
	else if (PAD_justRepeated(BTN_RIGHT) && f < nlorder - 1)
		f++;
	else if (PAD_justRepeated(BTN_UP) && f - cols >= 0)
		f -= cols;
	else if (PAD_justRepeated(BTN_DOWN) && f < nlorder - 1)
		f = f + cols < nlorder ? f + cols : nlorder - 1;
	else if (PAD_justPressed(BTN_B)) {
		markSeen();
		lib = false;
	} else if (PAD_justPressed(BTN_X) && lfocus < nlorder) {
		int app = lorder[lfocus], at = pinIndex(app);
		if (at >= 0) {
			memmove(&pins[at], &pins[at + 1], (npins - at - 1) * sizeof(int));
			npins--;
		} else if (npins < MAX_APPS)
			pins[npins++] = app;
		savePins();
		buildLibrary();
		for (int i = 0; i < nlorder; i++) // stay on the same app
			if (lorder[i] == app)
				f = i;
		if (afocus > npins)
			afocus = npins;
	} else if (PAD_justPressed(BTN_A) && lfocus < nlorder) {
		markSeen();
		openApp(lorder[lfocus], dirty);
	}
	lfocus = f;
	*dirty = true;
	return HOMEBOS_HANDLED;
}

static HomeBosInput reorderInput(bool* dirty) {
	if (PAD_justRepeated(BTN_LEFT) && afocus > 0) {
		int t = pins[afocus];
		pins[afocus] = pins[afocus - 1];
		pins[--afocus] = t;
	} else if (PAD_justRepeated(BTN_RIGHT) && afocus < npins - 1) {
		int t = pins[afocus];
		pins[afocus] = pins[afocus + 1];
		pins[++afocus] = t;
	} else if (PAD_justPressed(BTN_A)) {
		moving = false;
		savePins();
	} else if (PAD_justPressed(BTN_B)) {
		int app = pins[afocus];
		memcpy(pins, saved_pins, nsaved * sizeof(int));
		npins = nsaved;
		afocus = pinIndex(app) >= 0 ? pinIndex(app) : 0;
		moving = false;
	}
	*dirty = true;
	return HOMEBOS_HANDLED;
}

HomeBosInput HomeBos_handleInput(unsigned long now, bool* dirty, bool tap_menu) {
	if (!built)
		build();
	if (lib)
		return libraryInput(dirty);
	if (moving)
		return reorderInput(dirty);
	// MENU: a tap on a game = its options (§6.10); held 0.5 s = the Game Switcher
	if (PAD_justPressed(BTN_MENU))
		menu_down = (uint32_t)now;
	if (menu_down && PAD_isPressed(BTN_MENU) && (uint32_t)now - menu_down >= 500) {
		menu_down = 0;
		return HOMEBOS_SWITCHER;
	}
	if (!PAD_isPressed(BTN_MENU))
		menu_down = 0;
	if (tap_menu)
		return HomeBos_focusedGame() ? HOMEBOS_OPTIONS : HOMEBOS_HANDLED;
	if (PAD_justPressed(BTN_Y))
		return HOMEBOS_SEARCH;
	if (PAD_justPressed(BTN_L1) || PAD_justPressed(BTN_R1))
		return HOMEBOS_HANDLED; // no tabs on Home
	if (PAD_justRepeated(BTN_LEFT)) {
		if (row == 0)
			moveCarousel(cfocus - 1);
		else if (afocus > 0)
			afocus--;
	} else if (PAD_justRepeated(BTN_RIGHT)) {
		if (row == 0)
			moveCarousel(cfocus + 1);
		else if (afocus < npins)
			afocus++;
	} else if (PAD_justPressed(BTN_UP)) {
		if (ncards > 0)
			row = 0;
	} else if (PAD_justPressed(BTN_DOWN)) {
		row = 1;
	} else if (PAD_justPressed(BTN_X) && row == 0 && cfocus < ncards) {
		// ปิด: the game leaves เล่นต่อ and สลับเกม (its save stays), as สลับเกม's X
		Recents_removeAt(cards[cfocus].recent);
		build();
		cfrom = cpos = (float)cfocus;
		cstart = 0;
	} else if (PAD_justPressed(BTN_X) && row == 1 && afocus < npins) {
		memcpy(saved_pins, pins, npins * sizeof(int));
		nsaved = npins;
		moving = true;
	} else if (PAD_justPressed(BTN_A)) {
		if (row == 0 && cfocus < ncards) {
			Entry* e = cards[cfocus].entry;
			readyResume(e);
			resume.should_resume = resume.can_resume;
			MenuTabs_markHomeLaunch();
			Entry_open(e);
		} else if (row == 1) {
			if (afocus < npins)
				openApp(pins[afocus], dirty);
			else {
				buildLibrary();
				lib = true;
				lfocus = ltop = 0;
			}
		}
		*dirty = true;
		return HOMEBOS_HANDLED;
	} else
		return HOMEBOS_UNHANDLED;
	*dirty = true;
	return HOMEBOS_HANDLED;
}

void HomeBos_quit(void) {
	freeCards();
	BOS_quit();
	built = false;
}

// ---- สลับเกม (DESIGN.md 6.5): the เกม cover carousel over the open games ----
static int sw_last_sel = -1, sw_last_count = -1;
static float sw_shift, sw_from;
static uint32_t sw_start;

static void switcherCard(SDL_Surface* s, const BosSwitchCard* c, int x, int y, int w, int h, int r, float a, bool ring) {
	if (ring) {
		int o = BOS_px(BOS_RING), q = 2 * o;
		BOS_rrect(s, x - q, y - q, w + 2 * q, h + 2 * q, r + q, BOS_theme()->focus);
		BOS_rrect(s, x - o, y - o, w + 2 * o, h + 2 * o, r + o, BOS_theme()->bg);
	}
	SDL_Surface* pic = NULL;
	HomeArtState st = HomeArt_continue(c->path, c->preview[0] ? c->preview : NULL, w, h, r, &pic);
	if (st == HOMEART_LOADING)
		pending = true;
	if (st == HOMEART_READY && pic) {
		SDL_SetSurfaceAlphaMod(pic, (Uint8)(255 * a));
		SDL_BlitSurface(pic, NULL, s, &(SDL_Rect){x, y});
		SDL_SetSurfaceAlphaMod(pic, 255);
	} else {
		BOS_rrect(s, x, y, w, h, r, over(BOS_C_SURFACE, a));
		int ic = h / 3;
		BOS_icon(s, "game", BOS_C_TEXTOFF, a, x + (w - ic) / 2, y + (h - ic) / 2, ic);
	}
}

void BosSwitcher_render(SDL_Surface* s, const BosSwitchCard* const cards[5], int sel, int count, bool playing,
						time_t saved_at, bool from_game) {
	pending = false;
	// the tween: a step of one (either way round) slides the row; anything else (a removal, a reopen) just cuts
	if (count != sw_last_count)
		sw_start = 0, sw_shift = 0;
	else if (sel != sw_last_sel && count > 1) {
		int d = count == 2 ? (sel > sw_last_sel ? 1 : count - 1) : ((sel - sw_last_sel) % count + count) % count;
		if (d == 1 || d == count - 1) {
			sw_from = (d == 1 ? 1.0f : -1.0f) + sw_shift; // where the new centre is drawn now
			sw_start = SDL_GetTicks();
			if (!sw_start)
				sw_start = 1;
		} else
			sw_start = 0, sw_shift = 0;
	}
	sw_last_sel = sel, sw_last_count = count;
	if (sw_start) {
		float t = (SDL_GetTicks() - sw_start) / (float)TWEEN_MS;
		if (t >= 1)
			t = 1, sw_start = 0;
		sw_shift = sw_from * (1 - easeOut(t));
	} else
		sw_shift = 0;

	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_statusBar(s, I18N_text("Switch game"));
	char sub[96];
	snprintf(sub, sizeof(sub), I18N_text("%d games · Recent first"), count);
	int top = BOS_px(BOS_BAR_H);
	top += BOS_largeTitle(s, top, I18N_text("Open games"), sub);

	BosHint r[] = {{"A", I18N_text("Play this game")}, {"B", I18N_text(from_game ? "Back to game" : "Back")}, {"X", I18N_text("Close")}};
	if (count <= 0) {
		const char* t = I18N_text("No open games");
		BOS_text(s, t, BOS_FS_BODY, 400, BOS_C_TEXT2, 1, (s->w - BOS_textW(t, BOS_FS_BODY, 400)) / 2, top + BOS_px(120));
		BOS_hintBar(s, r + 1, 1, NULL, 0);
		return;
	}

	// the same slots as เกม's covers (§6.3): 500x375 | 300x225 | 225x169, gap 28
	int cw = BOS_px(500), ch = BOS_px(375), sw = BOS_px(300), sh = BOS_px(225);
	int ow = sw * 3 / 4, oh = sh * 3 / 4, gap = BOS_px(28);
	float c1 = cw / 2.0f + gap + sw / 2.0f, c2 = cw / 2.0f + gap + sw + gap + ow / 2.0f;
	for (int pass = 2; pass >= 0; pass--)
		for (int i = 0; i < 5; i++) {
			if (!cards[i])
				continue;
			float d = (i - 2) + sw_shift, ad = fabsf(d);
			int slot = (int)(ad + 0.5f);
			if (slot != pass || ad > 2.6f)
				continue;
			float off = ad <= 1 ? ad * c1 : ad <= 2 ? c1 + (ad - 1) * (c2 - c1) : c2;
			int w = slot == 0 ? cw : slot == 1 ? sw : ow, h = slot == 0 ? ch : slot == 1 ? sh : oh;
			float a = slot == 0 ? 1.0f : slot == 1 ? 0.5f : 0.25f;
			int rr = BOS_px(slot == 0 ? 24 : slot == 1 ? 20 : 18);
			int cx = s->w / 2 + (int)(d < 0 ? -off : off);
			switcherCard(s, cards[i], cx - w / 2, top + (ch - h) / 2, w, h, rr, a, slot == 0 && !sw_start);
		}

	// caption: name; "<console> · บันทึกเมื่อ <time>" and the กำลังเล่น pill
	const BosSwitchCard* c = cards[2];
	int y = top + ch + BOS_px(14);
	char name[256];
	BOS_fit(c->name, BOS_FS_BODY, 600, s->w - 2 * BOS_px(BOS_PAGE_PAD), name, sizeof(name));
	BOS_text(s, name, BOS_FS_BODY, 600, BOS_C_TEXT, 1, (s->w - BOS_textW(name, BOS_FS_BODY, 600)) / 2, y);
	y += BOS_lineH(BOS_FS_BODY) + BOS_px(4);
	char tag[32], meta[160];
	tagOf(c->path, tag, sizeof(tag));
	if (saved_at > 0) {
		char t[16], saved[96];
		time_t now = time(NULL);
		struct tm a = *localtime(&saved_at), b = *localtime(&now);
		strftime(t, sizeof(t), a.tm_yday == b.tm_yday && a.tm_year == b.tm_year ? "%H:%M" : "%d/%m %H:%M", &a);
		snprintf(saved, sizeof(saved), I18N_text("Saved at %s"), t);
		snprintf(meta, sizeof(meta), tag[0] ? "%s · %s" : "%s%s", tag, saved);
	} else
		snprintf(meta, sizeof(meta), "%s", tag);
	const char* pl = I18N_text("Playing");
	int mw = BOS_textW(meta, BOS_FS_SMALL, 400), pw = playing ? (meta[0] ? BOS_px(16) : 0) + pillW(pl, false) : 0;
	int x = (s->w - mw - pw) / 2, cy = y + BOS_px(22);
	BOS_textMid(s, meta, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, x, cy);
	if (playing)
		pill(s, x + mw + (meta[0] ? BOS_px(16) : 0), cy, pl, 0);

	BOS_hintBar(s, r, 3, NULL, 0);
}

bool BosSwitcher_animating(void) {
	return pending || sw_start != 0;
}

#include "bos_games.inc"
