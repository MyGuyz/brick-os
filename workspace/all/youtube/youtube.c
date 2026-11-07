// Brick OS YouTube: search, followed channels and history, played with the
// Media Player's ffplay engine. yt-dlp and QuickJS live in the pak's bin/ and
// are installed (and updated) from the app itself.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "ffplay_engine.h"
#include "i18n.h"
#include "ui_buttonhintbar.h"
#include "ui_confirmdialog.h"
#include "ui_emptystate.h"
#include "ui_keyboard.h"
#include "ui_list.h"
#include "ui_loadingoverlay.h"
#include "ui_menubar.h"
#include "ui_message.h"
#include "yt_engine.h"

#define TITLE "YouTube"
#define ROW_TEXT_MAX 256
#define RESUME_MIN_SEC 30

enum { TAB_SEARCH, TAB_FEED, TAB_HISTORY, TAB_COUNT };
static const char* tab_names[TAB_COUNT] = {"Search", "Subscriptions", "History"};

static SDL_Surface* screen;
static YtOp op;
static YtList lists[TAB_COUNT];
static int sel[TAB_COUNT], scroll_pos[TAB_COUNT];
static char last_query[256];
static bool feed_loaded;

// ---- waiting on a worker with a loading screen ------------------------------
// Returns true when the operation finished (DONE or ERROR), false if B cancelled it.
static bool wait_op(const char* message) {
	bool dirty = true;
	IndicatorType ind = INDICATOR_NONE;
	while (op.state == YT_RUNNING) {
		GFX_startFrame();
		PAD_poll();
		PWR_update(&dirty, &ind, NULL, NULL);
		if (PAD_justPressed(BTN_B)) {
			Yt_cancel(&op);
			return false;
		}
		GFX_clear(screen);
		UI_renderLoadingOverlay(screen, TITLE, message);
		UI_renderButtonHintBar(screen, (char*[]){"B", "CANCEL", NULL});
		GFX_flip(screen);
	}
	return true;
}

// ---- engine install -------------------------------------------------------
static bool ensure_engine(void) {
	if (Yt_engineInstalled())
		return true;
	if (!UI_confirmModal(screen, "Install YouTube engine?",
						 "Downloads yt-dlp and Node.js (about 70 MB) over Wi-Fi.", NULL, true, true))
		return false;
	Yt_updateAsync(&op);
	if (!wait_op("Downloading..."))
		return false;
	if (op.state != YT_DONE) {
		UI_showMessage(screen, op.error, 3000);
		return false;
	}
	return true;
}

static void update_engine(void) {
	Yt_updateAsync(&op);
	if (!wait_op("Checking for update.."))
		return;
	UI_showMessage(screen, op.state == YT_DONE ? "Done" : op.error, 2500);
}

// ---- actions --------------------------------------------------------------
// yt-dlp streams into a FIFO and ffplay plays it. False if ffplay couldn't open it.
static bool play_stream(const YtVideo* v) {
	const char* fifo = Yt_streamStart();
	if (!fifo)
		return false;
	FfplayConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.path, sizeof(cfg.path), "%s", fifo);
	snprintf(cfg.title, sizeof(cfg.title), "%s", v->title);
	// The FIFO is opened like a local file: the engine's stream mode adds HTTP options that ffplay
	// refuses for a pipe. A pipe can't seek, so there is no resume position.
	cfg.is_stream = false;
	cfg.screen_width = screen->w;
	PWR_disableAutosleep();
	FfplayEngine_play(&cfg);
	PWR_enableAutosleep();
	Yt_streamStop();
	return !FfplayEngine_lastFailed();
}

static void play(const YtVideo* v) {
	if (!ensure_engine())
		return;
	Yt_resolveAsync(&op, v->id);
	if (!wait_op("Starting video (about 30 s)..."))
		return;
	if (op.state != YT_DONE) {
		UI_showMessage(screen, op.error, 3000);
		return;
	}
	Yt_addHistory(v);
	if (!play_stream(v))
		UI_showMessage(screen, "Failed to play", 2500);
	Yt_history(&lists[TAB_HISTORY]);
}

static void search(void) {
	char* q = UIKeyboard_openWith("Search", last_query, 0);
	if (!q)
		return;
	if (!q[0]) {
		free(q);
		return;
	}
	snprintf(last_query, sizeof(last_query), "%s", q);
	free(q);
	if (!ensure_engine())
		return;
	Yt_searchAsync(&op, last_query);
	if (!wait_op("Searching..."))
		return;
	if (op.state == YT_DONE) {
		lists[TAB_SEARCH] = op.list;
		sel[TAB_SEARCH] = scroll_pos[TAB_SEARCH] = 0;
	} else
		UI_showMessage(screen, op.error, 3000);
}

static void load_feed(void) {
	Yt_feedAsync(&op);
	if (!wait_op("Loading..."))
		return;
	feed_loaded = true;
	if (op.state == YT_DONE) {
		lists[TAB_FEED] = op.list;
		sel[TAB_FEED] = scroll_pos[TAB_FEED] = 0;
	} else
		UI_showMessage(screen, op.error, 3000);
}

static void toggle_follow(const YtVideo* v) {
	if (!v->channel_id[0]) {
		UI_showMessage(screen, "Not available", 2000);
		return;
	}
	if (Yt_isFollowing(v->channel_id)) {
		if (!UI_confirmModal(screen, "Unsubscribe?", v->channel, NULL, true, true))
			return;
		Yt_unfollow(v->channel_id);
		UI_showMessage(screen, "Unsubscribed", 1500);
	} else {
		Yt_follow(v->channel_id, v->channel);
		UI_showMessage(screen, "Subscribed!", 1500);
	}
	feed_loaded = false;
}

// ---- drawing --------------------------------------------------------------
static void draw(int tab) {
	GFX_clear(screen);
	char title[160];
	snprintf(title, sizeof(title), "%s  |  %s", TITLE, I18N_text(tab_names[tab]));
	UI_renderMenuBar(screen, title);

	YtList* l = &lists[tab];
	if (l->count == 0) {
		const char* msg = tab == TAB_SEARCH ? "Press Y to search" : tab == TAB_FEED ? "No subscriptions yet" : "Nothing played yet";
		const char* sub = tab == TAB_SEARCH ? "Search YouTube, then A to play" : tab == TAB_FEED ? "Press X on a video to follow its channel" : "";
		UI_renderEmptyState(screen, msg, sub, NULL);
	} else {
		ListLayout layout = UI_calcListLayout(screen);
		int per_page = layout.items_per_page > 2 ? layout.items_per_page - 1 : layout.items_per_page; // room for the info line
		UI_adjustListScroll(sel[tab], &scroll_pos[tab], per_page);
		char trunc[ROW_TEXT_MAX];
		for (int i = scroll_pos[tab]; i < l->count && i < scroll_pos[tab] + per_page; i++) {
			bool s = (i == sel[tab]);
			MenuItemPos pos = UI_renderMenuItemPill(screen, &layout, l->items[i].title, trunc, i - scroll_pos[tab], s, 0);
			UI_renderListItemText(screen, NULL, trunc, font.large, pos.text_x, pos.text_y, layout.max_width, s);
		}
		// details of the selected video above the hints
		const YtVideo* v = &l->items[sel[tab]];
		char dur[16], info[300];
		Yt_formatDuration(v->duration, dur, sizeof(dur));
		bool following = Yt_isFollowing(v->channel_id);
		snprintf(info, sizeof(info), "%s%s%s%s%s", v->channel, dur[0] ? "  ·  " : "", dur,
				 following ? "  ·  " : "", following ? I18N_text("Subscribed") : "");
		SDL_Surface* t = GFX_renderText(font.small, info, COLOR_GRAY);
		if (t) {
			SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){SCALE1(PADDING), screen->h - SCALE1(PILL_SIZE + PADDING) - t->h, 0, 0});
			SDL_FreeSurface(t);
		}
	}
	char* hints_list[] = {"L1/R1", "TAB", "Y", "SEARCH", "X", "SUBSCRIBE", "A", "PLAY", "B", "EXIT", NULL};
	char* hints_empty[] = {"L1/R1", "TAB", "Y", "SEARCH", "B", "EXIT", NULL};
	UI_renderButtonHintBar(screen, l->count ? hints_list : hints_empty);
	GFX_flip(screen);
}

int main(int argc, char* argv[]) {
	(void)argc;
	char pak[512];
	if (!getcwd(pak, sizeof(pak)))
		snprintf(pak, sizeof(pak), "%s", argv[0]);
	Yt_init(pak);

	screen = GFX_init(MODE_MAIN);
	PWR_pinToCores(CPU_CORE_EFFICIENCY);
	InitSettings();
	PAD_init();
	PWR_init();

	Yt_history(&lists[TAB_HISTORY]);
	int tab = Yt_channels((YtChannel[1]){0}, 1) > 0 ? TAB_FEED : TAB_SEARCH;
	bool dirty = true;
	IndicatorType ind = INDICATOR_NONE;

	while (1) {
		if (tab == TAB_FEED && !feed_loaded) {
			load_feed();
			dirty = true;
		}
		GFX_startFrame();
		PAD_poll();
		PWR_update(&dirty, &ind, NULL, NULL);
		if (UI_statusBarChanged())
			dirty = true;

		YtList* l = &lists[tab];
		if (PAD_justPressed(BTN_L1) || PAD_justPressed(BTN_R1)) {
			tab = (tab + (PAD_justPressed(BTN_R1) ? 1 : TAB_COUNT - 1)) % TAB_COUNT;
			if (tab == TAB_HISTORY)
				Yt_history(&lists[TAB_HISTORY]);
			dirty = true;
		} else if (PAD_justPressed(BTN_B)) {
			break;
		} else if (PAD_justPressed(BTN_Y)) {
			search();
			tab = TAB_SEARCH;
			dirty = true;
		} else if (PAD_justPressed(BTN_MENU)) {
			if (UI_confirmModal(screen, "Update yt-dlp?", "Use this when videos stop playing.", NULL, true, true))
				update_engine();
			dirty = true;
		} else if (l->count > 0) {
			if (PAD_navigateMenu(&sel[tab], l->count))
				dirty = true;
			else if (PAD_justPressed(BTN_A)) {
				YtVideo v = l->items[sel[tab]];
				play(&v);
				dirty = true;
			} else if (PAD_justPressed(BTN_X)) {
				toggle_follow(&l->items[sel[tab]]);
				dirty = true;
			}
		}
		if (dirty) {
			draw(tab);
			dirty = false;
		} else
			GFX_sync();
	}

	Yt_cancel(&op);
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return 0;
}
