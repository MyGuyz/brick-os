#include "ui_loadingoverlay.h"
#include "ui_draw.h"
#include "api.h"
#include "defines.h"

void BOS_uiLoading(SDL_Surface* s, const char* title, const char* subtitle) __attribute__((weak));
void UI_renderLoadingOverlay(SDL_Surface* dst, const char* title, const char* subtitle) {
	BOS_uiLoading(dst, title, subtitle);
}
