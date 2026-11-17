// Brick OS Home (DESIGN.md 6.1, the mockup "Home" artboard at 1024x768): status bar, the เล่นต่อ row of up to three
// recent games, a 3x2 grid of tool tiles and the hint bar. Drawn whole by this file when Layouts > Home layout is
// Grid (the Brick OS default); home.c hands over its render and input. Every size, colour and font here is the
// mockup's; px are the 1024x768 panel's (FIXED_WIDTH/HEIGHT), scaled by the screen's width when it differs.

#include "bos_apps.h"
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
#include "ui/ui_contextmenu.h"
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
#include "bos_keys.h"

// ---- ปุ่มพิเศษ pick mode (DESIGN §6.9b): Settings asked for an app, a console or a game for one of its keys ----
enum { PICK_NONE, PICK_APP, PICK_CONSOLE, PICK_GAME };
static int pick_kind;
static char pick_slot[32], pick_title[160];
static void pickDone(const char* value, const char* label);
static void barTitle(SDL_Surface* s, const char* title);

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
		f = weight >= 600 ? TTF_OpenFont(RES_PATH "/font1-thai-semibold.ttf", px) : NULL; // a real SemiBold (bos_ui.c)
		if (!f)
			f = TTF_OpenFont(GFX_getThaiFontPath(), px);
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
	return BOS_textW(s, px, weight); // kept between frames by the kit
}
static int lineH(int px) {
	TTF_Font* f = fontFor(px, 400, false);
	return f ? TTF_FontHeight(f) : px;
}
static void text(SDL_Surface* dst, const char* s, int px, int weight, uint32_t c, float a, int x, int y) {
	BOS_text(dst, s, px, weight, c, a, x, y); // the kit keeps the drawn string: FreeType ran for every string every frame
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

// ---- apps: the shared list and pins (bos_apps.c; Settings › แอปที่ปักหมุด edits the same file) ----
typedef BosApp App;
#define apps bos_apps
#define napps bos_napps
#define pins bos_pins
#define npins bos_npins
#define MAX_APPS BOS_APPS_MAX
#define APP_GAMES BOS_APP_KIND_GAMES
#define APP_PORTS BOS_APP_KIND_PORTS
#define findApp BosApps_find
#define pinIndex BosApps_pinIndex
#define savePins BosApps_savePins
#define markSeen BosApps_markSeen
#define buildApps BosApps_build

static bool lineInFile(const char* path, const char* s) { // bos_games.inc: is this exact line in the file
	FILE* f = fopen(path, "r");
	if (!f)
		return false;
	char line[1024];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		found = !strcmp(line, s);
	}
	fclose(f);
	return found;
}

static float gdim = 1.0f;		 // the เล่นต่อ block's alpha (0.35 while reordering)
static bool moving;				 // จัดเรียง: the focused pin is picked up
static int saved_pins[MAX_APPS]; // the order to restore on B
static int nsaved;
static bool lib;				 // แอปทั้งหมด is showing
static int lorder[MAX_APPS], nlorder, lfocus, ltop;

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
static float cpos, cvel; // carousel position shown and its speed (springStep to cfocus)
static uint32_t cstart, clast; // moving (non-zero) / the last step's time
static uint32_t menu_down; // when MENU went down (hold = switcher)
#define TWEEN_MS BOS_tweenMs() // 180 ms, 120 on น้อย, a cut on ปิด

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
	cpos = (float)cfocus;
	if (afocus > npins)
		afocus = npins;
	if (ncards == 0)
		row = 1;
	BOS_setMusicPlaying(musicPlaying());
	built = true;
}

// the picture a launch grows from (BOS_launchOut): the focused cover or tile of the page last drawn
static SDL_Rect launch_rect;
const SDL_Rect* HomeBos_launchRect(void) {
	return launch_rect.w > 0 ? &launch_rect : NULL;
}
bool HomeBos_pending(void) {
	return pending;
}

int HomeBos_page(void) {
	return lib ? 2 : 1;
}

bool HomeBos_active(void) {
	return Home_active() && CFG_getHomeStyle() == HOME_STYLE_GRID;
}

void HomeBos_reset(void) {
	built = false;
}

// ---- drawing ----
// a colour drawn at `a` alpha over the black page
static uint32_t over(uint32_t c, float a) { // c at alpha a over the page (พื้นหลัง: black or dark grey)
	uint32_t bg = BOS_theme()->bg, o = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		float x = (c >> sh) & 255, b = (bg >> sh) & 255;
		o |= (uint32_t)(b + (x - b) * a + 0.5f) << sh;
	}
	return o;
}

// a centred name on one line: centred when it fits, else scrolling across the page's width (BOS_textMarquee)
static void centredName(SDL_Surface* s, const char* nm, int px, int weight, uint32_t rgb, float a, int y) {
	int room = s->w - 2 * BOS_px(BOS_PAGE_PAD), w = BOS_textW(nm, px, weight);
	if (w <= room)
		BOS_text(s, nm, px, weight, rgb, a, (s->w - w) / 2, y);
	else
		BOS_textMarquee(s, nm, px, weight, rgb, a, BOS_px(BOS_PAGE_PAD), y + BOS_lineH(px) / 2, room);
}

static int sectionLabel(SDL_Surface* s, int y, const char* t) { // 30/600 text2, padding 8 40 10
	BOS_text(s, t, BOS_FS_SMALL, 600, BOS_C_TEXT2, 1, BOS_px(BOS_PAGE_PAD), y + BOS_px(8));
	return BOS_px(8) + BOS_lineH(BOS_FS_SMALL) + BOS_px(10);
}

static void pill(SDL_Surface* s, int x, int cy, const char* label, uint32_t dot) { // §5.4: 44 high, padding 0 18
	int h = BOS_px(44), d = dot ? BOS_px(12) : 0, gap = dot ? BOS_px(8) : 0;
	int w = 2 * BOS_px(18) + d + gap + BOS_textW(label, BOS_FS_BADGE, 600);
	BOS_rrect(s, x, cy - h / 2, w, h, h / 2, BOS_theme()->surface2);
	if (dot)
		BOS_rrect(s, x + BOS_px(18), cy - d / 2, d, d, d / 2, dot);
	BOS_textMid(s, label, BOS_FS_BADGE, 600, BOS_C_TEXT, 1, x + BOS_px(18) + d + gap, cy);
}

static int pillW(const char* label, bool dot) {
	return 2 * BOS_px(18) + (dot ? BOS_px(20) : 0) + BOS_textW(label, BOS_FS_BADGE, 600);
}

// ---- the cover carousels: เล่นต่อ (§5.3), เกม's wheel and covers (§6.2, 6.3), สลับเกม (§6.5) ----
// They move like YouTube's carousel (springStep), and every size, place and fade follows the exact distance from the
// middle. Each picture is made once, at the centre size; the GPU draws it as a sprite, scaled and darkened there, so
// a frame of motion leaves the page under it as it was and costs the CPU almost nothing. Under a context menu (drawn
// into the page, which the sprites would cover) and for the frame a picture's texture is made, the picture is scaled
// into the page instead (liveBlit).
static int car_tex_made; // textures made this frame: one a frame (a big one costs a few ms)

// src scaled to w x h at (x, y) straight into the page (nearest pixel), its colour scaled by m/256 (faded over the
// black page): carPic's way when the GPU can't draw it.
static void liveBlit(SDL_Surface* src, SDL_Surface* s, int x, int y, int w, int h, uint32_t m) {
	if (!src || w <= 0 || h <= 0 || src->format->format != SDL_PIXELFORMAT_ARGB8888 ||
		s->format->format != SDL_PIXELFORMAT_ARGB8888) {
		if (src && w > 0 && h > 0)
			SDL_BlitScaled(src, NULL, s, &(SDL_Rect){x, y, w, h});
		return;
	}
	SDL_Rect clip;
	SDL_GetClipRect(s, &clip);
	int x0 = x > clip.x ? x : clip.x, y0 = y > clip.y ? y : clip.y;
	int x1 = x + w < clip.x + clip.w ? x + w : clip.x + clip.w, y1 = y + h < clip.y + clip.h ? y + h : clip.y + clip.h;
	if (x1 <= x0 || y1 <= y0)
		return;
	uint32_t fx = ((uint32_t)src->w << 16) / (uint32_t)w, fy = ((uint32_t)src->h << 16) / (uint32_t)h;
	if (m > 256)
		m = 256;
	for (int row = y0; row < y1; row++) {
		uint32_t sy = ((uint32_t)(row - y) * fy) >> 16;
		const uint32_t* sp = (const uint32_t*)((const uint8_t*)src->pixels + sy * src->pitch);
		uint32_t* dp = (uint32_t*)((uint8_t*)s->pixels + row * s->pitch);
		uint32_t sx = (uint32_t)(x0 - x) * fx + fx / 2;
		for (int i = x0; i < x1; i++, sx += fx) {
			uint32_t p = sp[sx >> 16], sa = p >> 24;
			if (!sa)
				continue;
			uint32_t prb = p & 0xFF00FF, pg = p & 0xFF00;
			if (m != 256)
				prb = ((prb * m) >> 8) & 0xFF00FF, pg = ((pg * m) >> 8) & 0xFF00;
			if (sa == 255) {
				dp[i] = 0xFF000000u | prb | pg;
				continue;
			}
			uint32_t d = dp[i], rb = d & 0xFF00FF, g = d & 0xFF00;
			rb = (rb + (((prb - rb) * sa) >> 8)) & 0xFF00FF;
			g = (g + (((pg - g) * sa) >> 8)) & 0xFF00;
			dp[i] = 0xFF000000u | rb | g;
		}
	}
}

static bool carSprites(void) {
	return !ContextMenu_isOpen();
}

// pic scaled into b, its colour times a (faded over the black page)
static void carPic(SDL_Surface* s, SDL_Surface* pic, SDL_Rect b, float a) {
	if (!pic || b.w <= 0 || b.h <= 0 || a <= 0)
		return;
	if (a > 1)
		a = 1;
	if (carSprites()) {
		if (pic->userdata || car_tex_made++ == 0) {
			SDL_Texture* t = PLAT_textureForSurface(pic);
			if (t) {
				PLAT_spriteAddShaded(t, NULL, &b, 255, (Uint8)(255 * a + 0.5f), NULL);
				return;
			}
		} else
			pending = true; // its texture is made next frame
	}
	liveBlit(pic, s, b.x, b.y, b.w, b.h, (uint32_t)(256 * a + 0.5f));
}

// A w x h rounded box (radius r) in rgb on clear, smooth edged: filled, or a ring th thick (th > 0). For the pieces
// drawn over the carousels' sprites (the focus ring, the held letter), which must be sprites too.
static SDL_Surface* carShape(int w, int h, int r, int th, uint32_t rgb) {
	SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!d)
		return NULL;
	SDL_SetSurfaceBlendMode(d, SDL_BLENDMODE_BLEND);
	float hw = w / 2.0f, hh = h / 2.0f, rad = fminf((float)r, fminf(hw, hh));
	for (int y = 0; y < h; y++) {
		uint32_t* row = (uint32_t*)((uint8_t*)d->pixels + y * d->pitch);
		float qy = fabsf(y + 0.5f - hh) - hh + rad;
		for (int x = 0; x < w; x++) {
			float qx = fabsf(x + 0.5f - hw) - hw + rad, mx = fmaxf(qx, 0), my = fmaxf(qy, 0);
			float dist = sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0) - rad; // < 0 inside
			float k = fminf(fmaxf(0.5f - dist, 0), 1);
			if (th > 0)
				k -= fminf(fmaxf(0.5f - dist - th, 0), 1);
			row[x] = k <= 0 ? 0 : ((uint32_t)(k * 255 + 0.5f) << 24) | (rgb & 0xFFFFFF);
		}
	}
	return d;
}

// The focus ring around box b (BOS_focusRing's), over the sprites: a sprite too, made once a size
static void carRing(SDL_Surface* s, SDL_Rect b, int r) {
	int q = 2 * BOS_px(BOS_RING);
	SDL_Rect o = {b.x - q, b.y - q, b.w + 2 * q, b.h + 2 * q};
	if (carSprites()) {
		static SDL_Surface* rs;
		static int rw, rh, rr;
		static uint32_t rc;
		uint32_t c = BOS_theme()->focus;
		if (!rs || rw != o.w || rh != o.h || rr != r || rc != c) {
			GFX_freeSurfaceAndTexture(rs);
			rs = carShape(o.w, o.h, r + q, BOS_px(BOS_RING), c);
			rw = o.w, rh = o.h, rr = r, rc = c;
		}
		SDL_Texture* t = rs ? PLAT_textureForSurface(rs) : NULL;
		if (t) {
			PLAT_spriteAdd(t, NULL, &o, 255, NULL);
			return;
		}
	}
	BOS_focusRing(s, b.x, b.y, b.w, b.h, r);
}

// a value at distance ad from the middle: v0 in the middle, v1 one step out, v2 two or more, straight between
static float byDist(float ad, float v0, float v1, float v2) {
	return ad <= 1 ? v0 + (v1 - v0) * ad : ad <= 2 ? v1 + (v2 - v1) * (ad - 1) : v2;
}

// A cover carousel's look: the centred picture cw x ch (radius r), one and two steps out its size times s1, s2 and
// its colour times a1, a2; the centre one step out off1 from the middle, the next off2 further.
typedef struct {
	int cw, ch, r;
	float s1, s2, a1, a2;
	int off1, off2;
} CarSpec;
static const CarSpec CAR_COVERS = {500, 375, 24, 0.58f, 0.42f, 0.5f, 0.25f, 360, 230};			  // เกม, สลับเกม (§6.3)
static const CarSpec CAR_HOME = {346, 260, 24, 240 / 346.0f, 180 / 346.0f, 0.55f, 0.25f, 317, 234}; // เล่นต่อ (§5.3)

// The box of the item d from the middle (cx0) of carousel k whose centred box starts at top; its radius, its colour
static SDL_Rect carBox(const CarSpec* k, int cx0, int top, float d, int* r, float* a) {
	float ad = fabsf(d), sc = byDist(ad, 1, k->s1, k->s2), off = ad <= 1 ? ad * k->off1 : k->off1 + (ad - 1) * k->off2;
	int cw = BOS_px(k->cw), ch = BOS_px(k->ch), w = (int)(cw * sc + 0.5f), h = (int)(ch * sc + 0.5f);
	int cx = cx0 + BOS_px((int)(d < 0 ? -off : off));
	*r = (int)(BOS_px(k->r) * sc + 0.5f);
	*a = byDist(ad, 1, k->a1, k->a2);
	return (SDL_Rect){cx - w / 2, top + (ch - h) / 2, w, h};
}

// The items lo..hi of a carousel at pos that show, in drawing order: the farthest first, the centred one last
static int carOrder(float pos, int lo, int hi, int* out, int max) {
	int n = 0;
	for (int i = (int)floorf(pos - 2.6f); i <= (int)ceilf(pos + 2.6f) && n < max; i++)
		if (i >= lo && i <= hi && fabsf(i - pos) <= 2.6f)
			out[n++] = i;
	for (int a = 0; a < n; a++)
		for (int b = a + 1; b < n; b++)
			if (fabsf(out[b] - pos) > fabsf(out[a] - pos)) {
				int t = out[a];
				out[a] = out[b];
				out[b] = t;
			}
	return n;
}

// The carousels move like YouTube's (DESIGN 6.2): a critically damped spring (SmoothDamp). A press moves the
// target; the position follows, keeping its speed, so a held direction glides on instead of starting over at every
// step. It settles in about 0.2 s at the default motion (BOS_tweenMs 180), quicker on น้อย, a cut on ปิด.
// *moving is non-zero until it has settled.
static void springStep(float* pos, float* vel, uint32_t* last, uint32_t* moving, float target) {
	if (!*moving)
		return;
	uint32_t now = SDL_GetTicks();
	float dt = *last ? (now - *last) / 1000.0f : 1 / 60.0f;
	*last = now;
	int ms = TWEEN_MS;
	if (ms <= 0) {
		*pos = target, *vel = 0, *moving = 0;
		return;
	}
	if (dt > 0.05f) // after a hitch
		dt = 0.05f;
	if (dt <= 0)
		return;
	float om = 2.0f / (ms * (0.20f / 180)), x = om * dt, ex = 1 / (1 + x + 0.48f * x * x + 0.235f * x * x * x);
	float chg = *pos - target, tmp = (*vel + om * chg) * dt;
	*vel = (*vel - om * tmp) * ex;
	*pos = target + (chg + tmp) * ex;
	if (fabsf(*pos - target) < 0.002f && fabsf(*vel) < 0.02f)
		*pos = target, *vel = 0, *moving = 0;
}

// a card of เล่นต่อ in box b at colour a: its picture made once at the centre size (cw x ch)
static void cover(SDL_Surface* s, const Card* c, SDL_Rect b, int r, float a, int cw, int ch) {
	SDL_Surface* pic = NULL;
	HomeArtState st = HomeArt_continue(c->entry->path, c->preview[0] ? c->preview : NULL, cw, ch, BOS_px(CAR_HOME.r), &pic);
	if (st == HOMEART_LOADING)
		pending = true;
	if (st == HOMEART_READY && pic)
		carPic(s, pic, b, a * gdim);
	else
		BOS_rrect(s, b.x, b.y, b.w, b.h, r, over(BOS_theme()->surface, a * gdim));
}

// Home's carousel (§5.3 "home"): CAR_HOME, centre 346x260, sides 240x180 at .55, outer 180x135 at .25, gap 24
static int carousel(SDL_Surface* s, int top) {
	int cw = BOS_px(CAR_HOME.cw), ch = BOS_px(CAR_HOME.ch);
	bool focused = row == 0;
	if (ncards == 0) {
		BOS_rrect(s, (s->w - cw) / 2, top, cw, ch, BOS_px(24), BOS_theme()->surface);
		const char* t = I18N_text("Nothing to continue"); // ว่าง, large in the box
		BOS_textMid(s, t, BOS_FS_TITLE, 600, BOS_C_TEXTOFF, 1, (s->w - BOS_textW(t, BOS_FS_TITLE, 600)) / 2, top + ch / 2);
		return ch;
	}
	int order[8], no = carOrder(cpos, 0, ncards - 1, order, 8);
	for (int k = 0; k < no; k++) { // the farthest first: the centred one over them all
		int r;
		float a, d = order[k] - cpos;
		SDL_Rect b = carBox(&CAR_HOME, s->w / 2 - (focused ? BOS_bumpPx() : 0), top, d, &r, &a);
		cover(s, &cards[order[k]], b, r, a, cw, ch);
		if (focused && fabsf(d) < 0.001f && cstart == 0)
			carRing(s, b, r); // last (DESIGN 1: draw order)
	}
	if (focused)
		launch_rect = (SDL_Rect){(s->w - cw) / 2, top, cw, ch};
	return ch;
}

#define CAPTION_H (BOS_px(14) + BOS_lineH(BOS_FS_BODY) + BOS_px(4) + BOS_px(44))
static int caption(SDL_Surface* s, int top) { // name 40/600, then "<console> · <when>" 30 text2 + pill
	if (ncards == 0)
		return CAPTION_H; // its space stays: the app row never moves
	const Card* c = &cards[cfocus];
	int y = top + BOS_px(14);
	char name[256];
	centredName(s, c->entry->name, BOS_FS_BODY, 600, BOS_C_TEXT, gdim, y);
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
	BOS_rrect(s, cx - size / 2, y, size, size, r, focused ? BOS_theme()->focus : over(BOS_theme()->surface, a));
	BOS_icon(s, icon, focused ? BOS_theme()->onfocus : BOS_iconColor(col), a, cx - ic / 2, y + (size - ic) / 2, ic);
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
		if (f && !moving)
			cx += BOS_bumpPx(); // a held direction at the row's end: the tile knocks against it
		int box = BOS_px(BOS_TILE + 16 + 10); // the cell plus most of the gap
		if (i < npins) {
			const App* a = &apps[pins[i]];
			if (moving && f) { // picked up: lifted 12 px, < > beside it, nothing else moves (§6.1 จัดเรียง)
				SDL_Rect t = appTile(s, cx, top - BOS_px(12), a->icon, I18N_text(a->name), a->color, true, false, box);
				int ar = BOS_px(40), ay = t.y + t.h / 2 - ar / 2;
				BOS_icon(s, "chevl", BOS_C_TEXT, 1, t.x - ar + BOS_px(4), ay, ar);
				BOS_icon(s, "chev", BOS_C_TEXT, 1, t.x + t.w - BOS_px(4), ay, ar);
			} else {
				SDL_Rect t = appTile(s, cx, top, a->icon, I18N_text(a->name), a->color, f, !f, box);
				if (f)
					launch_rect = t;
			}
		} else
			appTile(s, cx, top, "grid", I18N_text("All"), BOS_C_TEXT, f, !f, box);
	}
}

// แอปทั้งหมด (§6.1b): 5 columns (cell 176, tile 112), row gap 18, scrolls by rows
static void libraryRender(SDL_Surface* s) {
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	barTitle(s, I18N_text("Home"));
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
	if (pick_kind) {
		BosHint p[] = {{"A", I18N_text("Choose")}, {"B", I18N_text("Cancel")}};
		BOS_hintBar(s, p, 2, NULL, 0);
		return;
	}
	BosHint r[] = {{"A", I18N_text("Open")}, {"B", I18N_text("Back")}, {"X", I18N_text(pinned ? "Unpin" : "Pin")}};
	BOS_hintBar(s, r, 3, NULL, 0);
}

void HomeBos_render(SDL_Surface* s) {
	if (!built)
		build();
	pending = false;
	launch_rect = (SDL_Rect){0, 0, 0, 0};
	if (lib) {
		libraryRender(s);
		return;
	}
	gdim = moving ? 0.35f : 1.0f;
	car_tex_made = 0;
	springStep(&cpos, &cvel, &clast, &cstart, (float)cfocus); // the carousel's spring
	BOS_benchLap(NULL);
	// the row is on its way and nothing else moves: only its rows are drawn (the frame it lands on is a whole one)
	static int band_h;
	uint32_t key = BOS_keyMix(BOS_keyMix(BOS_keyMix(BOS_keyMix(11, row), afocus), npins), ncards > 0);
	bool part = BOS_partialBegin(s, key, row == 0 && !moving, 0, band_h);
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_benchLap("fill");
	BOS_statusBar(s, I18N_text("Home"));
	BOS_benchLap("status");
	int y = BOS_px(BOS_BAR_H);
	y += sectionLabel(s, y, I18N_text("Continue"));
	y += carousel(s, y);
	BOS_benchLap("carousel");
	y += caption(s, y);
	BOS_benchLap("caption");
	band_h = y;
	if (part) {
		BOS_partialEnd(s);
		return;
	}
	appRow(s, y + BOS_px(14));
	BOS_benchLap("approw");
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
	return cstart != 0 || pending || HomeArt_checkAsyncLoaded() || GameInfo_checkAsyncLoaded() || BOS_marqueeActive();
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
	cfocus = to;
	if (!cstart) // from rest: no old speed, no old time
		cvel = 0, clast = 0;
	cstart = 1;
}

// one step along เล่นต่อ: at an end a fresh press goes round, a held one knocks (§5.7)
static void carouselStep(int dir, int btn) {
	int to = cfocus + dir;
	if (to < 0 || to >= ncards) {
		if (ncards < 2 || !PAD_justPressed(btn)) {
			BOS_bump(dir);
			return;
		}
		to = to < 0 ? ncards - 1 : 0;
	}
	moveCarousel(to);
}

Entry* HomeBos_focusedGame(void) {
	return row == 0 && cfocus < ncards ? cards[cfocus].entry : NULL;
}

static HomeBosInput libraryInput(bool* dirty) {
	int cols = 5, f = lfocus;
	if (pick_kind == PICK_APP && (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B) || PAD_justPressed(BTN_X))) {
		if (PAD_justPressed(BTN_B))
			pickDone(NULL, NULL);
		else if (PAD_justPressed(BTN_A) && lfocus < nlorder) {
			const App* a = &apps[lorder[lfocus]];
			char v[96];
			snprintf(v, sizeof(v), "app:%s", a->id);
			pickDone(v, I18N_text(a->name));
		}
		*dirty = true;
		return HOMEBOS_HANDLED;
	}
	if (PAD_gridNav(&f, nlorder, cols) < 0)
		BOS_bump(PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_UP) ? -1 : 1);
	else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT) || PAD_justRepeated(BTN_UP) ||
			 PAD_justRepeated(BTN_DOWN))
		;
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
	if (pick_kind == PICK_APP && !lib) {
		buildLibrary();
		lib = true;
	}
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
			carouselStep(-1, BTN_LEFT);
		else if (afocus > 0)
			afocus--;
		else if (PAD_justPressed(BTN_LEFT)) // the app row wraps on a fresh press; a held key stops at the end
			afocus = npins;
		else
			BOS_bump(-1);
	} else if (PAD_justRepeated(BTN_RIGHT)) {
		if (row == 0)
			carouselStep(1, BTN_RIGHT);
		else if (afocus < npins)
			afocus++;
		else if (PAD_justPressed(BTN_RIGHT))
			afocus = 0;
		else
			BOS_bump(1);
	} else if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_DOWN)) { // two rows: past either end, the other one
		if (ncards > 0)
			row = !row;
		else
			row = 1;
	} else if (PAD_justPressed(BTN_X) && row == 0 && cfocus < ncards) {
		// ปิด: the game leaves เล่นต่อ and สลับเกม (its save stays), as สลับเกม's X
		Recents_removeAt(cards[cfocus].recent);
		build();
		cpos = (float)cfocus;
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
static float sw_shift, sw_vel; // where the centred card is drawn (0: in the middle) and its speed
static uint32_t sw_start, sw_last; // moving (non-zero) / the last step's time

static void switcherCard(SDL_Surface* s, const BosSwitchCard* c, SDL_Rect b, int r, float a, int cw, int ch) {
	SDL_Surface* pic = NULL;
	HomeArtState st = HomeArt_continue(c->path, c->preview[0] ? c->preview : NULL, cw, ch, BOS_px(CAR_COVERS.r), &pic);
	if (st == HOMEART_LOADING)
		pending = true;
	if (st == HOMEART_READY && pic) {
		carPic(s, pic, b, a);
	} else {
		BOS_rrect(s, b.x, b.y, b.w, b.h, r, over(BOS_theme()->surface, a));
		int ic = b.h / 3;
		BOS_icon(s, "game", BOS_C_TEXTOFF, a, b.x + (b.w - ic) / 2, b.y + (b.h - ic) / 2, ic);
	}
}

void BosSwitcher_render(SDL_Surface* s, const BosSwitchCard* const cards[5], int sel, int count, bool playing,
						time_t saved_at, bool from_game) {
	pending = false;
	car_tex_made = 0;
	// a step of one (either way round) glides the row on (the spring); anything else (a removal, a reopen) just cuts
	if (count != sw_last_count)
		sw_start = 0, sw_shift = 0, sw_vel = 0;
	else if (sel != sw_last_sel && count > 1) {
		int d = count == 2 ? (sel > sw_last_sel ? 1 : count - 1) : ((sel - sw_last_sel) % count + count) % count;
		if (d == 1 || d == count - 1) {
			sw_shift += d == 1 ? 1.0f : -1.0f; // the new centre is drawn where it was, and glides to the middle
			if (!sw_start)
				sw_vel = 0, sw_last = 0;
			sw_start = 1;
		} else
			sw_start = 0, sw_shift = 0, sw_vel = 0;
	}
	sw_last_sel = sel, sw_last_count = count;
	springStep(&sw_shift, &sw_vel, &sw_last, &sw_start, 0);

	// while the row glides, only its rows are drawn (the caption and the rest change with the selection: a key)
	static int band_y, band_h;
	uint32_t key = BOS_keyMix(BOS_keyMix(BOS_keyMix(BOS_keyMix(31, sel), count), (uint32_t)(time(NULL) / 60)), from_game);
	bool part = BOS_partialBegin(s, key, sw_start != 0 && band_h > 0, band_y, band_h);
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
		BOS_partialEnd(s);
		return;
	}

	// เกม's cover carousel (§6.3): cards[2] is the selected one, the others 1 and 2 steps either way
	int cw = BOS_px(CAR_COVERS.cw), ch = BOS_px(CAR_COVERS.ch);
	float pos = 2 - sw_shift;
	int order[5], no = carOrder(pos, 0, 4, order, 5);
	for (int k = 0; k < no; k++) {
		int i = order[k], rr;
		if (!cards[i])
			continue;
		float a, d = i - pos;
		SDL_Rect b = carBox(&CAR_COVERS, s->w / 2, top, d, &rr, &a);
		switcherCard(s, cards[i], b, rr, a, cw, ch);
		if (fabsf(d) < 0.001f && !sw_start)
			carRing(s, b, rr); // last (DESIGN 1: draw order)
	}
	band_y = top - 2 * BOS_px(BOS_RING), band_h = ch + 4 * BOS_px(BOS_RING);
	if (part) {
		BOS_partialEnd(s);
		return;
	}

	// caption: name; "<console> · บันทึกเมื่อ <time>" and the กำลังเล่น pill
	const BosSwitchCard* c = cards[2];
	int y = top + ch + BOS_px(14);
	char name[256];
	centredName(s, c->name, BOS_FS_BODY, 600, BOS_C_TEXT, 1, y);
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
	BOS_partialEnd(s);
}

bool BosSwitcher_animating(void) {
	return pending || sw_start != 0 || BOS_marqueeActive();
}

#include "bos_games.inc"

// ---- ปุ่มพิเศษ (DESIGN.md §6.9b) ----

// the status bar's left label: the page's name, or in pick mode the primary pill "เลือกให้" and what is being set
static void barTitle(SDL_Surface* s, const char* title) {
	if (!pick_kind) {
		BOS_statusBar(s, title);
		return;
	}
	BOS_statusBar(s, NULL);
	int cy = BOS_px(BOS_BAR_H) / 2, x = BOS_px(BOS_BAR_PAD);
	const char* pl = I18N_text("Pick for");
	int ph = BOS_px(40), pw = BOS_textW(pl, 26, 700) + 2 * BOS_px(14);
	BOS_rrect(s, x, cy - ph / 2, pw, ph, BOS_px(12), BOS_theme()->primary);
	BOS_textMid(s, pl, 26, 700, 0xFFFFFF, 1, x + BOS_px(14), cy);
	BOS_textMid(s, pick_title, BOS_FS_SMALL, 600, BOS_C_TEXT, 1, x + pw + BOS_px(14), cy);
}

// back to Settings with the answer (value NULL: cancelled); Settings reopens on the key's popup
static void pickDone(const char* value, const char* label) {
	char r[MAX_PATH * 2 + 64];
	if (value)
		snprintf(r, sizeof(r), "%s\tset\t%s\t%s", pick_slot, value, label ? label : "");
	else
		snprintf(r, sizeof(r), "%s\tcancel", pick_slot);
	BosKeys_writeFile(BOS_PICK_RESULT_PATH, r);
	unlink(BOS_PICK_PATH);
	pick_kind = PICK_NONE;
	lib = false;
	bg_step2 = false;
	if (!built)
		build();
	int i = findApp("Settings");
	if (i >= 0 && apps[i].path[0])
		openPakInPlace(apps[i].path);
}

bool BosPick_active(void) {
	return pick_kind != PICK_NONE;
}

// a pick Settings asked for (/tmp/bos_pick): starts on its screen; true while picking
bool BosPick_poll(bool* dirty) {
	if (pick_kind)
		return true;
	if (access(BOS_PICK_PATH, F_OK) != 0)
		return false;
	char line[MAX_PATH];
	BosKeys_readFile(BOS_PICK_PATH, line, sizeof(line));
	char* k = strchr(line, '\t');
	char* t = k ? strchr(k + 1, '\t') : NULL;
	if (!k || !t) {
		unlink(BOS_PICK_PATH);
		return false;
	}
	*k = *t = '\0';
	snprintf(pick_slot, sizeof(pick_slot), "%s", line);
	snprintf(pick_title, sizeof(pick_title), "%s", t + 1);
	pick_kind = !strcmp(k + 1, "app") ? PICK_APP : !strcmp(k + 1, "console") ? PICK_CONSOLE : PICK_GAME;
	while (stack->count > 1)
		closeDirectory();
	if (pick_kind == PICK_APP) {
		GameList_openTab(MENU_TAB_HOME, dirty);
		if (!built)
			build();
		buildLibrary();
		lib = true;
		lfocus = ltop = 0;
	} else {
		GameList_openTab(MENU_TAB_CONSOLES, dirty);
		bg_step2 = false;
	}
	*dirty = true;
	return true;
}

// an action keymon left (/tmp/bos_action): 0 none, 1 run, 2 the caller opens the Game Switcher
int BosAction_poll(bool* dirty) {
	if (access(BOS_ACTION_PATH, F_OK) != 0)
		return 0;
	char v[BOS_KEY_VAL];
	BosKeys_readFile(BOS_ACTION_PATH, v, sizeof(v));
	unlink(BOS_ACTION_PATH);
	if (pick_kind || !v[0])
		return 0;
	*dirty = true;
	if (!strcmp(v, "sys:switcher")) {
		GameSwitcher_resetSelection();
		return 2;
	}
	while (stack->count > 1)
		closeDirectory();
	if (!built)
		build();
	lib = false;
	if (!strcmp(v, "sys:home")) {
		GameList_openTab(MENU_TAB_HOME, dirty);
	} else if (!strncmp(v, "app:", 4)) {
		int i = findApp(v + 4);
		if (i >= 0)
			openApp(i, dirty);
	} else if (!strncmp(v, "console:", 8)) {
		GameList_openTab(MENU_TAB_CONSOLES, dirty);
		if (!bg_built)
			bgBuild();
		for (int i = 0; i < bg_ncons; i++)
			if (!strcmp(bg_cons[i].path, v + 8)) {
				bg_sel = i;
				bg_pos = (float)i;
				bg_start = 0;
				bg_games_for = -1;
				bg_step2 = true;
			}
	} else if (!strncmp(v, "game:", 5) && exists(v + 5)) {
		Entry* e = Entry_new(v + 5, ENTRY_ROM);
		if (e) {
			readyResume(e);
			resume.should_resume = resume.can_resume;
			Entry_open(e);
			Entry_free(e);
		}
	}
	return 1;
}
