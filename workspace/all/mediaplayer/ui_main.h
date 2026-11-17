#ifndef __UI_MAIN_H__
#define __UI_MAIN_H__

#include "api.h"
#include "ui_listview.h"
#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>

// Render the main menu
void render_menu(SDL_Surface* screen, IndicatorType show_setting,
				 char* toast_message, uint32_t toast_time);

#endif
