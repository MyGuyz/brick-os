#include <stdint.h>
#include <stdbool.h>
#include "ui_album_art.h"

// Cache for album art background rendering
static SDL_Surface* cached_bg_surface = NULL;
static SDL_Surface* cached_bg_art_ptr = NULL; // Track pointer for cache invalidation
static int cached_bg_art_w = 0;				  // Track source dimensions for cache invalidation
static int cached_bg_art_h = 0;
static int cached_screen_w = 0; // Track screen dimensions
static int cached_screen_h = 0;

// Cleanup cached background surface (call on exit or when album art changes)
void cleanup_album_art_background(void) {
	if (cached_bg_surface) {
		SDL_FreeSurface(cached_bg_surface);
		cached_bg_surface = NULL;
	}
	cached_bg_art_ptr = NULL;
	cached_bg_art_w = 0;
	cached_bg_art_h = 0;
	cached_screen_w = 0;
	cached_screen_h = 0;
}
