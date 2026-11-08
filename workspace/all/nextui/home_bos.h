#ifndef HOME_BOS_H
#define HOME_BOS_H

// Brick OS Home (DESIGN.md 6.1): drawn whole when Layouts > Home layout is Grid.

#include <stdbool.h>

#include "sdl.h"

typedef enum {
	HOMEBOS_UNHANDLED, // the caller's own handlers see the press (SELECT, START, the F keys)
	HOMEBOS_HANDLED,
	HOMEBOS_SEARCH,	  // Y: the caller opens search
	HOMEBOS_SWITCHER, // MENU: the caller opens the Game Switcher
} HomeBosInput;

bool HomeBos_active(void); // Home is showing in the Brick OS layout
void HomeBos_reset(void);  // rebuild the cards and counts on the next frame
void HomeBos_render(SDL_Surface* screen);
bool HomeBos_animating(void);
HomeBosInput HomeBos_handleInput(unsigned long now, bool* dirty, bool tap_menu);
void HomeBos_quit(void);



// Game Switcher (DESIGN.md 6.2)
#include <time.h>
typedef struct {
	char path[512];	   // the ROM
	char name[256];
	char preview[512]; // its resume frame, "" for none
	int seconds;	   // play time, 0 unknown
} BosSwitchCard;
void BosSwitcher_render(SDL_Surface* screen, const BosSwitchCard* prev, const BosSwitchCard* cur,
						const BosSwitchCard* next, int sel, int count, bool playing, time_t saved_at);
bool BosSwitcher_pending(void);
#endif
