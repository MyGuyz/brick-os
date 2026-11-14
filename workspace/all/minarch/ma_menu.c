#include "ma_internal.h"
#include "ma_emutime.h"
#include "netplay.h"
#include "utils.h"
#include "brand.h"
#include "arcade_names.h"
#include "core_netplay.h"
#include "gbalink.h"
#include "ui_confirmdialog.h"
#include "config.h"
#include "ui_list.h"
#include "ui_buttonhintbar.h"
#include "ui_menubar.h"
#include "ma_menu.h"
#include "ma_menu_bos.h"
#include "i18n.h"
#include <sys/stat.h>
#include "ma_png.h"
#include "ma_present.h"
#include "ma_frontend_opts.h"
#include "ma_saves.h"
#include "ma_save_paths.h"
#include "ma_game.h"
#include "ma_config.h"
#include "ma_core.h"
#include "ma_video.h"
#include "ma_rewind.h"
#include "minarch.h"
#include "ra_integration.h"
#include "ra_badges.h"
#include "notification.h"
#include "netplay_helper.h"
#include <rcheevos/rc_client.h>
#include <SDL2/SDL_image.h>

// Achievement menu state (stored so on_confirm handler can access achievement data)
static const rc_client_achievement_list_t* ach_menu_list = NULL;
static const rc_client_achievement_t** ach_menu_achievements = NULL; // Flattened array for index lookup
static int ach_menu_count = 0;
static bool ach_filter_locked_only = false; // Y button toggle: show all or locked only


#define MENU_ITEM_COUNT 5
#define MENU_SLOT_COUNT 8

enum {
	ITEM_CONT,
	ITEM_SAVE,
	ITEM_LOAD,
	ITEM_OPTS,
	ITEM_QUIT,
};

#define MENU_MAX_DISCS 9

// TODO: I don't love how overloaded this has become
static struct {
	SDL_Surface* bitmap;
	SDL_Surface* overlay;
	char* items[MENU_ITEM_COUNT];
	char* disc_paths[MENU_MAX_DISCS]; // up to 9 paths, Arc the Lad Collection is 7 discs
	char minui_dir[MAX_PATH];
	char slot_path[MAX_PATH];
	char base_path[MAX_PATH];
	char bmp_path[MAX_PATH];
	char txt_path[MAX_PATH];
	int disc;
	int total_discs;
	int slot;
	int save_exists;
	int preview_exists;
	time_t save_time;					// the chosen slot's save, when it was written
	bool slot_used[MENU_SLOT_COUNT];	// which slots hold a save (read when the menu opens)
} menu = {
	.bitmap = NULL,
	.disc = -1,
	.total_discs = 0,
	.save_exists = 0,
	.preview_exists = 0,

	.items = {
		[ITEM_CONT] = "Continue",
		[ITEM_SAVE] = "Save",
		[ITEM_LOAD] = "Load",
		[ITEM_OPTS] = "Options",
		[ITEM_QUIT] = "Quit",
	}};

static bool menuDiscCb(const char* disc_path, int index, void* ctx) {
	(void)index;
	(void)ctx;
	if (menu.total_discs >= MENU_MAX_DISCS)
		return false;
	menu.disc_paths[menu.total_discs] = strdup(disc_path);
	if (exactMatch((char*)disc_path, game.path))
		menu.disc = menu.total_discs;
	menu.total_discs += 1;
	return true;
}

// Point menu.disc at the disc that is actually running. Called after every
// Game_changeDisc (success or not) so the disc markers written by the next
// save name the loaded disc.
static void Menu_syncDiscIndex(void) {
	int idx = SavePaths_discIndex(menu.disc_paths, menu.total_discs, game.path);
	if (idx >= 0)
		menu.disc = idx;
}

static void initFrontendVersionDesc(void);

void Menu_init(void) {
	initFrontendVersionDesc();
	// A crash during bind capture can leave the OSD suppress flag behind;
	// /tmp survives until reboot, so clear it on every launch
	unlink(OSD_SUPPRESS_PATH);

	menu.overlay = SDL_CreateRGBSurfaceWithFormat(SDL_SWSURFACE,
												  DEVICE_WIDTH, DEVICE_HEIGHT,
												  screen->format->BitsPerPixel, screen->format->format);
	SDL_SetSurfaceBlendMode(menu.overlay, SDL_BLENDMODE_BLEND);
	Uint32 color = SDL_MapRGBA(menu.overlay->format, 0, 0, 0, 0);
	SDL_FillRect(screen, NULL, color);

	char emu_name[MAX_PATH]; // getEmuName requires MAX_PATH buffers
	getEmuName(game.path, emu_name);
	// A netplay client on the host's save keeps its slot markers beside its
	// isolated states (see Core_open), so NextUI never lists them as its own.
	const char* netplay_saves = getenv("NETPLAY_SAVES_DIR");
	if (netplay_saves && netplay_saves[0]) {
		snprintf(menu.minui_dir, sizeof(menu.minui_dir), "%s/minui", netplay_saves);
		mkdir_p(menu.minui_dir);
	} else {
		char resume_tag[MAX_PATH]; // native PICO-8 keeps its markers apart from fake08's
		ROM_resumeTag(emu_name, resume_tag, sizeof(resume_tag));
		snprintf(menu.minui_dir, sizeof(menu.minui_dir), "%s/.minui/%s", SHARED_USERDATA_PATH, resume_tag);
		mkdir(menu.minui_dir, 0755);
	}

	// always sanitized/outer name, to keep main UI from having to inspect archives
	snprintf(menu.slot_path, sizeof(menu.slot_path), "%s/%s.txt", menu.minui_dir, game.name);

	if (simple_mode)
		menu.items[ITEM_OPTS] = "Reset";

	if (game.m3u_path[0]) {
		strcpy(menu.base_path, game.m3u_path);
		char* tmp = strrchr(menu.base_path, '/');
		if (tmp)
			tmp[1] = '\0';
		M3U_forEachDisc(game.m3u_path, menuDiscCb, NULL);
	}
}
void Menu_quit(void) {
	SDL_FreeSurface(menu.overlay);
	for (int i = 0; i < menu.total_discs; i++) {
		free(menu.disc_paths[i]);
		menu.disc_paths[i] = NULL;
	}
	menu.total_discs = 0;
}
void Menu_beforeSleep() {
	SRAM_write();
	RTC_write();
	if (Multiplayer_isActive()) {
		// No state, no auto-resume: a netplay session can't be resumed, and
		// serializing flycast stops its emulator, which silently ends GGPO.
		// Sleeping (or powering off) in a core-run session is leaving it: tell
		// the other player now, and quit to the list on wake.
		if (CoreNetplay_isActive()) {
			CoreNetplay_byeSend(CORE_NETPLAY_BYE_PORT);
			quit = 1;
		}
	} else {
		State_autosave();
		// a netplay client's autosave holds the host's data in /tmp: nothing
		// NextUI should resume into
		if (prefixMatch(SDCARD_PATH, game.path) && Menu_netplaySavesMode() != NETPLAY_SAVES_COPY)
			putFile(AUTO_RESUME_PATH, game.path + strlen(SDCARD_PATH));
	}

	PWR_setCPUSpeed(CPU_SPEED_MENU);
}
NetplaySavesMode Menu_netplaySavesMode(void) {
	return SavePaths_netplayMode(getenv("NETPLAY_ROLE"), getenv("NETPLAY_SAVES_DIR"));
}
// A session on the device's own save (link sessions, the lockstep host) writes
// that save but never an auto-resume state, so the one from before the session
// would resume the game to before it, and the next in-game save would then
// undo the session (a trade, say). Drop it, and the markers that point at it,
// as the session starts so a crash or flat battery mid-session is covered too.
// Manual slots (0-8) are never touched.
void Menu_dropAutoResumeForSession(void) {
	if (Menu_netplaySavesMode() != NETPLAY_SAVES_REAL)
		return;

	char removed[MAX_PATH] = "";
	char path[MAX_PATH];
	// a truncated path could name a manual slot: leave the state alone then
	if (State_getSlotPath(AUTO_RESUME_SLOT, path) != 0)
		LOG_error("Netplay: auto-resume state path too long for %s, state kept\n", game.name);
	else if (unlink(path) == 0)
		strcat(removed, " state");
	snprintf(path, sizeof(path), "%s/%s.%d.bmp", menu.minui_dir, game.name, AUTO_RESUME_SLOT);
	if (unlink(path) == 0)
		strcat(removed, " preview");
	snprintf(path, sizeof(path), "%s/%s.%d.txt", menu.minui_dir, game.name, AUTO_RESUME_SLOT);
	if (unlink(path) == 0)
		strcat(removed, " disc-marker");
	// the RESUME slot file only when it points at the auto slot just removed
	if (exists(menu.slot_path) && getInt(menu.slot_path) == AUTO_RESUME_SLOT && unlink(menu.slot_path) == 0)
		strcat(removed, " slot-file");
	if (exists(AUTO_RESUME_PATH)) {
		char marker[MAX_PATH];
		getFile(AUTO_RESUME_PATH, marker, sizeof(marker));
		if (SavePaths_autoResumeIsGame(marker, game.path, SDCARD_PATH) && unlink(AUTO_RESUME_PATH) == 0)
			strcat(removed, " auto_resume.txt");
	}
	LOG_info("Netplay: session on the real save, dropped auto-resume for %s:%s\n", game.name, removed[0] ? removed : " (none)");
}
void Menu_afterSleep() {
	// a netplay client never writes it (Menu_beforeSleep), so never drops it
	if (Menu_netplaySavesMode() != NETPLAY_SAVES_COPY)
		unlink(AUTO_RESUME_PATH);
	setOverclock(overclock);
	EmuTime_reset();
}
static int ach_compare_unlocked_first(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	// Unlocked achievements come first
	if (achA->unlocked != achB->unlocked)
		return achB->unlocked - achA->unlocked;
	return 0;
}

// Sort entry: `ach` is the first member, so the comparators below that take a
// `const rc_client_achievement_t**` read it straight from an entry pointer.
typedef struct {
	const rc_client_achievement_t* ach;
	uint32_t subset; // ordinal of the achievement's subset in the list
} AchSortEntry;

static int ach_compare_display_first(const void* a, const void* b) {
	const AchSortEntry* A = (const AchSortEntry*)a;
	const AchSortEntry* B = (const AchSortEntry*)b;
	// Display order is the server's list order (IDs follow creation, not order). rc_client keeps each
	// subset's achievements in one array in that order and the list points into it, so within a
	// subset the address is the position.
	if (A->subset != B->subset)
		return A->subset < B->subset ? -1 : 1;
	if (A->ach != B->ach)
		return (uintptr_t)A->ach < (uintptr_t)B->ach ? -1 : 1;
	return 0;
}

static int ach_compare_display_last(const void* a, const void* b) {
	return -ach_compare_display_first(a, b);
}

static int ach_compare_won_by_most(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	// Higher rarity (more common) first
	if (achA->rarity != achB->rarity)
		return (achB->rarity - achA->rarity) > 0 ? 1 : -1;
	return 0;
}

static int ach_compare_won_by_least(const void* a, const void* b) {
	return -ach_compare_won_by_most(a, b);
}

static int ach_compare_points_most(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	return (int)achB->points - (int)achA->points;
}

static int ach_compare_points_least(const void* a, const void* b) {
	return -ach_compare_points_most(a, b);
}

static int ach_compare_title_az(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	return strcmp(achA->title, achB->title);
}

static int ach_compare_title_za(const void* a, const void* b) {
	return -ach_compare_title_az(a, b);
}

static int ach_compare_type_asc(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	return (int)achA->type - (int)achB->type;
}

static int ach_compare_type_desc(const void* a, const void* b) {
	return -ach_compare_type_asc(a, b);
}

static void ach_sort_achievements(AchSortEntry* entries, int count) {
	int sort_order = CFG_getRAAchievementSortOrder();
	int (*compare)(const void*, const void*) = NULL;

	switch (sort_order) {
	case RA_SORT_UNLOCKED_FIRST:
		compare = ach_compare_unlocked_first;
		break;
	case RA_SORT_DISPLAY_ORDER_FIRST:
		compare = ach_compare_display_first;
		break;
	case RA_SORT_DISPLAY_ORDER_LAST:
		compare = ach_compare_display_last;
		break;
	case RA_SORT_WON_BY_MOST:
		compare = ach_compare_won_by_most;
		break;
	case RA_SORT_WON_BY_LEAST:
		compare = ach_compare_won_by_least;
		break;
	case RA_SORT_POINTS_MOST:
		compare = ach_compare_points_most;
		break;
	case RA_SORT_POINTS_LEAST:
		compare = ach_compare_points_least;
		break;
	case RA_SORT_TITLE_AZ:
		compare = ach_compare_title_az;
		break;
	case RA_SORT_TITLE_ZA:
		compare = ach_compare_title_za;
		break;
	case RA_SORT_TYPE_ASC:
		compare = ach_compare_type_asc;
		break;
	case RA_SORT_TYPE_DESC:
		compare = ach_compare_type_desc;
		break;
	default:
		compare = ach_compare_unlocked_first;
		break;
	}

	if (compare && count > 1) {
		qsort(entries, count, sizeof(AchSortEntry), compare);
	}
}

static int OptionAchievements_openMenu(MenuList* list, int i) {
	if (!RA_isGameLoaded()) {
		if (RA_isGameUnknown())
			Menu_message("RetroAchievements doesn't recognize\nthis game file. Use a supported\ndump listed on retroachievements.org.", (char*[]){"B", "BACK", NULL});
		else
			Menu_message("No game loaded for achievements", (char*[]){"B", "BACK", NULL});
		return MENU_CALLBACK_NOP;
	}

	uint32_t unlocked, total;
	RA_getAchievementSummary(&unlocked, &total);

	if (total == 0) {
		Menu_message("This game has no achievements yet", (char*[]){"B", "BACK", NULL});
		return MENU_CALLBACK_NOP;
	}

	// Clean up any previous achievement list
	if (ach_menu_list) {
		RA_destroyAchievementList(ach_menu_list);
		ach_menu_list = NULL;
	}
	if (ach_menu_achievements) {
		free(ach_menu_achievements);
		ach_menu_achievements = NULL;
	}
	ach_menu_count = 0;

	// Create achievement list grouped by lock state
	ach_menu_list = (const rc_client_achievement_list_t*)RA_createAchievementList(
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);

	if (!ach_menu_list) {
		Menu_message("Failed to load achievements", (char*[]){"B", "BACK", NULL});
		return MENU_CALLBACK_NOP;
	}

	// Count total achievements across all buckets and build flattened array
	int total_achievements = 0;
	for (uint32_t b = 0; b < ach_menu_list->num_buckets; b++) {
		total_achievements += ach_menu_list->buckets[b].num_achievements;
	}

	if (total_achievements == 0) {
		RA_destroyAchievementList(ach_menu_list);
		ach_menu_list = NULL;
		// This can happen with unsupported game versions where pseudo-achievements
		// are counted in the summary but not available in the achievement list
		Menu_message("Achievement list not available", (char*[]){"B", "BACK", NULL});
		return MENU_CALLBACK_NOP;
	}

	// Create flattened array of all achievement pointers
	const rc_client_achievement_t** all_achievements = calloc(total_achievements, sizeof(rc_client_achievement_t*));
	AchSortEntry* sort_entries = calloc(total_achievements, sizeof(AchSortEntry));
	// Allocate filtered array once (max size = all achievements)
	const rc_client_achievement_t** filtered = calloc(total_achievements, sizeof(rc_client_achievement_t*));
	if (!all_achievements || !sort_entries || !filtered) {
		free(all_achievements);
		free(sort_entries);
		free(filtered);
		RA_destroyAchievementList(ach_menu_list);
		ach_menu_list = NULL;
		Menu_message("Failed to load achievements", (char*[]){"B", "BACK", NULL});
		return MENU_CALLBACK_NOP;
	}
	int idx = 0;
	uint32_t subset = 0;
	for (uint32_t b = 0; b < ach_menu_list->num_buckets && idx < total_achievements; b++) {
		const rc_client_achievement_bucket_t* bucket = &ach_menu_list->buckets[b];
		// buckets come subset by subset (subset_id is 0 when the game has only one)
		if (b > 0 && bucket->subset_id != ach_menu_list->buckets[b - 1].subset_id)
			subset++;
		for (uint32_t a = 0; a < bucket->num_achievements && idx < total_achievements; a++) {
			sort_entries[idx].ach = bucket->achievements[a];
			sort_entries[idx++].subset = subset;
		}
	}

	// Sort achievements according to settings
	ach_sort_achievements(sort_entries, idx);
	for (int j = 0; j < idx; j++)
		all_achievements[j] = sort_entries[j].ach;
	free(sort_entries);

	// the view's rows (Brick OS) and the summary: what is got of all, in count and points
	MenuBosAch* ach_rows = calloc(total_achievements, sizeof(MenuBosAch));
	char (*ach_state)[64] = calloc(total_achievements, 64);
	int ach_got = 0, ach_total = 0, ach_pts_got = 0, ach_pts_total = 0;
	for (int j = 0; j < idx; j++) {
		const rc_client_achievement_t* a = all_achievements[j];
		if (a->id == RA_UNKNOWN_EMULATOR_ACHIEVEMENT_ID)
			continue;
		ach_total++, ach_pts_total += a->points;
		if (a->unlocked)
			ach_got++, ach_pts_got += a->points;
	}
	if (!ach_rows || !ach_state) {
		free(ach_rows);
		free(ach_state);
		free(all_achievements);
		free(filtered);
		RA_destroyAchievementList(ach_menu_list);
		ach_menu_list = NULL;
		Menu_message("Failed to load achievements", (char*[]){"B", "BACK", NULL});
		return MENU_CALLBACK_NOP;
	}

	// Custom menu loop with X (mute) and Y (filter) support
	bool dirty = true;
	bool filter_dirty = true; // Rebuild filtered list when this is set
	int show_menu = 1;
	int selected = 0;
	int start = 0;
	int max_visible = (screen->h - ((SCALE1(PADDING + PILL_SIZE) * 2) + SCALE1(BUTTON_SIZE))) / SCALE1(BUTTON_SIZE);

	int filtered_count = 0;

	// Hide the "Unknown Emulator" warning when hardcore mode is disabled.
	// Show it when enabled so users understand why they only get softcore unlocks.
	// Note: We intentionally show "Unsupported Game Version" so users know to find a supported ROM.
	bool hide_unknown_emulator = !CFG_getRAHardcoreMode();

	while (show_menu) {
		GFX_startFrame();
		PAD_poll();

		// Rebuild filtered list only when filter changes
		if (filter_dirty) {
			filtered_count = 0;
			for (int j = 0; j < total_achievements; j++) {
				// Skip "Unknown Emulator" warning when hardcore mode is disabled
				if (hide_unknown_emulator && all_achievements[j]->id == RA_UNKNOWN_EMULATOR_ACHIEVEMENT_ID) {
					continue;
				}
				if (!ach_filter_locked_only || !all_achievements[j]->unlocked) {
					filtered[filtered_count++] = all_achievements[j];
				}
			}

			if (filtered_count == 0) {
				if (ach_filter_locked_only) {
					// No locked achievements when filter is on - revert filter
					ach_filter_locked_only = false;
					continue; // Will rebuild with filter off
				}
				// No achievements at all after filtering - exit menu
				free(all_achievements);
				free(filtered);
				RA_destroyAchievementList(ach_menu_list);
				ach_menu_list = NULL;
				ach_menu_achievements = NULL;
				ach_menu_count = 0;
				free(ach_rows);
				free(ach_state);
				MenuBos_achDone();
				Menu_message("No achievements found", (char*[]){"B", "BACK", NULL});
				return MENU_CALLBACK_NOP;
			}

			// Store for detail view
			ach_menu_achievements = filtered;
			ach_menu_count = filtered_count;

			// Reset scroll position when filter changes
			if (selected >= filtered_count)
				selected = filtered_count - 1;
			if (selected < 0)
				selected = 0;
			start = 0;

			filter_dirty = false;
			dirty = true;
		}

		int end = MIN(start + max_visible, filtered_count);

		if (PAD_justRepeated(BTN_UP)) {
			selected--;
			if (selected < 0) {
				selected = filtered_count - 1;
				start = MAX(0, filtered_count - max_visible);
			} else if (selected < start) {
				start--;
			}
			dirty = true;
		} else if (PAD_justRepeated(BTN_DOWN)) {
			selected++;
			if (selected >= filtered_count) {
				selected = 0;
				start = 0;
			} else if (selected >= end) {
				start++;
			}
			dirty = true;
		} else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_L1)) {
			// Page up (move up by max_visible items)
			selected -= max_visible;
			if (selected < 0) {
				selected = 0;
				start = 0;
			} else {
				start = selected;
			}
			dirty = true;
		} else if (PAD_justRepeated(BTN_RIGHT) || PAD_justRepeated(BTN_R1)) {
			// Page down (move down by max_visible items)
			selected += max_visible;
			if (selected >= filtered_count) {
				selected = filtered_count - 1;
				start = MAX(0, filtered_count - max_visible);
			} else {
				start = selected;
			}
			dirty = true;
		} else if (PAD_justPressed(BTN_B)) {
			show_menu = 0;
		} else if (PAD_justPressed(BTN_X)) {
			// Toggle mute for selected achievement
			if (filtered_count > 0) {
				const rc_client_achievement_t* ach = filtered[selected];
				RA_toggleAchievementMute(ach->id);
				dirty = true;
			}
		} else if (PAD_justPressed(BTN_Y)) {
			// Toggle filter: All <-> Locked Only
			ach_filter_locked_only = !ach_filter_locked_only;
			selected = 0;
			start = 0;
			filter_dirty = true;
		}

		if (MenuBos_achMoving())
			dirty = true;
		if (dirty) {
			// Brick OS (DESIGN.md 7.1, boards v2-GameMenuAch*)
			for (int j = 0; j < filtered_count; j++) {
				const rc_client_achievement_t* a = filtered[j];
				MenuBosAch* r = &ach_rows[j];
				*r = (MenuBosAch){.title = a->title, .desc = a->description, .points = (int)a->points,
								  .unlocked = a->unlocked != 0, .muted = RA_isAchievementMuted(a->id),
								  .badge = RA_Badges_get(RA_getGameId(), a->badge_name, !a->unlocked),
								  .state = ach_state[j], .progress = -1, .rarity = a->rarity};
				if (a->unlocked && a->unlock_time > 0) {
					struct tm tm;
					localtime_r(&a->unlock_time, &tm);
					static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
					char when[48];
					snprintf(when, sizeof(when), "%d %s %02d:%02d", tm.tm_mday, I18N_text(mon[tm.tm_mon]), tm.tm_hour, tm.tm_min);
					snprintf(ach_state[j], 64, I18N_text("Unlocked %s"), when);
				} else if (a->unlocked)
					snprintf(ach_state[j], 64, "%s", I18N_text("Unlocked"));
				else if (a->measured_progress[0]) {
					snprintf(ach_state[j], 64, I18N_text("Progress %s"), a->measured_progress);
					r->progress = a->measured_percent / 100.0f;
				} else
					snprintf(ach_state[j], 64, "%s", I18N_text("Not yet"));
				r->tag = a->type == RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE		 ? I18N_text("Missable")
						 : a->type == RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION ? I18N_text("Progression")
						 : a->type == RC_CLIENT_ACHIEVEMENT_TYPE_WIN		 ? I18N_text("Win Condition")
																			 : NULL;
			}
			BosHint hr[3] = {{"Y", I18N_text(ach_filter_locked_only ? "Show all" : "Not yet only")},
							 {"X", I18N_text(filtered_count > 0 && RA_isAchievementMuted(filtered[selected]->id) ? "Unmute notice" : "Mute notice")},
							 {"B", I18N_text("Back")}};
			BosHint hl[1] = {{"L1 R1", I18N_text("Page")}};
			MenuBosAchView v = {.rows = ach_rows, .n = filtered_count, .sel = selected, .scroll = &start,
								.got = ach_got, .total = ach_total, .pts_got = ach_pts_got, .pts_total = ach_pts_total,
								.locked_only = ach_filter_locked_only, .hints = hr, .nhints = 3, .left_hints = hl,
								.nleft = filtered_count > max_visible ? 1 : 0};
			MenuBos_renderAch(screen, &v);
			GFX_flip(screen);
			dirty = false;
		} else
			GFX_delay();
	}

	// Cleanup
	free(ach_rows);
	free(ach_state);
	MenuBos_achDone();
	free(all_achievements);
	free(filtered); // This was assigned to ach_menu_achievements
	ach_menu_achievements = NULL;
	ach_menu_count = 0;
	if (ach_menu_list) {
		RA_destroyAchievementList(ach_menu_list);
		ach_menu_list = NULL;
	}

	return MENU_CALLBACK_NOP;
}

// Frontend hint: the release tag ("NX Redux (v1.8.0)") when version.txt has
// one, otherwise the compiled-in date+hash that identifies dev builds.
static char frontend_version_desc[64] = BRAND_NAME " (" BUILD_DATE " " BUILD_HASH ")";
static void initFrontendVersionDesc(void) {
	char version_path[MAX_PATH];
	snprintf(version_path, sizeof(version_path), "%sversion.txt", ROOT_SYSTEM_PATH);
	FILE* f = fopen(version_path, "r");
	if (!f)
		return;
	char line[64];
	char tag[64] = {0};
	int line_num = 0;
	while (fgets(line, sizeof(line), f) && line_num < 3) {
		line_num++;
		if (line_num == 3) {
			int len = (int)strlen(line);
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
				line[--len] = '\0';
			strncpy(tag, line, sizeof(tag) - 1);
		}
	}
	fclose(f);
	if (tag[0] && strcmp(tag, "untagged") != 0)
		snprintf(frontend_version_desc, sizeof(frontend_version_desc), BRAND_NAME " (%s)", tag);
}

// options_menu index of Achievements (Save Changes follows it, or takes its
// place while RA is off)
#define OPTIONS_RA_INDEX 6
static MenuList options_menu = {
	.type = MENU_LIST,
	.title = "Options",
	.items = (MenuItem[]){
		{"Frontend", frontend_version_desc, .on_confirm = OptionFrontend_openMenu},
		// Core options that apply without a core restart; the full set (incl.
		// restart-only options) lives in the pre-launch editor (options.elf)
		{"Core Options", (char*)core.version, .on_confirm = OptionEmulator_openMenu},
		{"Shaders", .on_confirm = OptionShaders_openMenu},
		{"Cheats", .on_confirm = OptionCheats_openMenu},
		{"Controls", .on_confirm = OptionControls_openMenu},
		{"Shortcuts", .on_confirm = OptionShortcuts_openMenu},
		{"Achievements", .on_confirm = OptionAchievements_openMenu},
		{"Save Changes", .on_confirm = OptionSaveChanges_openMenu},
		{NULL},
		{NULL},
	}};

// Track the index of Save Changes menu item (changes based on RA visibility)
static int save_changes_index = OPTIONS_RA_INDEX + 1;

// Update options menu visibility based on RA enable state
void Options_updateVisibility(void) {
	if (CFG_getRAEnable()) {
		// RA enabled: Achievements, then Save Changes
		options_menu.items[OPTIONS_RA_INDEX].name = "Achievements";
		options_menu.items[OPTIONS_RA_INDEX].on_confirm = OptionAchievements_openMenu;
		options_menu.items[OPTIONS_RA_INDEX + 1].name = "Save Changes";
		options_menu.items[OPTIONS_RA_INDEX + 1].on_confirm = OptionSaveChanges_openMenu;
		save_changes_index = OPTIONS_RA_INDEX + 1;
	} else {
		// RA disabled: hide Achievements, move Save Changes up
		options_menu.items[OPTIONS_RA_INDEX].name = "Save Changes";
		options_menu.items[OPTIONS_RA_INDEX].desc = NULL;
		options_menu.items[OPTIONS_RA_INDEX].on_confirm = OptionSaveChanges_openMenu;
		options_menu.items[OPTIONS_RA_INDEX + 1].name = NULL;
		options_menu.items[OPTIONS_RA_INDEX + 1].on_confirm = NULL;
		save_changes_index = OPTIONS_RA_INDEX;
	}
}

void OptionSaveChanges_updateDesc(void) {
	options_menu.items[save_changes_index].desc = getSaveDesc();
}

// Update the Achievements menu item to show count (only when RA enabled)
static char ach_desc_buffer[64] = {0};
void OptionAchievements_updateDesc(void) {
	if (!CFG_getRAEnable())
		return;

	if (RA_isGameLoaded()) {
		uint32_t unlocked, total;
		RA_getAchievementSummary(&unlocked, &total);
		if (total > 0) {
			snprintf(ach_desc_buffer, sizeof(ach_desc_buffer), "%u / %u unlocked", unlocked, total);
			options_menu.items[OPTIONS_RA_INDEX].desc = ach_desc_buffer;
			return;
		}
		options_menu.items[OPTIONS_RA_INDEX].desc = (char*)"No achievements yet";
		return;
	}
	options_menu.items[OPTIONS_RA_INDEX].desc = RA_isGameUnknown() ? (char*)"Game not recognized" : NULL;
}

// Arcade zips without a map.txt alias get the title the launcher shows for them
// (.system/res/arcade/<TAG>.txt, keyed by the Roms folder's tag), so the menu
// says "Metal Slug 6" rather than "mslug6". Leaves name alone otherwise. The
// table is only read for .zip/.7z files, once per game: the answer is cached by
// ROM path, so later menu opens don't re-parse it (FBN.txt has ~8000 lines).
static void getArcadeTitle(const char* path, char* name) {
	static char cached_path[MAX_PATH];
	static char cached_title[MAX_PATH]; // "" = the table has no title for it
	if (!ArcadeNames_isArcadeFile(baseName(path)))
		return;
	if (strcmp(cached_path, path) != 0) {
		cached_title[0] = '\0';
		char tag[MAX_PATH];
		getEmuName(path, tag);
		char table_path[MAX_PATH];
		snprintf(table_path, sizeof(table_path), "%s/arcade/%s.txt", RES_PATH, ArcadeNames_tableTag(tag));
		ArcadeNames* names = ArcadeNames_load(table_path);
		const char* title = ArcadeNames_get(names, baseName(path));
		if (title && strcmp(title, ".") != 0)
			snprintf(cached_title, sizeof(cached_title), "%s", title);
		ArcadeNames_free(names);
		snprintf(cached_path, sizeof(cached_path), "%s", path);
	}
	if (cached_title[0])
		snprintf(name, MAX_PATH, "%s", cached_title);
}


// The rows of an options page for the kit (DESIGN.md 7.1): an Off/On pair is a switch, a run of numbers a slider
// with its number, other values cycle, a binding shows its buttons, a row that opens something has a chevron.
static bool optIsToggle(char** values, int n) {
	return n == 2 && ((!strcmp(values[0], "Off") && !strcmp(values[1], "On")) ||
					  (!strcmp(values[0], "On") && !strcmp(values[1], "Off")));
}
static bool optIsNumeric(char** values, int n) {
	if (n < 5)
		return false;
	for (int i = 0; i < n; i++) {
		const char* v = values[i];
		if (!v || !v[0])
			return false;
		for (const char* c = v; *c; c++)
			if ((*c < '0' || *c > '9') && *c != '%' && !(*c == '-' && c == v))
				return false;
	}
	return true;
}
static void optRow(MenuList* list, MenuItem* mi, bool waiting, MenuBosRow* r) {
	*r = (MenuBosRow){.name = I18N_text(mi->name), .kind = MB_ROW_NONE};
	if (waiting) {
		r->kind = MB_ROW_TEXT, r->value = I18N_text("Waiting…");
	} else if (mi->values && mi->values != button_labels) {
		int n = 0;
		while (mi->values[n])
			n++;
		const char* cur = mi->value >= 0 && mi->value < n ? mi->values[mi->value] : "";
		if (optIsToggle(mi->values, n))
			r->kind = MB_ROW_TOGGLE, r->on = !strcmp(cur, "On");
		else if (optIsNumeric(mi->values, n))
			r->kind = MB_ROW_SLIDER, r->value = cur, r->frac = n > 1 ? (float)mi->value / (n - 1) : 0;
		else
			r->kind = MB_ROW_CYCLE, r->value = I18N_text(cur);
	} else if (mi->values == button_labels) {
		int n = 0;
		while (mi->values[n])
			n++;
		r->kind = MB_ROW_BIND;
		r->value = mi->value >= 0 && mi->value < n ? PAD_buttonLabel(mi->values[mi->value]) : "";
	} else if (mi->submenu || mi->on_confirm || list->on_confirm)
		r->kind = MB_ROW_NAV;
}

// the pages open now, so a page's left column shows the one it was opened from
static MenuList* opt_stack[8];
static int* opt_sel[8];
static int opt_depth;

int Menu_options(MenuList* list) {
	MenuItem* items = list->items;
	int type = list->type;

	bool dirty = true;
	int show_options = 1;
	IndicatorType show_settings = INDICATOR_NONE;
	int await_input = 0;

	int count;
	for (count = 0; items[count].name; count++)
		;
	int selected = 0;
	ListLayout layout = UI_calcListLayout(screen);
	// an untitled options page keeps the pill list's top gutter (LIST-LAYOUT §10.2)
	layout.titled = (list->title && list->title[0]) || (list->desc && list->desc[0]);
	int scroll = 0;

	OptionSaveChanges_updateDesc();
	OptionAchievements_updateDesc();

	int defer_menu = false;
	if (opt_depth < 8) {
		opt_stack[opt_depth] = list;
		opt_sel[opt_depth] = &selected;
	}
	opt_depth++;
	while (show_options) {
		if (await_input) {
			defer_menu = true;
			list->on_confirm(list, selected);

			selected += 1;
			if (selected >= count)
				selected = 0;
			dirty = true;
			await_input = false;
		}

		GFX_startFrame();
		PAD_poll();
		if (PAD_justRepeated(BTN_UP)) {
			selected -= 1;
			if (selected < 0)
				selected = count - 1;
			dirty = true;
		} else if (PAD_justRepeated(BTN_DOWN)) {
			selected += 1;
			if (selected >= count)
				selected = 0;
			dirty = true;
		} else {
			MenuItem* item = &items[selected];
			if (item->values && item->values != button_labels) { // not an input binding
				if (PAD_justRepeated(BTN_LEFT)) {
					if (item->value > 0)
						item->value -= 1;
					else {
						int j;
						for (j = 0; item->values[j]; j++)
							;
						item->value = j - 1;
					}

					if (item->on_change)
						item->on_change(list, selected);
					else if (list->on_change)
						list->on_change(list, selected);

					dirty = true;
				} else if (PAD_justRepeated(BTN_RIGHT)) {
					// first check if its not out of bounds already
					int i = 0;
					while (item->values[i])
						i++;
					if (item->value >= i)
						item->value = 0;

					if (item->values[item->value + 1])
						item->value += 1;
					else
						item->value = 0;

					if (item->on_change)
						item->on_change(list, selected);
					else if (list->on_change)
						list->on_change(list, selected);

					dirty = true;
				}
			}
		}

		// uint32_t now = SDL_GetTicks();
		if (PAD_justPressed(BTN_B)) { // || PAD_tappedMenu(now)
			show_options = 0;
		} else if (PAD_justPressed(BTN_A)) {
			MenuItem* item = &items[selected];
			int result = MENU_CALLBACK_NOP;
			int nv = 0;
			while (item->values && item->values != button_labels && item->values[nv])
				nv++;
			if (!item->on_confirm && !item->submenu && !list->on_confirm && optIsToggle(item->values, nv)) {
				item->value = !item->value; // a switch: A flips it (DESIGN.md 6.6)
				if (item->on_change)
					item->on_change(list, selected);
				else if (list->on_change)
					list->on_change(list, selected);
			} else if (item->on_confirm)
				result = item->on_confirm(list, selected); // item-specific action, eg. Save for all games
			else if (item->submenu)
				result = Menu_options(item->submenu); // drill down, eg. main options menu
			// TODO: is there a way to defer on_confirm for MENU_INPUT so we can clear the currently set value to indicate it is awaiting input?
			// eg. set a flag to call on_confirm at the beginning of the next frame?
			else if (list->on_confirm) {
				if (item->values == button_labels)
					await_input = 1; // button binding
				else
					result = list->on_confirm(list, selected); // list-specific action, eg. show item detail view or input binding
			}
			if (result == MENU_CALLBACK_EXIT)
				show_options = 0;
			else {
				if (result == MENU_CALLBACK_NEXT_ITEM) {
					selected += 1;
					if (selected >= count)
						selected = 0;
				}
				dirty = true;
			}
		} else if (PAD_justPressed(BTN_Y) && opt_depth > 1 && count > 0 && items[selected].desc &&
				   MenuBos_optDescLong(I18N_text(items[selected].desc))) {
			Menu_textDialog(I18N_text(items[selected].name), I18N_text(items[selected].desc));
			dirty = true;
		} else if (type == MENU_INPUT) {
			if (PAD_justPressed(BTN_X)) {
				MenuItem* item = &items[selected];
				item->value = 0;

				if (item->on_change)
					item->on_change(list, selected);
				else if (list->on_change)
					list->on_change(list, selected);

				selected += 1;
				if (selected >= count)
					selected = 0;
				dirty = true;
			}
		}

		if (!defer_menu)
			PWR_update(&dirty, &show_settings, Menu_beforeSleep, Menu_afterSleep);

		// defer_menu suppresses power handling while the just-bound button (and
		// any held MENU modifier) is released. Clear it once input has settled
		// — keying off BTN_MENU alone left it stuck on forever when the user
		// bound a plain button without holding MENU, disabling sleep/power for
		// the rest of the options session.
		if (defer_menu && !PAD_anyPressed())
			defer_menu = false;

		// an on_change handler may rebuild the rows (the Shaders page hides
		// unused shader slots), so recount them after input and before this
		// frame renders: a row past the new end has a NULL name, and drawing
		// it with the old count crashed in the text truncation
		for (count = 0; items[count].name; count++)
			;
		if (selected >= count)
			selected = count - 1;
		if (selected < 0)
			selected = 0;
		if (scroll > 0 && scroll + layout.items_per_page > count) {
			scroll = count - layout.items_per_page;
			if (scroll < 0)
				scroll = 0;
		}

		if (MenuBos_optMoving())
			dirty = true;
		if (dirty) {
			// Brick OS (DESIGN.md 7.1): Settings' two columns over the paused game
			MenuBosRow rows[count > 0 ? count : 1];
			for (int i = 0; i < count; i++)
				optRow(list, &items[i], await_input && i == selected, &rows[i]);
			MenuBosOptView v = {0};
			int d = opt_depth - 1;
			static const char* root_icons[] = {"display", "cpu", "spark", "pencil", "pad", "keyboard", "trophy", "save", NULL, NULL};
			static const uint32_t root_colors[] = {0x5E5CE6, 0x0A84FF, 0xBF5AF2, 0xFF9F0A, 0x8E8E93, 0x30B0C7, 0xFFD60A, 0x30D158, 0, 0};
			MenuList* left = d == 0 || d > 8 ? list : opt_stack[d - 1];
			int nl = 0;
			while (left->items[nl].name)
				nl++;
			const char* lnames[nl > 0 ? nl : 1];
			const char* licons[nl > 0 ? nl : 1];
			for (int i = 0; i < nl; i++) {
				lnames[i] = I18N_text(left->items[i].name);
				licons[i] = NULL;
				if (left == &options_menu) {
					const char* nm = left->items[i].name;
					licons[i] = !strcmp(nm, "Frontend")		  ? "display"
								: !strcmp(nm, "Core Options") ? "cpu"
								: !strcmp(nm, "Shaders")	  ? "spark"
								: !strcmp(nm, "Cheats")		  ? "pencil"
								: !strcmp(nm, "Controls")	  ? "pad"
								: !strcmp(nm, "Shortcuts")	  ? "keyboard"
								: !strcmp(nm, "Achievements") ? "trophy"
															  : "save";
				}
			}
			uint32_t lcolors[nl > 0 ? nl : 1];
			for (int i = 0; i < nl; i++) {
				const char* ic = licons[i];
				lcolors[i] = !ic ? 0x2C2C30
							 : !strcmp(ic, "display")  ? root_colors[0]
							 : !strcmp(ic, "cpu")	   ? root_colors[1]
							 : !strcmp(ic, "spark")	   ? root_colors[2]
							 : !strcmp(ic, "pencil")   ? root_colors[3]
							 : !strcmp(ic, "pad")	   ? root_colors[4]
							 : !strcmp(ic, "keyboard") ? root_colors[5]
							 : !strcmp(ic, "trophy")   ? root_colors[6]
													   : root_colors[7];
			}
			(void)root_icons;
			v.left = lnames, v.nleft = nl;
			v.left_icons = left == &options_menu ? licons : NULL;
			v.left_colors = lcolors;
			char head_desc[512] = "";
			if (d == 0) { // the Options page itself: the focus on the left, the right says what the row holds
				v.left_sel = selected, v.left_focused = true;
				v.head = count > 0 ? I18N_text(items[selected].name) : "";
				if (count > 0 && items[selected].desc)
				{
					snprintf(head_desc, sizeof(head_desc), "%s", I18N_text(items[selected].desc));
					for (char* c = head_desc; *c; c++)
						if (*c == '\n')
							*c = ' ';
				}
				v.head_desc = head_desc;
			} else {
				v.left_sel = d - 1 < 8 ? *opt_sel[d - 1] : 0;
				v.head = I18N_text(list->title ? list->title : (list->desc ? list->desc : ""));
				v.rows = rows, v.nrows = count, v.sel = selected, v.scroll = &scroll;
				const char* dsc = count > 0 && items[selected].desc ? items[selected].desc : (list->title ? list->desc : NULL);
				static char flat[1024]; // one paragraph: the line breaks were for the old narrow layout
				snprintf(flat, sizeof(flat), "%s", dsc ? I18N_text(dsc) : "");
				for (char* c = flat; *c; c++)
					if (*c == '\n')
						*c = ' ';
				v.desc = flat;
			}
			BosHint r[3], l[1];
			int nr = 0, nlh = 0;
			bool long_desc = d > 0 && count > 0 && items[selected].desc && MenuBos_optDescLong(I18N_text(items[selected].desc));
			MenuBosRow* hot = d > 0 && count > 0 ? &rows[selected] : NULL;
			if (type == MENU_INPUT) {
				r[nr++] = (BosHint){"A", I18N_text("Set")};
				r[nr++] = (BosHint){"X", I18N_text("Clear")};
			} else if (d == 0 || (hot && hot->kind == MB_ROW_NAV))
				r[nr++] = (BosHint){"A", I18N_text("Open")};
			else if (hot && hot->kind == MB_ROW_TOGGLE)
				r[nr++] = (BosHint){"A", I18N_text("On/Off")};
			if (hot && (hot->kind == MB_ROW_CYCLE || hot->kind == MB_ROW_SLIDER || hot->kind == MB_ROW_TOGGLE))
				l[nlh++] = (BosHint){"\xE2\x86\x90 \xE2\x86\x92", I18N_text("Change")};
			r[nr++] = (BosHint){"B", I18N_text("Back")};
			if (long_desc) // board v2-GameMenuCheat: Y takes the left
				nlh = 0, l[nlh++] = (BosHint){"Y", I18N_text("Read all")};
			if (await_input && count > 0) { // binding: the sheet says it all
				v.prompt = I18N_text(items[selected].name);
				nr = nlh = 0;
			}
			v.hints = r, v.nhints = nr, v.left_hints = l, v.nleft_hints = nlh;
			MenuBos_renderOptions(screen, &v);
			GFX_flip(screen);
			dirty = false;
		} else {
			GFX_delay();
		}

		hdmimon();
	}

	opt_depth--;
	return 0;
}

void Menu_initState(void) {
	if (exists(menu.slot_path))
		menu.slot = getInt(menu.slot_path);
	// The slot file may point at a hidden slot: RESUME_SLOT_DEFAULT (8) or the
	// AUTO_RESUME_SLOT (9) that a plain quit writes to. Neither is user-visible
	// in the Save/Load menu, so snap back to a regular slot (0-7) for the UI.
	// A corrupt slot file can also yield a negative value.
	if (menu.slot < 0 || menu.slot >= MENU_SLOT_COUNT)
		menu.slot = 0;

	menu.save_exists = 0;
	menu.preview_exists = 0;
}
void Menu_updateState(void) {
	int last_slot = state_slot;
	state_slot = menu.slot;

	char save_path[MAX_PATH];
	State_getPath(save_path);

	state_slot = last_slot;

	// always sanitized/outer name, to keep main UI from having to inspect archives
	snprintf(menu.bmp_path, sizeof(menu.bmp_path), "%s/%s.%d.bmp", menu.minui_dir, game.name, menu.slot);
	snprintf(menu.txt_path, sizeof(menu.txt_path), "%s/%s.%d.txt", menu.minui_dir, game.name, menu.slot);

	menu.save_exists = exists(save_path);
	menu.preview_exists = menu.save_exists && exists(menu.bmp_path);
	struct stat st;
	menu.save_time = menu.save_exists && stat(save_path, &st) == 0 ? st.st_mtime : 0;
}

// which of the eight slots hold a save (the strip under the slot's picture)
static void Menu_readSlots(void) {
	int last_slot = state_slot;
	for (int i = 0; i < MENU_SLOT_COUNT; i++) {
		char path[MAX_PATH];
		state_slot = i;
		State_getPath(path);
		menu.slot_used[i] = exists(path);
	}
	state_slot = last_slot;
}

typedef struct {
	char* pixels;		  // raw GL capture (ABGR8888), or NULL when surface is set
	SDL_Surface* surface; // ready-to-save surface the worker owns, or NULL
	char* path;
	int w;
	int h;
} SaveImageArgs;

// Wraps a raw GL-capture pixel buffer (ABGR8888, GFX_GL_screenCapture's format)
// in an SDL_Surface converted to `format`. Frees the intermediate raw surface;
// caller still owns (and must free) `pixels`.
static SDL_Surface* rawCaptureToSurface(unsigned char* pixels, int w, int h, Uint32 format) {
	SDL_Surface* rawSurface = SDL_CreateRGBSurfaceWithFormatFrom(
		pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
	SDL_Surface* converted = rawSurface ? SDL_ConvertSurfaceFormat(rawSurface, format, 0) : NULL;
	SDL_FreeSurface(rawSurface);
	return converted;
}
int save_screenshot_thread(void* data) {
	PWR_pinHelperThread(); // minarch_cpu_affinity=big -> SLOW set (no-op otherwise)
	SaveImageArgs* args = (SaveImageArgs*)data;
	SDL_Surface* converted = args->surface ? args->surface
										   : rawCaptureToSurface(args->pixels, args->w, args->h, SDL_PIXELFORMAT_ARGB8888);

	if (converted && converted->format->format != SDL_PIXELFORMAT_ARGB8888) {
		SDL_Surface* argb = SDL_ConvertSurfaceFormat(converted, SDL_PIXELFORMAT_ARGB8888, 0);
		SDL_FreeSurface(converted);
		converted = argb;
	}
	if (!converted) {
		SDL_Log("Failed to convert screenshot surface: %s", SDL_GetError());
	} else if (PNG_saveRGB(args->path, converted->pixels, converted->w, converted->h, converted->pitch) != 0) {
		// zlib's fastest level: ~0.15 s for a 1280x720 frame where SDL_image took ~0.8 s (Smart Pro S)
		SDL_Log("Failed to save PNG: %s", args->path);
	}
	SDL_FreeSurface(converted);
	free(args->path);
	free(args->pixels);
	free(args);
	return 0;
}
SDL_Thread* screenshotsavethread;
// Hands `pixels` or `surface` (ownership passes here) to the PNG-save worker
// after any previous save finishes. Without memory for the hand-off the image
// is dropped; without a thread it is saved inline.
static void Menu_startImageSave(unsigned char* pixels, SDL_Surface* surface, int w, int h, const char* png_path) {
	SaveImageArgs* args = malloc(sizeof(SaveImageArgs));
	char* path = SDL_strdup(png_path);
	if (!args || !path) {
		SDL_Log("Out of memory saving %s", png_path);
		free(args);
		free(path);
		free(pixels);
		SDL_FreeSurface(surface);
		return;
	}
	args->pixels = pixels;
	args->surface = surface;
	args->w = w;
	args->h = h;
	args->path = path;
	Menu_waitScreenshotSave();
	screenshotsavethread = SDL_CreateThread(save_screenshot_thread, "SaveScreenshotThread", args);
	if (!screenshotsavethread)
		save_screenshot_thread(args); // frees args
}
// GL-captures the current frame, without notifications, and returns it as a converted SDL_Surface
// (caller owns and must SDL_FreeSurface it), or NULL on capture failure.
SDL_Surface* Menu_captureScreenSurface(Uint32 pixel_format) {
	Present_stop();
	int cw, ch;
	unsigned char* pixels = GFX_GL_screenCaptureClean(&cw, &ch); // no toasts baked into the picture
	if (!pixels)
		return NULL;
	SDL_Surface* converted = rawCaptureToSurface(pixels, cw, ch, pixel_format);
	free(pixels);
	return converted;
}
// GL-captures the current frame, without notifications, and hands it to the background PNG-save
// worker at `png_path`; waits for any previous save to finish first.
void Menu_queueScreenshotSave(const char* png_path) {
	Present_stop();
	int cw, ch;
	unsigned char* pixels = GFX_GL_screenCaptureClean(&cw, &ch);
	if (!pixels) {
		SDL_Log("Failed to capture screenshot");
		return;
	}
	Menu_startImageSave(pixels, NULL, cw, ch, png_path);
}
// Hands a copy of `surface` to the background PNG-save worker at `png_path`.
// The in-game menu runs at a capped CPU clock, and a shader's scanlines or
// LCD grid leave no flat runs for the encoder, so saving the full-screen slot
// preview inline stalled Save for several seconds with a shader on.
static void Menu_queueSurfaceSave(SDL_Surface* surface, const char* png_path) {
	SDL_Surface* copy = SDL_ConvertSurface(surface, surface->format, 0);
	if (!copy) {
		SDL_Log("Failed to copy screenshot surface: %s", SDL_GetError());
		return;
	}
	Menu_startImageSave(NULL, copy, copy->w, copy->h, png_path);
}
// Blocks until a queued PNG save has finished writing, so its file can be read.
void Menu_waitScreenshotSave(void) {
	SDL_WaitThread(screenshotsavethread, NULL);
	screenshotsavethread = NULL;
}
void Menu_screenshot(void) {
	char rom_name[MAX_PATH]; // getDisplayName/getAlias can write up to MAX_PATH
	getDisplayName(game.alt_name, rom_name);
	getAlias(game.path, rom_name);

	time_t now = time(NULL);
	struct tm* t = localtime(&now);
	char buffer[100];
	strftime(buffer, sizeof(buffer), "%Y-%m-%d-%H-%M-%S", t);

	// make sure this actually exists
	char screenshots_dir[MAX_PATH];
	snprintf(screenshots_dir, sizeof(screenshots_dir), "%s/Screenshots", SDCARD_PATH);
	mkdir(screenshots_dir, 0755);

	char png_path[MAX_PATH];
	snprintf(png_path, sizeof(png_path), "%s/Screenshots/%s.%s.png", SDCARD_PATH, rom_name, buffer);
	Menu_queueScreenshotSave(png_path);

	// Show notification if enabled
	if (CFG_getNotifyScreenshot()) {
		Notification_push(NOTIFICATION_SETTING, "Screenshot saved", NULL);
	}
}
// X on Quit skips the autosave below, so it is only offered when there is one to skip.
static bool Menu_canAutosaveQuit(void) {
	// Block save states during multiplayer - causes connection breaks
	if (Multiplayer_isActive())
		return false;
	return core.serialize_size && core.serialize_size(); // core cannot save states otherwise
}
// Quit with A auto-saves to the hidden auto-resume slot with a switcher
// screenshot and repoints the .minui slot file at it, so RESUME always returns
// to the exact quit moment (parity with DC.pak's quit behavior). Without this,
// a bare quit leaves the switcher pointing at a stale (or no) state.
// Quitting from the menu: the screen as shown stays up while minarch exits
// and nextui starts on it (HELD_FRAME_PATH), instead of a fade to black. A BMP:
// encoding the PNG took ~0.3 s.
static void Menu_holdScreenForQuit(void) {
	// the display flips between two buffers, the one read back holding the frame
	// before (the previous item highlighted): show this one in both first
	GFX_flip(screen);
	GFX_flip(screen);
	SDL_Surface* shown = GFX_captureRendererToSurface();
	if (!shown)
		return;
	if (SDL_SaveBMP(shown, HELD_FRAME_BMP) == 0)
		putFile(HELD_FRAME_PATH, HELD_FRAME_BMP);
	SDL_FreeSurface(shown);
}
static void Menu_autosaveQuit(void) {
	if (!Menu_canAutosaveQuit())
		return;
	// leaving: the menu's low clock would only slow the save and the teardown
	PWR_setCPUSpeed(CPU_SPEED_PERFORMANCE);

	// disc path marker for multi-disc games, matching Menu_saveState
	if (menu.total_discs && menu.disc >= 0) {
		char txt_path[MAX_PATH];
		snprintf(txt_path, sizeof(txt_path), "%s/%s.%d.txt", menu.minui_dir, game.name, AUTO_RESUME_SLOT);
		char* disc_path = menu.disc_paths[menu.disc];
		putFile(txt_path, disc_path + strlen(menu.base_path));
	}

	// switcher screenshot for the auto slot; the menu is open, so menu.bitmap
	// holds the frame the user quit on. Encoded in the background (~0.3 s)
	// while minarch winds down; main waits for it before exiting
	if (menu.bitmap) {
		char bmp_path[MAX_PATH];
		snprintf(bmp_path, sizeof(bmp_path), "%s/%s.%d.bmp", menu.minui_dir, game.name, AUTO_RESUME_SLOT);
		Menu_queueSurfaceSave(menu.bitmap, bmp_path);
	}

	// only repoint RESUME at the auto slot if the state actually wrote —
	// minarch exits right after this, so state_slot needs no restoring
	state_slot = AUTO_RESUME_SLOT;
	if (State_write())
		putInt(menu.slot_path, AUTO_RESUME_SLOT);
}

const char* Menu_slotPreviewPath(void) {
	return menu.bmp_path;
}

void Menu_saveState(void) {
	// Block save states during multiplayer - causes connection breaks
	if (Multiplayer_isActive())
		return;

	Menu_updateState();

	if (menu.total_discs && menu.disc >= 0) {
		char* disc_path = menu.disc_paths[menu.disc];
		putFile(menu.txt_path, disc_path + strlen(menu.base_path));
	}

	// if already in menu use menu.bitmap instead for saving screenshots otherwise create new one on the fly
	if (newScreenshot) {
		Menu_queueScreenshotSave(menu.bmp_path);
		newScreenshot = 0;
	} else if (menu.bitmap) {
		Menu_queueSurfaceSave(menu.bitmap, menu.bmp_path);
	}

	state_slot = menu.slot;
	putInt(menu.slot_path, menu.slot);
	int success = State_write();

	// Show notification if enabled
	if (CFG_getNotifyManualSave()) {
		char msg[NOTIFICATION_MAX_MESSAGE];
		// User-facing slots are 1-8 (internal 0-7)
		snprintf(msg, sizeof(msg), success ? "State Saved - Slot %d" : "Save Failed - Slot %d", menu.slot + 1);
		Notification_push(NOTIFICATION_SAVE_STATE, msg, NULL);
	}
}
void Menu_loadState(void) {
	// Block load states during multiplayer - causes connection breaks
	if (Multiplayer_isActive())
		return;

	Menu_updateState();

	if (menu.save_exists) {
		int disc_changed = 0;
		if (menu.total_discs && menu.disc >= 0) {
			char slot_disc_name[MAX_PATH];
			slot_disc_name[0] = '\0'; // getFile writes nothing if the .txt is missing
			getFile(menu.txt_path, slot_disc_name, MAX_PATH);

			char slot_disc_path[MAX_PATH];
			if (slot_disc_name[0] == '/')
				snprintf(slot_disc_path, sizeof(slot_disc_path), "%s", slot_disc_name);
			else
				snprintf(slot_disc_path, sizeof(slot_disc_path), "%s%s", menu.base_path, slot_disc_name);

			char* disc_path = menu.disc_paths[menu.disc];
			if (!exactMatch(slot_disc_path, disc_path)) {
				int changed = Game_changeDisc(slot_disc_path);
				Menu_syncDiscIndex();
				if (!changed) {
					// wrong disc and no way to swap it - loading the state
					// anyway would silently mix disc and save
					Notification_push(NOTIFICATION_LOAD_STATE, "Load Failed - Wrong Disc", NULL);
					return;
				}
				disc_changed = 1;
			}
		}

		state_slot = menu.slot;
		putInt(menu.slot_path, menu.slot);
		int success;
		if (disc_changed) {
			// The pre-load state belongs to the disc that was just ejected.
			State_invalidateUndo();
			success = State_read();
		} else {
			success = State_readWithUndo();
		}
		Rewind_on_state_change();

		// Show notification if enabled
		if (CFG_getNotifyLoad()) {
			char msg[NOTIFICATION_MAX_MESSAGE];
			// User-facing slots are 1-8 (internal 0-7)
			snprintf(msg, sizeof(msg), success ? "State Loaded - Slot %d" : "Load Failed - Slot %d", menu.slot + 1);
			Notification_push(NOTIFICATION_LOAD_STATE, msg, NULL);
		}
	}
}

void Menu_undoLoadState(void) {
	// Deserializing state during multiplayer breaks the active connection.
	if (Multiplayer_isActive())
		return;

	int success = State_undoLoad();

	// Hardcore mode emits its own achievement notification.
	if (CFG_getNotifyLoad() && !RA_isHardcoreModeActive()) {
		Notification_push(NOTIFICATION_LOAD_STATE,
						  success ? "Load State Undone" : "Nothing To Undo", NULL);
	}
}

// During any netplay session MENU only asks whether to leave: states, rewind
// and fast-forward are off anyway, and a menu left open stalls the other player.
// minarch's own engines pause both sides cleanly, so their dialog simply waits;
// a session the core runs itself (flycast GGPO) can't pause, the other
// player's game just waits for this one, so no answer within the grace ends
// the session (leaves), well before the core's disconnect timeout.
typedef struct {
	uint32_t start;
	int seconds_left; // -1: no countdown
	bool menu_down;	  // MENU pressed while the dialog is up
} LeaveNetplayCtx;

static void leaveNetplay_render(SDL_Surface* dst, void* data) {
	LeaveNetplayCtx* ctx = data;
	char subtitle[128];
	if (ctx->seconds_left >= 0)
		snprintf(subtitle, sizeof(subtitle), "The other player is waiting: %d s left.", ctx->seconds_left);
	else
		snprintf(subtitle, sizeof(subtitle), "Leaving ends the netplay session.");
	UI_renderConfirmDialogHints(dst, "Leave netplay?", subtitle, (char*[]){"B", "CONTINUE", "A", "LEAVE", NULL});
}

static int leaveNetplay_handle(void* data) {
	LeaveNetplayCtx* ctx = data;
	if (Netplay_isConnected())
		Netplay_pollWhilePaused(); // keep the lockstep link alive, as the full menu did
	if (CoreNetplay_byePoll()) {   // the other player left meanwhile
		CoreNetplay_markEnded();
		return 1;
	}
	if (PAD_justPressed(BTN_A))
		return 1;
	if (PAD_justPressed(BTN_B))
		return 0;
	// MENU continues on its release: the game opens this dialog on a MENU
	// release (ma_input.c), so closing on the press let that same press's
	// release reopen it at once.
	if (PAD_justPressed(BTN_MENU))
		ctx->menu_down = true;
	if (ctx->menu_down && PAD_justReleased(BTN_MENU))
		return 0;
	if (ctx->seconds_left >= 0) {
		int left = CoreNetplay_leaveSecondsLeft(ctx->start, SDL_GetTicks());
		if (left != ctx->seconds_left) {
			ctx->seconds_left = left;
			return UI_MODAL_DIRTY;
		}
	}
	return UI_MODAL_CONTINUE;
}

// true = leave the session (and the game)
static bool Menu_leaveNetplay(void) {
	// A GBA link peer waits too (GBALink_setPaused), for as long as its pause
	// lasts (GBALINK_PEER_PAUSE_MAX_MS / SIOLS_HOLD_MAX_US, 10 s past this grace).
	bool timed = CoreNetplay_isActive() || GBALink_isConnected();
	LeaveNetplayCtx ctx = {SDL_GetTicks(), timed ? CORE_NETPLAY_LEAVE_GRACE_MS / 1000 : -1};
	UI_ModalOpts opts = {
		.screen = screen,
		.render = leaveNetplay_render,
		.handle = leaveNetplay_handle,
		.ctx = &ctx,
		.timeout_ms = timed ? CORE_NETPLAY_LEAVE_GRACE_MS : 0, // expiry returns -1: leave
		.reset_pad = true,
	};
	return UI_modalLoop(&opts) != 0;
}

void Menu_notice(const char* title, const char* subtitle, int hold_ms) {
	Present_stop();
	if (screen->w != DEVICE_WIDTH || screen->h != DEVICE_HEIGHT)
		screen = GFX_resize(DEVICE_WIDTH, DEVICE_HEIGHT, DEVICE_PITCH);
	GFX_clearShaders();
	// the leave dialog's look, as a notice: no button row
	UI_renderConfirmDialogHints(screen, title, subtitle, (char*[]){NULL});
	GFX_flip(screen);
	if (hold_ms > 0)
		SDL_Delay(hold_ms);
}

void Menu_loop(void) {
	Present_stop(); // the menu draws (and reads the frame back) on this thread
	RA_onMenuOpen();
	// the slot previews below are read back from disk
	Menu_waitScreenshotSave();
	menu.bitmap = Menu_captureScreenSurface(SDL_PIXELFORMAT_ARGB8888);
	SDL_Surface* backing = SDL_CreateRGBSurfaceWithFormat(0, DEVICE_WIDTH, DEVICE_HEIGHT, 32, SDL_PIXELFORMAT_ARGB8888);


	SDL_Rect dst = {
		0,
		0,
		screen->w,
		screen->h};
	if (menu.bitmap)
		SDL_BlitScaled(menu.bitmap, NULL, backing, &dst);

	int restore_w = screen->w;
	int restore_h = screen->h;
	int restore_p = screen->pitch;
	if (restore_w != DEVICE_WIDTH || restore_h != DEVICE_HEIGHT) {
		screen = GFX_resize(DEVICE_WIDTH, DEVICE_HEIGHT, DEVICE_PITCH);
	}

	SRAM_write();
	RTC_write();
	if (!HAS_POWER_BUTTON)
		PWR_enableSleep();
	PWR_setCPUSpeed(CPU_SPEED_MENU); // set Hz directly

	GFX_setEffect(EFFECT_NONE);

	int rumble_strength = VIB_getStrength();
	VIB_setStrength(0);

	PWR_enableAutosleep();
	PAD_reset();

	// path and string things
	char* tmp;
	char rom_name[MAX_PATH]; // without extension or cruft
	getDisplayName(game.name, rom_name);
	if (!getAlias(game.path, rom_name))
		getArcadeTitle(game.path, rom_name);

	int rom_disc = -1;
	char disc_name[16];
	if (menu.total_discs) {
		rom_disc = menu.disc;
		sprintf(disc_name, "Disc %i", menu.disc + 1);
	}

	int selected = 0; // resets every launch
	Menu_initState();
	Menu_readSlots();
	MenuBos_open(menu.bitmap, DEVICE_WIDTH, DEVICE_HEIGHT);
	// the console, from the game's folder ("Game Boy Advance (GBA)" -> "Game Boy Advance")
	char console_name[MAX_PATH] = "";
	{
		char dir[MAX_PATH];
		snprintf(dir, sizeof(dir), "%s", game.path);
		char* sl = strrchr(dir, '/');
		if (sl) {
			*sl = '\0';
			char* b = strrchr(dir, '/');
			snprintf(console_name, sizeof(console_name), "%s", b ? b + 1 : dir);
			char* paren = strrchr(console_name, '(');
			if (paren && paren > console_name) {
				*paren = '\0';
				for (char* e = paren - 1; e >= console_name && *e == ' '; e--)
					*e = '\0';
			}
		}
	}

	IndicatorType show_setting = INDICATOR_NONE;
	bool dirty = true;
	int ignore_menu = 0;
	int menu_start = 0;
	SDL_Surface* preview = SDL_CreateRGBSurface(SDL_SWSURFACE, DEVICE_WIDTH / 2, DEVICE_HEIGHT / 2, 32, RGBA_MASK_8888);
	char preview_path[MAX_PATH] = ""; // slot screenshot currently decoded into preview
	int preview_valid = 0;

	//set vid.blit to null for menu drawing no need for blitrender drawing
	GFX_clearShaders();
	if (Multiplayer_isActive()) {
		// The GBA link peer was paused before we got here (minarch.c) and is
		// resumed after the menu's teardown; leaving resumes it first, so a
		// real-cable peer stuck mid-transfer ends as on a pulled cable.
		if (Menu_leaveNetplay()) {
			GBALink_setPaused(false);
			Netplay_quitAll(); // as the full menu's Quit: close the link cleanly
			quit = 1;
		}
		show_menu = 0; // straight to the teardown below
	}
	while (show_menu) {
		GFX_startFrame();
		uint32_t now = SDL_GetTicks();

		PAD_poll();

		if (Netplay_isConnected()) {
			Netplay_pollWhilePaused();
		}
		// keep RA moving while paused (login retries, server replies). Not
		// in the achievements page: its list points into the game data a
		// finishing load would replace.
		RA_idle();
		int mp_active = Multiplayer_isActive();

		if (PAD_justPressed(BTN_UP)) {
			do {
				selected -= 1;
				if (selected < 0)
					selected += MENU_ITEM_COUNT;
			} while (mp_active && (selected == ITEM_SAVE || selected == ITEM_LOAD));
			dirty = true;
		} else if (PAD_justPressed(BTN_DOWN)) {
			do {
				selected += 1;
				if (selected >= MENU_ITEM_COUNT)
					selected -= MENU_ITEM_COUNT;
			} while (mp_active && (selected == ITEM_SAVE || selected == ITEM_LOAD));
			dirty = true;
		} else if (PAD_justPressed(BTN_LEFT)) {
			if (menu.total_discs > 1 && selected == ITEM_CONT) {
				menu.disc -= 1;
				if (menu.disc < 0)
					menu.disc += menu.total_discs;
				dirty = true;
				sprintf(disc_name, "Disc %i", menu.disc + 1);
			} else if (selected == ITEM_SAVE || selected == ITEM_LOAD) {
				menu.slot -= 1;
				if (menu.slot < 0)
					menu.slot += MENU_SLOT_COUNT;
				dirty = true;
			}
		} else if (PAD_justPressed(BTN_RIGHT)) {
			if (menu.total_discs > 1 && selected == ITEM_CONT) {
				menu.disc += 1;
				if (menu.disc == menu.total_discs)
					menu.disc -= menu.total_discs;
				dirty = true;
				sprintf(disc_name, "Disc %i", menu.disc + 1);
			} else if (selected == ITEM_SAVE || selected == ITEM_LOAD) {
				menu.slot += 1;
				if (menu.slot >= MENU_SLOT_COUNT)
					menu.slot -= MENU_SLOT_COUNT;
				dirty = true;
			}
		}

		if (dirty && (selected == ITEM_SAVE || selected == ITEM_LOAD)) {
			Menu_updateState();
		}

		if (PAD_justPressed(BTN_B) || (BTN_WAKE != BTN_MENU && PAD_tappedMenu(now))) {
			show_menu = 0;
		} else if (PAD_justPressed(BTN_X)) {
			if (!mp_active && selected == ITEM_LOAD && State_hasUndo()) {
				Menu_undoLoadState();
				show_menu = 0;
			} else if (selected == ITEM_QUIT && Menu_canAutosaveQuit()) {
				// quit without the auto-resume save: RESUME keeps its old point
				Menu_holdScreenForQuit();
				Netplay_quitAll();
				show_menu = 0;
				quit = 1;
			}
		} else if (PAD_justPressed(BTN_A)) {
			switch (selected) {
			case ITEM_CONT:
				if (menu.total_discs && menu.disc >= 0 && rom_disc != menu.disc) {
					char* disc_path = menu.disc_paths[menu.disc];
					Game_changeDisc(disc_path);
					Menu_syncDiscIndex();
				}
				show_menu = 0;
				break;

			case ITEM_SAVE: {
				Menu_saveState();
				show_menu = 0;
			} break;
			case ITEM_LOAD: {
				Menu_loadState();
				show_menu = 0;
			} break;
			case ITEM_OPTS: {
				if (simple_mode) {
					Core_reset();
					show_menu = 0;
				} else {
					int old_scaling = screen_scaling;
					Options_updateVisibility();
					Menu_options(&options_menu);
					if (screen_scaling != old_scaling) {
						selectScaler(renderer.true_w, renderer.true_h, renderer.src_p);

						restore_w = screen->w;
						restore_h = screen->h;
						restore_p = screen->pitch;
						screen = GFX_resize(DEVICE_WIDTH, DEVICE_HEIGHT, DEVICE_PITCH);
						SDL_Rect dst = {0, 0, DEVICE_WIDTH, DEVICE_HEIGHT};
						SDL_BlitScaled(menu.bitmap, NULL, backing, &dst);
					}
					dirty = true;
				}
			} break;
			case ITEM_QUIT:
				Menu_holdScreenForQuit();
				// before Netplay_quitAll so the netplay guard inside can skip
				// the autosave while a session is live
				Menu_autosaveQuit();
				Netplay_quitAll();
				show_menu = 0;
				quit = 1; // TODO: tmp?
				break;
			}
			if (!show_menu)
				break;
		}

		PWR_update(&dirty, &show_setting, Menu_beforeSleep, Menu_afterSleep);
		if (MenuBos_moving())
			dirty = true;
		if (dirty) {
			// Brick OS (DESIGN.md 7.1): the kit's page over the paused game
			MenuBosView v = {0};
			v.title = rom_name;
			v.game = rom_name;
			v.console = console_name;
			static const char* names[MENU_ITEM_COUNT] = {"Continue", "Save", "Load", "Options", "Quit game"};
			for (int i = 0; i < MENU_ITEM_COUNT; i++)
				v.items[i] = I18N_text(i == ITEM_OPTS && simple_mode ? "Reset" : names[i]);
			v.item_off[ITEM_SAVE] = v.item_off[ITEM_LOAD] = mp_active;
			v.selected = selected;
			char disc_label[32] = "";
			if (menu.total_discs > 1) {
				snprintf(disc_label, sizeof(disc_label), I18N_text("Disc %d"), menu.disc + 1);
				v.disc = disc_label;
			}
			v.slots = selected == ITEM_SAVE || selected == ITEM_LOAD;
			v.slot = menu.slot;
			for (int i = 0; i < MENU_SLOT_COUNT && i < MENU_BOS_SLOTS; i++)
				v.slot_used[i] = menu.slot_used[i];
			v.save_exists = menu.save_exists;
			v.preview_path = menu.preview_exists ? menu.bmp_path : NULL;
			v.save_time = menu.save_time;
			BosHint r[3], l[1];
			int nr = 0, nl = 0;
			char a_label[64];
			if (selected == ITEM_SAVE || selected == ITEM_LOAD) {
				l[nl++] = (BosHint){"\xE2\x86\x90 \xE2\x86\x92", I18N_text("Choose slot")};
				if (selected == ITEM_SAVE) {
					snprintf(a_label, sizeof(a_label), I18N_text("Save to slot %d"), menu.slot + 1);
					r[nr++] = (BosHint){"A", a_label};
				} else if (menu.save_exists) {
					snprintf(a_label, sizeof(a_label), I18N_text("Load slot %d"), menu.slot + 1);
					r[nr++] = (BosHint){"A", a_label};
				}
				if (!mp_active && selected == ITEM_LOAD && State_hasUndo())
					r[nr++] = (BosHint){"X", I18N_text("Undo load")};
				r[nr++] = (BosHint){"B", I18N_text("Back")};
			} else if (selected == ITEM_QUIT && Menu_canAutosaveQuit()) {
				r[nr++] = (BosHint){"A", I18N_text("Save & quit")};
				r[nr++] = (BosHint){"X", I18N_text("Quit without saving")};
				r[nr++] = (BosHint){"B", I18N_text("Resume")};
			} else {
				if (selected == ITEM_CONT && menu.total_discs > 1)
					l[nl++] = (BosHint){"\xE2\x86\x90 \xE2\x86\x92", I18N_text("Change disc")};
				r[nr++] = (BosHint){"A", I18N_text("Select")};
				r[nr++] = (BosHint){"B", I18N_text("Resume")};
			}
			v.hints = r, v.nhints = nr, v.left_hints = l, v.nleft = nl;
			MenuBos_render(screen, &v);
			GFX_flip(screen);
			dirty = false;
		} else {
			// please dont flip cause it will cause current_fps dip and audio is weird first seconds
			GFX_delay();
		}
		hdmimon();
	}

	SDL_FreeSurface(preview);
	MenuBos_close();
	if (menu.bitmap) {
		SDL_FreeSurface(menu.bitmap);
		menu.bitmap = NULL;
	}
	PAD_reset();

	// quitting with the menu held on screen (Menu_holdScreenForQuit): clearing
	// would flip it once more without its dimmed game behind
	if (!(quit && exists(HELD_FRAME_PATH)))
		GFX_clearAll();

	int count = 0;
	char** overlayList = config.frontend.options[FE_OPT_OVERLAY].values;
	while (overlayList && overlayList[count])
		count++;
	if (overlay >= 0 && overlay < count) {
		GFX_setOverlay(overlayList[overlay], core.tag);
	}

	GFX_setOffsetX(cfg_screenx);
	GFX_setOffsetY(cfg_screeny);
	if (!quit) {
		if (restore_w != DEVICE_WIDTH || restore_h != DEVICE_HEIGHT) {
			screen = GFX_resize(restore_w, restore_h, restore_p);
		}
		GFX_setEffect(screen_effect);

		GFX_clear(screen);

		setOverclock(overclock); // restore overclock value
		if (rumble_strength)
			VIB_setStrength(rumble_strength);

		if (!HAS_POWER_BUTTON)
			PWR_disableSleep();
	} else if (exists(NOUI_PATH))
		PWR_powerOff(0); // TODO: won't work with threaded core, only check this once per launch


	SDL_FreeSurface(backing);
	PWR_disableAutosleep();
}
