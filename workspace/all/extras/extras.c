/*
 * extras.elf — the Xtras catalog: browse curated games/tools, install
 * on-device. Entries live in ./catalog/<id>/{meta.txt,install.sh,files/}.
 * Games install into "Roms/Xtra Games (EXTRAS)"; tools into Tools/ - except
 * where an entry's install.sh targets somewhere else entirely (e.g. an
 * emulator pak installed into Emus/), flagged per-entry via meta.txt's done_msg.
 */

#include <ctype.h>
#include <dirent.h>
#include <netdb.h>
#include <pthread.h> // background latest-release check thread
#include <signal.h>	 // sig_atomic_t for the thread's done flag
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strcasecmp
#include <sys/stat.h> // mkdir (state dir), stat (latest-cache TTL)
#include <sys/statvfs.h>
#include <sys/wait.h> // WIFEXITED/WEXITSTATUS on run_install's pclose() status
#include <time.h>	  // latest-cache TTL comparison
#include <unistd.h>

#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "bos_ui.h"
#include "wget_fetch.h" // latest-release API queries (settings_updater's helper)
#include "ui_accent.h"
#include "ui_buttonhintbar.h"
#include "ui_confirmdialog.h"
#include "ui_downloadprogress.h"
#include "i18n.h"
#include "bos_ui.h"
#include "ui_draw.h" // UI_renderCenteredButtons (draw_result_dialog)
#include "ui_list.h"
#include "ui_listview.h"
#include "ui_menubar.h"
#include "ui_message.h"
#include "ui_splash.h"
#include "utils.h" // escapeSingleQuotes
#include "xtras_compat.h"
#include "xtras_meta.h"

#define MAX_ENTRIES 32

// Moved up from the detail-screen section (still defined there originally
// for wrap_text() below) so both the description block AND Task 13's result
// dialog can size against them - the dialog needs a forward declaration of
// wrap_text() ahead of its own first use, and that declaration's signature
// needs these two visible.
#define DESC_LINE_MAX 160
#define DESC_MAX_LINES 16

// Display name of the Roms subfolder installed GAME entries land in - the
// single source of truth for EXTRAS_ROMS_DIR/EXTRAS_DATA_DIR (env passed to
// install.sh/uninstall.sh below) and for locating each entry's version
// marker/launcher directly. Adjacent-string-literal use (e.g. "%s/Roms/"
// EXTRAS_ROMS_DIRNAME "/...") relies on this expanding to a bare string
// literal, not an expression. Display name renamed per user request
// 2026-08-08 (see spec doc). Task 13b renamed the platform tag itself too
// (ADDON -> EXTRAS: this macro, the env names above, Emus/<plat>/EXTRAS.pak,
// extras_games_launch.sh) as a full-coherence sweep alongside the pak/app
// rename (Add-ons.pak -> Xtras.pak, this app addons.c/addons.elf ->
// extras.c/extras.elf) - the on-disk marker filename (.nx_addon_version,
// see read_installed()/run_entry_script() below) is the one deliberate
// exception, left unrenamed so already-installed markers on a user's SD
// card keep reading as installed across the upgrade (no migration path for
// that one exists, unlike the Roms folder/Emus pak, which do get an
// explicit one-time `mv`/reinstall migration note).
#define EXTRAS_ROMS_DIRNAME "Xtra Games (EXTRAS)"

// Per-entry version state lives OUTSIDE the catalog (update-tracking spec,
// 2026-08-10): install.sh records the release tag it actually installed in
// the xtras state dir's <id>.version, and the background check thread below
// caches each repo's latest release tag in <id>.latest. "Update available"
// is simply installed != latest - the catalog itself no longer pins
// versions.
//
// The state dir is SHARED_USERDATA_PATH "/xtras", resolved via snprintf.
static void xtras_state_dir(char* buf, size_t n) {
	snprintf(buf, n, "%s/xtras", SHARED_USERDATA_PATH);
}
// Latest-release cache freshness window: within it the check thread skips
// the GitHub API entirely (rate limit is 60/hr/IP unauthenticated - ample,
// but re-hitting it on every app open is still pointless).
#define LATEST_CACHE_TTL_S 3600

static AddonEntry entries[MAX_ENTRIES];
static int entry_count = 0;
static SDL_Surface* screen = NULL;
static char pak_dir[MAX_PATH];

// First line of a small state file -> out (newline stripped). false (and an
// empty out) when the file is missing/unreadable/empty.
static bool read_line_file(const char* path, char* out, int out_size) {
	out[0] = '\0';
	FILE* f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(out, out_size, f) != NULL;
	fclose(f);
	if (!ok) {
		out[0] = '\0';
		return false;
	}
	char* nl = strchr(out, '\n');
	if (nl)
		*nl = '\0';
	return out[0] != '\0';
}

// Installed-version record, written by install.sh to the xtras state dir.
// Falls back to the legacy pre-update-tracking marker (.nx_addon_version inside
// the entry's data dir) and migrates the value forward so cards installed
// before the switch keep reading as installed; the legacy file itself is
// left for uninstall.sh to clear (and for an older Xtras.pak to read, if
// the system is ever downgraded).
static void read_installed(AddonEntry* e) {
	char state_dir[MAX_PATH];
	xtras_state_dir(state_dir, sizeof(state_dir));
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s/%s.version", state_dir, e->id);
	if (read_line_file(path, e->installed, sizeof(e->installed)))
		return;

	char legacy[MAX_PATH];
	snprintf(legacy, sizeof(legacy),
			 "%s/Roms/" EXTRAS_ROMS_DIRNAME "/.data/%s/.nx_addon_version",
			 SDCARD_PATH, e->id);
	if (!read_line_file(legacy, e->installed, sizeof(e->installed)))
		return;

	// One-time forward migration; best-effort (a read-only card just retries
	// next launch). SHARED_USERDATA_PATH itself always exists on a booted card.
	mkdir(state_dir, 0755);
	FILE* out = fopen(path, "w");
	if (out) {
		fprintf(out, "%s\n", e->installed);
		fclose(out);
	}
}

// Latest upstream release tag, cached by the background check thread.
static void read_latest(AddonEntry* e) {
	char state_dir[MAX_PATH];
	xtras_state_dir(state_dir, sizeof(state_dir));
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s/%s.latest", state_dir, e->id);
	read_line_file(path, e->latest, sizeof(e->latest));
}

// Tag comparison ignores a leading v/V: upstream tagging drifts between
// "v0.2.4" and "0.2.4" (the v is a convention some releases forget), and a
// spelling-only difference must not read as an update. The guard on [1]
// keeps a literal "v" tag intact.
static const char* tag_norm(const char* tag) {
	return (tag[0] == 'v' || tag[0] == 'V') && tag[1] ? tag + 1 : tag;
}

// The one predicate behind the "Update Available" group, the row badge and
// the detail page's UPDATE action: a tracked entry is updatable once both
// tags are known and differ (plain inequality - no semver parsing, matching
// the spec - but v-prefix-insensitive, see tag_norm).
static bool entry_update_available(const AddonEntry* e) {
	return e->installed[0] && e->latest[0] &&
		   strcmp(tag_norm(e->installed), tag_norm(e->latest)) != 0;
}

// --- background latest-release check --------------------------------------
// One sweep per app open (same worker-thread shape as settings_updater.c's
// download thread): for every repo-tracked entry whose <id>.latest cache is
// stale, fetch the repo's latest-release JSON and rewrite the cache with its
// tag_name. The thread only ever touches the cache FILES - never entries[]
// - and raises latest_check_done when the sweep ends; run_list's loop sees
// the flag, re-reads the caches into entries[] and rebuilds its rows. That
// file-based handoff is the whole synchronization story: no locks, no
// shared mutable state beyond one sig_atomic_t flag.
//
// Offline behaviour: a single getaddrinfo() gate skips the entire sweep
// (and each wget_fetch failure just leaves that entry's cache alone), so
// stale-or-missing info degrades to "no update shown" - it never blocks the
// UI or surfaces an error. The flag is raised on every exit path so nothing
// waits forever.
static volatile sig_atomic_t latest_check_done = 0;

// The thread's private snapshot of each tracked entry's id+repo, filled by
// main() BEFORE the thread starts. entries[] itself is off-limits to the
// thread: run_detail's post-install path calls catalog_load(), which
// rewrites entries[] while a slow sweep may still be running.
static struct {
	char id[64];
	char repo[128];
} check_items[MAX_ENTRIES];
static int check_count = 0;

static void* latest_check_thread(void* arg) {
	(void)arg;
	struct addrinfo* res = NULL;
	struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
	if (getaddrinfo("api.github.com", "443", &hints, &res) != 0 || !res) {
		latest_check_done = 1;
		return NULL;
	}
	freeaddrinfo(res);

	char state_dir[MAX_PATH];
	xtras_state_dir(state_dir, sizeof(state_dir));
	mkdir(state_dir, 0755); // best-effort; parent always exists on a booted card

	time_t now = time(NULL);
	for (int i = 0; i < check_count; i++) {
		char cache[MAX_PATH];
		snprintf(cache, sizeof(cache), "%s/%s.latest", state_dir, check_items[i].id);
		struct stat st;
		if (stat(cache, &st) == 0 && now - st.st_mtime < LATEST_CACHE_TTL_S)
			continue;

		char url[256];
		snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest",
				 check_items[i].repo);
		// 64 KB holds any realistic release JSON (asset lists included);
		// wget_fetch truncates beyond that, which at worst loses trailing
		// assets - tag_name sits at the top of the document.
		static uint8_t buf[64 * 1024];
		int n = wget_fetch(url, buf, sizeof(buf) - 1);
		if (n <= 0)
			continue;
		buf[n] = '\0';

		char* tag = strstr((char*)buf, "\"tag_name\"");
		if (!tag)
			continue;
		tag = strchr(tag + 10, '"'); // opening quote of the value
		if (!tag)
			continue;
		tag++;
		char* end = strchr(tag, '"');
		if (!end || end == tag || end - tag >= 64)
			continue;

		FILE* f = fopen(cache, "w");
		if (f) {
			fprintf(f, "%.*s\n", (int)(end - tag), tag);
			fclose(f);
		}
	}
	latest_check_done = 1;
	return NULL;
}

static void catalog_load(void) {
	entry_count = 0;
	char dir[MAX_PATH];
	snprintf(dir, sizeof(dir), "%s/catalog", pak_dir);
	DIR* d = opendir(dir);
	if (!d)
		return;
	struct dirent* de;
	while ((de = readdir(d)) && entry_count < MAX_ENTRIES) {
		if (de->d_name[0] == '.')
			continue;
		char meta[MAX_PATH];
		snprintf(meta, sizeof(meta), "%s/%s/meta.txt", dir, de->d_name);
		AddonEntry* e = &entries[entry_count];
		memset(e, 0, sizeof(*e));
		snprintf(e->id, sizeof(e->id), "%s", de->d_name);
		if (!xtras_meta_parse(meta, e))
			continue;
		e->compatible = xtras_platform_compatible(e->platforms, PLATFORM);
		// Resolve version_source default: explicit internal wins; else an entry
		// with a repo is external; else (no repo) internal.
		if (!e->version_internal && e->repo[0] == '\0')
			e->version_internal = true;

		read_installed(e);
		if (e->version_internal)
			// Internal entries have no network "latest": our catalog version IS
			// the latest. Reusing e->latest keeps entry_update_available and the
			// list grouping unchanged.
			snprintf(e->latest, sizeof(e->latest), "%s", e->version);
		else
			read_latest(e);
		entry_count++;
	}
	closedir(d);
	qsort(entries, entry_count, sizeof(entries[0]), xtras_entry_cmp);
}

// --- list screen --------------------------------------------------------

// Task 12: GAMES/TOOLS tabs (D-pad LEFT/RIGHT) replace Task 8's inline per-category
// headers as the top-level split. TAB_CATEGORY ties each tab back to
// AddonEntry.category ("GAME"/"TOOL" - the only two values the catalog
// contract defines; see AddonEntry's own comment above), TAB_LABEL is what
// gets drawn on the tab pill itself.
typedef enum { TAB_GAMES,
			   TAB_TOOLS,
			   TAB_COUNT } AddonTab;
static const char* const TAB_CATEGORY[TAB_COUNT] = {"GAME", "TOOL"};
static const char* const TAB_LABEL[TAB_COUNT] = {"GAMES", "TOOLS"};

// One tab's rows, in display order: every not-installed entry first (own
// group, no header), then installed entries with a newer upstream release
// (the "Update Available" header), then the rest of the installed entries
// (the "Installed" header) - headers are drawn separately by the renderer
// when it reaches each group's start index. Rebuilt on tab switch, after
// any install/uninstall, and when the background latest-check completes, so
// it always reflects current installed/latest state - entries[] itself is
// never reordered by this, only xtras_entry_cmp() (via catalog_load's qsort,
// unchanged) still governs category+name order within each group.
typedef struct {
	int indices[MAX_ENTRIES]; // entries[] index for each row, in display order
	int count;				  // total rows in this tab
	int update_start;		  // indices[update_start..installed_start) is the "Update Available" group
	int installed_start;	  // indices[installed_start..count) is the "Installed" group; == count if none installed
} TabRows;

static void build_tab_rows(AddonTab tab, TabRows* rows) {
	rows->count = 0;
	for (int i = 0; i < entry_count; i++)
		if (!strcmp(entries[i].category, TAB_CATEGORY[tab]) && !entries[i].installed[0])
			rows->indices[rows->count++] = i;
	rows->update_start = rows->count;
	for (int i = 0; i < entry_count; i++)
		if (!strcmp(entries[i].category, TAB_CATEGORY[tab]) && entry_update_available(&entries[i]))
			rows->indices[rows->count++] = i;
	rows->installed_start = rows->count;
	for (int i = 0; i < entry_count; i++)
		if (!strcmp(entries[i].category, TAB_CATEGORY[tab]) && entries[i].installed[0] && !entry_update_available(&entries[i]))
			rows->indices[rows->count++] = i;
}

// Shared-ListView row model (Task 17): the widget's rows are the TabRows
// entries PLUS synthesized section-header rows, interleaved in display
// order:
//   [not-installed...] ["Update Available"] [update...] ["Installed"] [installed...]
// A header only exists when its group is non-empty (update_start ==
// installed_start when nothing is updatable; installed_start == count when
// nothing is current). The widget's `selected` and every ListViewAction
// index are WIDGET-row indices - convert with extras_widget_to_entry /
// extras_entry_to_widget at the A-press and every relocate-after-regroup
// site; headers themselves are never selectable (the widget skips them).
static int extras_h1(const TabRows* r) { // header before the update group?
	return r->update_start < r->installed_start ? 1 : 0;
}
static int extras_h2(const TabRows* r) { // header before the installed group?
	return r->installed_start < r->count ? 1 : 0;
}
static int extras_widget_count(const TabRows* r) {
	return r->count + extras_h1(r) + extras_h2(r);
}
static int extras_h1_pos(const TabRows* r) {
	return r->update_start;
}
static int extras_h2_pos(const TabRows* r) {
	return r->installed_start + extras_h1(r);
}
// widget row -> TabRows entry index (-1 for a header row)
static int extras_widget_to_entry(const TabRows* r, int wi) {
	if (extras_h1(r) && wi == extras_h1_pos(r))
		return -1;
	if (extras_h2(r) && wi == extras_h2_pos(r))
		return -1;
	int ei = wi;
	if (extras_h1(r) && wi > extras_h1_pos(r))
		ei--;
	if (extras_h2(r) && wi > extras_h2_pos(r))
		ei--;
	return ei;
}
// TabRows entry index -> widget row
static int extras_entry_to_widget(const TabRows* r, int ei) {
	int wi = ei;
	if (extras_h1(r) && ei >= r->update_start)
		wi++;
	if (extras_h2(r) && ei >= r->installed_start)
		wi++;
	return wi;
}

typedef struct {
	const TabRows* rows;
} ExtrasListCtx;
static ExtrasListCtx extras_ctx;
static ListView extras_view;

// No name suffixes or badges: the group an entry sits in ("Update
// Available"/"Installed" section headers) already says everything the old
// right-aligned "update" marker said (user feedback 2026-08-10 - the marker
// was redundant once the group existed).
static void extras_get_row(void* ctx, int i, bool selected, ListViewRow* out) {
	(void)selected;
	const TabRows* r = ((ExtrasListCtx*)ctx)->rows;
	if (extras_h1(r) && i == extras_h1_pos(r)) {
		out->is_header = true;
		out->label = "Update Available";
		return;
	}
	if (extras_h2(r) && i == extras_h2_pos(r)) {
		out->is_header = true;
		out->label = "Installed";
		return;
	}
	AddonEntry* e = &entries[r->indices[extras_widget_to_entry(r, i)]];
	out->label = e->name;
	if (!e->compatible)
		out->annotation = "Not available";
}

// Tab bar/menu bar chrome stays app-drawn (title=NULL keeps the widget out
// of the menu bar); the list band starts where the tab bar ends, passed via
// list_y_override. list_id = TAB_LABEL[active_tab] gives each tab a static
// identity, so a tab switch snaps the glide/marquee inside the widget - the
// old glide_last_tab hack is gone, as is the bespoke adjacent-step-across-
// header pre-target (the widget's internal pre-target handles it, including
// the index-distance-2 adjacency headers-as-rows creates, via its
// adjacent_selectable walk).
static uint32_t xt_colour(const char* t) { // one colour per name, the kit's palette
	static const uint32_t pal[] = {0x0A84FF, 0x30D158, 0xBF5AF2, 0xFF453A, 0xFF9F0A, 0x5E5CE6, 0x30B0C7, 0xFF375F};
	unsigned h = 5381;
	for (const char* p = t ? t : ""; *p; p++)
		h = h * 33 + (unsigned char)*p;
	return pal[h % 8];
}
static void xt_row(void* ctx, int i, bool hot, ListViewRow* out) {
	extras_get_row(ctx, i, hot, out);
	if (out->is_header) {
		out->label = I18N_text(out->label);
		return;
	}
	const TabRows* r = ((ExtrasListCtx*)ctx)->rows;
	AddonEntry* e = &entries[r->indices[extras_widget_to_entry(r, i)]];
	static char sb[96];
	snprintf(sb, sizeof(sb), "%s · %d MB", I18N_text(strcmp(e->category, "TOOL") ? "Game" : "Tool"), e->size_mb);
	out->sub = sb;
	out->sq_text = e->name;
	out->sq_color = xt_colour(e->name);
	out->annotation = NULL;
	if (!e->compatible)
		out->annotation = I18N_text("Not available");
	else if (entry_update_available(e))
		out->pill = I18N_text("Update"), out->pill_bg = BOS_C_WARN, out->pill_fg = 0x000000;
	else if (e->installed[0])
		out->pill = I18N_text("Installed"), out->pill_bg = BOS_C_OK, out->pill_fg = 0x000000;
	else
		out->annotation = I18N_text("Not installed");
}

static void render_extras_list(SDL_Surface* screen, AddonTab active_tab, const TabRows* rows) {
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("Xtras"));
	int top = BOS_px(BOS_BAR_H), gx = BOS_px(40), gw = screen->w - 2 * gx;
	static const char* tabs[] = {"Games", "Tools"};
	int y = top + BOS_px(8);
	y += BOS_chips(screen, gx, y, tabs, 2, (int)active_tab);
	extras_ctx.rows = rows;
	extras_view.count = extras_widget_count(rows);
	extras_view.get_row = extras_get_row;
	extras_view.ctx = &extras_ctx;
	int bottom = screen->h - BOS_px(BOS_BAR_H) - BOS_px(12);
	if (rows->count) {
		BOS_groupRows(screen, &(SDL_Rect){gx, y, gw, bottom - y}, BOS_px(BOS_ROW_H), extras_view.count, extras_view.selected,
					  true, &extras_view.scroll, &extras_view.bos_pill, &extras_view.bos_scroll, false, xt_row, &extras_ctx);
		extras_view.last_visible = (bottom - y) / BOS_px(BOS_ROW_H);
	} else
		BOS_resultCard(screen, "store", BOS_theme()->surface2, "Nothing here yet", NULL);
	extras_view.input_pending = false;
	BosHint h[] = {{"A", I18N_text("Details")}, {"B", I18N_text("Exit")}};
	BosHint l[] = {{"L1", ""}, {"R1", I18N_text("Tab")}};
	if (rows->count)
		BOS_hintBar(screen, h, 2, l, 2);
	else
		BOS_hintBar(screen, h + 1, 1, l, 2);
}

static int run_detail(AddonEntry* e); // Task 4; returns 1 if install state changed

// GFX_startFrame/PWR_update/UI_statusBarChanged and the shared ListView's
// UI_listViewHandleInput (rather than hand-rolled BTN_UP/BTN_DOWN math)
// match every other migrated pak-tool loop in this tree — PWR_update still
// has to run every frame so the power button and battery/clock status bar
// work here same as anywhere else, even though sleep/autosleep/power-off
// themselves are held off for the whole session.
//
// The widget owns the selection (extras_view.selected), and it is a
// WIDGET-row index scoped to the ACTIVE tab's rows-plus-headers - not a
// global entries[] index - so every read/write below goes through
// extras_widget_to_entry/extras_entry_to_widget. Switching tabs (D-pad
// LEFT/RIGHT) resets it via UI_listViewReset (selected=0 may land on a
// header; the widget clamps to the first selectable row, i.e. the first
// entry - same as the old selected=0-of-entries) rather than trying to carry
// a position across two differently-ordered/differently-sized row lists;
// exactly two tabs makes wrap and clamp the same thing. The switch moved
// shoulder buttons -> d-pad LEFT/RIGHT
// (user request 2026-08-18: first added alongside L1/R1, then L1/R1
// retired the same day) - the ListView's LEFT/RIGHT page-jump is opted out
// via no_lr_paging so one press never both flips the tab and pages the
// fresh list; both tabs are short and UP/DOWN wraps, so page-jump loses
// nothing. Task 13 added the tab hint (user feedback: the switch wasn't
// discoverable) - one combined button-pill hint ("LEFT/RIGHT" as the
// button label, GFX_blitButton renders any >1-char button string as a text
// pill rather than two separate circular buttons; same label the bootlogo/
// musicplayer settings bars use for d-pad hints), since
// UI_renderButtonHintBar caps at 4 total pairs and doesn't clip pixel
// overflow past dst->w on its own. The Task 13 width math measured the
// three-pair bar at ~577px on the Brick's 1024px bar / ~387px on tg5050's
// 1280px with the "L1/R1" label; "LEFT/RIGHT" adds ~5 glyphs' width -
// still comfortable margin on both.
static void run_list(void) {
	AddonTab active_tab = TAB_GAMES;
	TabRows rows;
	build_tab_rows(active_tab, &rows);
	// Wire the provider before the first HandleInput (which runs before the
	// first render this frame) so header-skip probes see real rows.
	extras_ctx.rows = &rows;
	extras_view.get_row = extras_get_row;
	extras_view.ctx = &extras_ctx;
	// D-pad LEFT/RIGHT is claimed for the tab switch below, so the
	// widget's LEFT/RIGHT page-jump must not also fire on the same press.
	// Both tabs are short lists; UP/DOWN wrap covers traversal.
	extras_view.no_lr_paging = true;
	UI_listViewReset(&extras_view, extras_widget_count(&rows), TAB_LABEL[active_tab]);
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;
	while (1) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_LEFT) || PAD_justPressed(BTN_RIGHT) || PAD_justPressed(BTN_L1) || PAD_justPressed(BTN_R1)) {
			active_tab = (active_tab == TAB_GAMES) ? TAB_TOOLS : TAB_GAMES;
			build_tab_rows(active_tab, &rows);
			UI_listViewReset(&extras_view, extras_widget_count(&rows), TAB_LABEL[active_tab]);
			dirty = true;
		}
		// Background latest-check finished: pull the fresh caches into
		// entries[] and regroup - rows may shuffle into/out of the "Update
		// Available" group, so keep the selection at the same (clamped)
		// entry position rather than letting it dangle past the
		// (unchanged-length, but regrouped) list. Reset-then-assign: the
		// widget count may change even when rows.count doesn't (headers
		// appear/disappear with their groups).
		if (latest_check_done) {
			latest_check_done = 0;
			int sel_entry = extras_widget_to_entry(&rows, extras_view.selected);
			// Internal entries keep their catalog version: a .latest cache
			// left from when one was repo-tracked (PortMaster) is stale.
			for (int i = 0; i < entry_count; i++)
				if (!entries[i].version_internal)
					read_latest(&entries[i]);
			build_tab_rows(active_tab, &rows);
			if (sel_entry < 0)
				sel_entry = 0;
			if (sel_entry >= rows.count)
				sel_entry = rows.count ? rows.count - 1 : 0;
			UI_listViewReset(&extras_view, extras_widget_count(&rows), TAB_LABEL[active_tab]);
			if (rows.count)
				extras_view.selected = extras_entry_to_widget(&rows, sel_entry);
			dirty = true;
		}
		ListViewAction act = UI_listViewHandleInput(&extras_view);
		if (act.type == LISTVIEW_BACK) { // B: back to the first tab, then ask before leaving (DESIGN 7.7)
			if (active_tab != TAB_GAMES) {
				active_tab = TAB_GAMES;
				build_tab_rows(active_tab, &rows);
				UI_listViewReset(&extras_view, extras_widget_count(&rows), TAB_LABEL[active_tab]);
			} else {
				GFX_clearLayers(LAYER_SCROLLTEXT);
				if (BOS_askQuit(screen, "Xtras"))
					break;
			}
			act.type = LISTVIEW_NONE;
			dirty = true;
		}
		if (act.type == LISTVIEW_ACTIVATED) {
			// act.index is a widget row; ACTIVATED is never a header row.
			AddonEntry* e = &entries[rows.indices[extras_widget_to_entry(&rows, act.index)]];
			// A long entry name's marquee band (LAYER_SCROLLTEXT) must not
			// bleed over the detail screen.
			GFX_clearLayers(LAYER_SCROLLTEXT);
			if (run_detail(e)) {
				// The entry just acted on may move from the not-installed
				// group to Installed (or back) once rows are rebuilt below
				// - re-locate it by id so the selection stays on the same
				// entry instead of landing on whatever unrelated row now
				// occupies the same index in the regrouped list.
				// Reset-then-assign, as everywhere a regroup moves rows.
				char id[64];
				snprintf(id, sizeof(id), "%s", e->id);
				catalog_load();
				build_tab_rows(active_tab, &rows);
				int found = 0;
				for (int i = 0; i < rows.count; i++) {
					if (!strcmp(entries[rows.indices[i]].id, id)) {
						found = i;
						break;
					}
				}
				UI_listViewReset(&extras_view, extras_widget_count(&rows), TAB_LABEL[active_tab]);
				if (rows.count)
					extras_view.selected = extras_entry_to_widget(&rows, found);
			}
			dirty = true;
		}
		if (UI_statusBarChanged())
			dirty = true;
		if (UI_listViewBusy(&extras_view))
			dirty = true;
		PWR_update(&dirty, &show_setting, NULL, NULL);
		if (dirty) {
			GFX_clear(screen);
			render_extras_list(screen, active_tab, &rows);
			GFX_flip(screen);
			dirty = false;
		} else {
			UI_listViewTickIdle(&extras_view);
			GFX_sync();
		}
	}
}

// --- pre-flight -----------------------------------------------------------
// Checked right before A on the detail screen: install.sh assumes both a
// working internet connection and enough free space, and fails ugly (partway
// through a download, or mid-extract) when either is missing. Catch the
// common cases up front with a friendly message instead.
static const char* preflight(AddonEntry* e) {
	if (!e->version_internal) {
		struct addrinfo* res = NULL;
		struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
		if (getaddrinfo("github.com", "443", &hints, &res) != 0 || !res)
			return "No network - connect WiFi first";
		freeaddrinfo(res);
	}

	struct statvfs vfs;
	if (statvfs(SDCARD_PATH, &vfs) == 0) {
		long free_mb = (long)((unsigned long long)vfs.f_bavail * vfs.f_frsize / (1024 * 1024));
		if (free_mb < e->size_mb * 2)
			return "Not enough free space on SD card";
	}
	return NULL;
}

// Terminal message: GFX_clear + UI_renderCenteredMessage + GFX_flip + a fixed
// hold via SDL_Delay.
#define EXTRAS_MESSAGE_MS 2000

// --- entry-script runner: popen install.sh/uninstall.sh, stream lines -----
// Task 11: the in-flight screen is the shared UIDownloadProgress bar
// (ui_downloadprogress.h - same component settings_updater.c/scraper.c
// use for their own downloads) instead of a scrolling log tail. Task 13
// dropped the terminal log-tail-list screen too (see draw_result_dialog
// below) - the tail buffer shrinks from a TAIL_LINES ring to a single
// `error_line` slot (display text only, hints stripped): the LAST line seen
// whose text starts with "ERROR: " (install.sh/uninstall.sh's `fail()`
// convention - see its own comment there).
//
// Round-1 review, two findings on the naive "just keep the last line seen"
// version of this:
// - Critical: `fail()` can print a line AFTER its own "ERROR: ..." (e.g.
//   "install broken, launcher removed - re-run install" once TARGET_DIRTY +
//   a registered launcher - scripts/tests/test-extras-gen1recomp-install.sh's
//   scenario 4 exercises exactly this), so "the last line" is sometimes that
//   retry notice, not the actual reason. Fixed by matching the "ERROR: "
//   PREFIX specifically (whichever line last had it), independent of
//   whatever prints after.
// - Important: a script killed by signal, or one that exits non-zero
//   without ever printing an "ERROR: " line, has no such line to show at
//   all - falling back to "whatever the last line happened to be" would
//   then show an ordinary progress/status line (e.g. "Extracting game...")
//   as if it explained the failure, which it doesn't. So there is
//   deliberately NO "last line of any kind" fallback buffer here anymore -
//   see draw_result_dialog's caller below, which uses a fixed generic
//   sentence instead whenever `error_line` is empty.
// Full fidelity (every line, hints included) still lands in
// $LOGS_PATH/Xtras.txt regardless, via the unconditional stderr tee below
// - this buffer is purely for the on-screen failure subtitle.
#define TAIL_LINE_MAX 96
// Convention shared with install.sh/uninstall.sh's fail() helper (their own
// header comment): a failure's real explanation is always its own line
// starting with exactly this prefix, even if fail() goes on to print more
// after it.
#define ERROR_LINE_PREFIX "ERROR: "

// wrap_text() is defined later, in the detail-screen section below - this
// forward declaration (same pattern as run_detail's own above) lets
// draw_result_dialog() reuse it here rather than duplicating a third
// pixel-measured wrap implementation next to this one and
// ui_confirmdialog.c's file-local confirm_wrap_lines(). DESC_LINE_MAX/
// DESC_MAX_LINES (this signature's array dimensions) now live in the top
// macros section for the same reason.
static int wrap_text(TTF_Font* f, const char* text, int max_width,
					 char lines[DESC_MAX_LINES][DESC_LINE_MAX], int max_lines);

#define RESULT_SUB_MAX_LINES 3

// Terminal result dialog (Task 13, user feedback round 3): replaces the old
// log-tail list screen with the same centered-title/subtitle/hint-buttons
// visual pattern ui_confirmdialog.h's UI_renderConfirmDialog uses - but not
// that component itself, since it hardcodes a CANCEL/CONFIRM button pair
// with no single-button "OK" mode, and adding one for this single caller
// wasn't worth a shared-component change. `subtitle` is centered, word-
// wrapped (via wrap_text, font.small - same treatment
// UI_renderConfirmDialog gives its own subtitle, just reusing this file's
// own wrapper instead of that file's private one); `detail` (optional -
// NULL/empty to omit) renders as a second, dimmer, smaller single-line
// block below it - used for the "full log is at ..." pointer on failure.
static void draw_result_dialog(const char* title, const char* subtitle, const char* detail) {
	(void)detail; // the log's path stays in the log
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("Xtras"));
	bool bad = title && (strstr(title, "failed") || strstr(title, "rror"));
	BOS_resultCard(screen, bad ? "warn" : "check", bad ? BOS_C_DANGER : BOS_C_OK, title, subtitle);
	BosHint h[] = {{"B", I18N_text("OK")}};
	BOS_hintBar(screen, h, 1, NULL, 0);
	GFX_flip(screen);
}

// In-flight progress screen: menu bar carries the entry's name (continuity
// with the detail page this was launched from), UIDownloadProgress carries
// the moving parts. show_bar is always true here - install.sh/uninstall.sh
// always emit at least one @NN hint (see xtras_parse_hint in xtras_meta.c) before this is
// first called, per their own contract comment.
static void draw_progress_screen(const char* menu_title, const char* status,
								 const char* detail, int progress) {
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("Xtras"));
	UI_renderDownloadProgress(screen, &(UIDownloadProgress){.status = status, .sub = menu_title, .detail = detail,
															 .progress = progress, .show_bar = true, .icon = "download"});
	BOS_hintBar(screen, NULL, 0, NULL, 0);
	GFX_flip(screen);
}

// install.sh/uninstall.sh's shared env contract (Task 1, extended Task 9):
// PLATFORM and SDCARD_PATH are already C-level build/platform constants
// (defines.h / -DPLATFORM), so they're used directly. LOGS_PATH has no such
// constant - it only exists as a shell var exported by MinUI.pak's
// launch.sh and inherited down the process tree that eventually execs
// extras.elf - so it's read at runtime via getenv(), with a fallback built
// the same way launch.sh derives it (USERDATA_PATH/logs) in case extras.elf
// is ever launched with a bare environment.
//
// `title` is a verb stem ("Install"/"Uninstall") shared by both callers
// below; every on-screen string is built from it by plain concatenation -
// "Install"+"ing..." == "Installing..." (progress screen), "Install"+"ed"
// == "Installed" (Task 13 result-dialog title on success), "Install"+"
// failed" == "Install failed" (result-dialog title on failure) - so this
// one function produces every wording variant for either script, without
// duplicating the popen/streaming/result-dialog logic itself.
//
// The fgets() loop below blocks on the child's stdout with no watchdog of
// its own; its boundedness is entirely the script's own responsibility per
// its contract comment - every network command it runs must carry a
// timeout, or a stalled/half-open connection hangs this whole UI (no
// redraw, no power button) for as long as the connection stays open.
// uninstall.sh's contract requires no network access at all, so in practice
// this only bites install.sh.
static int run_entry_script(AddonEntry* e, const char* script_name, const char* title) {
	const char* logs_path = getenv("LOGS_PATH");
	// "<USERDATA_PATH>/logs", resolved into a local buffer via snprintf.
	char logs_path_buf[MAX_PATH];
	if (!logs_path) {
		snprintf(logs_path_buf, sizeof(logs_path_buf), "%s/logs", USERDATA_PATH);
		logs_path = logs_path_buf;
	}

	char state_dir[MAX_PATH];
	xtras_state_dir(state_dir, sizeof(state_dir));

	// Escape the catalog id before it goes into the single-quoted CATALOG_DIR
	// and sh-path arguments below (worst case each quote expands to 4 chars).
	char id_esc[sizeof(e->id) * 4];
	snprintf(id_esc, sizeof(id_esc), "%s", e->id);
	escapeSingleQuotes(id_esc, sizeof(id_esc));

	// EXTRAS_DATA_DIR is the per-entry payload root for native standalone games
	// (e.g. gen1recomp's LOVE engine + saves live in .data/<id>/). Named .data,
	// not .ports: native entries own their whole runtime and don't touch
	// PortMaster. A PortMaster-dependent extra is expected to install into the
	// normal Roms/Ports (PORTS) tree from its own install.sh (as a TOOL entry
	// may install outside Tools/), so "Xtra Games (EXTRAS)" stays native-only.
	char cmd[MAX_PATH * 4];
	snprintf(cmd, sizeof(cmd),
			 "PLATFORM='%s' SDCARD_PATH='%s' LOGS_PATH='%s' "
			 "EXTRAS_ROMS_DIR='%s/Roms/" EXTRAS_ROMS_DIRNAME "' "
			 "EXTRAS_DATA_DIR='%s/Roms/" EXTRAS_ROMS_DIRNAME "/.data' "
			 "XTRAS_STATE_DIR='%s' "
			 "CATALOG_DIR='%s/catalog/%s' "
			 "sh '%s/catalog/%s/%s' 2>&1",
			 PLATFORM, SDCARD_PATH, logs_path,
			 SDCARD_PATH, SDCARD_PATH, state_dir, pak_dir, id_esc, pak_dir, id_esc, script_name);

	FILE* p = popen(cmd, "r");
	if (!p)
		return -1;

	char progress_title[32];
	snprintf(progress_title, sizeof(progress_title), "%sing...", title);

	// status/detail/progress: fed by xtras_parse_hint(), this is the exact
	// state UI_renderDownloadProgress needs each frame. status starts as
	// progress_title so the bar reads something sane for the brief instant
	// before the script's first @NN hint line arrives.
	char status[64];
	snprintf(status, sizeof(status), "%s", progress_title);
	char detail[TAIL_LINE_MAX] = "";
	int progress = 0;

	char error_line[TAIL_LINE_MAX] = "";
	char buf[512];
	while (fgets(buf, sizeof(buf), p)) {
		char* nl = strchr(buf, '\n');
		if (nl)
			*nl = '\0';
		// The child's stdout is only this popen() pipe (consumed line-by-line
		// below, never touches a file) - the pak's launch.sh redirects
		// extras.elf's own stderr into $LOGS_PATH/Xtras.txt, so mirroring
		// every line there is what actually gives the full run a persistent
		// record on disk, not just the on-screen tail. The RAW line (hint
		// prefix included, if any) is tee'd unchanged - the on-disk log
		// keeps full fidelity even though the on-screen UI below strips it.
		fprintf(stderr, "%s\n", buf);
		if (!buf[0])
			continue;

		int pct;
		const char *status_text, *stripped;
		const char* display; // hint-stripped text, for error_line below
		if (xtras_parse_hint(buf, &pct, &status_text, &stripped)) {
			progress = pct;
			snprintf(status, sizeof(status), "%s", status_text);
			display = status_text;
		} else if (stripped) {
			// Malformed but digit-leading "@..." syntax: never show the raw
			// hint token - route whatever's left after stripping it (often
			// nothing) to the detail line like ordinary output, or drop the
			// line entirely if nothing's left.
			if (!stripped[0])
				continue;
			snprintf(detail, sizeof(detail), "%s", stripped);
			display = stripped;
		} else {
			snprintf(detail, sizeof(detail), "%s", buf);
			display = buf;
		}

		// The LAST "ERROR: " line wins if the script ever prints more than
		// one (fail() calls are meant to be terminal, but nothing stops a
		// script from calling it more than once on unrelated paths) - see
		// TAIL_LINE_MAX's own comment for why this can't just be "the last
		// line of any kind".
		if (!strncmp(display, ERROR_LINE_PREFIX, sizeof(ERROR_LINE_PREFIX) - 1))
			snprintf(error_line, sizeof(error_line), "%s", display);
		// Non-interactive: B is disabled while the script runs (nothing reads
		// PAD state here), but PAD_poll() still runs every rendered frame so
		// the input core doesn't go stale across a long popen read - same as
		// every other blocking flow in this tree.
		PAD_poll();
		draw_progress_screen(e->name, status, detail, progress);
	}
	int wstatus = pclose(p);
	int code = (wstatus >= 0 && WIFEXITED(wstatus)) ? WEXITSTATUS(wstatus) : -1;

	// Task 13: single dismiss button now says the whole story (title +
	// subtitle), so "- press B"/" FAILED - press B" no longer belong baked
	// into the title text itself - draw_result_dialog's own "B OK" hint
	// says that part.
	if (code == 0) {
		char ok_title[24];
		snprintf(ok_title, sizeof(ok_title), "%sed", title); // "Installed"/"Uninstalled"
		char subtitle[128];
		if (!strcmp(title, "Install") && e->done_msg[0])
			// Entry-provided override (meta.txt done_msg) for payloads that
			// don't land in the category's default folder - see the struct
			// field's own comment.
			snprintf(subtitle, sizeof(subtitle), "%s", e->done_msg);
		else if (!strcmp(title, "Install"))
			// GAME entries land in EXTRAS_ROMS_DIRNAME ("Xtra Games (EXTRAS)",
			// shown to the user without the "(EXTRAS)" tag suffix - see that
			// macro's own comment); TOOL entries land in Tools/ instead
			// (this file's own header comment). Both are the friendly,
			// user-facing folder name, not a literal path.
			snprintf(subtitle, sizeof(subtitle), "Find it in %s.",
					 !strcmp(e->category, "TOOL") ? "Tools" : "Xtra Games");
		else
			snprintf(subtitle, sizeof(subtitle), "Saves and ROMs kept.");
		draw_result_dialog(ok_title, subtitle, NULL);
	} else {
		char fail_title[24];
		snprintf(fail_title, sizeof(fail_title), "%s failed", title); // "Install failed"/"Uninstall failed"
		char log_detail[128];
		snprintf(log_detail, sizeof(log_detail), "Full log: %s/Xtras.txt", logs_path);
		// error_line (the last "ERROR: "-prefixed line, tracked above) is
		// the real reason whenever the script's own fail()-style convention
		// fired, no matter what fail() went on to print afterward. If no
		// such line was ever seen - the process died to a signal
		// (WIFEXITED false above) or exited non-zero without explaining
		// itself - a generic sentence is shown instead of guessing from
		// whatever ordinary output happened to arrive last (round-1 review
		// Important: that would misrepresent a progress line as the
		// reason). Either way the dim log-path line still points at the
		// full record, so nothing is actually lost, just not guessed at.
		const char* reason;
		if (error_line[0])
			reason = error_line;
		else if (!strcmp(title, "Install"))
			reason = "The installer stopped unexpectedly.";
		else
			reason = "The uninstaller stopped unexpectedly.";
		draw_result_dialog(fail_title, reason, log_detail);
	}
	while (1) {
		PAD_poll();
		if (PAD_justPressed(BTN_B))
			break;
		GFX_sync();
	}
	PAD_poll();
	PAD_reset(); // swallow the B before the caller's back-check
	return code;
}

static int run_install(AddonEntry* e) {
	return run_entry_script(e, "install.sh", "Install");
}

// Uninstall fallback for an entry that ships no uninstall.sh (catalog
// contract: "removes only the launcher + marker (safest possible)").
// Never touches anything under EXTRAS_DATA_DIR/<id>/ beyond the marker
// itself, so any user data such an entry might have written there is left
// completely alone. The installed launcher name(s) are derived from
// install.sh's own convention rather than hardcoded here: install.sh cp's
// each file under catalog/<id>/files/ into EXTRAS_ROMS_DIR under the SAME
// basename (see gen1recomp/install.sh's LAST STEP), so listing that
// directory recovers exactly what got registered without this app needing
// its own separate record of it. Best-effort/idempotent like uninstall.sh
// itself: remove() on an already-missing path is silently ignored, and this
// always reports success (nothing here can meaningfully fail in a way the
// user needs to know about).
static int uninstall_generic(AddonEntry* e) {
	char files_dir[MAX_PATH];
	snprintf(files_dir, sizeof(files_dir), "%s/catalog/%s/files", pak_dir, e->id);
	DIR* d = opendir(files_dir);
	if (d) {
		struct dirent* de;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			char launcher[MAX_PATH];
			snprintf(launcher, sizeof(launcher), "%s/Roms/" EXTRAS_ROMS_DIRNAME "/%s",
					 SDCARD_PATH, de->d_name);
			remove(launcher);
		}
		closedir(d);
	}

	char marker[MAX_PATH];
	snprintf(marker, sizeof(marker),
			 "%s/Roms/" EXTRAS_ROMS_DIRNAME "/.data/%s/.nx_addon_version",
			 SDCARD_PATH, e->id);
	remove(marker);

	// The update-tracking installed-version record (the legacy marker above
	// is its pre-migration location; both must go or the entry reads as
	// still installed).
	char state_dir[MAX_PATH];
	xtras_state_dir(state_dir, sizeof(state_dir));
	snprintf(marker, sizeof(marker), "%s/%s.version", state_dir, e->id);
	remove(marker);

	UI_showMessage(screen, "Saves and ROMs kept.", EXTRAS_MESSAGE_MS);
	return 0;
}

// X on the detail screen (Task 9): run the entry's own uninstall.sh if it
// ships one (streamed the same way install.sh is), else fall back to the
// generic launcher+marker removal above.
static int run_uninstall(AddonEntry* e) {
	char script[MAX_PATH];
	snprintf(script, sizeof(script), "%s/catalog/%s/uninstall.sh", pak_dir, e->id);
	if (access(script, F_OK) == 0)
		return run_entry_script(e, "uninstall.sh", "Uninstall");
	return uninstall_generic(e);
}

// X on an installed entry: confirm before uninstalling. UI_confirmModal's
// trailing PAD_poll()+PAD_reset() (reset_pad=true) keeps the confirming A or
// cancelling B press from leaking into run_detail's own PAD_justPressed
// checks next frame - same precedent/shape as settings.c's run_confirm_dialog
// (a different translation unit, so the identical name doesn't clash).
static bool run_confirm_dialog(const char* title, const char* subtitle) {
	return UI_confirmModal(screen, title, subtitle, NULL, false, true);
}

// --- detail screen ----------------------------------------------------

// Pixel-measured word-wrap for the description block below: each candidate
// line is measured with the real TTF font (TTF_SizeUTF8), never a fixed
// character count, because the two device widths (1024 tg5040 / 1280
// tg5050) fit a different number of glyphs at the same point size. A word
// wider than max_width all by itself is hard-broken character by character
// instead of being left to overflow the screen edge.
//
// Not api.c's GFX_wrapText/GFX_blitWrappedText: neither hard-breaks an
// over-width word, and blitWrappedText only center-aligns - this page is
// left-aligned under the title/metadata lines above it.
//
// Returns the number of lines written to `lines` (0 for an empty desc). If
// there's more text than max_lines holds, the last line is shortened and
// "..." appended so the truncation stays legible and still fits max_width.
static int wrap_text(TTF_Font* f, const char* text, int max_width,
					 char lines[DESC_MAX_LINES][DESC_LINE_MAX], int max_lines) {
	int n = 0;
	const char* p = text;
	while (*p == ' ')
		p++;

	while (*p && n < max_lines) {
		char cur[DESC_LINE_MAX] = "";
		bool has_word = false;
		bool hard_broke = false;

		while (*p) {
			const char* word_start = p;
			const char* q = p;
			while (*q && *q != ' ')
				q++;
			int word_len = (int)(q - word_start);
			if (word_len >= DESC_LINE_MAX)
				word_len = DESC_LINE_MAX - 1;
			char word[DESC_LINE_MAX];
			memcpy(word, word_start, word_len);
			word[word_len] = '\0';

			char trial[DESC_LINE_MAX];
			if (has_word)
				snprintf(trial, sizeof(trial), "%s %s", cur, word);
			else
				snprintf(trial, sizeof(trial), "%s", word);

			int tw, th;
			GFX_measureText(f, trial, &tw, &th);
			if (tw <= max_width) {
				snprintf(cur, sizeof(cur), "%s", trial);
				has_word = true;
				p = q;
				while (*p == ' ')
					p++;
				continue;
			}

			if (has_word) {
				// Doesn't fit after what's already on this line - leave it
				// (word_start unconsumed) for the next line to retry.
				p = word_start;
				break;
			}

			// Empty line and the word alone still doesn't fit: hard-break
			// it across as many lines as it takes.
			int lo = 0;
			while (word[lo] && n < max_lines) {
				int hi = lo + 1, last_fit = lo + 1;
				while (word[hi]) {
					int plen = hi - lo;
					if (plen >= DESC_LINE_MAX - 1)
						break;
					char piece[DESC_LINE_MAX];
					memcpy(piece, word + lo, plen);
					piece[plen] = '\0';
					int pw, ph;
					GFX_measureText(f, piece, &pw, &ph);
					if (pw > max_width)
						break;
					last_fit = hi;
					hi++;
				}
				int plen = last_fit - lo;
				if (plen <= 0)
					plen = 1; // always make progress, even if one glyph alone overflows max_width
				snprintf(lines[n], DESC_LINE_MAX, "%.*s", plen, word + lo);
				n++;
				lo += plen;
			}
			p = q;
			while (*p == ' ')
				p++;
			hard_broke = true;
			break;
		}

		if (!hard_broke && has_word && n < max_lines)
			snprintf(lines[n++], DESC_LINE_MAX, "%s", cur);
	}

	// Overflow: more text remains than max_lines could hold - ellipsize the
	// last line drawn so the cut is visually obvious.
	while (*p == ' ')
		p++;
	if (*p && n > 0) {
		char* last = lines[n - 1];
		int len = (int)strlen(last);
		while (len > 0) {
			char trial[DESC_LINE_MAX];
			snprintf(trial, sizeof(trial), "%.*s...", len, last);
			int tw, th;
			GFX_measureText(f, trial, &tw, &th);
			if (tw <= max_width) {
				// `trial` already holds the winning "<prefix>..." string -
				// copy it in rather than re-running "%.*s..." with `last`
				// as both the snprintf destination and a source argument
				// (undefined behavior; -Wrestrict catches the self-alias).
				snprintf(last, DESC_LINE_MAX, "%s", trial);
				break;
			}
			len--;
		}
		if (len == 0)
			snprintf(last, DESC_LINE_MAX, "...");
	}

	return n;
}

// Same loop idiom as run_list (GFX_startFrame/PAD_poll/PWR_update with a
// persisted show_setting/UI_statusBarChanged/GFX_flip|GFX_sync) rather than a
// bare PAD_poll spin, so the brightness/volume indicator and status bar keep
// working on this screen too.
// board v2-ToolsXtrasDetail: a 360 squircle with the letters left (status and size pills under it); right the name
// 48/700, the description 28 text2, then a group of the actions (install / update / reinstall, uninstall)
static int xd_sel = 0;
static BosGlide xd_pill, xd_scroll;
static AddonEntry* xd_e;
static int xd_count(const AddonEntry* e) {
	return !e->compatible ? 0 : e->installed[0] ? 2 : 1;
}
static void xd_row(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	AddonEntry* e = xd_e;
	if (i == 0)
		*r = (ListViewRow){.label = I18N_text(!e->installed[0] ? "Install" : entry_update_available(e) ? "Update" : "Reinstall"),
						   .sq_icon = "download", .sq_color = BOS_C_PRIMARY,
						   .sub = e->installed[0] ? NULL : I18N_text("Downloads from the internet")};
	else
		*r = (ListViewRow){.label = I18N_text("Uninstall"), .sub = I18N_text("Saves and game files stay"), .sq_icon = "trash",
						   .sq_color = BOS_C_DANGER, .danger = true};
}
static void render_detail_bos(AddonEntry* e) {
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("Xtras"));
	int top = BOS_px(BOS_BAR_H), x = BOS_px(40), y = top + BOS_px(12), pw = BOS_px(360);
	uint32_t c = xt_colour(e->name);
	BOS_rrect(screen, x, y, pw, pw, BOS_px(28), c);
	char ini[8];
	snprintf(ini, sizeof(ini), "%.2s", e->name);
	for (char* p = ini; *p; p++)
		if (*p >= 'a' && *p <= 'z')
			*p -= 32;
	BOS_textMid(screen, ini, 120, 700, 0xFFFFFF, 0.9f, x + (pw - BOS_textW(ini, 120, 700)) / 2, y + pw / 2);
	// the pills under the picture
	int py = y + pw + BOS_px(18), px = x, ph = BOS_px(40);
	char sz[32];
	snprintf(sz, sizeof(sz), "%d MB", e->size_mb);
	const char* st = !e->compatible ? I18N_text("Not available") : entry_update_available(e) ? I18N_text("Update")
					 : e->installed[0] ? I18N_text("Installed") : NULL;
	uint32_t sb = !e->compatible ? BOS_theme()->surface2 : entry_update_available(e) ? BOS_C_WARN : BOS_C_OK;
	const char* pills[2] = {st, sz};
	for (int i = 0; i < 2; i++) {
		if (!pills[i])
			continue;
		int w = BOS_textW(pills[i], 24, 600) + BOS_px(32);
		bool coloured = i == 0 && e->compatible;
		BOS_rrect(screen, px, py, w, ph, ph / 2, i == 0 ? sb : BOS_theme()->surface2);
		BOS_textMid(screen, pills[i], 24, 600, coloured ? 0x000000 : BOS_C_TEXT, 1, px + BOS_px(16), py + ph / 2);
		px += w + BOS_px(10);
	}
	// right column
	int rx = x + pw + BOS_px(36), rw = screen->w - BOS_px(40) - rx, ry = y;
	ry += BOS_textWrap(screen, e->name, 48, 700, BOS_C_TEXT, 1, rx, ry, rw, 2) + BOS_px(10);
	int n = xd_count(e), rh = BOS_px(BOS_ROW_H);
	int bottom = screen->h - BOS_px(BOS_BAR_H) - BOS_px(12);
	int desc_lines = (bottom - ry - n * rh - BOS_px(22)) / BOS_lineH(28);
	if (desc_lines > 8)
		desc_lines = 8;
	if (desc_lines > 0 && e->desc[0])
		ry += BOS_textWrap(screen, I18N_text(e->desc), 28, 400, BOS_C_TEXT2, 1, rx, ry, rw, desc_lines);
	ry += BOS_px(22);
	if (!e->compatible) {
		char m[128];
		snprintf(m, sizeof(m), I18N_text("Not available on %s"), PLAT_getModel());
		BOS_textWrap(screen, m, 30, 600, BOS_C_WARN, 1, rx, ry, rw, 2);
	} else {
		if (xd_sel >= n)
			xd_sel = n - 1;
		xd_e = e;
		BOS_groupRows(screen, &(SDL_Rect){rx, ry, rw, n * rh}, rh, n, xd_sel, true, NULL, &xd_pill, &xd_scroll, false, xd_row, NULL);
	}
	BosHint h[] = {{"A", I18N_text("Select")}, {"B", I18N_text("Back")}};
	BOS_hintBar(screen, e->compatible ? h : h + 1, e->compatible ? 2 : 1, NULL, 0);
}

static int run_detail(AddonEntry* e) {
	bool dirty = true;
	xd_sel = 0;
	bool changed = false;
	IndicatorType show_setting = INDICATOR_NONE;
	while (1) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_B))
			break;
		bool xd_un = false; // A on the uninstall row
		if (xd_count(e) > 1 && (PAD_justRepeated(BTN_UP) || PAD_justRepeated(BTN_DOWN))) {
			xd_sel = !xd_sel;
			dirty = true;
		}
		if (xd_sel == 1 && PAD_justPressed(BTN_A))
			xd_un = true;
		if (PAD_justPressed(BTN_A) && !xd_un) {
			if (!e->compatible) {
				char msg[128];
				snprintf(msg, sizeof(msg), "Not available on %s", PLAT_getModel());
				UI_showMessage(screen, msg, EXTRAS_MESSAGE_MS);
			} else {
				const char* err = preflight(e);
				if (err) {
					UI_showMessage(screen, err, EXTRAS_MESSAGE_MS);
				} else {
					if (run_install(e) == 0) {
						changed = true;
						// Internal entries: extras.c owns the version marker (the
						// install.sh does not write it). Write our catalog version
						// on success so the entry reads as up to date.
						if (e->version_internal && e->version[0]) {
							char state_dir[MAX_PATH];
							xtras_state_dir(state_dir, sizeof(state_dir));
							mkdir(state_dir, 0755);
							char mpath[MAX_PATH];
							snprintf(mpath, sizeof(mpath), "%s/%s.version", state_dir, e->id);
							FILE* mf = fopen(mpath, "w");
							if (mf) {
								fprintf(mf, "%s\n", e->version);
								fclose(mf);
							}
						}
						// Refresh e->installed from the marker for this open page.
						e->installed[0] = '\0';
						read_installed(e);
					}
				}
			}
			dirty = true;
		}
		// X/UNINSTALL is guarded on installed[0] only (not compatible): in
		// SP1 no incompatible entry can ever reach the installed state, so
		// this branch is unreachable for them. Anything that lets an entry's
		// tag flip to incompatible while installed must decide whether to keep
		// uninstall available, or block it to match the hidden hint, before
		// this assumption is relied on.
		if ((PAD_justPressed(BTN_X) || xd_un) && e->installed[0]) {
			if (run_confirm_dialog("Uninstall?",
								   "Saves and ROMs are kept - reinstall to play again.")) {
				if (run_uninstall(e) == 0) {
					changed = true;
					// Same refresh as the install path above: a successful
					// uninstall leaves no marker, so this reads back as "not
					// installed" and the detail page shows uninstalled right
					// away instead of on the next list visit.
					e->installed[0] = '\0';
					read_installed(e);
				}
			}
			dirty = true;
		}
		if (UI_statusBarChanged())
			dirty = true;
		PWR_update(&dirty, &show_setting, NULL, NULL);
		if (BOS_glideMoving(&xd_pill))
			dirty = true;
		if (dirty) {
			render_detail_bos(e);
			GFX_flip(screen);
			dirty = false;
		} else {
			GFX_sync();
		}
	}
	return changed ? 1 : 0;
}

int main(int argc, char* argv[]) {
	(void)argc;
	char* slash = strrchr(argv[0], '/');
	if (slash) {
		snprintf(pak_dir, sizeof(pak_dir), "%.*s", (int)(slash - argv[0]), argv[0]);
	} else
		getcwd(pak_dir, sizeof(pak_dir));

	screen = GFX_init(MODE_MAIN);
	UI_showSplashScreen(screen, "Xtras");

	PWR_pinToCores(CPU_CORE_EFFICIENCY);
	InitSettings();
	PAD_init();
	PWR_init();
	PWR_disableSleep();
	PWR_disableAutosleep();
	PWR_disablePowerOff();

	catalog_load();

	// Snapshot the tracked entries for the release-check thread (its own
	// comment explains why it must not read entries[] directly), then fire
	// it. Detached: it owns no UI and the file-based handoff means nothing
	// joins on it; if it outlives an early quit the process teardown reaps
	// it. Creation failure just raises the done flag so run_list never
	// waits on a sweep that isn't coming.
	check_count = 0;
	for (int i = 0; i < entry_count; i++) {
		if (entries[i].version_internal || !entries[i].repo[0])
			continue;
		snprintf(check_items[check_count].id, sizeof(check_items[0].id), "%s", entries[i].id);
		snprintf(check_items[check_count].repo, sizeof(check_items[0].repo), "%s", entries[i].repo);
		check_count++;
	}
	pthread_t latest_thread;
	if (pthread_create(&latest_thread, NULL, latest_check_thread, NULL) == 0)
		pthread_detach(latest_thread);
	else
		latest_check_done = 1;

	run_list();

	PWR_enableSleep();
	PWR_enableAutosleep();
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return 0;
}
