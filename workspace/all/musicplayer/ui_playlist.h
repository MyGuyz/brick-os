#ifndef __UI_PLAYLIST_H__
#define __UI_PLAYLIST_H__

#include <SDL2/SDL.h>
#include "api.h"
#include "playlist_m3u.h"
#include "playlist.h"
#include "ui_listview.h"

// Render the playlist list screen (full-mode ListView; the view owns
// selection and scroll)
void render_playlist_list(SDL_Surface* screen, IndicatorType show_setting,
						  PlaylistInfo* playlists, int count);

// Render the playlist detail screen (tracks in a playlist)
void render_playlist_detail(SDL_Surface* screen, IndicatorType show_setting,
							const char* playlist_name,
							PlaylistTrack* tracks, int count);

// Full-mode ListViews owned by ui_playlist.c (module_playlist drives input
// and selection through these)
ListView* PlaylistList_view(void);
ListView* PlaylistDetail_view(void);

bool PlaylistList_bosMoving(void);
#endif
