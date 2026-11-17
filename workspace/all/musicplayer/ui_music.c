#include <stdio.h>
#include <string.h>
#include "defines.h"
#include "api.h"
#include "ui_buttonhintbar.h"
#include "ui_emptystate.h"
#include "ui_menubar.h"
#include "ui_music.h"
#include "ui_icons.h"
#include "ui_utils.h"
#include "utils.h"
#include "ui_list.h"
#include "ui_listview.h"
#include "ui_album_art.h"
#include "spectrum.h"
#include "music_client.h"
#include "album_art.h"
#include "lyrics.h"
#include "settings.h"
#include "ui_text_sizes.h"
#include "i18n.h"
#include "ui_draw.h"
#include "music_bos.h"
#include <SDL2/SDL_image.h>
#include <unistd.h>

// The folder's own picture (cover.jpg, folder.jpg …), loaded once per folder: the songs' picture in the list,
// and the first place the player looks for art (DESIGN.md 7.2)
static SDL_Surface* folder_art;
static char folder_art_dir[512];
SDL_Surface* MusicBos_folderArt(const char* dir) {
	if (!dir)
		return NULL;
	if (strcmp(dir, folder_art_dir) == 0)
		return folder_art;
	if (folder_art) {
		BOS_picForget(folder_art);
		SDL_FreeSurface(folder_art);
		folder_art = NULL;
	}
	snprintf(folder_art_dir, sizeof(folder_art_dir), "%s", dir);
	static const char* names[] = {"cover.jpg", "folder.jpg", "cover.png", "folder.png", "Cover.jpg", "Folder.jpg",
								  "front.jpg", "AlbumArt.jpg", NULL};
	char p[768];
	for (int i = 0; names[i] && !folder_art; i++) {
		snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
		if (access(p, R_OK) == 0)
			folder_art = IMG_Load(p);
	}
	return folder_art;
}

// Short kHz label for the rate badge: 48000 -> "48", 44100 -> "44.1"
static void format_khz(int rate, char* buf, size_t size) {
	if (rate % 1000 == 0)
		snprintf(buf, size, "%d", rate / 1000);
	else
		snprintf(buf, size, "%.1f", rate / 1000.0);
}

// Full-mode ListView for the file browser (the widget owns selection,
// scroll, glide and marquee; module_player drives input through the
// accessor below).
static ListView browser_view;
static char browser_row_buf[256];

ListView* MusicBrowser_view(void) {
	return &browser_view;
}

// Build the display label for a browser entry (folder brackets / play-all
// prefix only when icons are unavailable)
static void browser_entry_display(const FileEntry* entry, char* display, size_t size) {
	if (Icons_isLoaded()) {
		// With icons, use clean names
		if (entry->is_dir || entry->is_play_all) {
			snprintf(display, size, "%s", entry->name);
		} else {
			Browser_getDisplayName(entry->name, display, size);
		}
	} else {
		// Without icons, use text indicators
		if (entry->is_dir) {
			snprintf(display, size, "[%s]", entry->name);
		} else if (entry->is_play_all) {
			snprintf(display, size, "> %s", entry->name);
		} else {
			Browser_getDisplayName(entry->name, display, size);
		}
	}
}

// Row provider for the browser ListView: label via browser_entry_display,
// icon variant follows the widget-supplied `selected` flag (the
// glide-tracking row_sel), preserving the pill-follows-icon behavior.
static void browser_get_row(void* ctx, int i, bool selected, ListViewRow* out) {
	BrowserContext* b = ctx;
	FileEntry* entry = &b->entries[i];
	browser_entry_display(entry, browser_row_buf, sizeof(browser_row_buf));
	out->label = browser_row_buf;
	(void)selected;
	// Brick OS rows (DESIGN.md 7.2): a folder squircle with a chevron, play-all, or the song with the folder's picture
	if (entry->is_dir && strcmp(entry->name, "..") == 0) {
		out->label = "Back", out->sq_icon = "chevl", out->sq_color = 0x8E8E93;
	} else if (entry->is_dir) {
		out->sq_icon = "folder", out->sq_color = 0x0A84FF, out->chevron = true;
	} else if (entry->is_play_all) {
		out->sq_icon = "play", out->sq_color = 0x30D158;
	} else {
		out->art = MusicBos_folderArt(b->current_path);
		if (!out->art)
			out->sq_icon = "music", out->sq_color = 0x5E5CE6;
		out->sub = get_format_name(entry->format);
		const MusicSnapshotWire* snap = MusicClient_snapshot();
		out->playing = snap && snap->current_file[0] && strcmp(snap->current_file, entry->path) == 0;
	}
}

// Playtime GPU state
static int playtime_x = 0, playtime_y = 0, playtime_dur_x = 0;
static int last_rendered_position = -1;
static int last_rendered_duration = -1;
static bool playtime_position_set = false;

// Lyrics GPU state
static int lyrics_gpu_x = 0, lyrics_gpu_y = 0, lyrics_gpu_max_w = 0, lyrics_gpu_max_h = 0;
static int last_lyric_index = -2;
static bool lyrics_gpu_position_set = false;
static char last_lyric_line[256] = "";
static char last_next_lyric_line[256] = "";

// Render the file browser
void render_browser(SDL_Surface* screen, IndicatorType show_setting, BrowserContext* browser) {
	(void)show_setting;
	GFX_clear(screen);

	// "Music Player | Files" at the root, "Files | <folder>" deeper (LIST-LAYOUT §10.1)
	const char* header_title = "Music Player | Files";
	char folder_title[600];
	if (Browser_hasParent(browser)) {
		const char* slash = strrchr(browser->current_path, '/');
		if (slash && slash[1] != '\0')
			header_title = UI_pageTitle(folder_title, sizeof(folder_title), "Files", slash + 1);
	}
	header_title = "Music files"; // Brick OS: the status bar names the part of the app; the folder is the large title
	UI_renderMenuBar(screen, header_title);

	// Empty state at root: no playable music anywhere
	if (Browser_countAudioFiles(browser) == 0 && !Browser_hasParent(browser)) {
		if (!Browser_hasAudioRecursive(browser->current_path)) {
			UI_renderEmptyState(screen, "No music yet", "Put music in the Music folder of the SD card, or send it with WiFi Transfer", NULL);
			return;
		}
	}

	ListView* v = &browser_view;
	v->title = NULL; // menu bar drawn above (caller-owned chrome)
	v->font = font.medium;
	// large title: the folder (the root is ไฟล์เพลง), with what it holds
	static char title[256], sub[96];
	const char* slash = strrchr(browser->current_path, '/');
	snprintf(title, sizeof(title), "%s", Browser_hasParent(browser) && slash ? slash + 1 : I18N_text("Music files"));
	int songs = Browser_countAudioFiles(browser), dirs = 0;
	for (int i = 0; i < browser->entry_count; i++)
		dirs += browser->entries[i].is_dir && strcmp(browser->entries[i].name, "..") != 0;
	if (dirs)
		snprintf(sub, sizeof(sub), I18N_text("%d songs · %d folders"), songs, dirs);
	else
		snprintf(sub, sizeof(sub), I18N_text("%d songs"), songs);
	v->bos_title = title, v->bos_sub = sub;
	static BosHint hr[3], hl[1];
	int nr = 0;
	hr[nr++] = (BosHint){"A", I18N_text(browser->entry_count > 0 && browser->entries[v->selected < browser->entry_count ? v->selected : 0].is_dir ? "Open" : "Play")};
	hr[nr++] = (BosHint){"B", I18N_text("Back")};
	if (songs > 0)
		hr[nr++] = (BosHint){"Y", I18N_text("Play all")};
	hl[0] = (BosHint){"MENU", I18N_text("Options")};
	v->bos_hints = hr, v->bos_nhints = nr, v->bos_left = hl, v->bos_nleft = 1;
	v->bos_empty_icon = "music";
	v->count = browser->entry_count;
	v->get_row = browser_get_row;
	v->ctx = browser;
	v->list_id = (const void*)browser->entries;
	v->empty_title = "No music yet";
	v->empty_subtitle = "Put music in the Music folder of the SD card, or send it with WiFi Transfer";
	v->hint_pairs = (char*[]){"B", "BACK", "A", "SELECT", NULL};
	UI_listViewRender(v, screen);
}

// Brick OS (DESIGN.md 7.2, boards v2-MusicPlaying / -Lyrics / -Visualizer): everything on the page, redrawn once a
// second (the time) or when a lyric line turns; only the visualizer keeps its own layer
static bool bos_page_wanted;
static void render_playing_bos(SDL_Surface* screen, BrowserContext* browser, bool shuffle_enabled,
							   bool repeat_enabled, int playlist_track_num, int playlist_total) {
	const MusicSnapshotWire* snap = MusicClient_snapshot();
	MusicPlayView v = {0};
	v.status = I18N_text("Music");
	v.art = album_art_get();
	static char rate[24], count[24], title[256];
	v.pills[0] = get_format_name(snap->format);
	int src_rate = snap->source_sample_rate, out_rate = snap->output_sample_rate;
	if (src_rate > 0 && out_rate > 0) {
		char a[8], b[8];
		format_khz(src_rate, a, sizeof(a));
		format_khz(out_rate, b, sizeof(b));
		if (src_rate == out_rate)
			snprintf(rate, sizeof(rate), "%s kHz", a);
		else
			snprintf(rate, sizeof(rate), "%s→%s kHz", a, b);
		v.pills[1] = rate;
	}
	int track_num = playlist_total > 0 ? playlist_track_num : Browser_getCurrentTrackNumber(browser, browser_view.selected);
	int total = playlist_total > 0 ? playlist_total : Browser_countAudioFiles(browser);
	if (total > 0) {
		snprintf(count, sizeof(count), "%d / %d", track_num, total);
		v.pills[v.pills[1] ? 2 : 1] = count;
	}
	if (snap->title[0])
		snprintf(title, sizeof(title), "%s", snap->title);
	else {
		const char* f = strrchr(snap->current_file, '/');
		Browser_getDisplayName(f ? f + 1 : snap->current_file, title, sizeof(title));
	}
	v.title = title;
	v.artist = snap->artist[0] ? snap->artist : I18N_text("Unknown Artist");
	v.album = snap->album;
	v.lyrics_on = Settings_getLyricsEnabled();
	if (v.lyrics_on) {
		v.lyric[1] = Lyrics_getCurrentLine(snap->position_ms);
		v.lyric[2] = Lyrics_getNextLine();
		v.lyric[3] = Lyrics_getLineAfter(2);
	}
	v.pos_ms = snap->position_ms, v.dur_ms = snap->duration_ms;
	v.paused = snap->state != MUSIC_STATE_PLAYING;
	v.modes = true, v.shuffle = shuffle_enabled, v.repeat = repeat_enabled;
	static BosHint hr[4], hl[1];
	hr[0] = (BosHint){"A", I18N_text(v.paused ? "Play" : "Pause")};
	hr[1] = (BosHint){"B", I18N_text("Back")};
	hr[2] = (BosHint){"X", I18N_text("Shuffle")};
	hr[3] = (BosHint){"Y", I18N_text("Repeat")};
	hl[0] = (BosHint){"MENU", I18N_text("Options")};
	v.hints = hr, v.nhints = 4, v.left = hl, v.nleft = 1;
	SDL_Rect vis = {0};
	GFX_clearLayers(LAYER_SCROLLTEXT);
	MusicBos_playing(screen, &v, &vis);
	if (vis.w > 0)
		Spectrum_setPosition(vis.x, vis.y, vis.w, vis.h);
	else
		Spectrum_clearPosition();
	last_lyric_index = Lyrics_getCurrentIndex();
	last_rendered_position = snap->position_ms / 1000;
	bos_page_wanted = false;
}

// Render the now playing screen
void render_playing(SDL_Surface* screen, IndicatorType show_setting, BrowserContext* browser,
					bool shuffle_enabled, bool repeat_enabled,
					int playlist_track_num, int playlist_total) {
	(void)show_setting;
	render_playing_bos(screen, browser, shuffle_enabled, repeat_enabled, playlist_track_num, playlist_total);
}

// Check if player title has active scrolling (for refresh optimization)
bool player_needs_scroll_refresh(void) {
	return false;
}

// Check if player title scroll needs a render to transition (delay phase)
bool player_title_scroll_needs_render(void) {
	const MusicSnapshotWire* snap = MusicClient_snapshot();
	if (snap->state == MUSIC_STATE_PLAYING && snap->position_ms / 1000 != last_rendered_position)
		bos_page_wanted = true;
	if (Settings_getLyricsEnabled() && Lyrics_getCurrentIndex() != last_lyric_index)
		bos_page_wanted = true;
	return bos_page_wanted;
}

// Animate player title scroll (GPU mode, no screen redraw needed)
void player_animate_scroll(void) {
	return;
}

// === PLAYTIME GPU FUNCTIONS ===

void PlayTime_setPosition(int x, int y, int duration_x) {
	playtime_x = x;
	playtime_y = y;
	playtime_dur_x = duration_x;
	playtime_position_set = true;
}

void PlayTime_clear(void) {
	playtime_position_set = false;
	last_rendered_position = -1;
	last_rendered_duration = -1;
	// Note: Caller should clear LAYER_PLAYTIME and call PLAT_GPU_Flip() if needed
}

bool PlayTime_needsRefresh(void) {
	return false;
}

void PlayTime_renderGPU(void) {
	return;
}

// === LYRICS GPU FUNCTIONS ===

void Lyrics_setGPUPosition(int x, int y, int max_w, int max_h) {
	lyrics_gpu_x = x;
	lyrics_gpu_y = y;
	lyrics_gpu_max_w = max_w;
	lyrics_gpu_max_h = max_h;
	lyrics_gpu_position_set = true;
}

void Lyrics_clearGPU(void) {
	lyrics_gpu_position_set = false;
	last_lyric_index = -2;
	last_lyric_line[0] = '\0';
	last_next_lyric_line[0] = '\0';
	PLAT_clearLayers(LAYER_LYRICS);
}

bool Lyrics_GPUneedsRefresh(void) {
	return false;
}

#define LYRICS_MAX_ROWS 16

void Lyrics_renderGPU(void) {
	return;
}
