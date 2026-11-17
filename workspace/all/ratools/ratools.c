#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <msettings.h>

#include "ui_draw.h"
#include "defines.h"
#include "api.h"
#include "bos_ui.h"
#include "i18n.h"
#include "config.h"
#include "settings_menu.h"
#include "ui_list.h"
#include "ui_listview.h"
#include "ui_splash.h"
#include "ui_quitrequest.h"
#include "ui_confirmdialog.h"
#include "ui_menubar.h"
#include "ui_buttonhintbar.h"
#include "ui_font.h"
#include "display_helper.h"
#include "ra_auth.h"
#include "ra_offline.h"
#include "ra_offline_net.h"
#include "ra_hash_cache.h"
#include "ra_paths.h"

#include "ratools_browser.h"
#include "ratools_data.h"
#include "ratools_prefetch.h"
#include "ratools_reset.h"
#include "ratools_sync.h"

SDL_Surface* g_screen = NULL;

// ============================================
// Pages
// ============================================
static SettingsPage ra_settings_page;
static SettingItem ra_settings_items[16];

static void init_page(SettingsPage* page, const char* title, SettingItem* items,
					  int count, int is_list) {
	memset(page, 0, sizeof(SettingsPage));
	page->title = title;
	page->items = items;
	page->item_count = count;
	page->selected = 0;
	page->scroll = 0;
	page->is_list = is_list;
	page->dynamic_start = -1;
	page->max_items = count;
}

// ============================================
// Value labels (copied from settings.c)
// ============================================
static const char* on_off_labels[] = {"Off", "On"};
static int on_off_values[] = {0, 1};
static const char* notify_duration_labels[] = {"1s", "2s", "3s", "4s", "5s"};
static int notify_duration_values[] = {1, 2, 3, 4, 5};
#define NOTIFY_DURATION_COUNT 5
static const char* progress_duration_labels[] = {"Off", "1s", "2s", "3s", "4s", "5s"};
static int progress_duration_values[] = {0, 1, 2, 3, 4, 5};
#define PROGRESS_DURATION_COUNT 6

/* RA sort order */
static int ra_sort_values[] = {
	RA_SORT_UNLOCKED_FIRST,
	RA_SORT_DISPLAY_ORDER_FIRST,
	RA_SORT_DISPLAY_ORDER_LAST,
	RA_SORT_WON_BY_MOST,
	RA_SORT_WON_BY_LEAST,
	RA_SORT_POINTS_MOST,
	RA_SORT_POINTS_LEAST,
	RA_SORT_TITLE_AZ,
	RA_SORT_TITLE_ZA,
	RA_SORT_TYPE_ASC,
	RA_SORT_TYPE_DESC};
static const char* ra_sort_labels[] = {
	"Unlocked First",
	"Display Order (First)",
	"Display Order (Last)",
	"Won By (Most)",
	"Won By (Least)",
	"Points (Most)",
	"Points (Least)",
	"Title (A-Z)",
	"Title (Z-A)",
	"Type (Asc)",
	"Type (Desc)"};
#define RA_SORT_LABEL_COUNT 11

// ============================================
// Status display helpers (reused by the custom main-screen header and,
// where noted, by the settings page)
// ============================================
static const char* get_user_display(void) {
	const char* u = CFG_getRAUsername();
	return (u && u[0]) ? u : "(not set)";
}

static char lastsync_buf[64];
static const char* get_lastsync_display(void) {
	time_t t = RA_Offline_lastSyncTime();
	if (t == 0)
		return "Never";
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(lastsync_buf, sizeof(lastsync_buf), "%Y-%m-%d %H:%M", &tm);
	return lastsync_buf;
}

static void on_prefetch(void) {
	RATPrefetch_run(g_screen);
}

// Blocking confirm dialog: A confirms, B cancels. Returns true on confirm.
static bool ra_confirm(const char* title, const char* subtitle) {
	bool dirty = true;
	while (1) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_A)) {
			PAD_reset();
			return true;
		}
		if (PAD_justPressed(BTN_B)) {
			PAD_reset();
			return false;
		}
		if (dirty) {
			GFX_clear(g_screen);
			UI_renderConfirmDialog(g_screen, title, subtitle);
			GFX_flip(g_screen);
			dirty = false;
		} else {
			GFX_sync();
		}
	}
}

static void on_reset_account(void) {
	if (ra_confirm("Reset account data?",
				   "Clears login, unlock progress, and pending syncs. Game data and badges are kept.")) {
		RATReset_clearAccountData();
	}
}

static void on_reset_all(void) {
	if (ra_confirm("Erase all achievement data?",
				   "Removes everything including cached game data. Redownloaded on next online play. Badges in rom folders are kept.")) {
		RATReset_clearAll();
	}
}

// ============================================
// RA settings callbacks (moved from settings.c)
// ============================================
static int get_ra_enable(void) {
	return CFG_getRAEnable() ? 1 : 0;
}
static void set_ra_enable(int v) {
	CFG_setRAEnable(v != 0);
}
static void reset_ra_enable(void) {
	CFG_setRAEnable(CFG_DEFAULT_RA_ENABLE);
}

// Hardcore mode is deliberately not offered: nx-redux is not an
// RA-approved hardcore-compliant emulator, so hardcore unlocks are
// softcore-locked server-side and enabling it only risks the account.

static const char* get_ra_username_display(void) {
	// identical to get_user_display() (defined above); kept as a named
	// function so it can be used as an ITEM_TEXT_INPUT_INIT getter pointer
	return get_user_display();
}
static void on_ra_username_set(const char* text) {
	if (text)
		CFG_setRAUsername(text);
}

static const char* get_ra_password_display(void) {
	const char* p = CFG_getRAPassword();
	return (p && p[0]) ? "********" : "(not set)";
}
static void on_ra_password_set(const char* text) {
	if (text)
		CFG_setRAPassword(text);
}

static char ra_auth_status_msg[256] = "";

static void on_ra_authenticate(void) {
	const char* username = CFG_getRAUsername();
	const char* password = CFG_getRAPassword();

	if (!username || !username[0] || !password || !password[0]) {
		snprintf(ra_auth_status_msg, sizeof(ra_auth_status_msg),
				 "Error: Username and password required");
		return;
	}

	RA_AuthResponse response;
	RA_AuthResult result = RA_authenticateSync(username, password, &response);

	if (result == RA_AUTH_SUCCESS) {
		CFG_setRAToken(response.token);
		CFG_setRAAuthenticated(true);
		// minarch only starts an offline session from a cached "login2"
		// response, which it otherwise writes on its own first online login.
		// Write it here too so "log in, prefetch, go offline" works without
		// ever playing a game online (issue #105).
		if (RA_OfflineNet_cacheLogin(username, response.token) != 0)
			LOG_warn("ratools: authenticated but could not write offline login cache\n");
		snprintf(ra_auth_status_msg, sizeof(ra_auth_status_msg),
				 "Authenticated as %s", response.display_name);
	} else {
		CFG_setRAToken("");
		CFG_setRAAuthenticated(false);
		snprintf(ra_auth_status_msg, sizeof(ra_auth_status_msg),
				 "Error: %s", response.error_message);
	}
}

static const char* get_ra_status(void) {
	if (ra_auth_status_msg[0])
		return ra_auth_status_msg;
	if (CFG_getRAAuthenticated() && strlen(CFG_getRAToken()) > 0)
		return "Authenticated";
	return "Not authenticated";
}

static int get_ra_show_notifications(void) {
	return CFG_getRAShowNotifications() ? 1 : 0;
}
static void set_ra_show_notifications(int v) {
	CFG_setRAShowNotifications(v != 0);
}
static void reset_ra_show_notifications(void) {
	CFG_setRAShowNotifications(CFG_DEFAULT_RA_SHOW_NOTIFICATIONS);
}

static int get_ra_notify_duration(void) {
	return CFG_getRANotificationDuration();
}
static void set_ra_notify_duration(int val) {
	CFG_setRANotificationDuration(val);
}
static void reset_ra_notify_duration(void) {
	CFG_setRANotificationDuration(CFG_DEFAULT_RA_NOTIFICATION_DURATION);
}

static int get_ra_progress_duration(void) {
	return CFG_getRAProgressNotificationDuration();
}
static void set_ra_progress_duration(int val) {
	CFG_setRAProgressNotificationDuration(val);
}
static void reset_ra_progress_duration(void) {
	CFG_setRAProgressNotificationDuration(CFG_DEFAULT_RA_PROGRESS_NOTIFICATION_DURATION);
}

static int get_ra_sort_order(void) {
	return CFG_getRAAchievementSortOrder();
}
static void set_ra_sort_order(int val) {
	CFG_setRAAchievementSortOrder(val);
}
static void reset_ra_sort_order(void) {
	CFG_setRAAchievementSortOrder(CFG_DEFAULT_RA_ACHIEVEMENT_SORT_ORDER);
}

static void reset_ra_page(void) {
	reset_ra_enable();
	reset_ra_show_notifications();
	reset_ra_notify_duration();
	reset_ra_progress_duration();
	reset_ra_sort_order();
	for (int i = 0; i < ra_settings_page.item_count; i++)
		settings_item_sync(&ra_settings_page.items[i]);
}

// ============================================
// Menu construction (Settings sub-page only — the main page is
// custom-rendered, see render_main_screen())
// ============================================
static void build_menu_tree(void) {
	int idx = 0;
	ra_settings_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Enable achievements", "Enable RetroAchievements integration",
		on_off_labels, 2, on_off_values, get_ra_enable, set_ra_enable, reset_ra_enable);
	ra_settings_items[idx++] = (SettingItem)ITEM_TEXT_INPUT_INIT(
		"Username", "RetroAchievements username",
		get_ra_username_display, on_ra_username_set);
	ra_settings_items[idx++] = (SettingItem)ITEM_TEXT_INPUT_INIT(
		"Password", "RetroAchievements password",
		get_ra_password_display, on_ra_password_set);
	ra_settings_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Authenticate", "Test credentials and retrieve API token",
		on_ra_authenticate);
	ra_settings_items[idx++] = (SettingItem)ITEM_STATIC_INIT(
		"Status", "Authentication status",
		get_ra_status);
	ra_settings_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Download all game data", "Cache achievement data for every game in your library",
		on_prefetch);
	ra_settings_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show notifications", "Show achievement unlock notifications",
		on_off_labels, 2, on_off_values, get_ra_show_notifications, set_ra_show_notifications, reset_ra_show_notifications);
	ra_settings_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Notification duration", "How long achievement notifications stay on screen",
		notify_duration_labels, NOTIFY_DURATION_COUNT, notify_duration_values, get_ra_notify_duration, set_ra_notify_duration, reset_ra_notify_duration);
	ra_settings_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Progress duration", "Duration for progress updates (top-left). Off to disable.",
		progress_duration_labels, PROGRESS_DURATION_COUNT, progress_duration_values, get_ra_progress_duration, set_ra_progress_duration, reset_ra_progress_duration);
	ra_settings_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Achievement sort order", "How achievements are sorted in the in-game menu",
		ra_sort_labels, RA_SORT_LABEL_COUNT, ra_sort_values, get_ra_sort_order, set_ra_sort_order, reset_ra_sort_order);
	ra_settings_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Reset account data", "Clear login and progress. Keeps game data.",
		on_reset_account);
	ra_settings_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Erase all achievement data", "Remove everything, including cached game data.",
		on_reset_all);
	ra_settings_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Reset settings to defaults", "Restore the options above to their default values.",
		reset_ra_page);
	init_page(&ra_settings_page, "RetroAchievements | Settings", ra_settings_items, idx, 0);

	for (int i = 0; i < ra_settings_page.item_count; i++)
		settings_item_sync(&ra_settings_page.items[i]);
}

// ============================================
// Custom main screen
// ============================================
#define MAIN_ITEM_COUNT 4

static const char* main_item_labels[MAIN_ITEM_COUNT] = {
	"Achievements", "Sync now", "Library", "Settings"};

static ListView main_view;

static bool ra_is_authenticated(void) {
	return CFG_getRAAuthenticated() && *CFG_getRAToken();
}

// Selectable list of the 4 main actions, drawn by the shared ListView widget
// with its origin pushed below the custom status header.
static void main_row_bos(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	static char pend[96];
	switch (i) {
	case 0:
		*r = (ListViewRow){.label = I18N_text("Achievements"), .sub = I18N_text("Every game you have played"), .sq_icon = "trophy",
						   .sq_color = 0xFF9F0A, .chevron = true};
		break;
	case 1:
		snprintf(pend, sizeof(pend), I18N_text("%d achievements to send"), RA_Offline_pendingCount());
		*r = (ListViewRow){.label = I18N_text("Sync now"), .sub = pend, .sq_icon = "sync", .sq_color = BOS_C_PRIMARY};
		break;
	case 2:
		*r = (ListViewRow){.label = I18N_text("Supported games"), .sub = I18N_text("Games on this device that have achievements"),
						   .sq_icon = "books", .sq_color = 0x5E5CE6, .chevron = true};
		break;
	default:
		*r = (ListViewRow){.label = I18N_text("Settings"), .sub = ra_is_authenticated() ? NULL : I18N_text("Sign in to RetroAchievements"),
						   .sq_icon = "settings", .sq_color = 0x8E8E93, .chevron = true};
	}
}

// the account card (board v2-ToolsRA): a circle 96 with the initial, the name 40/700, points 28 text2, last sync right
static void render_main_bos(SDL_Surface* s) {
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_statusBar(s, "RetroAchievements");
	int top = BOS_px(BOS_BAR_H), gx = BOS_px(40), gw = s->w - 2 * gx, ch = BOS_px(144), y = top + BOS_px(8);
	BOS_rrect(s, gx, y, gw, ch, BOS_px(24), BOS_theme()->surface);
	int av = BOS_px(96), ax = gx + BOS_px(28), cy = y + ch / 2, tx = ax + av + BOS_px(24);
	bool auth = ra_is_authenticated();
	char t[160], l2[160];
	if (auth) {
		const char* u = get_user_display();
		BOS_rrect(s, ax, cy - av / 2, av, av, av / 2, BOS_C_WARN);
		char ini[2] = {u && u[0] ? (char)((u[0] >= 'a' && u[0] <= 'z') ? u[0] - 32 : u[0]) : '?', 0};
		BOS_textMid(s, ini, 44, 700, 0x000000, 1, ax + (av - BOS_textW(ini, 44, 700)) / 2, cy);
		uint32_t score = 0, soft = 0;
		if (RAT_getCachedScore(&score, &soft))
			snprintf(l2, sizeof(l2), I18N_text("%u points"), soft);
		else
			l2[0] = 0;
		const char* ls = I18N_text("Last sync");
		const char* lt = I18N_text(get_lastsync_display());
		int rw = BOS_textW(ls, 26, 400), rw2 = BOS_textW(lt, 26, 600), rx = gx + gw - BOS_px(28);
		BOS_text(s, ls, 26, 400, BOS_C_TEXT2, 1, rx - rw, cy - BOS_lineH(26));
		BOS_text(s, lt, 26, 600, BOS_C_TEXT, 1, rx - rw2, cy);
		int room = rx - (rw > rw2 ? rw : rw2) - BOS_px(24) - tx;
		BOS_fit(u, 40, 700, room, t, sizeof(t));
		BOS_textMid(s, t, 40, 700, BOS_C_TEXT, 1, tx, cy - BOS_px(18));
		BOS_fit(l2, 28, 400, room, t, sizeof(t));
		BOS_textMid(s, t, 28, 400, BOS_C_TEXT2, 1, tx, cy + BOS_px(22));
	} else {
		BOS_rrect(s, ax, cy - av / 2, av, av, av / 2, BOS_theme()->surface2);
		int ic = BOS_px(56);
		BOS_icon(s, "user", BOS_C_TEXT2, 1, ax + (av - ic) / 2, cy - ic / 2, ic);
		BOS_textMid(s, I18N_text("Not signed in"), 40, 700, BOS_C_TEXT, 1, tx, cy - BOS_px(18));
		BOS_textMid(s, I18N_text("Enter your username and password in Settings"), 28, 400, BOS_C_TEXT2, 1, tx, cy + BOS_px(22));
	}
	y += ch + BOS_px(20);
	int rh = BOS_px(BOS_ROW_H), bottom = s->h - BOS_px(BOS_BAR_H) - BOS_px(12);
	BOS_groupRows(s, &(SDL_Rect){gx, y, gw, bottom - y}, rh, MAIN_ITEM_COUNT, main_view.selected, true, &main_view.scroll,
				  &main_view.bos_pill, &main_view.bos_scroll, false, main_row_bos, NULL);
	main_view.input_pending = false;
	BosHint h[] = {{"A", I18N_text("Open")}, {"B", I18N_text("Exit")}};
	BOS_hintBar(s, h, 2, NULL, 0);
}

static void render_main_screen(SDL_Surface* screen) {
	render_main_bos(screen);
}

// Run the settings_menu framework for ra_settings_page only, then return to
// the custom main screen (does NOT exit the app). settings_menu_handle_input
// pops the page and reports *quit=true once stack depth reaches 0 — since
// ra_settings_page has no submenu items, a single B press at depth 1 always
// triggers exactly that. We feed it a *local* quit flag so that report only
// breaks this sub-loop instead of the whole app.
static void run_settings_menu(SDL_Surface** screen_ptr) {
	settings_menu_push(&ra_settings_page);

	bool sub_quit = false;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (!sub_quit) {
		GFX_startFrame();
		PAD_poll();

		settings_menu_handle_input(&sub_quit, &dirty);

		if (settings_menu_glide_active())
			dirty = true;

		// TG5050: keyboard may have triggered display recovery
		{
			SDL_Surface* ns = DisplayHelper_getReinitScreen();
			if (ns) {
				*screen_ptr = ns;
				g_screen = ns;
				dirty = true;
			}
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (UI_statusBarChanged())
			dirty = true;

		if (dirty) {
			GFX_clear(*screen_ptr);
			settings_menu_render(*screen_ptr, show_setting);
			GFX_flip(*screen_ptr);
			dirty = false;
		} else {
			GFX_sync();
		}
	}
}

// ============================================
// Main
// ============================================
int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;
	UI_bos = true; // Brick OS look for every shared piece (DESIGN.md 7.4)
	SDL_Surface* screen = GFX_init(MODE_MAIN);
	g_screen = screen;
	UI_showSplashScreen(screen, "RetroAchievements");

	InitSettings();
	PWR_init();
	PAD_init();

	RA_Offline_init(RA_USER_DIR, SDCARD_PATH);
	RA_HashCache_init(ROMS_PATH);

	build_menu_tree();
	settings_menu_init();

	bool quit = false;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	UI_listViewReset(&main_view, MAIN_ITEM_COUNT, main_item_labels);

	while (!quit) {
		GFX_startFrame();
		PAD_poll();

		UI_handleQuitRequest(screen, &quit, &dirty, "Exit RetroAchievements?",
							 "Your settings are automatically saved");

		if (!quit) {
			ListViewAction act = UI_listViewHandleInput(&main_view);
			if (act.type == LISTVIEW_BACK) { // the first page: ask before leaving (DESIGN 7.7)
				GFX_clearLayers(LAYER_SCROLLTEXT);
				quit = BOS_askQuit(screen, "RetroAchievements");
				dirty = true;
			} else if (act.type == LISTVIEW_ACTIVATED) {
				switch (act.index) {
				case 0:
					RATBrowser_run(g_screen);
					break;
				case 1:
					RATSync_run(g_screen);
					break;
				case 2:
					RATPrefetch_runLibrary(g_screen);
					break;
				case 3:
					run_settings_menu(&screen);
					break;
				}
				// These are blocking sub-screens with their own input loops;
				// the B (or A) press that closed them is still latched in
				// the global pad state (no PAD_poll happened since). Clear
				// it so it isn't misread this frame — same convention as
				// settings.c/settings_wifi.c after modal transitions.
				PAD_reset();
				dirty = true;
			}

			if (UI_listViewBusy(&main_view))
				dirty = true;
		}

		// TG5050: keyboard may have triggered display recovery
		{
			SDL_Surface* ns = DisplayHelper_getReinitScreen();
			if (ns) {
				screen = ns;
				g_screen = ns;
				dirty = true;
			}
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (UI_statusBarChanged())
			dirty = true;

		if (dirty) {
			render_main_screen(screen);
			GFX_flip(screen);
			dirty = false;
		} else {
			UI_listViewTickIdle(&main_view);
			GFX_sync();
		}
	}

	GFX_clear(screen);
	GFX_flip(screen);

	QuitSettings();
	PWR_quit();
	PAD_quit();

	GFX_quit();

	return EXIT_SUCCESS;
}
