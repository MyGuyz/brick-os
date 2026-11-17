#include "ui_downloadprogress.h"
#include "api.h"
#include "defines.h"
#include "ui_buttonhintbar.h"
#include "ui_font.h"
#include "ui_menubar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_draw.h"

void BOS_uiProgress(SDL_Surface* s, const UIDownloadProgress* info);

// A long-running tool page (LIST-LAYOUT §10.6 "Progress page"): the status line at the secondary size
// (0.8 x the list label, bold for the semi-bold weight), the bar under it with a gap of 1.0 x the status
// size, and the detail line (the current item) at the caption size with 0.75 x the status size above it.
// No headline-size text. The column is centred between the page title strip (+8 dp) and the hint bar.
void UI_renderDownloadProgress(SDL_Surface* screen, const UIDownloadProgress* info) {
	BOS_uiProgress(screen, info);
}
