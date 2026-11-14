#ifndef VID_BOS_H
#define VID_BOS_H
// The Video app's own Brick OS pages (DESIGN.md 7.3): the home; and a video's picture (a same-name .jpg / .png
// beside it). Lists, sheets, boxes and toasts come from the shared pieces with UI_bos on.
#include <SDL2/SDL.h>
#include <stdbool.h>
#include "bos_ui.h"

SDL_Surface* VidBos_thumb(const char* video_path); // NULL when there is none; the kit owns it

#define VID_HOME_CARDS 6
typedef struct {
	const char* name;
	char left[48];	 // "เหลือ 4 นาที" / "ดูถึง 8:07"
	float progress;	 // 0..1, < 0 none
	const char* path;
} VidCard;
typedef struct {
	VidCard cards[VID_HOME_CARDS];
	int ncards;
	char files_sub[64];
	bool tv;		   // the Online TV pack is installed
	char tv_sub[64];
	int sel;		   // 0..ncards-1 a card, ncards + 0/1 a tile
	const BosHint* hints;
	int nhints;
	const BosHint* left;
	int nleft;
} VidHomeView;
void VidBos_home(SDL_Surface* s, const VidHomeView* v);
bool VidBos_homeMoving(void);
#endif
