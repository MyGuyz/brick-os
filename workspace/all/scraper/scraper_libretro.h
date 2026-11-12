#ifndef SCRAPER_LIBRETRO_H
#define SCRAPER_LIBRETRO_H

#include <stdbool.h>

#include "scraper_api.h"

// Fill info with libretro-thumbnails URLs for rom_filename (boxart, snap, logo). True when its box art (or snap)
// exists. Used by ScraperAPI_search when the build has no ScreenScraper developer key.
bool ScraperLibretro_search(const char* rom_filename, int system_id, ScraperGameInfo* info);

#endif
