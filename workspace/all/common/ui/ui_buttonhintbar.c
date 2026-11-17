#include "ui_buttonhintbar.h"
#include "ui_hintbar_layout.h"
#include "ui_draw.h"
#include "ui_list.h"
#include "api.h"
#include "defines.h"

int UI_renderButtonHintBar(SDL_Surface* dst, char** pairs) {
	return UI_renderButtonHintBarEx(dst, pairs, 204);
}

int UI_renderButtonHintBarEx(SDL_Surface* dst, char** pairs, Uint8 scrim_alpha) {
	return UI_renderButtonHintBarAt(dst, pairs, scrim_alpha, -1);
}

int BOS_uiHintBar(SDL_Surface* s, char** pairs);
int UI_renderButtonHintBarAt(SDL_Surface* dst, char** pairs, Uint8 scrim_alpha, int left) {
	return BOS_uiHintBar(dst, pairs);
}

int UI_buttonHintIconTop(int screen_h) {
	int btn_sz = SCALE1(BUTTON_SIZE);
	int bar_h = BAR_HEIGHT;
	return screen_h - bar_h + UI_hintBarIconOffset(bar_h, btn_sz);
}

int UI_buttonHintBarTop(int screen_h) {
	return screen_h - BAR_HEIGHT;
}
