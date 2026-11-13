// Brick OS motion (DESIGN.md §5.7): page changes, the arrival fade, the launch, the end-of-list nudge and the glide
// of a focus highlight. A page is drawn once, kept as a picture, and the pictures are moved: a full page costs
// 15 ms to draw on the Brick Pro, two copies of a picture cost 3.
#include "bos_ui.h"
#include "api.h"
#include "config.h"
#include "defines.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// ภาพเคลื่อนไหว: 0 เต็ม, 1 น้อย (fades only, 120 ms at most), 2 ปิด
static int motionMode(void) {
	return !CFG_getMenuAnimations() ? 2 : CFG_getMenuTransitions() ? 0 : 1;
}

float BOS_ease(float t) { // out-quart: quick off the mark, soft landing (the mockup's cubic-bezier(.2,.8,.2,1))
	if (t <= 0)
		return 0;
	if (t >= 1)
		return 1;
	float u = 1 - t;
	return 1 - u * u * u * u;
}

int BOS_motionMs(int full_ms) {
	static int slow; // BOS_SLOW=20 stretches every movement, to look at one on a screenshot
	if (!slow) {
		const char* e = getenv("BOS_SLOW");
		slow = e && atoi(e) > 0 ? atoi(e) : 1;
	}
	if (slow > 1)
		full_ms *= slow;
	int m = motionMode();
	if (slow > 1 && m == 0)
		return full_ms;
	return m == 2 ? 0 : m == 1 ? (full_ms < 120 ? full_ms : 120) : full_ms;
}

static bool busy_glide; // a glide was still on its way when it was last asked: one more frame is owed

// ---- page transitions ----
static SDL_Surface *tr_from, *tr_to;
static SDL_Rect tr_area;
static BosTrans tr_kind;
static int tr_state; // 0 idle, 1 waiting for the new page's first frame, 2 moving
static uint32_t tr_t0, tr_begun;
static int tr_ms;
static bool tr_gpu; // the pictures are textures, moved by the compositor (BOS_SWTRANS=1: the old way, to compare)

static bool gpuMoves(void) {
	static int sw = -1;
	if (sw < 0)
		sw = getenv("BOS_SWTRANS") != NULL;
	return !sw && PLAT_pageGrab(NULL);
}

static SDL_Surface* keep(SDL_Surface** slot, SDL_Surface* s, const SDL_Rect* area) {
	if (*slot && ((*slot)->w != area->w || (*slot)->h != area->h)) {
		SDL_FreeSurface(*slot);
		*slot = NULL;
	}
	if (!*slot)
		*slot = SDL_CreateRGBSurfaceWithFormat(0, area->w, area->h, 32, SDL_PIXELFORMAT_RGB888); // no alpha: fades by surface alpha
	if (*slot && s) {
		SDL_BlendMode was;
		SDL_GetSurfaceBlendMode(s, &was);
		SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
		SDL_BlitSurface(s, (SDL_Rect*)area, *slot, NULL);
		SDL_SetSurfaceBlendMode(s, was);
	}
	return *slot;
}

static void put(SDL_Surface* src, SDL_Surface* s, int x, int y, float light, float alpha) {
	int c = (int)(255 * light);
	SDL_SetSurfaceColorMod(src, c, c, c);
	if (alpha < 1) {
		SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_BLEND);
		SDL_SetSurfaceAlphaMod(src, (Uint8)(255 * alpha));
	} else {
		SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
		SDL_SetSurfaceAlphaMod(src, 255);
	}
	SDL_BlitSurface(src, NULL, s, &(SDL_Rect){x, y, src->w, src->h});
}

void BOS_transBegin(SDL_Surface* s, BosTrans kind, const SDL_Rect* area) {
	int full = kind == BOS_TR_FADE || kind == BOS_TR_ARRIVE ? 200 : 240;
	tr_ms = BOS_motionMs(full);
	tr_state = 0;
	if (tr_ms <= 0 || !s)
		return;
	tr_area = area ? *area : (SDL_Rect){0, 0, s->w, s->h};
	tr_gpu = gpuMoves() && (kind == BOS_TR_ARRIVE || PLAT_pageGrab(&tr_area));
	if (!tr_gpu) {
		if (!keep(&tr_from, kind == BOS_TR_ARRIVE ? NULL : s, &tr_area))
			return;
		if (kind == BOS_TR_ARRIVE)
			SDL_FillRect(tr_from, NULL, 0);
	}
	if (kind != BOS_TR_ARRIVE && motionMode() == 1)
		kind = BOS_TR_FADE; // น้อย: nothing slides
	tr_kind = kind;
	tr_state = 1;
	tr_begun = SDL_GetTicks();
}

// The new page was drawn on s but a picture of it is still loading: the old page goes back on s and the move waits,
// max_ms at most (real time). False: the wait is over, call BOS_transFrame.
bool BOS_transHold(SDL_Surface* s, int max_ms) {
	if (tr_state != 1 || !s || SDL_GetTicks() - tr_begun >= (uint32_t)max_ms)
		return false;
	if (tr_gpu) {
		PLAT_pageShow(&tr_area, tr_kind == BOS_TR_ARRIVE, false, 0, 1, 0, 1, 0);
		return true;
	}
	if (!tr_from)
		return false;
	put(tr_from, s, tr_area.x, tr_area.y, 1, 1);
	return true;
}

bool BOS_transMoving(void) {
	return tr_state == 2;
}
bool BOS_transActive(void) {
	return tr_state != 0;
}
void BOS_transCancel(void) {
	tr_state = 0;
}

// After the new page is drawn (or instead of drawing it, while BOS_transMoving): puts the transition's frame on s.
// True while there is more to come.
bool BOS_transFrame(SDL_Surface* s) {
	if (!tr_state || !s)
		return false;
	if (tr_state == 1) {
		if (!tr_gpu && !keep(&tr_to, s, &tr_area)) {
			tr_state = 0;
			return false;
		}
		tr_state = 2;
		tr_t0 = SDL_GetTicks();
	}
	float t = (float)(SDL_GetTicks() - tr_t0) / (float)tr_ms;
	if (t >= 1) {
		if (!tr_gpu)
			put(tr_to, s, tr_area.x, tr_area.y, 1, 1);
		tr_state = 0;
		return false;
	}
	float e = BOS_ease(t);
	int w = tr_area.w;
	if (tr_gpu) { // s holds the new page, and so does its texture: the compositor moves the two
		if (tr_kind == BOS_TR_PUSH)
			PLAT_pageShow(&tr_area, false, false, -(int)(w * 0.3f * e), 1 - 0.65f * e, (int)(w * (1 - e)), 1, 1);
		else if (tr_kind == BOS_TR_POP)
			PLAT_pageShow(&tr_area, false, true, (int)(w * e), 1, -(int)(w * 0.3f * (1 - e)), 0.35f + 0.65f * e, 1);
		else
			PLAT_pageShow(&tr_area, tr_kind == BOS_TR_ARRIVE, false, 0, 1, 0, 1, e < 0.004f ? 0.004f : e);
		return true;
	}
	SDL_Rect clip;
	SDL_GetClipRect(s, &clip);
	SDL_SetClipRect(s, &tr_area);
	if (tr_kind == BOS_TR_PUSH) { // the new page comes in from the right; the old one gives way 30 % and dims to 35 %
		put(tr_from, s, tr_area.x - (int)(w * 0.3f * e), tr_area.y, 1 - 0.65f * e, 1);
		put(tr_to, s, tr_area.x + (int)(w * (1 - e)), tr_area.y, 1, 1);
	} else if (tr_kind == BOS_TR_POP) { // the same, backwards
		put(tr_to, s, tr_area.x - (int)(w * 0.3f * (1 - e)), tr_area.y, 0.35f + 0.65f * e, 1);
		put(tr_from, s, tr_area.x + (int)(w * e), tr_area.y, 1, 1);
	} else {
		put(tr_from, s, tr_area.x, tr_area.y, 1, 1);
		put(tr_to, s, tr_area.x, tr_area.y, 1, e);
	}
	SDL_SetClipRect(s, clip.w > 0 ? &clip : NULL);
	return true;
}

bool BOS_transFrameOnly(SDL_Surface* s) {
	bool was_moving = tr_state == 2;
	bool more = BOS_transFrame(s);
	if (tr_gpu && was_moving)
		PLAT_skipUpload(); // nothing was drawn on s
	return more;
}

// A game or app starts: the picture that was chosen grows a little while the page goes dark, 260 ms, then the caller
// leaves. Blocking: nothing else can happen once a launch is under way.
void BOS_launchOut(SDL_Surface* s, const SDL_Rect* focus) {
	int ms = BOS_motionMs(260);
	if (ms <= 0 || !s)
		return;
	SDL_Rect all = {0, 0, s->w, s->h};
	SDL_Surface *page = NULL, *pic = NULL;
	bool zoom = focus && focus->w > 0 && focus->h > 0 && motionMode() == 0;
	if (gpuMoves()) { // the page is on the glass and in the screen's texture: it is dimmed and zoomed there
		uint32_t g0 = SDL_GetTicks();
		for (;;) {
			float t = (float)(SDL_GetTicks() - g0) / (float)ms;
			if (t > 1)
				t = 1;
			float e = BOS_ease(t);
			GFX_startFrame();
			PLAT_pageShow(&all, false, false, 0, -1, 0, 1 - e, 1);
			if (zoom) {
				float k = 1 + 0.12f * e;
				int w = (int)(focus->w * k), h = (int)(focus->h * k);
				SDL_Rect dst = {focus->x - (w - focus->w) / 2, focus->y - (h - focus->h) / 2, w, h};
				PLAT_pageZoom(focus, &dst, 1 - e * e);
			}
			PLAT_skipUpload();
			GFX_flip(s);
			if (t >= 1)
				break;
		}
		SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 0, 0, 0, 255)); // what the glass shows now
		return;
	}
	if (!keep(&page, s, &all))
		return;
	if (zoom)
		keep(&pic, s, focus);
	uint32_t t0 = SDL_GetTicks();
	for (;;) {
		float t = (float)(SDL_GetTicks() - t0) / (float)ms;
		if (t > 1)
			t = 1;
		float e = BOS_ease(t);
		GFX_startFrame();
		put(page, s, 0, 0, 1 - e, 1);
		if (pic) {
			float k = 1 + 0.12f * e;
			int w = (int)(focus->w * k), h = (int)(focus->h * k);
			SDL_Rect dst = {focus->x - (w - focus->w) / 2, focus->y - (h - focus->h) / 2, w, h};
			int c = (int)(255 * (1 - e * e));
			SDL_SetSurfaceColorMod(pic, c, c, c);
			SDL_SetSurfaceBlendMode(pic, SDL_BLENDMODE_NONE);
			SDL_BlitScaled(pic, NULL, s, &dst);
		}
		GFX_flip(s);
		if (t >= 1)
			break;
	}
	SDL_FreeSurface(page);
	if (pic)
		SDL_FreeSurface(pic);
}

// ---- partial frames ----
static uint32_t pf_key;
static unsigned pf_flip; // the flip count the frame after the last drawn one starts with
static SDL_Surface* pf_s;
static SDL_Rect pf_band;
static bool pf_on;

uint32_t BOS_keyMix(uint32_t key, uint32_t v) {
	key ^= v + 0x9E3779B9u + (key << 6) + (key >> 2);
	return key ? key : 1;
}

bool BOS_partialBeginR(SDL_Surface* s, uint32_t key, bool ok, const SDL_Rect* r) {
	static int off = -1; // BOS_NOPARTIAL=1: every frame whole (to compare)
	if (off < 0)
		off = getenv("BOS_NOPARTIAL") != NULL;
	SDL_Rect all = {0, 0, s->w, s->h}, in = {0, 0, 0, 0};
	bool same = !off && ok && key && key == pf_key && s == pf_s && GFX_flipCount() == pf_flip && tr_state == 0 && r &&
				SDL_IntersectRect(r, &all, &in);
	pf_key = key;
	pf_s = s;
	pf_flip = GFX_flipCount() + 1;
	pf_on = same;
	if (same) {
		pf_band = in;
		SDL_SetClipRect(s, &pf_band);
	}
	return same;
}

bool BOS_partialBegin(SDL_Surface* s, uint32_t key, bool ok, int y, int h) {
	SDL_Rect r = {0, y, s->w, h};
	return BOS_partialBeginR(s, key, ok && h > 0 && y >= 0 && y + h <= s->h, &r);
}

void BOS_partialEnd(SDL_Surface* s) {
	if (!pf_on)
		return;
	SDL_SetClipRect(s, NULL);
}

void BOS_partialCancel(void) {
	pf_on = false;
	pf_key = 0;
}

bool BOS_partialOn(void) {
	return pf_on;
}

void BOS_clip(SDL_Surface* s, const SDL_Rect* r) {
	if (!pf_on || s != pf_s) {
		SDL_SetClipRect(s, r);
		return;
	}
	if (!r) {
		SDL_SetClipRect(s, &pf_band);
		return;
	}
	SDL_Rect in;
	if (!SDL_IntersectRect(r, &pf_band, &in))
		in = (SDL_Rect){0, pf_band.y, 1, 1}, in.w = 0; // nothing of it is in the band
	if (in.w <= 0 || in.h <= 0)
		in = (SDL_Rect){-1, -1, 1, 1};
	SDL_SetClipRect(s, &in);
}

void BOS_blit(SDL_Surface* pic, SDL_Surface* s, int x, int y, float a) {
	if (!pic || !s || a <= 0)
		return;
	SDL_BlendMode bm = SDL_BLENDMODE_NONE;
	SDL_GetSurfaceBlendMode(pic, &bm);
	uint32_t df = s->format->format;
	if (pic->format->format != SDL_PIXELFORMAT_ARGB8888 || bm != SDL_BLENDMODE_BLEND ||
		(df != SDL_PIXELFORMAT_ARGB8888 && df != SDL_PIXELFORMAT_RGB888) || SDL_MUSTLOCK(pic) || SDL_MUSTLOCK(s)) {
		SDL_SetSurfaceAlphaMod(pic, (Uint8)(255 * (a > 1 ? 1 : a)));
		SDL_BlitSurface(pic, NULL, s, &(SDL_Rect){x, y});
		SDL_SetSurfaceAlphaMod(pic, 255);
		return;
	}
	SDL_Rect clip;
	SDL_GetClipRect(s, &clip);
	int x0 = x > clip.x ? x : clip.x, y0 = y > clip.y ? y : clip.y;
	int x1 = x + pic->w < clip.x + clip.w ? x + pic->w : clip.x + clip.w;
	int y1 = y + pic->h < clip.y + clip.h ? y + pic->h : clip.y + clip.h;
	if (x1 <= x0 || y1 <= y0)
		return;
	uint32_t m = a >= 1 ? 256 : (uint32_t)(a * 256 + 0.5f);
	int n = x1 - x0;
	for (int row = y0; row < y1; row++) {
		const uint32_t* sp = (const uint32_t*)((const uint8_t*)pic->pixels + (row - y) * pic->pitch) + (x0 - x);
		uint32_t* dp = (uint32_t*)((uint8_t*)s->pixels + row * s->pitch) + x0;
		for (int i = 0; i < n; i++) {
			uint32_t p = sp[i], sa = p >> 24;
			if (m != 256)
				sa = (sa * m) >> 8;
			if (!sa)
				continue;
			if (sa == 255) {
				dp[i] = p;
				continue;
			}
			uint32_t d = dp[i], rb = d & 0xFF00FF, g = d & 0xFF00;
			rb = (rb + ((((p & 0xFF00FF) - rb) * sa) >> 8)) & 0xFF00FF;
			g = (g + ((((p & 0xFF00) - g) * sa) >> 8)) & 0xFF00;
			dp[i] = 0xFF000000u | rb | g;
		}
	}
}

// ---- the nudge at the end of a list (a held direction that has nowhere left to go) ----
static uint32_t bump_t0;
static int bump_dir;
#define BUMP_MS 160

void BOS_bump(int dir) {
	// called for every repeat of a held direction that cannot move: the first one knocks, the rest are silent
	static uint32_t last;
	uint32_t now = SDL_GetTicks();
	bool again = last && now - last < 250;
	last = now;
	if (again) {
		BOS_sfx(BOS_SFX_NONE);
		return;
	}
	BOS_sfx(BOS_SFX_BUMP);
	if (BOS_motionMs(BUMP_MS) < BUMP_MS)
		return; // น้อย and ปิด: the sound only
	bump_t0 = SDL_GetTicks();
	bump_dir = dir;
}

int BOS_bumpPx(void) {
	if (!bump_t0)
		return 0;
	float t = (float)(SDL_GetTicks() - bump_t0) / (float)BUMP_MS;
	if (t >= 1) {
		bump_t0 = 0;
		return 0;
	}
	return (int)(bump_dir * BOS_px(12) * sinf(t * (float)M_PI)); // towards the wall and back
}

bool BOS_bumpActive(void) {
	return bump_t0 != 0;
}

// ---- a value that follows its target (a focus highlight's place, a scroll offset, a switch's knob) ----
// *g is the state; the first call, or snap, lands on the target at once.
float BOS_glideTo(BosGlide* g, float target, int full_ms, bool snap) {
	uint32_t now = SDL_GetTicks();
	int ms = BOS_motionMs(full_ms);
	if (!g->set || snap || ms <= 0) {
		g->set = true;
		g->from = g->to = g->cur = target;
		g->t0 = 0;
		return target;
	}
	if (target != g->to) {
		g->from = g->cur; // from wherever it is: a new press mid-way bends the path, it never jumps
		g->to = target;
		g->t0 = now;
	}
	if (g->t0) {
		busy_glide = true;
		float t = (float)(now - g->t0) / (float)ms;
		if (t >= 1) {
			g->cur = g->to;
			g->t0 = 0;
		} else
			g->cur = g->from + (g->to - g->from) * BOS_ease(t);
	}
	return g->cur;
}

bool BOS_glideMoving(const BosGlide* g) {
	return g->t0 != 0;
}

// Something of the kit's is still moving: the app keeps drawing frames. Ask once a frame: the answer for the glides
// is used up by asking (a glide that is not drawn any more, its page gone, must not keep the app awake).
bool BOS_motionBusy(void) {
	bool g = busy_glide;
	busy_glide = false;
	if (bump_t0 && SDL_GetTicks() - bump_t0 >= BUMP_MS)
		bump_t0 = 0; // a nudge nobody drew to its end (its page gone)
	return tr_state != 0 || bump_t0 != 0 || g;
}
