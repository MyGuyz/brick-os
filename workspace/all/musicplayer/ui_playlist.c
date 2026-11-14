#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_menubar.h"
#include "ui_playlist.h"
#include "ui_icons.h"
#include "ui_listview.h"
#include "ui_draw.h"
#include "i18n.h"
#include "music_bos.h"
#include "browser.h"
#include "playlist_m3u.h"
#include "ui_utils.h"

// Full-mode ListViews (the widget owns selection, scroll, glide and marquee;
// module_playlist drives input through the accessors below).
static ListView playlist_list_view;
static ListView playlist_detail_view;

ListView* PlaylistList_view(void) {
	return &playlist_list_view;
}
ListView* PlaylistDetail_view(void) {
	return &playlist_detail_view;
}

// "%s (%d)" scratch: the widget consumes the label before the next get_row
// call, so one static buffer per provider is enough.
static char playlist_row_buf[256];

static void playlist_list_get_row(void* ctx, int i, bool selected,
								  ListViewRow* out) {
	PlaylistInfo* pls = ctx;
	snprintf(playlist_row_buf, sizeof(playlist_row_buf), "%s (%d)",
			 pls[i].name, pls[i].track_count);
	out->label = playlist_row_buf;
	(void)selected;
}

// Row provider for the tracks list: icon variant follows the widget-supplied
// `selected` flag (the glide-tracking row_sel), preserving the
// pill-follows-icon behavior.
static void playlist_detail_get_row(void* ctx, int i, bool selected,
									ListViewRow* out) {
	PlaylistTrack* tracks = ctx;
	out->label = tracks[i].name;
	out->icon = Icons_isLoaded() ? Icons_getForFormat(tracks[i].format, selected) : NULL;
	if (UI_bos) {
		out->icon = NULL;
		const char* slash = strrchr(tracks[i].path, '/');
		char dir[512];
		snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - tracks[i].path) : 0, tracks[i].path);
		out->art = MusicBos_folderArt(dir);
		if (!out->art)
			out->sq_icon = "music", out->sq_color = 0x5E5CE6;
		out->sub = get_format_name(tracks[i].format);
	}
}

// Brick OS (board v2-MusicPlaylists): two columns; the ListView still owns the input (count + the "new" row)
static void render_playlists_bos(SDL_Surface* screen, PlaylistInfo* playlists, int count) {
	ListView* v = &playlist_list_view;
	v->count = count + 1;
	if (v->selected > count)
		v->selected = count;
	static const char* names[64];
	static int counts[64];
	int n = count < 64 ? count : 64;
	for (int i = 0; i < n; i++)
		names[i] = playlists[i].name, counts[i] = playlists[i].track_count;
	// the focused playlist's first songs, read when the focus moves
	static PlaylistTrack prev[6];
	static int nprev, prev_for = -2;
	static const char* prev_names[6];
	if (prev_for != v->selected || prev_for < 0) {
		prev_for = v->selected;
		nprev = 0;
		if (v->selected < n)
			M3U_loadTracks(playlists[v->selected].path, prev, 6, &nprev);
		static char pn[6][256];
		for (int i = 0; i < nprev; i++) {
			Browser_getDisplayName(prev[i].name, pn[i], sizeof(pn[i]));
			prev_names[i] = pn[i];
		}
	}
	static BosHint hr[3], hl[1];
	int nr = 0;
	bool on_new = v->selected >= n;
	hr[nr++] = (BosHint){"A", I18N_text(on_new ? "Create" : "Open")};
	hr[nr++] = (BosHint){"B", I18N_text("Back")};
	if (!on_new && counts[v->selected] > 0)
		hr[nr++] = (BosHint){"Y", I18N_text("Play all")};
	hl[0] = (BosHint){"MENU", I18N_text("Options")};
	MusicPlaylistsView pv = {names, counts, n, v->selected, &v->scroll, prev_names, nprev, hr, nr, hl, on_new ? 0 : 1};
	v->input_pending = false;
	MusicBos_playlists(screen, &pv);
}

bool PlaylistList_bosMoving(void) {
	return MusicBos_playlistsMoving();
}

void render_playlist_list(SDL_Surface* screen, IndicatorType show_setting,
						  PlaylistInfo* playlists, int count) {
	(void)show_setting;
	if (UI_bos) {
		render_playlists_bos(screen, playlists, count);
		return;
	}
	GFX_clear(screen);

	UI_renderMenuBar(screen, "Music Player | Playlists");

	ListView* v = &playlist_list_view;
	v->title = NULL; // menu bar drawn above (caller-owned chrome)
	v->font = font.medium;
	v->count = count;
	v->get_row = playlist_list_get_row;
	v->ctx = playlists;
	v->list_id = (const void*)playlists;
	v->empty_title = "No playlists saved";
	v->empty_subtitle = "Press A to create a playlist";
	v->empty_y_label = NULL;
	// Empty state carries all hints; the bottom bar only appears with content
	v->empty_btn_pairs = (char*[]){"B", "BACK", "A", "NEW", NULL};
	v->hint_pairs = (count > 0)
						? (char*[]){"B", "BACK", "A", "SELECT", NULL}
						: NULL;
	UI_listViewRender(v, screen);
}

void render_playlist_detail(SDL_Surface* screen, IndicatorType show_setting,
							const char* playlist_name,
							PlaylistTrack* tracks, int count) {
	(void)show_setting;
	GFX_clear(screen);

	UI_renderMenuBarPage(screen, "Music Player", playlist_name);

	ListView* v = &playlist_detail_view;
	if (UI_bos) { // board v2-MusicPlaylists, opened: the playlist's songs
		static char sub[48];
		snprintf(sub, sizeof(sub), I18N_text("%d songs"), count);
		v->bos_status = I18N_text("Playlists");
		v->bos_title = playlist_name, v->bos_sub = sub;
		v->bos_empty_icon = "list";
		static BosHint hr[2], hl[1];
		hr[0] = (BosHint){"A", I18N_text("Play")};
		hr[1] = (BosHint){"B", I18N_text("Back")};
		hl[0] = (BosHint){"MENU", I18N_text("Options")};
		v->bos_hints = hr, v->bos_nhints = count > 0 ? 2 : 1, v->bos_left = hl, v->bos_nleft = count > 0 ? 1 : 0;
		if (count == 0)
			v->bos_hints = hr + 1;
	}
	v->title = NULL; // menu bar drawn above (caller-owned chrome)
	v->font = font.medium;
	v->count = count;
	v->get_row = playlist_detail_get_row;
	v->ctx = tracks;
	// Identity: the playlist's name pointer, NOT the tracks array — the
	// module refills one static tracks buffer in place for every playlist,
	// so the array pointer never changes. The name pointer differs per
	// playlist entry, making a playlist switch snap instead of glide.
	v->list_id = (const void*)playlist_name;
	v->empty_title = "No tracks in playlist";
	v->empty_subtitle = "Add tracks from the music browser";
	v->empty_y_label = NULL;
	// Empty state carries all hints; the bottom bar only appears with content
	v->empty_btn_pairs = (char*[]){"B", "BACK", NULL};
	v->hint_pairs = (count > 0)
						? (char*[]){"B", "BACK", "A", "PLAY", NULL}
						: NULL;
	UI_listViewRender(v, screen);
}
