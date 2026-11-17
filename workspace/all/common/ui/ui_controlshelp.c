#include "ui_controlshelp.h"
#include "api.h"
#include "defines.h"
#include "ui_draw.h"

void BOS_uiControls(SDL_Surface* s, const char* title, const char* const* keys, const char* const* acts, int n) __attribute__((weak));

void UI_renderControlsHelp(SDL_Surface* screen, const char* title,
						   const ControlHelp* controls) {
	const char* k[24];
	const char* a[24];
	int n = 0;
	while (controls[n].button && n < 24)
		k[n] = controls[n].button, a[n] = controls[n].action, n++;
	BOS_uiControls(screen, title, k, a, n);
}
