#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <msettings.h>

#include "api.h"
#include "bos_ui.h"
#include "i18n.h"
#include "iv_defines.h"
#include "iv_browser.h"
#include "iv_fileops.h"
#include "iv_loader.h"
#include "iv_viewer.h"
#include "ui_buttonhintbar.h"
#include "ui_confirmdialog.h"
#include "ui_contextmenu.h"
#include "ui_draw.h"
#include "ui_emptystate.h"
#include "ui_icons.h"
#include "ui_keyboard.h"
#include "ui_listview.h"
#include "ui_menubar.h"
#include "ui_splash.h"
#include "ui_toast.h"

#define ICON_FOLDER RES_PATH "/icon-folder.png"
#define ICON_IMAGE RES_PATH "/icon-image.png"

#define IV_CTX_RENAME 1
#define IV_CTX_DELETE 2

typedef enum { SCREEN_BROWSER,
			   SCREEN_VIEWER } AppScreen;

static volatile sig_atomic_t quit = false;
static SDL_Surface* screen;
static AppScreen app_screen = SCREEN_BROWSER;
static ImageBrowserContext browser;
static ListView browser_view;

static char browser_toast[64];
static uint32_t browser_toast_time;
static SDL_Surface* icon_folder;
static SDL_Surface* icon_folder_inv;
static SDL_Surface* icon_image;
static SDL_Surface* icon_image_inv;

// Remembers which row the browser context menu (MENU tap) was opened for,
// since the ListView is not fed input while the menu is up.
static int ctx_target_index = -1;

// Preview pane geometry, computed once from the screen size in main().
static int pane_x, pane_y, pane_w, pane_h;

// Preview debounce/result state. preview_want is the selected file's path
// ("" for a dir/empty selection); preview_requested/preview_have/preview_error
// track, by path comparison, what's been asked for, what's on screen, and
// what most recently failed, so a stale result for an old selection is never
// drawn.
static char preview_want[512];
static uint32_t preview_want_tick;
static char preview_requested[512];
static char preview_have[512];
static char preview_error[512];
static SDL_Surface* preview_surf;
static int preview_ow, preview_oh;

static void sigHandler(int sig) {
	if (sig == SIGINT || sig == SIGTERM)
		quit = true;
}

static void load_image_directory(const char* path) {
	ImageBrowser_loadDirectory(&browser, path, IMAGES_ROOT);
	UI_listViewReset(&browser_view, browser.entry_count, browser.entries);
}

// Format a file size for the preview metadata line: "245 KB" below 1 MB,
// "1.2 MB" at or above.
static void format_size(long bytes, char* out, size_t out_len) {
	if (bytes >= 1024 * 1024)
		snprintf(out, out_len, "%.1f MB", bytes / (1024.0 * 1024.0));
	else
		snprintf(out, out_len, "%ld KB", bytes / 1024);
}

// ---- Brick OS look (DESIGN 7.9) ----
#define IV_BOX_H 352 // mockup px: the picture's box; the name and its line go under it
static BosGlide iv_pill, iv_scroll;
static void iv_row(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)hot;
	ImageBrowserContext* b = ctx;
	ImageEntry* e = &b->entries[i];
	static char name[256], sb[64];
	if (e->is_dir) {
		if (!strcmp(e->name, ".."))
			*r = (ListViewRow){.label = I18N_text("Up a folder"), .sq_icon = "chevl", .sq_color = 0x48484A, .sq_raw = true};
		else {
			snprintf(name, sizeof(name), "%s", I18N_text(e->name));
			*r = (ListViewRow){.label = name, .sq_icon = "folder", .sq_color = BOS_C_PRIMARY, .chevron = true};
		}
	} else {
		ImageBrowser_getDisplayName(e->name, name, sizeof(name));
		format_size(e->size_bytes, sb, sizeof(sb));
		*r = (ListViewRow){.label = name, .sub = sb, .sq_icon = "image", .sq_color = 0xBF5AF2};
	}
}
static void render_browser_bos(void) {
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	const char* title = I18N_text("Images");
	if (strcmp(browser.current_path, IMAGES_ROOT) != 0) {
		const char* slash = strrchr(browser.current_path, '/');
		if (slash && slash[1])
			title = I18N_text(slash + 1);
	}
	BOS_statusBar(screen, title);
	int top = BOS_px(BOS_BAR_H), bottom = screen->h - BOS_px(BOS_BAR_H) - BOS_px(12);
	bool root = strcmp(browser.current_path, IMAGES_ROOT) == 0;
	BosHint h[] = {{"A", I18N_text("Open")}, {"B", I18N_text(root ? "Exit" : "Back")}};
	BosHint l[] = {{"MENU", I18N_text("Options")}};
	if (browser.entry_count <= 0) {
		BOS_resultCard(screen, "image", BOS_theme()->surface2, "No images found", "Screenshots you take will appear here");
		BOS_hintBar(screen, h + 1, 1, NULL, 0);
		return;
	}
	int lx = BOS_px(24), lw = BOS_px(470);
	BOS_groupRows(screen, &(SDL_Rect){lx, top + BOS_px(8), lw, bottom - top - BOS_px(8)}, BOS_px(84), browser.entry_count,
				  browser_view.selected, true, &browser_view.scroll, &iv_pill, &iv_scroll, false, iv_row, &browser);
	browser_view.input_pending = false;
	ImageEntry* e = browser_view.selected >= 0 && browser_view.selected < browser.entry_count ? &browser.entries[browser_view.selected] : NULL;
	if (e && !e->is_dir) {
		int bh = BOS_px(IV_BOX_H), r = BOS_px(24);
		bool have = preview_surf && preview_have[0] && strcmp(preview_have, e->path) == 0;
		BOS_rrect(screen, pane_x, pane_y, pane_w, bh, r, BOS_theme()->surface);
		if (have) {
			SDL_Surface* p = BOS_picRounded(preview_surf, preview_surf->w, preview_surf->h, r);
			if (p)
				BOS_blit(p, screen, pane_x + (pane_w - preview_surf->w) / 2, pane_y + (bh - preview_surf->h) / 2, 1);
		} else if (preview_error[0] && strcmp(preview_error, e->path) == 0) {
			const char* m = I18N_text("Can't preview");
			BOS_textMid(screen, m, 28, 400, BOS_C_TEXT2, 1, pane_x + (pane_w - BOS_textW(m, 28, 400)) / 2, pane_y + bh / 2);
		}
		char n[256], t[256];
		ImageBrowser_getDisplayName(e->name, n, sizeof(n));
		BOS_fit(n, 32, 600, pane_w, t, sizeof(t));
		BOS_text(screen, t, 32, 600, BOS_C_TEXT, 1, pane_x, pane_y + bh + BOS_px(14));
		char sz[32], meta[96];
		format_size(e->size_bytes, sz, sizeof(sz));
		if (have)
			snprintf(meta, sizeof(meta), "%d×%d · %s", preview_ow, preview_oh, sz);
		else
			snprintf(meta, sizeof(meta), "%s", sz);
		BOS_text(screen, meta, 26, 400, BOS_C_TEXT2, 1, pane_x, pane_y + bh + BOS_px(14) + BOS_lineH(32));
	}
	BOS_hintBar(screen, h, 2, l, 1);
}

static void render_browser(void) {
	render_browser_bos();
	UI_renderToast(screen, browser_toast, browser_toast_time);
	if (ContextMenu_isOpen())
		ContextMenu_render(screen);
}

// Display name for the context-menu prompt/title: extension-stripped for
// files (rename only ever touches the base name), full name for dirs.
static void ctx_display_name(const ImageEntry* e, char* out, int out_sz) {
	if (e->is_dir)
		snprintf(out, out_sz, "%s", e->name);
	else
		ImageBrowser_getDisplayName(e->name, out, out_sz);
}

// Rename flow for the MENU > Rename item: blocking keyboard prompt, then the
// matching fileops call. "Name already used" is distinguished from any other
// failure via the access(out_path, F_OK) contract documented in
// iv_fileops.h (do_rename there writes out_path with the colliding path on
// that specific refusal, and leaves it untouched otherwise).
static void handle_ctx_rename(ImageEntry* e) {
	char disp[256];
	ctx_display_name(e, disp, sizeof(disp));
	char prompt[300];
	snprintf(prompt, sizeof(prompt), "Rename \"%s\"", disp);

	char* newname = UIKeyboard_open(prompt);
	if (!newname || !newname[0]) {
		free(newname);
		return;
	}

	char out_path[512] = "";
	bool ok = e->is_dir
				  ? IvFileops_renameDir(e->path, newname, out_path, sizeof(out_path))
				  : IvFileops_renameFileKeepExt(e->path, newname, out_path, sizeof(out_path));
	free(newname);

	if (ok) {
		char path_copy[512];
		snprintf(path_copy, sizeof(path_copy), "%s", browser.current_path);
		load_image_directory(path_copy);
		for (int i = 0; i < browser.entry_count; i++) {
			if (strcmp(browser.entries[i].path, out_path) == 0) {
				browser_view.selected = i;
				break;
			}
		}
	} else {
		const char* msg = (out_path[0] && access(out_path, F_OK) == 0)
							  ? "Name already used"
							  : "Rename failed";
		snprintf(browser_toast, sizeof(browser_toast), "%s", msg);
		browser_toast_time = SDL_GetTicks();
	}
}

// Delete flow for the MENU > Delete item: blocking confirm modal, then the
// matching fileops call. The listing is only reloaded on
// success; a failed delete just toasts (deleteTree's own best-effort
// continuation means a partial failure is rare and not worth reloading for).
static void handle_ctx_delete(ImageEntry* e, int target_index) {
	char disp[256];
	ctx_display_name(e, disp, sizeof(disp));
	char title[300];
	snprintf(title, sizeof(title), "Delete \"%s\"?", disp);

	bool confirmed = UI_confirmModal(
		screen, title,
		e->is_dir ? "The folder and everything inside it will be deleted" : NULL,
		NULL, true, true);
	if (!confirmed)
		return;

	bool ok = e->is_dir ? IvFileops_deleteTree(e->path) : IvFileops_deleteFile(e->path);
	if (ok) {
		char path_copy[512];
		snprintf(path_copy, sizeof(path_copy), "%s", browser.current_path);
		load_image_directory(path_copy);
		if (browser.entry_count > 0)
			browser_view.selected = target_index < browser.entry_count
										? target_index
										: browser.entry_count - 1;
	} else {
		snprintf(browser_toast, sizeof(browser_toast), "Delete failed");
		browser_toast_time = SDL_GetTicks();
	}
}

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	UI_bos = true; // Brick OS look for every shared piece (DESIGN.md 7.4)
	screen = GFX_init(MODE_MAIN);
	PWR_pinToCores(CPU_CORE_EFFICIENCY);
	UI_showSplashScreen(screen, "Image Viewer");

	InitSettings();
	PAD_init();
	PWR_init();
	UI_loadIconPair(ICON_FOLDER, &icon_folder, &icon_folder_inv);
	UI_loadIconPair(ICON_IMAGE, &icon_image, &icon_image_inv);
	UI_initEmptyIcon();

	pane_x = (int)(screen->w * 0.58);
	pane_w = screen->w - pane_x - SCALE1(BUTTON_MARGIN * 2);
	pane_y = SCALE1(PILL_SIZE) + SCALE1(BUTTON_MARGIN);
	pane_h = screen->h - pane_y - SCALE1(PILL_SIZE);
	if (UI_bos) { // the list 470 at x 24, the picture's box from x 522 to 32 short of the edge
		pane_x = BOS_px(24 + 470 + 28);
		pane_w = screen->w - BOS_px(32) - pane_x;
		pane_y = BOS_px(BOS_BAR_H + 8);
		pane_h = BOS_px(IV_BOX_H) + SCALE1(24);
	}
	IvLoader_init(screen, pane_w, pane_h - SCALE1(24)); // 24: reserved for the metadata line

	signal(SIGINT, sigHandler);
	signal(SIGTERM, sigHandler);

	mkdir(IMAGES_ROOT, 0755);
	load_image_directory(IMAGES_ROOT);

	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (!quit) {
		GFX_startFrame();
		PAD_poll();

		if (app_screen == SCREEN_BROWSER) {
			// The context menu (opened below on a MENU tap) consumes input
			// on its own while up; the ListView gets none of it, so it can't
			// see the row move out from under the menu.
			if (ContextMenu_isOpen()) {
				ContextMenuResult cmr = ContextMenu_handleInput();
				if (cmr.action == CONTEXTMENU_SELECTED) {
					if (ctx_target_index >= 0 && ctx_target_index < browser.entry_count) {
						ImageEntry* e = &browser.entries[ctx_target_index];
						if (cmr.id == IV_CTX_RENAME)
							handle_ctx_rename(e);
						else if (cmr.id == IV_CTX_DELETE)
							handle_ctx_delete(e, ctx_target_index);
					}
					dirty = true;
				} else if (cmr.action == CONTEXTMENU_CANCEL) {
					dirty = true;
				}
			} else {
				ListViewAction act = UI_listViewHandleInput(&browser_view);
				switch (act.type) {
				case LISTVIEW_BACK:
					if (strcmp(browser.current_path, IMAGES_ROOT) != 0) {
						// Copy to a local first: load_image_directory -> ImageBrowser_loadDirectory
						// does strncpy(ctx->current_path, path, ...), and passing browser.current_path
						// directly makes src and dst the same buffer (restrict/aliasing UB).
						char parent[512];
						snprintf(parent, sizeof(parent), "%s", browser.current_path);
						char* last_slash = strrchr(parent, '/');
						if (last_slash && last_slash != parent) {
							*last_slash = '\0';
							load_image_directory(parent);
						} else {
							load_image_directory(IMAGES_ROOT);
						}
						GFX_clearLayers(LAYER_SCROLLTEXT);
						dirty = true;
					} else { // the first page: ask before leaving (DESIGN 7.7)
						GFX_clearLayers(LAYER_SCROLLTEXT);
						if (BOS_askQuit(screen, "Image Viewer"))
							quit = true;
						dirty = true;
					}
					break;
				case LISTVIEW_ACTIVATED:
					if (act.index >= 0 && act.index < browser.entry_count) {
						ImageEntry* e = &browser.entries[act.index];
						if (e->is_dir) {
							char path_copy[512];
							snprintf(path_copy, sizeof(path_copy), "%s", e->path);
							load_image_directory(path_copy);
							GFX_clearLayers(LAYER_SCROLLTEXT);
							dirty = true;
						} else {
							IvViewer_open(&browser, act.index);
							app_screen = SCREEN_VIEWER;
							GFX_clearLayers(LAYER_SCROLLTEXT);
							dirty = true;
						}
					}
					break;
				case LISTVIEW_MENU:
					if (act.index >= 0 && act.index < browser.entry_count &&
						strcmp(browser.entries[act.index].name, "..") != 0) {
						ctx_target_index = act.index;
						ContextMenuItem items[2];
						snprintf(items[0].label, sizeof(items[0].label), "Rename");
						items[0].id = IV_CTX_RENAME;
						snprintf(items[1].label, sizeof(items[1].label), "Delete");
						items[1].id = IV_CTX_DELETE;
						ContextMenu_open(items, 2);
						GFX_clearLayers(LAYER_SCROLLTEXT);
						dirty = true;
					}
					break;
				default:
					break;
				}
			}

			// Selected-file preview: debounce the request so fast scrolling
			// never fires a decode per row, then drain whatever the loader
			// has finished. This runs every frame, ahead of the dirty
			// check below, so results land whether this frame renders or
			// idles.
			//
			// Re-check app_screen: LISTVIEW_ACTIVATED above may have just
			// called IvViewer_open() and flipped us to SCREEN_VIEWER while
			// still inside this BROWSER branch. IvViewer_open() issues its
			// own FULL request for the same file the debounce below would
			// also target, and a tiny image can decode within this same
			// frame; without this guard the drain loop's else-branch (which
			// frees any result that isn't ours) would steal and free that
			// FULL result, leaving the viewer stuck on "Loading image..."
			// until B is pressed.
			if (app_screen == SCREEN_BROWSER) {
				char selected_path[512] = "";
				if (browser_view.selected >= 0 && browser_view.selected < browser.entry_count) {
					ImageEntry* e = &browser.entries[browser_view.selected];
					if (!e->is_dir)
						snprintf(selected_path, sizeof(selected_path), "%s", e->path);
				}
				if (strcmp(selected_path, preview_want) != 0) {
					snprintf(preview_want, sizeof(preview_want), "%s", selected_path);
					preview_want_tick = SDL_GetTicks();
				}
				if (preview_want[0] && strcmp(preview_want, preview_have) != 0 &&
					strcmp(preview_want, preview_requested) != 0 &&
					SDL_GetTicks() - preview_want_tick > 150) {
					IvLoader_request(preview_want, IV_LOAD_PREVIEW);
					snprintf(preview_requested, sizeof(preview_requested), "%s", preview_want);
				}

				// Drains the whole shared loader queue while the browser is the
				// active screen. Only PREVIEW results are ours; the viewer's
				// FULL/PREFETCH requests are only ever in flight while it's the
				// active screen instead (mutually exclusive with this branch),
				// so anything else here is stale and just needs freeing.
				IvLoadResult res;
				while (IvLoader_poll(&res)) {
					if (res.purpose != IV_LOAD_PREVIEW || strcmp(res.path, preview_want) != 0) {
						if (res.surface)
							SDL_FreeSurface(res.surface);
						continue;
					}
					if (preview_surf) {
						SDL_FreeSurface(preview_surf);
						preview_surf = NULL;
					}
					if (res.status == IV_LOAD_OK) {
						preview_surf = res.surface;
						preview_ow = res.orig_w;
						preview_oh = res.orig_h;
						snprintf(preview_have, sizeof(preview_have), "%s", res.path);
						preview_error[0] = '\0';
					} else {
						preview_have[0] = '\0';
						snprintf(preview_error, sizeof(preview_error), "%s", res.path);
					}
					dirty = true;
				}

				// Toast tick: keeps redrawing while a browser toast is
				// visible, then once more on the expiry edge (clearing the
				// message) so that one last render call wipes the GPU
				// layer.
				UI_tickToast(browser_toast, browser_toast_time, &dirty);
			}
		} else { // SCREEN_VIEWER
			IvViewerStatus st = IvViewer_update(screen, &dirty);
			if (st != IV_VIEWER_CONTINUE) {
				if (st == IV_VIEWER_ERROR) {
					snprintf(browser_toast, sizeof(browser_toast), "%s",
							 IvViewer_lastError() == IV_LOAD_ERR_TOO_LARGE ? "Image too large" : "Couldn't load image");
					browser_toast_time = SDL_GetTicks();
				}
				IvViewer_close();
				// A rotated file's preview (and its size line) must be re-read.
				if (IvViewer_filesChanged())
					preview_have[0] = '\0';
				app_screen = SCREEN_BROWSER;
				UI_clearToast();
				// A preview request for the still-selected file may have been
				// in flight when the viewer opened; its result was drained
				// (and freed) by the viewer, not applied here, so drop the
				// "already requested" marker to let the debounce re-request
				// it now that we're back.
				preview_requested[0] = '\0';
				dirty = true;
			}
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (dirty || (app_screen == SCREEN_BROWSER
						  ? (UI_listViewBusy(&browser_view) || BOS_glideMoving(&iv_pill) || BOS_glideMoving(&iv_scroll) || ContextMenu_isOpen())
						  : IvViewer_busy())) {
			if (app_screen == SCREEN_BROWSER)
				render_browser();
			else
				IvViewer_render(screen);
			GFX_flip(screen);
			dirty = false;
		} else {
			if (app_screen == SCREEN_BROWSER)
				UI_listViewTickIdle(&browser_view);
			GFX_sync();
		}
	}

	if (app_screen == SCREEN_VIEWER)
		IvViewer_close();
	ImageBrowser_freeEntries(&browser);
	if (preview_surf)
		SDL_FreeSurface(preview_surf);
	UI_clearToast();
	UI_freeIconPair(&icon_folder, &icon_folder_inv);
	UI_freeIconPair(&icon_image, &icon_image_inv);
	UI_quitEmptyIcon();
	QuitSettings();
	IvLoader_quit();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return EXIT_SUCCESS;
}
