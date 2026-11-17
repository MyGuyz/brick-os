#include "ui_message.h"
#include "api.h"
#include "ui_draw.h"

void BOS_uiMessage(SDL_Surface* s, const char* message);

void UI_showMessage(SDL_Surface* screen, const char* message, int hold_ms) {
	GFX_clear(screen);
	UI_renderCenteredMessage(screen, message);
	GFX_flip(screen);
	SDL_Delay(hold_ms);
}

void UI_renderCenteredMessage(SDL_Surface* dst, const char* message) {
	BOS_uiMessage(dst, message);
}
