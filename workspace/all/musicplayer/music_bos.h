#ifndef MUSIC_BOS_H
#define MUSIC_BOS_H
// The Music app's own Brick OS screens (DESIGN.md 7.2): the home and the playing pages. Lists, sheets, boxes and
// toasts come from the shared pieces with UI_bos on.
#include <SDL2/SDL.h>
#include <stdbool.h>
#include "bos_ui.h"

#define MUSIC_HOME_TILES 6
typedef struct {
	const char* icon;
	uint32_t colour;
	const char* name; // translated
	char sub[48];	  // translated count, or "" with a pill
	const char* pill; // e.g. "ใหม่ 3", or NULL
	int action;		  // what A opens (module_menu.h MENU_*)
} MusicHomeTile;

typedef struct {
	bool card;			// something plays or can resume
	bool card_playing;	// it plays now (else paused / resumable)
	const char* card_label; // "กำลังเล่น" / "เล่นต่อ"
	const char* title;
	const char* line2;	// artist · 1:42 / 3:58
	float progress;		// 0..1, < 0 none
	SDL_Surface* art;	// may be NULL
	MusicHomeTile tiles[MUSIC_HOME_TILES];
	int ntiles;
	int sel;			// -1 the card, else a tile
	const BosHint* hints;
	int nhints;
	const BosHint* left;
	int nleft;
} MusicHomeView;

SDL_Surface* MusicBos_folderArt(const char* dir); // ui_music.c
void MusicBos_home(SDL_Surface* s, const MusicHomeView* v);

typedef struct {
	const char* status;			   // the status bar's title
	SDL_Surface* art;			   // the picture, or NULL:
	const char* art_text;		   //   these letters (a station) or
	const char* icon;			   //   this icon on
	uint32_t icon_colour;		   //   this colour
	const char* pills[3];		   // format · rate · "3 / 12" (or สด · bitrate …)
	const char* title, *artist, *album;
	const char* line_extra_label;  // the radio: "เพลงตอนนี้"
	const char* line_extra;		   //   and the stream's title
	bool lyrics_on;
	const char* lyric[4];		   // before, now (bold), next, after
	int pos_ms, dur_ms;			   // dur 0: no seek bar (live radio)
	bool paused, modes, shuffle, repeat; // modes: draw shuffle and repeat
	const char* back_icon, *fwd_icon;	 // prev/next, or rew10/fwd30
	const BosHint* hints;
	int nhints;
	const BosHint* left;
	int nleft;
} MusicPlayView;
// vis: where the visualizer goes this frame ({0} none)
void MusicBos_playing(SDL_Surface* s, const MusicPlayView* v, SDL_Rect* vis);

typedef struct {
	const char* const* names;	// the playlists
	const int* counts;
	int n;
	int sel;					// n = the "new playlist" row
	int* scroll;
	const char* const* preview; // the focused one's first songs
	int npreview;
	const BosHint* hints;
	int nhints;
	const BosHint* left;
	int nleft;
} MusicPlaylistsView;
void MusicBos_playlists(SDL_Surface* s, const MusicPlaylistsView* v);
bool MusicBos_playlistsMoving(void);
bool MusicBos_homeMoving(void);
#endif
