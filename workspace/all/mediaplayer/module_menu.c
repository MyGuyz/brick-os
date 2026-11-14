// The Video home (DESIGN.md 7.3, board v2-Video): ดูต่อ (videos with a saved place), then the two places.
// A opens / plays on · B leaves · MENU the page's menu (quit).
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "ui_toast.h"
#include "module_menu.h"
#include "bos_feature.h"
#include "positions.h"
#include "iptv.h"
#include "video_browser.h"
#include "vp_defines.h"
#include "vid_bos.h"
#include "i18n.h"

static char menu_toast_message[128] = "";
static uint32_t menu_toast_time = 0;
static char picked_path[512];

const char* MenuModule_pickedPath(void) {
	return picked_path;
}

static int countVideos(const char* path, int depth, int* dirs) {
	DIR* d = opendir(path);
	if (!d)
		return 0;
	int n = 0;
	struct dirent* e;
	char sub[768];
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		if (e->d_type == DT_DIR) {
			if (depth == 0 && dirs)
				(*dirs)++;
			if (depth < 4) {
				snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
				n += countVideos(sub, depth + 1, NULL);
			}
		} else if (VideoBrowser_isVideoFile(e->d_name))
			n++;
	}
	closedir(d);
	return n;
}

static void buildView(VidHomeView* v) {
	static int files = -1, dirs = 0;
	if (files < 0)
		files = countVideos(VIDEO_ROOT, 0, &dirs);
	snprintf(v->files_sub, sizeof(v->files_sub), I18N_text("%d files · %d folders"), files, dirs);
	v->tv = BOS_hasFeature(BOS_FEATURE_IPTV);
	snprintf(v->tv_sub, sizeof(v->tv_sub), I18N_text("%d channels · needs WiFi"), IPTV_getUserChannelCount());
	Positions_init();
	v->ncards = 0;
	for (int i = 0; i < Positions_count() && v->ncards < VID_HOME_CARDS; i++) {
		int sec = 0, dur = 0;
		const char* p = Positions_at(i, &sec, &dur);
		if (!p || access(p, R_OK) != 0)
			continue;
		VidCard* c = &v->cards[v->ncards++];
		c->path = p;
		const char* slash = strrchr(p, '/');
		static char names[VID_HOME_CARDS][256];
		VideoBrowser_getDisplayName(slash ? slash + 1 : p, names[v->ncards - 1], sizeof(names[0]));
		c->name = names[v->ncards - 1];
		if (dur > 0) {
			int left = (dur - sec + 59) / 60;
			snprintf(c->left, sizeof(c->left), I18N_text("%d min left"), left > 0 ? left : 1);
			c->progress = (float)sec / dur;
		} else {
			snprintf(c->left, sizeof(c->left), I18N_text("Watched to %d:%02d"), sec / 60, sec % 60);
			c->progress = -1;
		}
	}
}

int MenuModule_run(SDL_Surface* screen) {
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;
	static int sel = 0;
	VidHomeView v;
	buildView(&v);
	if (sel >= v.ncards + (v.tv ? 2 : 1))
		sel = 0;

	while (1) {
		GFX_startFrame();
		PAD_poll();
		ContextMenuItem ctx_items[1];
		int ctx_count = 0;
		ContextMenu_addItem(ctx_items, &ctx_count, "Quit App", CTX_ID_QUIT);
		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, ctx_items, ctx_count);
		if (global.should_quit)
			return MENU_QUIT;
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}
		int ntiles = v.tv ? 2 : 1, old = sel;
		if (PAD_justRepeated(BTN_LEFT) && sel > 0 && sel != v.ncards)
			sel--;
		else if (PAD_justRepeated(BTN_RIGHT) && sel + 1 < v.ncards + ntiles && sel + 1 != v.ncards)
			sel++;
		else if (PAD_justRepeated(BTN_DOWN) && sel < v.ncards)
			sel = v.ncards;
		else if (PAD_justRepeated(BTN_UP) && sel >= v.ncards && v.ncards > 0)
			sel = 0;
		else if (PAD_justPressed(BTN_A)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			if (sel < v.ncards) {
				snprintf(picked_path, sizeof(picked_path), "%s", v.cards[sel].path);
				return MENU_PLAY_PATH;
			}
			return sel == v.ncards ? MENU_LOCAL : MENU_IPTV;
		} else if (PAD_justPressed(BTN_B)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			return MENU_QUIT;
		}
		if (sel != old || VidBos_homeMoving())
			dirty = 1;
		ModuleCommon_PWR_update(&dirty, &show_setting);
		if (dirty) {
			v.sel = sel;
			static BosHint hr[2], hl[1];
			hr[0] = (BosHint){"A", I18N_text(sel < v.ncards ? "Continue" : "Open")};
			hr[1] = (BosHint){"B", I18N_text("Exit")};
			hl[0] = (BosHint){"MENU", I18N_text("Options")};
			v.hints = hr, v.nhints = 2, v.left = hl, v.nleft = 1;
			VidBos_home(screen, &v);
			UI_renderToast(screen, menu_toast_message, menu_toast_time);
			GFX_flip(screen);
			dirty = 0;
			UI_tickToast(menu_toast_message, menu_toast_time, &dirty);
		} else
			GFX_sync();
	}
}

void MenuModule_setToast(const char* message) {
	snprintf(menu_toast_message, sizeof(menu_toast_message), "%s", message);
	menu_toast_time = SDL_GetTicks();
}
