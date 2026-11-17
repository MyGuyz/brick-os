#ifndef __UI_ALBUM_ART_H__
#define __UI_ALBUM_ART_H__

#include <SDL2/SDL.h>

// Cleanup cached background surface (call on exit or when switching tracks)
void cleanup_album_art_background(void);

#endif
