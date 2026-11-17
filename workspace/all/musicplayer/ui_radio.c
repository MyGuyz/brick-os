#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_buttonhintbar.h"
#include "ui_menubar.h"
#include "ui_radio.h"
#include "ui_fonts.h"
#include "ui_list.h"
#include "ui_listview.h"
#include "bos_ui.h"
#include "music_bos.h"
#include "i18n.h"
#include "ui_draw.h"
#include "ui_toast.h"
#include "ui_album_art.h"
#include "album_art.h"
#include "radio.h"
#include "music_client.h"

// Full-mode ListViews (the widget owns selection, scroll, glide and marquee;
// module_radio drives input through the accessors below).
static ListView radio_list_view;

static const RadioMetadata* service_radio_metadata(void) {
	static RadioMetadata metadata;
	const MusicSnapshotWire* snapshot = MusicClient_snapshot();
	memset(&metadata, 0, sizeof(metadata));
	strncpy(metadata.title, snapshot->title, sizeof(metadata.title) - 1);
	strncpy(metadata.artist, snapshot->artist, sizeof(metadata.artist) - 1);
	strncpy(metadata.station_name, snapshot->album, sizeof(metadata.station_name) - 1);
	metadata.bitrate = snapshot->radio_bitrate;
	return &metadata;
}
static ListView radio_countries_view;

ListView* RadioList_view(void) {
	return &radio_list_view;
}
ListView* RadioCountries_view(void) {
	return &radio_countries_view;
}

static void radio_list_get_row(void* ctx, int i, bool selected,
							   ListViewRow* out) {
	RadioStation* stations = ctx;
	out->label = stations[i].name;
	if (UI_bos) { // board v2-MusicRadio: initials, genre (slogan), สด on the one playing
		out->sq_text = stations[i].name, out->sq_raw = true;
		out->sub = stations[i].genre[0] ? stations[i].genre : stations[i].slogan;
		const MusicSnapshotWire* snap = MusicClient_snapshot();
		if (snap->source == MUSIC_SOURCE_RADIO && strcmp(snap->current_file, stations[i].url) == 0)
			out->pill = "Live", out->pill_bg = BOS_C_DANGER, out->pill_fg = 0xFFFFFF, out->pill_dot = true;
	} else if (stations[i].genre[0])
		out->annotation = stations[i].genre;
	(void)selected;
}

static void radio_countries_get_row(void* ctx, int i, bool selected,
									ListViewRow* out) {
	const CuratedCountry* countries = ctx;
	out->label = countries[i].name;
	out->annotation = NULL; // country name only (station counts vary from playable)
	(void)selected;
}

// Render the radio station list
void render_radio_list(SDL_Surface* screen, IndicatorType show_setting,
					   const char* toast_message, uint32_t toast_time) {
	(void)show_setting;
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;

	UI_renderMenuBar(screen, "Music Player | Online Radio");

	// Station list
	RadioStation* stations;
	int station_count = Radio_getStations(&stations);

	ListView* v = &radio_list_view;
	v->title = NULL; // menu bar drawn above (caller-owned chrome)
	v->font = font.medium;
	v->count = station_count;
	v->get_row = radio_list_get_row;
	v->ctx = stations;
	v->list_id = (const void*)stations;
	v->empty_title = "No stations saved";
	v->empty_subtitle = "Press A to manage stations";
	v->empty_y_label = NULL;
	// Empty state carries all hints; the bottom bar only appears with content
	v->empty_btn_pairs = (char*[]){"B", "BACK", "A", "MANAGE", NULL};
	v->hint_pairs = (station_count > 0)
						? (char*[]){"B", "BACK", "A", "PLAY", NULL}
						: NULL;
	if (UI_bos) {
		static char sub[64];
		snprintf(sub, sizeof(sub), I18N_text("%d stations · needs WiFi"), station_count);
		v->bos_title = I18N_text("Online Radio"), v->bos_sub = sub;
		v->bos_empty_icon = "radio";
		v->bos_footer = station_count > 0 && !Radio_hasUserStations() ? "Default stations · MENU to manage stations" : NULL;
		static BosHint hr[2], hl[1];
		hr[0] = (BosHint){"A", I18N_text("Listen")};
		hr[1] = (BosHint){"B", I18N_text("Back")};
		hl[0] = (BosHint){"MENU", I18N_text("Options")};
		v->bos_hints = station_count > 0 ? hr : hr + 1, v->bos_nhints = station_count > 0 ? 2 : 1;
		v->bos_left = hl, v->bos_nleft = 1;
		if (station_count == 0)
			v->bos_hints = NULL, v->bos_nhints = 0, v->bos_left = NULL, v->bos_nleft = 0; // the empty state's own
	}
	UI_listViewRender(v, screen);

	// Show note for users using default stations (no custom stations yet)
	if (!UI_bos && station_count > 0 && !Radio_hasUserStations()) {
		int note_y = hh - SCALE1(BUTTON_SIZE + BUTTON_MARGIN + PADDING + 55);

		const char* note1 = "These are default stations";
		SDL_Surface* note1_surf = GFX_renderText(font.tiny, note1, COLOR_GRAY);
		if (note1_surf) {
			SDL_BlitSurface(note1_surf, NULL, screen, &(SDL_Rect){(hw - note1_surf->w) / 2, note_y});
			SDL_FreeSurface(note1_surf);
		}

		const char* note2 = "Press MENU to manage stations";
		SDL_Surface* note2_surf = GFX_renderText(font.tiny, note2, COLOR_GRAY);
		if (note2_surf) {
			SDL_BlitSurface(note2_surf, NULL, screen, &(SDL_Rect){(hw - note2_surf->w) / 2, note_y + SCALE1(14)});
			SDL_FreeSurface(note2_surf);
		}
	}

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);
}

// Helper to get current station by index
static RadioStation* get_station_by_index(int index) {
	RadioStation* stations;
	int count = Radio_getStations(&stations);
	if (count > 0 && index >= 0 && index < count) {
		return &stations[index];
	}
	return NULL;
}

// Render the radio playing screen
// Brick OS (board v2-MusicRadioPlaying): the logo as the picture, สด · bitrate · "1 / 12", the stream's song
static void render_radio_playing_bos(SDL_Surface* screen, int radio_selected) {
	const MusicSnapshotWire* snap = MusicClient_snapshot();
	RadioStation* stations;
	int count = Radio_getStations(&stations);
	RadioStation* st = radio_selected >= 0 && radio_selected < count ? &stations[radio_selected] : NULL;
	MusicPlayView v = {0};
	v.status = I18N_text("Radio");
	v.art = album_art_is_fetching() ? NULL : album_art_get();
	v.art_text = st ? st->name : "FM";
	static char br[24], cnt[24], song[300];
	v.pills[0] = I18N_text("Live");
	int np = 1;
	if (snap->radio_bitrate > 0) {
		snprintf(br, sizeof(br), "%d kbps", snap->radio_bitrate);
		v.pills[np++] = br;
	}
	if (count > 0) {
		snprintf(cnt, sizeof(cnt), "%d / %d", radio_selected + 1, count);
		v.pills[np++] = cnt;
	}
	v.title = st ? st->name : snap->album;
	v.artist = st ? (st->genre[0] ? st->genre : st->slogan) : "";
	RadioState rs = (RadioState)snap->source_state;
	if (rs == RADIO_STATE_CONNECTING || rs == RADIO_STATE_BUFFERING)
		v.album = I18N_text("Connecting...");
	if (snap->title[0]) {
		snprintf(song, sizeof(song), "%s%s%s", snap->title, snap->artist[0] ? " — " : "", snap->artist);
		v.line_extra_label = I18N_text("Playing now");
		v.line_extra = song;
	}
	v.dur_ms = 0;
	v.paused = snap->state != MUSIC_STATE_PLAYING;
	static BosHint hr[2], hl[1];
	hr[0] = (BosHint){"A", I18N_text(v.paused ? "Play" : "Stop")};
	hr[1] = (BosHint){"B", I18N_text("Back")};
	hl[0] = (BosHint){"MENU", I18N_text("Options")};
	v.hints = hr, v.nhints = 2, v.left = hl, v.nleft = 1;
	GFX_clearLayers(LAYER_SCROLLTEXT);
	MusicBos_playing(screen, &v, NULL);
}

void render_radio_playing(SDL_Surface* screen, IndicatorType show_setting, int radio_selected) {
	GFX_clear(screen);
	if (UI_bos) {
		(void)show_setting;
		render_radio_playing_bos(screen, radio_selected);
		return;
	}

	// Render album art as triangular background (if available and not being fetched)
	// Skip during fetch to avoid accessing potentially invalid surface
	if (!album_art_is_fetching()) {
		SDL_Surface* album_art = album_art_get();
		if (album_art && album_art->w > 0 && album_art->h > 0) {
			render_album_art_background(screen, album_art);
		}
	}

	int hw = screen->w;
	int hh = screen->h;
	char truncated[256];

	RadioState state = (RadioState)MusicClient_snapshot()->source_state;
	const RadioMetadata* meta = service_radio_metadata();
	RadioStation* current_station = get_station_by_index(radio_selected);
	RadioStation* stations;
	int station_count = Radio_getStations(&stations);

	// === TOP BAR ===
	int top_y = SCALE1(PADDING);

	// "RADIO" badge with border (like format badge in local player)
	const char* badge_text = "RADIO";
	SDL_Surface* badge_surf = GFX_renderText(font.tiny, badge_text, COLOR_GRAY);
	int badge_h = badge_surf ? badge_surf->h + SCALE1(4) : SCALE1(16);
	int badge_x = SCALE1(PADDING);
	int badge_w = 0;

	if (badge_surf) {
		badge_w = badge_surf->w + SCALE1(10);
		// Draw border (gray)
		SDL_Rect border = {badge_x, top_y, badge_w, badge_h};
		SDL_FillRect(screen, &border, RGB_GRAY);
		SDL_Rect inner = {badge_x + 1, top_y + 1, badge_w - 2, badge_h - 2};
		SDL_FillRect(screen, &inner, RGB_BLACK);
		SDL_BlitSurface(badge_surf, NULL, screen, &(SDL_Rect){badge_x + SCALE1(5), top_y + SCALE1(2)});
		SDL_FreeSurface(badge_surf);
	}

	// Station counter "01 - 12" (like track counter in local player)
	char station_str[32];
	if (radio_selected >= 0 && radio_selected < station_count)
		snprintf(station_str, sizeof(station_str), "%02d - %02d", radio_selected + 1, station_count);
	else
		snprintf(station_str, sizeof(station_str), "-- - %02d", station_count);
	SDL_Surface* station_surf = GFX_renderText(font.tiny, station_str, COLOR_GRAY);
	if (station_surf) {
		int station_x = badge_x + badge_w + SCALE1(8);
		int station_y = top_y + (badge_h - station_surf->h) / 2;
		SDL_BlitSurface(station_surf, NULL, screen, &(SDL_Rect){station_x, station_y});
		SDL_FreeSurface(station_surf);
	}

	// Hardware status (clock, battery) on right
	GFX_blitHardwareGroup(screen, show_setting);

	// === STATION INFO SECTION ===
	int info_y = SCALE1(PADDING + 45);

	// Max widths for text (album art is now only shown as background)
	int max_w_half = (hw - SCALE1(PADDING * 2)) / 2;
	int max_w_full = hw - SCALE1(PADDING * 2);

	// Genre (like Artist in local player) - gray, medium font
	const char* genre = (current_station && current_station->genre[0]) ? current_station->genre : "Radio";
	GFX_truncateText(font.medium, genre, truncated, sizeof(truncated), max_w_half, 0);
	SDL_Surface* genre_surf = GFX_renderText(font.medium, truncated, COLOR_GRAY);
	if (genre_surf) {
		SDL_BlitSurface(genre_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
		info_y += genre_surf->h + SCALE1(2);
		SDL_FreeSurface(genre_surf);
	} else {
		info_y += SCALE1(18);
	}

	// Station name (extra large font) - white. Prefer the user's configured
	// name; fall back to the stream's ICY name only when we have none (some
	// servers announce junk like the mount id, e.g. revma's "488kt4sbv4uvv")
	const char* station_name = (current_station && current_station->name[0]) ? current_station->name
							   : meta->station_name[0]						 ? meta->station_name
																			 : "Unknown Station";
	GFX_truncateText(font.xlarge, station_name, truncated, sizeof(truncated), max_w_full, 0);
	SDL_Surface* name_surf = GFX_renderText(font.xlarge, truncated, COLOR_WHITE);
	if (name_surf) {
		SDL_BlitSurface(name_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
		info_y += name_surf->h + SCALE1(2);
		SDL_FreeSurface(name_surf);
	} else {
		info_y += SCALE1(40);
	}

	// Now Playing - Title on top (white, large), Artist below (gray, small)
	if (meta->title[0]) {
		// Title with text wrapping (max 3 lines)
		TTF_Font* title_font = font.medium;
		const char* src = meta->title;
		int max_lines = 3;
		int lines_rendered = 0;

		while (*src && lines_rendered < max_lines) {
			// Find how many characters fit on this line
			int text_len = strlen(src);
			int char_count = text_len;

			// Binary search for characters that fit
			while (char_count > 0) {
				char line_buf[256];
				int copy_len = (char_count < 255) ? char_count : 255;
				strncpy(line_buf, src, copy_len);
				line_buf[copy_len] = '\0';

				int w, h;
				GFX_measureText(title_font, line_buf, &w, &h);
				if (w <= max_w_full)
					break;
				char_count--;
			}

			if (char_count == 0)
				char_count = 1; // At least one character

			// Try to break at a space if not last line and not at end
			if (lines_rendered < max_lines - 1 && char_count < text_len) {
				int last_space = -1;
				for (int i = char_count - 1; i > 0; i--) {
					if (src[i] == ' ') {
						last_space = i;
						break;
					}
				}
				if (last_space > 0)
					char_count = last_space + 1;
			}

			// Render this line
			char line_buf[256];
			size_t copy_len = (char_count > 0 && char_count < 255) ? (size_t)char_count : 255;
			memcpy(line_buf, src, copy_len);
			line_buf[copy_len] = '\0';

			// Trim trailing space
			while (copy_len > 0 && line_buf[copy_len - 1] == ' ') {
				line_buf[--copy_len] = '\0';
			}

			if (strlen(line_buf) > 0) {
				SDL_Surface* title_surf = GFX_renderText(title_font, line_buf, COLOR_WHITE);
				if (title_surf) {
					SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
					info_y += title_surf->h + SCALE1(2);
					SDL_FreeSurface(title_surf);
				}
			}

			src += char_count;
			// Skip leading spaces on next line
			while (*src == ' ')
				src++;
			lines_rendered++;
		}
	}
	if (meta->artist[0]) {
		// Artist line (smaller font)
		GFX_truncateText(font.small, meta->artist, truncated, sizeof(truncated), max_w_full, 0);
		SDL_Surface* artist_surf = GFX_renderText(font.small, truncated, COLOR_GRAY);
		if (artist_surf) {
			SDL_BlitSurface(artist_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
			info_y += artist_surf->h + SCALE1(2);
			SDL_FreeSurface(artist_surf);
		}
	}

	// Show slogan if no title/artist available
	if (!meta->title[0] && !meta->artist[0] && current_station && current_station->slogan[0]) {
		GFX_truncateText(font.small, current_station->slogan, truncated, sizeof(truncated), max_w_full, 0);
		SDL_Surface* slogan_surf = GFX_renderText(font.small, truncated, COLOR_GRAY);
		if (slogan_surf) {
			SDL_BlitSurface(slogan_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
			info_y += slogan_surf->h + SCALE1(2);
			SDL_FreeSurface(slogan_surf);
		}
	}

	// Position for error message
	int vis_y = hh - SCALE1(90);

	// === BOTTOM BAR (GPU layer - position set here, rendering done independently) ===
	int bottom_y = hh - SCALE1(35);
	int bar_w = SCALE1(60);
	int bar_h = SCALE1(8);
	int bar_x = hw - SCALE1(PADDING) - bar_w;
	int bar_y = bottom_y + SCALE1(4);

	// Set position for GPU rendering (actual rendering happens in main loop)
	RadioStatus_setPosition(bar_x, bar_y, bar_w, bar_h, SCALE1(PADDING), bottom_y);

	// Error message (displayed prominently if in error state)
	if (state == RADIO_STATE_ERROR) {
		SDL_Surface* err_text = GFX_renderText(font.small, Radio_getError(), (SDL_Color){255, 100, 100, 255});
		if (err_text) {
			SDL_BlitSurface(err_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING), vis_y - SCALE1(20)});
			SDL_FreeSurface(err_text);
		}
	}
}

// Render add stations - country selection screen
// Brick OS (board v2-MusicRadioAdd): countries left, the country's stations right
typedef struct {
	const CuratedStation* st;
	const int* order;
} AddCtx;
static void bos_country_row(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	r->label = Radio_getCuratedCountries()[i].name;
}
static void bos_station_row(void* ctx, int i, bool hot, ListViewRow* r) {
	AddCtx* a = ctx;
	const CuratedStation* st = &a->st[a->order[i]];
	(void)hot;
	r->label = st->name;
	r->sq_text = st->name, r->sq_raw = true;
	r->sub = st->genre[0] ? st->genre : st->slogan;
	bool added = Radio_stationExists(st->url);
	r->right_icon = added ? "checkc" : "plus";
	r->right_icon_colour = added ? BOS_C_OK : BOS_C_TEXT2;
}
static void render_radio_columns(SDL_Surface* screen, const char* code, int sel, int* scroll, const int* order, int n) {
	BosColumnsView v = {0};
	v.status = I18N_text("Add stations");
	v.nleft = Radio_getCuratedCountryCount();
	v.left_sel = radio_countries_view.selected, v.left_scroll = &radio_countries_view.scroll;
	v.get_left = bos_country_row;
	static BosHint hr[2], hl[1];
	static char sub[96];
	AddCtx a = {0};
	if (code) {
		int count = 0;
		a.st = Radio_getCuratedStations(code, &count), a.order = order;
		const char* name = "";
		const CuratedCountry* cs = Radio_getCuratedCountries();
		for (int i = 0; i < v.nleft; i++)
			if (strcmp(cs[i].code, code) == 0)
				name = cs[i].name;
		v.title = name;
		snprintf(sub, sizeof(sub), I18N_text("%d stations · from radio-browser.info"), n);
		v.sub = sub;
		v.nright = n, v.right_sel = sel, v.right_scroll = scroll, v.right_focused = true;
		v.get_right = bos_station_row;
		bool added = n > 0 && sel < n && Radio_stationExists(a.st[order[sel]].url);
		hr[0] = (BosHint){"A", I18N_text(added ? "Remove" : "Add")};
		hl[0] = (BosHint){"L1 R1", I18N_text("Country")};
		v.lhints = hl, v.nlhints = 0;
	} else {
		v.title = I18N_text("Add stations");
		v.note = I18N_text("Pick a country to see its stations. They come from radio-browser.info; some may be offline.");
		hr[0] = (BosHint){"A", I18N_text("Open")};
	}
	hr[1] = (BosHint){"B", I18N_text("Back")};
	v.hints = hr, v.nhints = 2;
	v.ctx = &a;
	radio_countries_view.input_pending = false;
	BOS_columns(screen, &v);
}

bool RadioAdd_bosMoving(void) {
	return UI_bos && BOS_columnsMoving();
}

void render_radio_add(SDL_Surface* screen, IndicatorType show_setting,
					  const char* toast_message, uint32_t toast_time) {
	(void)show_setting;
	if (UI_bos) {
		radio_countries_view.count = Radio_getCuratedCountryCount();
		render_radio_columns(screen, NULL, 0, NULL, NULL, 0);
		UI_renderToast(screen, toast_message, toast_time);
		return;
	}
	GFX_clear(screen);

	UI_renderMenuBar(screen, "Music Player | Manage Stations");

	// Country list
	int country_count = Radio_getCuratedCountryCount();
	const CuratedCountry* countries = Radio_getCuratedCountries();

	ListView* v = &radio_countries_view;
	v->title = NULL; // menu bar drawn above (caller-owned chrome)
	v->font = font.medium;
	v->count = country_count;
	v->get_row = radio_countries_get_row;
	v->ctx = (void*)countries;
	v->list_id = (const void*)countries;
	v->empty_title = "No countries"; // now possible if a fetch fails
	v->empty_subtitle = "Press MENU to refresh";
	v->hint_pairs = (char*[]){"B", "BACK", "A", "SELECT", NULL};
	UI_listViewRender(v, screen);

	UI_renderToast(screen, toast_message, toast_time);
}

// Render add stations - station selection screen
void render_radio_add_stations(SDL_Surface* screen, IndicatorType show_setting,
							   const char* country_code,
							   int add_station_selected, int* add_station_scroll,
							   const int* sorted_indices, int sorted_count,
							   const char* toast_message, uint32_t toast_time) {
	(void)show_setting;
	render_radio_columns(screen, country_code, add_station_selected, add_station_scroll, sorted_indices, sorted_count);
	UI_renderToast(screen, toast_message, toast_time);
}

// Render help/instructions screen
void render_radio_help(SDL_Surface* screen, IndicatorType show_setting, int* help_scroll) {
	(void)show_setting, (void)help_scroll;
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("Add stations"));
	int y = BOS_px(BOS_BAR_H) + BOS_largeTitle(screen, BOS_px(BOS_BAR_H), I18N_text("Add your own stations"), NULL);
	int x = BOS_px(40), w = screen->w - 2 * x, pad = BOS_px(28);
	static const char* para[] = {
		"Edit this file on the SD card:", "/.userdata/shared/music-player/radio/stations.txt",
		"One station per line: Name|URL|Genre|Slogan", "e.g. My Radio|http://example.com/stream|Music|Slogan",
		"MP3, AAC and M3U8 streams · up to 32 stations · the slogan shows when there is no song name",
		"More stations: fmstream.org"};
	int h = BOS_px(560) - y + BOS_px(64);
	BOS_rrect(screen, x, y, w, BOS_px(440), BOS_px(24), BOS_theme()->surface);
	int ty = y + pad;
	for (int i = 0; i < 6; i++) {
		bool code = i == 1 || i == 3;
		ty += BOS_textWrap(screen, code ? para[i] : I18N_text(para[i]), code ? 26 : 30, code ? 600 : 400,
						   code ? BOS_C_TEXT : BOS_C_TEXT2, 1, x + pad, ty, w - 2 * pad, 2) + BOS_px(12);
	}
	(void)h;
	BosHint hr[1] = {{"B", I18N_text("Back")}};
	BOS_hintBar(screen, hr, 1, NULL, 0);
}

// === GPU STATUS AND BUFFER INDICATOR ===
// Following the same pattern as Spectrum (spectrum.c) and PlayTime (ui_music.c):
// - Position is set during main screen render (when dirty)
// - GPU layer rendering happens independently in main loop

static int status_bar_x = 0, status_bar_y = 0, status_bar_w = 0, status_bar_h = 0;
static int status_left_x = 0, status_left_y = 0;
static bool status_position_set = false;


void RadioStatus_setPosition(int bar_x, int bar_y, int bar_w, int bar_h,
							 int left_x, int left_y) {
	status_bar_x = bar_x;
	status_bar_y = bar_y;
	status_bar_w = bar_w;
	status_bar_h = bar_h;
	status_left_x = left_x;
	status_left_y = left_y;
	status_position_set = true;
}

void RadioStatus_clear(void) {
	status_position_set = false;
	PLAT_clearLayers(LAYER_BUFFER);
	PLAT_GPU_Flip();
}

bool RadioStatus_needsRefresh(void) {
	if (!status_position_set)
		return false;
	RadioState state = (RadioState)MusicClient_snapshot()->source_state;
	// Also refresh once when transitioning to STOPPED, to clear the layer
	static RadioState prev_state = RADIO_STATE_STOPPED;
	if (state == RADIO_STATE_STOPPED) {
		if (prev_state != RADIO_STATE_STOPPED) {
			prev_state = state;
			return true;
		}
		return false;
	}
	prev_state = state;
	return true;
}

void RadioStatus_renderGPU(void) {
	if (!status_position_set)
		return;

	RadioState state = (RadioState)MusicClient_snapshot()->source_state;

	// When stopped, clear the status layer and reset cache
	static RadioState last_state = RADIO_STATE_STOPPED;
	static int last_bitrate = 0;
	static int last_buf_pct = -1;

	if (state == RADIO_STATE_STOPPED) {
		if (last_state != RADIO_STATE_STOPPED) {
			PLAT_clearLayers(LAYER_BUFFER);
			PLAT_GPU_Flip();
			last_state = RADIO_STATE_STOPPED;
			last_bitrate = 0;
			last_buf_pct = -1;
		}
		return;
	}

	float buffer_level = MusicClient_snapshot()->radio_buffer_percent / 100.0f;

	// Get bitrate from metadata
	const RadioMetadata* meta = service_radio_metadata();
	int current_bitrate = meta ? meta->bitrate : 0;

	// Skip expensive surface recreation if nothing changed
	int buf_pct = (int)(buffer_level * 100);
	if (state == last_state && current_bitrate == last_bitrate && buf_pct == last_buf_pct) {
		return;
	}
	last_state = state;
	last_bitrate = current_bitrate;
	last_buf_pct = buf_pct;

	// Get status text
	// Show "buffering" only during initial connect or actual rebuffer (low buffer).
	// Once buffer is healthy, show "streaming" even if state is still BUFFERING.
	const char* status_text = "";
	switch (state) {
	case RADIO_STATE_CONNECTING:
		status_text = "connecting";
		break;
	case RADIO_STATE_BUFFERING:
		status_text = (buffer_level < 0.5f) ? "buffering" : "streaming";
		break;
	case RADIO_STATE_PLAYING:
		status_text = "streaming";
		break;
	case RADIO_STATE_ERROR:
		status_text = "error";
		break;
	default:
		break;
	}

	// Prepare bitrate string
	char bitrate_str[32] = "";
	if (current_bitrate > 0) {
		snprintf(bitrate_str, sizeof(bitrate_str), "%d kbps", current_bitrate);
	}

	// Measure all elements
	TTF_Font* bitrate_font = font.small;
	TTF_Font* status_font = font.tiny;

	int bitrate_w = 0, bitrate_h = 0;
	if (bitrate_str[0]) {
		GFX_measureText(bitrate_font, bitrate_str, &bitrate_w, &bitrate_h);
	}

	int status_w = 0, status_h = 0;
	if (status_text[0]) {
		GFX_measureText(status_font, status_text, &status_w, &status_h);
	}

	// Layout: [bitrate] [status] ----space---- [buffer bar]
	int gap = SCALE1(6);
	int left_x = status_left_x;
	int right_x = status_bar_x + status_bar_w;

	// Calculate line height for vertical centering
	int line_h = bitrate_h;
	if (status_h > line_h)
		line_h = status_h;
	if (status_bar_h > line_h)
		line_h = status_bar_h;
	int base_y = status_left_y;

	// Surface covers from left edge to right edge
	int surface_w = right_x - left_x;
	int surface_h = line_h;

	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0,
														  surface_w, surface_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surface)
		return;

	SDL_FillRect(surface, NULL, 0); // Transparent background

	int x_offset = 0;

	// Draw bitrate on left (white)
	if (bitrate_str[0]) {
		SDL_Color color = {255, 255, 255, 255};
		SDL_Surface* text_surf = GFX_renderText(bitrate_font, bitrate_str, color);
		if (text_surf) {
			int y_pos = (line_h - bitrate_h) / 2;
			SDL_Rect dst = {x_offset, y_pos, 0, 0};
			SDL_BlitSurface(text_surf, NULL, surface, &dst);
			SDL_FreeSurface(text_surf);
		}
		x_offset += bitrate_w + gap;
	}

	// Draw status text next to bitrate (gray)
	if (status_text[0]) {
		SDL_Color color = {128, 128, 128, 255};
		SDL_Surface* text_surf = GFX_renderText(status_font, status_text, color);
		if (text_surf) {
			int y_pos = (line_h - status_h) / 2;
			SDL_Rect dst = {x_offset, y_pos, 0, 0};
			SDL_BlitSurface(text_surf, NULL, surface, &dst);
			SDL_FreeSurface(text_surf);
		}
	}

	// Draw buffer bar on right side
	int bar_x_in_surface = surface_w - status_bar_w;
	int bar_y_pos = (line_h - status_bar_h) / 2;

	// Buffer bar background (dark gray)
	SDL_Rect bar_bg = {bar_x_in_surface, bar_y_pos, status_bar_w, status_bar_h};
	SDL_FillRect(surface, &bar_bg, SDL_MapRGBA(surface->format, 60, 60, 60, 255));

	// Buffer fill (white)
	int fill_w = (int)(status_bar_w * buffer_level);
	if (fill_w > 0) {
		SDL_Rect bar_fill = {bar_x_in_surface, bar_y_pos, fill_w, status_bar_h};
		SDL_FillRect(surface, &bar_fill, SDL_MapRGBA(surface->format, 255, 255, 255, 255));
	}

	// Render to GPU layer
	PLAT_clearLayers(LAYER_BUFFER);
	PLAT_drawOnLayer(surface, left_x, base_y, surface_w, surface_h, 1.0f, false, LAYER_BUFFER);
	SDL_FreeSurface(surface);

	PLAT_GPU_Flip();
}
