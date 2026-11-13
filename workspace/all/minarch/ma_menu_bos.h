#ifndef MA_MENU_BOS_H
#define MA_MENU_BOS_H
// The in-game menu drawn with the Brick OS kit (DESIGN.md 7.1)
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <time.h>
#include "bos_ui.h"

#define MENU_BOS_ITEMS 5
#define MENU_BOS_SLOTS 8

typedef struct {
	const char* title;					// the status bar's title (the game)
	const char* game;					// under the picture
	const char* console;				// under the game's name
	const char* items[MENU_BOS_ITEMS];	// translated
	bool item_off[MENU_BOS_ITEMS];		// not available now (netplay: save, load)
	int selected;
	const char* disc;					// "แผ่น 2" when there is more than one, else NULL
	bool slots;							// save / load focused: the slot instead of the game
	int slot;							// 0-7
	bool slot_used[MENU_BOS_SLOTS];
	bool save_exists;
	const char* preview_path;			// the slot's picture, NULL when there is none
	time_t save_time;
	const BosHint* hints;
	int nhints;
	const BosHint* left_hints;
	int nleft;
} MenuBosView;

void MenuBos_open(SDL_Surface* frame, int w, int h); // the paused frame: the blurred backdrop and the picture
void MenuBos_close(void);
void MenuBos_render(SDL_Surface* s, const MenuBosView* v);
bool MenuBos_moving(void); // the focus is still gliding: draw again


// ---- the options pages (DESIGN.md 7.1, board v2-GameMenuOptions): Settings' two columns over the paused game ----
enum { MB_ROW_NONE, MB_ROW_TEXT, MB_ROW_CYCLE, MB_ROW_TOGGLE, MB_ROW_SLIDER, MB_ROW_NAV, MB_ROW_BIND };
typedef struct {
	const char* name;  // translated
	const char* value; // translated value text (or the number for a slider)
	int kind;
	bool on;		   // a toggle's state
	float frac;		   // a slider's place, 0..1
} MenuBosRow;

typedef struct {
	const char* const* left;	// the left column: the parent page's rows (translated)
	const char* const* left_icons; // their icons (the Options page), or NULL
	const uint32_t* left_colors;
	int nleft;
	int left_sel;				// the open row
	bool left_focused;			// at the Options page itself: the focus is on the left
	const char* head;			// the right column's large title
	const char* head_desc;		// with the focus on the left: what the row holds
	const MenuBosRow* rows;
	int nrows;
	int sel;					// the focused row (-1: the focus is on the left)
	int* scroll;				// the first row in view (kept by the caller, per page)
	const char* desc;			// under the rows: the focused row's description
	const BosHint* hints;
	int nhints;
	const BosHint* left_hints;
	int nleft_hints;
} MenuBosOptView;
void MenuBos_renderOptions(SDL_Surface* s, const MenuBosOptView* v);
bool MenuBos_optMoving(void); // a glide or a scrolling name: draw again
#endif
