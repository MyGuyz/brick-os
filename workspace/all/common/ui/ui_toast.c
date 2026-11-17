#include "ui_toast.h"
#include "api.h"
#include "defines.h"
#include "toast_tick.h"
#include "ui_draw.h"

SDL_Surface* BOS_uiToast(const char* message, int screen_w, int screen_h, int* x, int* y);

// Toast is rendered to GPU layer 5 (highest) to appear above all content
#define LAYER_TOAST 5

void UI_renderToast(SDL_Surface* screen, const char* message, uint32_t toast_time) {
	if (!message || message[0] == '\0') {
		PLAT_clearLayers(LAYER_TOAST);
		return;
	}

	uint32_t now = SDL_GetTicks();
	if (now - toast_time >= TOAST_DURATION) {
		PLAT_clearLayers(LAYER_TOAST);
		return;
	}

	int hw = screen->w;
	int hh = screen->h;

	int tx, ty;
	SDL_Surface* p = BOS_uiToast(message, hw, hh, &tx, &ty);
	if (p) {
		PLAT_clearLayers(LAYER_TOAST);
		PLAT_drawOnLayer(p, tx, ty, p->w, p->h, 1.0f, false, LAYER_TOAST);
		SDL_FreeSurface(p);
	}
}

void UI_tickToast(char* message, uint32_t toast_time, bool* dirty) {
	UI_toastTickAt(message, toast_time, SDL_GetTicks(), TOAST_DURATION, dirty);
}

void UI_clearToast(void) {
	PLAT_clearLayers(LAYER_TOAST);
}
