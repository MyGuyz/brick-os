#ifndef MODULE_ONLINE_H
#define MODULE_ONLINE_H
// Own server (Subsonic API) and free music (Internet Archive) pages (DESIGN.md 7.2)
#include "sdl.h"
#include "module_common.h"
ModuleExitReason ServerModule_run(SDL_Surface* screen);
ModuleExitReason FreeModule_run(SDL_Surface* screen);
#endif
