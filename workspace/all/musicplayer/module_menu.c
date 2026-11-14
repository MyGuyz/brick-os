// The Music home (DESIGN.md 7.2, board v2-Music): the playing / resume card, then the sources as tiles.
// A opens · B leaves · X stops (or forgets the resume point) · MENU the app's settings · SELECT screen off.

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "ui_toast.h"
#include "module_menu.h"
#include "bos_feature.h"
#include "resume.h"
#include "background.h"
#include "music_client.h"
#include "album_art.h"
#include "radio.h"
#include "podcast.h"
#include "playlist_m3u.h"
#include "music_bos.h"
#include "i18n.h"
#include "browser.h"
#include "online.h"

static char menu_toast_message[128] = "";
static uint32_t menu_toast_time = 0;

#define MENU_CTX_SETTINGS 2 // MENU: straight to the settings (a direct item, no sheet)

static void clear_first_item(int first_item_mode, bool* dirty) {
	if (first_item_mode == MENU_FIRST_NOW_PLAYING) {
		Background_stopAll();
		GFX_clearLayers(LAYER_SCROLLTEXT);
		*dirty = 1;
	} else if (first_item_mode == MENU_FIRST_RESUME) {
		if (MusicClient_isConnected())
			(void)MusicClient_stop();
		Resume_clear();
		GFX_clearLayers(LAYER_SCROLLTEXT);
		*dirty = 1;
	}
}

// songs under /Music, counted once per run (capped: the tile only needs a rough size)
static int countAudio(const char* path, int depth, int cap) {
	DIR* d = opendir(path);
	if (!d)
		return 0;
	int n = 0;
	struct dirent* e;
	char sub[768];
	while ((e = readdir(d)) && n < cap) {
		if (e->d_name[0] == '.')
			continue;
		if (e->d_type == DT_DIR) {
			if (depth < 6) {
				snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
				n += countAudio(sub, depth + 1, cap - n);
			}
		} else if (Browser_isAudioFile(e->d_name))
			n++;
	}
	closedir(d);
	return n;
}

static int countPlaylists(void) {
	DIR* d = opendir(PLAYLISTS_DIR);
	if (!d)
		return 0;
	int n = 0;
	struct dirent* e;
	while ((e = readdir(d))) {
		const char* dot = strrchr(e->d_name, '.');
		if (e->d_name[0] != '.' && dot && !strcasecmp(dot, ".m3u"))
			n++;
	}
	closedir(d);
	return n;
}

static void fmtTime(int ms, char* out, int size) {
	int s = ms / 1000;
	if (s >= 3600)
		snprintf(out, size, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

static void buildTiles(MusicHomeView* v) {
	static int files = -1;
	if (files < 0)
		files = countAudio(SDCARD_PATH "/Music", 0, 99999);
	v->ntiles = 0;
	MusicHomeTile* t = &v->tiles[v->ntiles++];
	*t = (MusicHomeTile){"folder", 0x0A84FF, I18N_text("Music files"), "", NULL, MENU_FILES};
	snprintf(t->sub, sizeof(t->sub), I18N_text("%d songs"), files);
	t = &v->tiles[v->ntiles++];
	*t = (MusicHomeTile){"server", 0x30B0C7, I18N_text("Music server"), "", NULL, MENU_SERVER};
	snprintf(t->sub, sizeof(t->sub), "%s", I18N_text(Server_configured() ? "Your library" : "Set up"));
	t = &v->tiles[v->ntiles++];
	*t = (MusicHomeTile){"list", 0xFF375F, I18N_text("Playlists"), "", NULL, MENU_PLAYLISTS};
	snprintf(t->sub, sizeof(t->sub), I18N_text("%d lists"), countPlaylists());
	t = &v->tiles[v->ntiles++];
	*t = (MusicHomeTile){"gift", 0x30D158, I18N_text("Free music"), "", NULL, MENU_FREE};
	snprintf(t->sub, sizeof(t->sub), "%s", I18N_text("Legal to keep"));
	if (BOS_hasFeature(BOS_FEATURE_RADIO)) {
		RadioStation* st = NULL;
		int ns = Radio_getStations(&st);
		t = &v->tiles[v->ntiles++];
		*t = (MusicHomeTile){"radio", 0xFF9F0A, I18N_text("Online Radio"), "", NULL, MENU_RADIO};
		snprintf(t->sub, sizeof(t->sub), I18N_text("%d stations"), ns);
		t = &v->tiles[v->ntiles++];
		*t = (MusicHomeTile){"mic", 0xBF5AF2, I18N_text("Podcasts"), "", NULL, MENU_PODCAST};
		snprintf(t->sub, sizeof(t->sub), I18N_text("%d shows"), Podcast_getSubscriptionCount());
	}
}

int MenuModule_run(SDL_Surface* screen) {
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;
	static int sel = -1;
	int prev_first_item_mode = -1;
	MusicHomeView v;

	while (1) {
		GFX_startFrame();
		PAD_poll();
		MusicClient_update();
		ModuleCommon_setAutosleepDisabled(Background_isPlaying());

		int first_item_mode = Background_isPlaying() ? MENU_FIRST_NOW_PLAYING
							  : Resume_isAvailable() ? MENU_FIRST_RESUME
													 : MENU_FIRST_NONE;
		bool has_first = first_item_mode != MENU_FIRST_NONE;
		if (first_item_mode != prev_first_item_mode) {
			if (prev_first_item_mode < 0)
				sel = has_first ? -1 : (sel < 0 ? 0 : sel);
			else if (!has_first && sel < 0)
				sel = 0;
			prev_first_item_mode = first_item_mode;
			dirty = 1;
		}

		// MENU goes straight to the settings (an item with no label is direct: no sheet)
		ContextMenuItem ctx_items[1];
		int ctx_count = 0;
		ContextMenu_addItem(ctx_items, &ctx_count, "", MENU_CTX_SETTINGS);
		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 0, ctx_items, ctx_count);
		if (global.should_quit)
			return MENU_QUIT;
		if (global.context_id == MENU_CTX_SETTINGS)
			return MENU_SETTINGS;
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}
		if (ModuleCommon_screenOffOnSelect(screen)) {
			dirty = 1;
			continue;
		}

		buildTiles(&v);
		int n = v.ntiles, cols = 3;
		if (sel >= n)
			sel = n - 1;
		int old = sel;
		if (PAD_justRepeated(BTN_UP)) {
			if (sel >= cols)
				sel -= cols;
			else if (sel >= 0 && has_first)
				sel = -1;
		} else if (PAD_justRepeated(BTN_DOWN)) {
			if (sel < 0)
				sel = 0;
			else if (sel + cols < n)
				sel += cols;
			else if (sel / cols < (n - 1) / cols)
				sel = n - 1;
		} else if (PAD_justRepeated(BTN_LEFT)) {
			if (sel > 0 && sel % cols)
				sel--;
		} else if (PAD_justRepeated(BTN_RIGHT)) {
			if (sel >= 0 && sel % cols < cols - 1 && sel + 1 < n)
				sel++;
		} else if (PAD_justPressed(BTN_A)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			if (sel < 0)
				return MENU_RESUME;
			return v.tiles[sel].action;
		} else if (PAD_justPressed(BTN_X) && sel < 0 && has_first) {
			clear_first_item(first_item_mode, &dirty);
		} else if (PAD_justPressed(BTN_B)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			if (ModuleCommon_confirmAppExit(screen))
				return MENU_QUIT;
			dirty = 1;
		}
		if (sel != old || MusicBos_homeMoving())
			dirty = 1;

		ModuleCommon_PWR_update(&dirty, &show_setting);

		// the card follows the music: redraw once a second while it plays
		static uint32_t last_tick;
		if (first_item_mode == MENU_FIRST_NOW_PLAYING && SDL_GetTicks() - last_tick > 1000) {
			last_tick = SDL_GetTicks();
			dirty = 1;
		}

		if (dirty) {
			v.card = has_first;
			v.sel = sel;
			v.art = NULL;
			v.progress = -1;
			v.line2 = NULL;
			static char line2[256], title[256];
			if (first_item_mode == MENU_FIRST_NOW_PLAYING) {
				const MusicSnapshotWire* s = MusicClient_snapshot();
				v.card_playing = true;
				v.card_label = I18N_text("Playing now");
				snprintf(title, sizeof(title), "%s", s->title[0] ? s->title : Resume_getLabel() ? Resume_getLabel() : "");
				char p[16], d[16];
				fmtTime(s->position_ms, p, sizeof(p));
				fmtTime(s->duration_ms, d, sizeof(d));
				if (s->duration_ms > 0) {
					snprintf(line2, sizeof(line2), "%s%s%s / %s", s->artist, s->artist[0] ? " · " : "", p, d);
					v.progress = (float)s->position_ms / s->duration_ms;
				} else
					snprintf(line2, sizeof(line2), "%s", s->artist);
				v.line2 = line2;
				v.art = album_art_get();
			} else if (first_item_mode == MENU_FIRST_RESUME) {
				v.card_playing = false;
				v.card_label = I18N_text("Continue");
				const char* l = Resume_getLabel();
				snprintf(title, sizeof(title), "%s", l ? l : "");
			}
			v.title = title;
			static BosHint r[3], l[1];
			int nr = 0;
			r[nr++] = (BosHint){"A", I18N_text(sel < 0 ? (first_item_mode == MENU_FIRST_NOW_PLAYING ? "Open" : "Continue") : "Open")};
			r[nr++] = (BosHint){"B", I18N_text("Exit")};
			if (sel < 0 && has_first)
				r[nr++] = (BosHint){"X", I18N_text(first_item_mode == MENU_FIRST_NOW_PLAYING ? "Stop" : "Clear")};
			l[0] = (BosHint){"MENU", I18N_text("Music settings")};
			v.hints = r, v.nhints = nr, v.left = l, v.nleft = 1;
			MusicBos_home(screen, &v);
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
