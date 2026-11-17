#include "ui_fonts.h"
#include "ui_list.h"

// Thin compatibility wrappers. The implementations live in ui_list.c
// (UI_getListTextColor / UI_drawListItemBg); these Fonts_* names are kept
// because musicplayer/mediaplayer call them. (Fonts_calcListPillWidth was
// unused and has been removed — use UI_calcListPillWidth.)

