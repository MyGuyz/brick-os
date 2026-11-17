#include "ui_emptystate.h"
#include "ui_draw.h"
#include "ui_icons.h"
#include "api.h"
#include "defines.h"

void BOS_uiEmpty(SDL_Surface* s, const char* message, const char* subtitle, char** pairs) __attribute__((weak));
void UI_renderEmptyStateButtons(SDL_Surface* screen, const char* message,
								const char* subtitle, char** btn_pairs) {
	BOS_uiEmpty(screen, message, subtitle, btn_pairs);
}

void UI_renderEmptyState(SDL_Surface* screen, const char* message,
						 const char* subtitle, const char* y_button_label) {
	if (y_button_label)
		UI_renderEmptyStateButtons(screen, message, subtitle,
								   (char*[]){"B", "BACK", "Y", (char*)y_button_label, NULL});
	else
		UI_renderEmptyStateButtons(screen, message, subtitle,
								   (char*[]){"B", "BACK", NULL});
}
