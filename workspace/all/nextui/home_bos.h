#ifndef HOME_BOS_H
#define HOME_BOS_H

// Brick OS Home (DESIGN.md 6.1): drawn whole when Layouts > Home layout is Grid.

#include <stdbool.h>

#include "sdl.h"
#include "types.h"

typedef enum {
	HOMEBOS_UNHANDLED, // the caller's own handlers see the press (SELECT, START, the F keys)
	HOMEBOS_HANDLED,
	HOMEBOS_SEARCH,	  // Y: the caller opens search
	HOMEBOS_SWITCHER, // MENU held: the caller opens the Game Switcher
	HOMEBOS_OPTIONS,  // MENU tapped on a game: the caller opens its options (HomeBos_focusedGame)
} HomeBosInput;

bool HomeBos_active(void); // Home is showing in the Brick OS layout
void HomeBos_reset(void);  // rebuild the cards and counts on the next frame
void HomeBos_render(SDL_Surface* screen);
bool HomeBos_animating(void);
HomeBosInput HomeBos_handleInput(unsigned long now, bool* dirty, bool tap_menu);
void HomeBos_quit(void);
Entry* HomeBos_focusedGame(void); // the เล่นต่อ card in focus, NULL on the app row



// สลับเกม (DESIGN.md 6.5)
#include <time.h>
typedef struct {
	char path[512];	   // the ROM
	char name[256];
	char preview[512]; // its resume frame, "" for none
	int seconds;	   // play time, 0 unknown
} BosSwitchCard;
// cards: sel-2 .. sel+2 (NULL where there is none); count 0 draws the empty state
void BosSwitcher_render(SDL_Surface* screen, const BosSwitchCard* const cards[5], int sel, int count, bool playing,
						time_t saved_at, bool from_game);
bool BosSwitcher_animating(void);

// เกม (DESIGN.md 6.3): the Consoles tab drawn as consoles | games
typedef enum { BOSGAMES_UNHANDLED, BOSGAMES_HANDLED, BOSGAMES_SEARCH, BOSGAMES_HOME, BOSGAMES_OPTIONS, BOSGAMES_SWITCHER } BosGamesInput;
bool BosGames_active(void);
void BosGames_reset(void);
void BosGames_render(SDL_Surface* screen);
bool BosGames_animating(void);
BosGamesInput BosGames_handleInput(unsigned long now, bool* dirty, bool tap_menu, bool tap_select);
Entry* BosGames_focusedGame(void); // step 2's focused game, else NULL
#endif
