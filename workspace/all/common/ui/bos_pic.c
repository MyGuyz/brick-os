// Brick OS kit: pictures scaled and rounded once, kept for the next frames (album art, covers, thumbnails).
#include "bos_ui.h"
#include <string.h>

typedef struct {
	SDL_Surface* src;
	int w, h, r;
	SDL_Surface* pic;
	uint32_t used;
} PicSlot;
#define PIC_SLOTS 48
static PicSlot slots[PIC_SLOTS];
static uint32_t pic_clock;

// src scaled into w x h (bilinear, aspect filled and centred), corners cut round (radius r)
static SDL_Surface* makePic(SDL_Surface* src, int w, int h, int r) {
	SDL_Surface* a = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_Surface* d = a ? SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888) : NULL;
	if (!d) {
		if (a)
			SDL_FreeSurface(a);
		return NULL;
	}
	// cover: the source area that fills w x h without stretching
	float sw = a->w, sh = a->h, k = sw / w > sh / h ? sh / h : sw / w;
	float cw = w * k, ch = h * k, ox = (sw - cw) / 2, oy = (sh - ch) / 2;
	for (int y = 0; y < h; y++) {
		float sy = oy + (y + 0.5f) * ch / h - 0.5f;
		if (sy < 0)
			sy = 0;
		int y0 = (int)sy, y1 = y0 + 1 < a->h ? y0 + 1 : y0;
		int fy = (int)((sy - y0) * 256);
		const uint32_t* r0 = (const uint32_t*)((uint8_t*)a->pixels + y0 * a->pitch);
		const uint32_t* r1 = (const uint32_t*)((uint8_t*)a->pixels + y1 * a->pitch);
		uint32_t* out = (uint32_t*)((uint8_t*)d->pixels + y * d->pitch);
		for (int x = 0; x < w; x++) {
			float sx = ox + (x + 0.5f) * cw / w - 0.5f;
			if (sx < 0)
				sx = 0;
			int x0 = (int)sx, x1 = x0 + 1 < a->w ? x0 + 1 : x0;
			int fx = (int)((sx - x0) * 256);
			uint32_t p = 0;
			for (int sh2 = 0; sh2 < 32; sh2 += 8) {
				int c00 = (r0[x0] >> sh2) & 255, c01 = (r0[x1] >> sh2) & 255, c10 = (r1[x0] >> sh2) & 255,
					c11 = (r1[x1] >> sh2) & 255;
				int top = c00 * (256 - fx) + c01 * fx, bot = c10 * (256 - fx) + c11 * fx;
				p |= (uint32_t)(((top * (256 - fy) + bot * fy) >> 16) & 255) << sh2;
			}
			if (r > 0) {
				float dx = x < r ? r - x - 0.5f : x >= w - r ? x - (w - r) + 0.5f : 0;
				float dy = y < r ? r - y - 0.5f : y >= h - r ? y - (h - r) + 0.5f : 0;
				if (dx > 0 && dy > 0) {
					float dd = SDL_sqrtf(dx * dx + dy * dy);
					float cov = dd <= r - 0.5f ? 1 : dd >= r + 0.5f ? 0 : r + 0.5f - dd;
					p = (p & 0xFFFFFF) | (uint32_t)(cov * ((p >> 24) & 255)) << 24;
				}
			}
			out[x] = p;
		}
	}
	SDL_FreeSurface(a);
	SDL_SetSurfaceBlendMode(d, SDL_BLENDMODE_BLEND);
	return d;
}

SDL_Surface* BOS_picRounded(SDL_Surface* src, int w, int h, int r) {
	if (!src || w <= 0 || h <= 0 || src->w <= 0 || src->h <= 0)
		return NULL;
	pic_clock++;
	PicSlot* old = &slots[0];
	for (int i = 0; i < PIC_SLOTS; i++) {
		PicSlot* p = &slots[i];
		if (p->src == src && p->w == w && p->h == h && p->r == r) {
			p->used = pic_clock;
			return p->pic;
		}
		if (p->used < old->used)
			old = p;
	}
	if (old->pic)
		SDL_FreeSurface(old->pic);
	*old = (PicSlot){src, w, h, r, makePic(src, w, h, r), pic_clock};
	return old->pic;
}

void BOS_picForget(SDL_Surface* src) {
	for (int i = 0; i < PIC_SLOTS; i++)
		if (slots[i].src == src || !src) {
			if (slots[i].pic)
				SDL_FreeSurface(slots[i].pic);
			slots[i] = (PicSlot){0};
		}
}
