#include "ui_splash.h"
#include "api.h"
#include "defines.h"
#include "ui_draw.h"

void BOS_uiSplash(SDL_Surface* s, const char* title) __attribute__((weak));

void UI_showSplashScreen(SDL_Surface* screen, const char* title) {
	BOS_uiSplash(screen, title);
	GFX_flip(screen);
}
