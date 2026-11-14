#include <stdio.h>
#include <string.h>

#include "vp_defines.h"
#include "api.h"
#include "ui_buttonhintbar.h"
#include "ui_emptystate.h"
#include "ui_menubar.h"
#include "ui_player.h"
#include "vid_bos.h"
#include "i18n.h"
#include "ui_draw.h"
#include "ui_icons.h"
#include "ui_list.h"
#include "ui_listview.h"
#include "video_browser.h"
#include "positions.h"

// Full-mode ListView for the file browser (the widget owns selection,
// scroll, glide and marquee; module_player drives input through the
// accessor below).
static ListView video_browser_view;
static char browser_row_buf[256];

ListView* VideoBrowser_view(void) {
	return &video_browser_view;
}

// Build the display name shown for a browser entry (matches the row loop).
static void browser_entry_display(const VideoFileEntry* entry, char* out, size_t out_sz) {
	if (entry->is_dir) {
		if (Icons_isLoaded()) {
			strncpy(out, entry->name, out_sz - 1);
			out[out_sz - 1] = '\0';
		} else {
			snprintf(out, out_sz, "[%s]", entry->name);
		}
	} else {
		VideoBrowser_getDisplayName(entry->name, out, out_sz);
	}
}

// Row provider for the browser ListView: label via browser_entry_display,
// icon variant follows the widget-supplied `selected` flag (the
// glide-tracking row_sel), preserving the pill-follows-icon behavior.
static void browser_get_row(void* ctx, int i, bool selected, ListViewRow* out) {
	VideoBrowserContext* b = ctx;
	VideoFileEntry* entry = &b->entries[i];
	browser_entry_display(entry, browser_row_buf, sizeof(browser_row_buf));
	out->label = browser_row_buf;
	if (UI_bos) { // Brick OS rows: a folder, or the video's picture (a film card without one) and how far
		if (entry->is_dir && strcmp(entry->name, "..") == 0)
			out->label = "Back", out->sq_icon = "chevl", out->sq_color = 0x8E8E93;
		else if (entry->is_dir)
			out->sq_icon = "folder", out->sq_color = 0x0A84FF, out->chevron = true;
		else {
			out->art = VidBos_thumb(entry->path), out->art_wide = true;
			if (!out->art)
				out->sq_icon = "film", out->sq_color = 0x3A3A3C, out->sq_raw = true;
			static char sub[96];
			const char* dot = strrchr(entry->name, '.');
			int sec = Positions_get(entry->path);
			char ext[16] = "";
			if (dot)
				for (int k = 0; dot[k + 1] && k < 15; k++)
					ext[k] = (dot[k + 1] >= 'a' && dot[k + 1] <= 'z') ? dot[k + 1] - 32 : dot[k + 1], ext[k + 1] = 0;
			if (sec > 0)
				snprintf(sub, sizeof(sub), I18N_text("Watched to %d:%02d · %s"), sec / 60, sec % 60, ext);
			else
				snprintf(sub, sizeof(sub), "%s", ext);
			out->sub = sub;
		}
		return;
	}
	if (Icons_isLoaded()) {
		if (entry->is_dir)
			out->icon = Icons_getFolder(selected);
		else
			out->icon = Icons_getForFormat(VideoBrowser_detectFormat(entry->name), selected);
	}
}

// Render the video file browser
void render_video_browser(SDL_Surface* screen, IndicatorType show_setting,
						  VideoBrowserContext* ctx) {
	(void)show_setting;
	GFX_clear(screen);

	// Header title: "Media Player | Videos" at root, "Videos | <folder>" in subdirectories (LIST-LAYOUT §10.1)
	const char* header_title = "Media Player | Videos";
	char folder_title[300];
	if (strcmp(ctx->current_path, VIDEO_ROOT) != 0) {
		const char* slash = strrchr(ctx->current_path, '/');
		if (slash && slash[1] != '\0')
			header_title = UI_pageTitle(folder_title, sizeof(folder_title), "Videos", slash + 1);
	}

	UI_renderMenuBar(screen, UI_bos ? "Video" : header_title);

	ListView* v = &video_browser_view;
	v->title = NULL; // menu bar drawn above (caller-owned chrome)
	v->font = font.medium;
	v->count = ctx->entry_count;
	v->get_row = browser_get_row;
	v->ctx = ctx;
	v->list_id = (const void*)ctx->entries;
	v->empty_title = "No videos found";
	v->empty_subtitle = "Add videos to /Videos on your SD card";
	// A resumes a video with a saved position (play-from-start moves to the
	// MENU context menu); surface that on the hint bar for the selected row.
	static char* hints_open[] = {"B", "BACK", "A", "OPEN", NULL};
	static char* hints_resume[] = {"B", "BACK", "A", "RESUME", NULL};
	VideoFileEntry* sel =
		(ctx->entry_count > 0 && v->selected >= 0 && v->selected < ctx->entry_count)
			? &ctx->entries[v->selected]
			: NULL;
	bool sel_can_resume = sel && !sel->is_dir && Positions_get(sel->path) > 0;
	v->hint_pairs = sel_can_resume ? hints_resume : hints_open;
	if (UI_bos) {
		static char title[256], sub[64];
		const char* slash = strrchr(ctx->current_path, '/');
		bool root = strcmp(ctx->current_path, VIDEO_ROOT) == 0;
		snprintf(title, sizeof(title), "%s", !root && slash ? slash + 1 : I18N_text("Videos on the card"));
		int files = 0, dirs = 0;
		for (int i = 0; i < ctx->entry_count; i++)
			ctx->entries[i].is_dir ? (dirs += strcmp(ctx->entries[i].name, "..") != 0) : files++;
		snprintf(sub, sizeof(sub), I18N_text("%d files · %d folders"), files, dirs);
		v->bos_status = NULL;
		v->bos_title = title, v->bos_sub = sub;
		v->bos_empty_icon = "film";
		v->empty_title = "No videos yet";
		v->empty_subtitle = "Put videos in the Videos folder of the SD card, or send them with WiFi Transfer";
		static BosHint hr[2], hl[1];
		hr[0] = (BosHint){"A", I18N_text(sel && sel->is_dir ? "Open" : sel_can_resume ? "Continue" : "Watch")};
		hr[1] = (BosHint){"B", I18N_text("Back")};
		hl[0] = (BosHint){"MENU", I18N_text("Options")};
		v->bos_hints = ctx->entry_count > 0 ? hr : hr + 1, v->bos_nhints = ctx->entry_count > 0 ? 2 : 1;
		v->bos_left = hl, v->bos_nleft = 1;
	}

	UI_listViewRender(v, screen);
}
