// ตั้งค่า › หน้าแรกและเกม › ภาพปก (DESIGN §6.7d): the Artwork Manager's pages, drawn by Settings. The work stays in
// scraper.elf: a worker thread runs its one-game mode (fetchart.sh = scraper.elf --fetch) for each queued game, so
// the files and rules are the app's. The queue (scrape_queue.c) and the game walk (scraper_scan.c) are the app's too.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "area_scale.h"
#include "bos_tokens.h"
#include "defines.h"
#include "i18n.h"
#include "scrape_queue.h"
#include "scraper_core.h"
#include "scraper_prefs.h"
#include "scraper_scan.h"
#include "scraper_systems.h"
#include "settings_menu.h"
#include "bos_ui.h"
#include "utils.h"

#define ART_PAK PAKS_PATH "/Tools/Artwork Manager.pak"
#define MAX_CONS 64
#define MAIN_FIXED 2  // คิว, โหลดภาพที่ขาด
#define MAIN_TAIL 4	  // ภาพที่ดาวน์โหลด, บัญชี, ลดขนาดไฟล์ภาพ, ลบภาพที่ดาวน์โหลด
#define HOT_OK 0x1E8F3C
#define HOT_DANGER 0xC0261D

bool settings_confirm(const char* title, const char* subtitle); // settings.c
SDL_Surface* settings_screen(void);

// ---- consoles and their games ----
typedef struct {
	char path[MAX_PATH];
	char name[128];
	char tag[32];
	int sid; // ScreenScraper system id, -1 = not supported
	int games, with_art;
} ArtConsole;

typedef struct {
	ScanGame g;
	bool art;
	SDL_Surface* thumb;
	bool thumb_tried;
} ArtGame;

static ArtConsole cons[MAX_CONS];
static int ncons;
static bool counted;
static ScraperPrefs prefs;
static int devkey = -1; // scraper.elf --caps, asked once

static bool hasArt(const char* art_png) {
	char v[MAX_PATH];
	Scraper_variantPath(art_png, "screenshot", v, sizeof(v));
	bool ss = ROM_artExists(v, sizeof(v));
	Scraper_variantPath(art_png, "boxart", v, sizeof(v));
	bool box = ROM_artExists(v, sizeof(v));
	Scraper_variantPath(art_png, "mix", v, sizeof(v));
	bool mix = ROM_artExists(v, sizeof(v));
	return ScraperPrefs_isComplete(&prefs, ss, box, mix);
}

static bool countCb(const ScanGame* g, void* ud) {
	ArtConsole* c = ud;
	c->games++;
	if (hasArt(g->art_png))
		c->with_art++;
	return true;
}

static int byName(const void* a, const void* b) {
	return strcasecmp(((const ArtConsole*)a)->name, ((const ArtConsole*)b)->name);
}

// the launcher's console list cache: "<Roms dir>\t<name>\t<games>"
static void scanConsoles(void) {
	ncons = 0;
	ScraperPrefs_load(&prefs);
	FILE* f = fopen(USERDATA_PATH "/emulist_cache.txt", "r");
	if (!f)
		return;
	char line[MAX_PATH * 2];
	while (ncons < MAX_CONS && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#')
			continue;
		char* name = strchr(line, '\t');
		char* count = name ? strchr(name + 1, '\t') : NULL;
		if (!count || atoi(count + 1) <= 0)
			continue;
		*name++ = 0, *count = 0;
		char* close = strrchr(line, ')');
		char* open = close ? strrchr(line, '(') : NULL;
		if (!open || close < open)
			continue;
		ArtConsole* c = &cons[ncons];
		memset(c, 0, sizeof(*c));
		snprintf(c->tag, sizeof(c->tag), "%.*s", (int)(close - open - 1), open + 1);
		if (!strcasecmp(c->tag, "PORTS") || !strcasecmp(c->tag, "CUSTOM") || !strcasecmp(c->tag, "EXTRAS"))
			continue;
		snprintf(c->path, sizeof(c->path), "%s", line);
		snprintf(c->name, sizeof(c->name), "%s", name);
		c->sid = ScraperSystems_getId(c->tag);
		ncons++;
	}
	fclose(f);
	qsort(cons, ncons, sizeof(ArtConsole), byName);
	for (int i = 0; i < ncons; i++)
		Scan_walk(cons[i].path, countCb, &cons[i]);
	counted = true;
}

static int totalGames(void) {
	int n = 0;
	for (int i = 0; i < ncons; i++)
		n += cons[i].games;
	return n;
}
static int totalArt(void) {
	int n = 0;
	for (int i = 0; i < ncons; i++)
		n += cons[i].with_art;
	return n;
}
static int totalMissing(void) {
	int n = 0;
	for (int i = 0; i < ncons; i++)
		if (cons[i].sid >= 0)
			n += cons[i].games - cons[i].with_art;
	return n;
}

// ---- the queue and its worker ----
static ScrapeQueue queue;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static volatile int worker_on;
static volatile int qdirty;

static ScrapeStatus runFetch(const ScrapeQueueItem* it) {
	const char* st = "/tmp/bos_art_status";
	unlink(st);
	char* a = shellQuoteAlloc(it->rom_path);
	char* b = shellQuoteAlloc(it->out_png);
	char* c = shellQuoteAlloc(it->system_name); // the console's tag (see queueGame)
	char cmd[MAX_PATH * 4];
	snprintf(cmd, sizeof(cmd), "sh \"%s/fetchart.sh\" %s %s %s %s >/dev/null 2>&1", ART_PAK, a ? a : "''",
			 b ? b : "''", c ? c : "''", st);
	free(a), free(b), free(c);
	system(cmd);
	char res[32] = "";
	getFile((char*)st, res, sizeof(res));
	if (!strncmp(res, "done", 4))
		return SCRAPE_STATUS_DONE;
	if (!strncmp(res, "notfound", 8))
		return SCRAPE_STATUS_NOT_FOUND;
	return SCRAPE_STATUS_ERROR;
}

static void* workerMain(void* arg) {
	(void)arg;
	for (;;) {
		ScrapeQueueItem it;
		pthread_mutex_lock(&qlock);
		bool got = ScrapeQueue_takeNext(&queue, &it);
		if (!got)
			worker_on = 0;
		pthread_mutex_unlock(&qlock);
		if (!got)
			return NULL;
		qdirty = 1;
		ScrapeStatus r = runFetch(&it);
		pthread_mutex_lock(&qlock);
		ScrapeQueue_finish(&queue, it.rom_path, r);
		pthread_mutex_unlock(&qlock);
		qdirty = 1;
	}
}

static void startWorker(void) {
	pthread_mutex_lock(&qlock);
	bool start = !worker_on && ScrapeQueue_isBusy(&queue);
	if (start)
		worker_on = 1;
	pthread_mutex_unlock(&qlock);
	if (start) {
		if (pthread_create(&worker, NULL, workerMain, NULL) == 0)
			pthread_detach(worker);
		else
			worker_on = 0;
	}
}

// 1 queued, 0 already there
static int queueGame(const ArtConsole* c, const ScanGame* g) {
	ScrapeQueueItem q;
	memset(&q, 0, sizeof(q));
	snprintf(q.filename, sizeof(q.filename), "%s", g->filename);
	snprintf(q.rom_path, sizeof(q.rom_path), "%s", g->path);
	snprintf(q.system_path, sizeof(q.system_path), "%s", c->path);
	snprintf(q.system_name, sizeof(q.system_name), "%s", c->tag); // fetchart.sh wants the tag
	snprintf(q.out_png, sizeof(q.out_png), "%s", g->art_png);
	q.system_id = c->sid;
	pthread_mutex_lock(&qlock);
	int r = ScrapeQueue_add(&queue, &q);
	pthread_mutex_unlock(&qlock);
	return r == 1;
}

bool SettingsArt_busy(void) {
	pthread_mutex_lock(&qlock);
	bool b = ScrapeQueue_isBusy(&queue);
	pthread_mutex_unlock(&qlock);
	return b;
}

static ScrapeStatus statusOf(const char* rom_path, bool* listed) {
	pthread_mutex_lock(&qlock);
	int i = ScrapeQueue_find(&queue, rom_path);
	ScrapeStatus s = i >= 0 ? queue.items[i].status : SCRAPE_STATUS_IDLE;
	pthread_mutex_unlock(&qlock);
	*listed = i >= 0;
	return s;
}

// ---- a console's games ----
static ArtGame* games;
static int ngames, games_cap;
static SettingItem* game_items;
static SettingsPage games_page;
static int cur_con = -1;
static char games_title[160], games_sub[160];

static bool gameCb(const ScanGame* g, void* ud) {
	(void)ud;
	if (ngames == games_cap) {
		int cap = games_cap ? games_cap * 2 : 256;
		ArtGame* ng = realloc(games, sizeof(ArtGame) * cap);
		if (!ng)
			return false;
		games = ng, games_cap = cap;
	}
	ArtGame* a = &games[ngames++];
	memset(a, 0, sizeof(*a));
	a->g = *g;
	a->art = hasArt(g->art_png);
	return true;
}

static int byLabel(const void* a, const void* b) {
	return strcasecmp(((const ArtGame*)a)->g.label, ((const ArtGame*)b)->g.label);
}

static void freeThumbs(void) {
	for (int i = 0; i < ngames; i++)
		if (games[i].thumb) {
			SDL_FreeSurface(games[i].thumb);
			games[i].thumb = NULL;
		}
}

// the box art (else the screenshot), shrunk to fit 50x64
static SDL_Surface* loadThumb(const char* art_png) {
	char p[MAX_PATH];
	Scraper_variantPath(art_png, "boxart", p, sizeof(p));
	if (!ROM_artExists(p, sizeof(p))) {
		Scraper_variantPath(art_png, "screenshot", p, sizeof(p));
		if (!ROM_artExists(p, sizeof(p)))
			return NULL;
	}
	SDL_Surface* img = IMG_Load(p);
	if (!img)
		return NULL;
	SDL_Surface* src = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(img);
	if (!src)
		return NULL;
	int bw = BOS_px(50), bh = BOS_px(64);
	float s = (float)bw / src->w < (float)bh / src->h ? (float)bw / src->w : (float)bh / src->h;
	int w = (int)(src->w * s + 0.5f), h = (int)(src->h * s + 0.5f);
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (dst && AreaScale_argb(src->pixels, src->w, src->h, src->pitch / 4, dst->pixels, w, h, dst->pitch / 4) != 0) {
		SDL_FreeSurface(dst);
		dst = NULL;
	}
	SDL_FreeSurface(src);
	return dst;
}

static bool artPoll(void);

static void gameRow(int i) {
	ArtGame* a = &games[i];
	SettingItem* it = &game_items[i];
	bool listed;
	ScrapeStatus st = statusOf(a->g.path, &listed);
	const char* t;
	it->value_icon = NULL, it->value_rgb = it->value_rgb_hot = 0;
	if (listed && st == SCRAPE_STATUS_IDLE)
		t = "Queued";
	else if (listed && (st == SCRAPE_STATUS_NOT_FOUND || st == SCRAPE_STATUS_ERROR))
		t = st == SCRAPE_STATUS_NOT_FOUND ? "Not found" : "Error", it->value_icon = "info",
		it->value_rgb = BOS_C_DANGER, it->value_rgb_hot = HOT_DANGER;
	else if (listed)
		t = "Downloading", it->value_icon = "update", it->value_rgb = BOS_C_PRIMARY,
		it->value_rgb_hot = BOS_C_ONFOCUS;
	else if (a->art)
		t = "Have it", it->value_icon = "check", it->value_rgb = BOS_C_OK, it->value_rgb_hot = HOT_OK;
	else
		t = "Missing", it->value_rgb = BOS_C_TEXTOFF;
	snprintf(it->display_text, sizeof(it->display_text), "%s", t);
	it->a_label = a->art ? "Download again" : "Download";
	if (cur_con >= 0 && cons[cur_con].sid < 0) // no source knows this console
		it->display_text[0] = 0, it->a_label = "", it->value_icon = NULL;
	it->lead_image = a->thumb;
	it->lead_frame = !a->thumb;
}

static void gamesSub(void) {
	const ArtConsole* c = &cons[cur_con];
	char n[96];
	snprintf(n, sizeof(n), I18N_text("Has cover art: %d of %d games"), c->with_art, c->games);
	snprintf(games_sub, sizeof(games_sub), "%s \xC2\xB7 %s", n, I18N_text("A–Z"));
}

static void queueAllOf(int ci); // Y

static void gamePress(void) {
	SettingsPage* p = settings_menu_current();
	SettingItem* it = p ? settings_page_visible_item(p, p->selected) : NULL;
	if (!it || cur_con < 0)
		return;
	int i = (int)(it - game_items);
	if (i < 0 || i >= ngames)
		return;
	queueGame(&cons[cur_con], &games[i].g);
	startWorker();
	gameRow(i);
}

static int gameInput(SettingItem* it) {
	(void)it;
	if (PAD_justPressed(BTN_Y) && cur_con >= 0) {
		queueAllOf(cur_con);
		for (int i = 0; i < ngames; i++)
			gameRow(i);
		return 1;
	}
	return 0;
}

static void gamesTick(SettingsPage* page) {
	page->needs_layout = artPoll();
	// pictures for the rows near the focus, two a frame; far ones are let go
	int sel = page->selected, loaded = 0;
	for (int i = 0; i < ngames; i++) {
		ArtGame* a = &games[i];
		bool near = i >= sel - 7 && i <= sel + 7;
		if (!near && a->thumb) {
			SDL_FreeSurface(a->thumb);
			a->thumb = NULL, a->thumb_tried = false;
			gameRow(i);
		} else if (near && !a->thumb_tried && a->art && loaded < 2) {
			a->thumb = loadThumb(a->g.art_png);
			a->thumb_tried = true;
			loaded++;
			gameRow(i);
		}
	}
	if (loaded)
		page->needs_layout = 1;
}

static void gamesHide(SettingsPage* page) {
	(void)page;
	freeThumbs();
	cur_con = -1;
}

static void openConsole(void) {
	SettingsPage* p = settings_menu_current();
	SettingItem* sel = p ? settings_page_visible_item(p, p->selected) : NULL;
	if (!sel)
		return;
	int ci = (int)(intptr_t)sel->user_data;
	if (ci < 0 || ci >= ncons)
		return;
	freeThumbs();
	ngames = 0;
	cur_con = ci;
	Scan_walk(cons[ci].path, gameCb, NULL);
	qsort(games, ngames, sizeof(ArtGame), byLabel);
	free(game_items);
	game_items = calloc(ngames + 1, sizeof(SettingItem));
	if (!game_items)
		return;
	gamesSub();
	for (int i = 0; i < ngames; i++) {
		SettingItem* it = &game_items[i];
		*it = (SettingItem)ITEM_BUTTON_INIT(games[i].g.label, "", gamePress);
		it->on_input = gameInput;
		it->y_label = cons[ci].sid >= 0 ? "Download all missing" : NULL;
		gameRow(i);
		if (cons[ci].sid < 0)
			it->on_press = NULL;
	}
	if (ngames)
		game_items[0].header = games_sub;
	snprintf(games_title, sizeof(games_title), "Settings | %s", cons[ci].name);
	memset(&games_page, 0, sizeof(games_page));
	games_page.title = games_title;
	games_page.items = game_items;
	games_page.item_count = ngames;
	games_page.dynamic_start = -1;
	games_page.on_tick = gamesTick;
	games_page.on_hide = gamesHide;
	settings_menu_push(&games_page);
}

// ---- the queue page ----
#define QMAX 200
static SettingItem q_items[QMAX + 1];
static char q_names[QMAX][SCAN_NAME_MAX];
static char q_paths[QMAX][SCAN_PATH_MAX];
static SettingsPage queue_page;
static char q_head[96];

static void queueStop(void) { // X: forget everything not yet started
	pthread_mutex_lock(&qlock);
	int w = 0;
	for (int i = 0; i < queue.count; i++)
		if (queue.items[i].status != SCRAPE_STATUS_IDLE)
			queue.items[w++] = queue.items[i];
	queue.count = w;
	ScrapeQueue_dropFinished(&queue);
	pthread_mutex_unlock(&qlock);
	qdirty = 1;
}

static void queueRetry(void) { // A on a failed game
	SettingsPage* p = settings_menu_current();
	SettingItem* it = p ? settings_page_visible_item(p, p->selected) : NULL;
	if (!it || !it->user_data)
		return;
	const char* path = it->user_data;
	pthread_mutex_lock(&qlock);
	int i = ScrapeQueue_find(&queue, path);
	if (i >= 0 && ScrapeQueue_isTerminal(queue.items[i].status)) {
		ScrapeQueueItem copy = queue.items[i];
		ScrapeQueue_add(&queue, &copy);
	}
	pthread_mutex_unlock(&qlock);
	startWorker();
	qdirty = 1;
}

static void queueBuild(SettingsPage* page) {
	int done = 0, total = 0, failed = 0;
	pthread_mutex_lock(&qlock);
	ScrapeQueue_stats(&queue, &done, &total, &failed);
	int n = queue.count < QMAX ? queue.count : QMAX;
	SettingItem* h = &q_items[0];
	*h = (SettingItem)ITEM_STATIC_INIT("", "", NULL);
	snprintf(q_head, sizeof(q_head), I18N_text("Done %d of %d games"), done, total);
	h->name = q_head;
	if (failed)
		snprintf(h->display_text, sizeof(h->display_text), I18N_text("Not found %d"), failed),
			h->value_rgb = BOS_C_DANGER, h->value_rgb_hot = HOT_DANGER;
	h->bar_pct = 1 + (total ? done * 100 / total : 0);
	h->on_clear = n ? queueStop : NULL;
	h->clear_label = "Stop all";
	for (int i = 0; i < n; i++) {
		const ScrapeQueueItem* q = &queue.items[i];
		SettingItem* it = &q_items[i + 1];
		snprintf(q_names[i], sizeof(q_names[i]), "%s", q->filename);
		char* dot = strrchr(q_names[i], '.');
		if (dot && dot != q_names[i])
			*dot = 0;
		snprintf(q_paths[i], sizeof(q_paths[i]), "%s", q->rom_path);
		bool fail = ScrapeQueue_isTerminal(q->status);
		*it = (SettingItem)ITEM_BUTTON_INIT(q_names[i], "", fail ? queueRetry : NULL);
		it->user_data = q_paths[i];
		it->a_label = fail ? "Try again" : "";
		it->on_clear = queueStop;
		it->clear_label = "Stop all";
		if (q->status == SCRAPE_STATUS_IDLE)
			snprintf(it->display_text, sizeof(it->display_text), "%s", "Queued");
		else if (fail)
			snprintf(it->display_text, sizeof(it->display_text), "%s",
					 q->status == SCRAPE_STATUS_NOT_FOUND ? "Not found" : "Error"),
				it->value_icon = "info", it->value_rgb = BOS_C_DANGER, it->value_rgb_hot = HOT_DANGER;
		else
			snprintf(it->display_text, sizeof(it->display_text), "%s", "Downloading"), it->value_icon = "update",
				it->value_rgb = BOS_C_PRIMARY, it->value_rgb_hot = BOS_C_ONFOCUS;
	}
	pthread_mutex_unlock(&qlock);
	page->item_count = n + 1;
	if (page->selected >= page->item_count)
		page->selected = page->item_count - 1;
	page->needs_layout = 1;
}

static void queueShow(SettingsPage* page) {
	queueBuild(page);
}

static void queueTick(SettingsPage* page) {
	page->needs_layout = 0;
	if (artPoll())
		queueBuild(page);
}

// ---- which pictures ----
static SettingItem kind_items[4];
static SettingsPage kinds_page;
static const char* ONOFF[] = {"Off", "On"};
static const char* always(void) { return "Always"; }
static int getBox2d(void) { return prefs.box2d; }
static void setBox2d(int v) { prefs.box2d = v, ScraperPrefs_save(&prefs); }
static int getWheel(void) { return ScraperPrefs_wantWheel(&prefs); }
static void setWheel(int v) {
	if (prefs.mix) // the mix needs the logo: locked on
		return;
	prefs.wheel = v, ScraperPrefs_save(&prefs);
}
static int getMix(void) { return prefs.mix; }
static void setMix(int v) {
	prefs.mix = v, ScraperPrefs_save(&prefs);
	kind_items[2].desc = prefs.mix ? "Needed by the mix" : "Also download the game's logo";
	settings_item_sync(&kind_items[2]);
}
static int nKinds(void) {
	return 1 + prefs.box2d + ScraperPrefs_wantWheel(&prefs) + prefs.mix;
}

// ---- the ScreenScraper account ----
static SettingItem acct_items[4];
static SettingsPage acct_page;
static char ss_user[64], ss_pass[128], quota[48];
static void acctLoad(void) {
	ss_user[0] = ss_pass[0] = 0;
	getFile(creds_user_path(), ss_user, sizeof(ss_user));
	getFile(creds_pass_path(), ss_pass, sizeof(ss_pass));
	ss_user[strcspn(ss_user, "\r\n")] = 0;
	ss_pass[strcspn(ss_pass, "\r\n")] = 0;
}
static void acctSave(void) {
	mkdir_p(creds_dir());
	putFile(creds_user_path(), ss_user);
	putFile(creds_pass_path(), ss_pass);
}
static const char* getUser(void) { return ss_user[0] ? ss_user : I18N_text("Not set"); }
static const char* getPass(void) { return ss_pass[0] ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : I18N_text("Not set"); }
static void setUser(const char* t) { snprintf(ss_user, sizeof(ss_user), "%s", t), acctSave(); }
static void setPass(const char* t) { snprintf(ss_pass, sizeof(ss_pass), "%s", t), acctSave(); }
static void acctRows(void) {
	bool set = ss_user[0] && ss_pass[0];
	acct_items[2].visible = set;
	acct_items[3].visible = set;
}
static void logout(void) {
	if (!settings_confirm("Log out of ScreenScraper?", "The username and password are removed from this device."))
		return;
	ss_user[0] = ss_pass[0] = 0;
	unlink(creds_user_path());
	unlink(creds_pass_path());
	acctRows();
	acct_page.selected = 0;
}
static void* quotaMain(void* arg) {
	(void)arg;
	FILE* p = popen("\"" ART_PAK "/scraper.elf\" --userinfo 2>/dev/null", "r");
	int a = 0, b = 0;
	if (p) {
		if (fscanf(p, "%d %d", &a, &b) != 2)
			a = b = 0;
		pclose(p);
	}
	if (b > 0)
		snprintf(quota, sizeof(quota), "%d / %d", a, b);
	else
		snprintf(quota, sizeof(quota), "-");
	return NULL;
}
static const char* getQuota(void) { return quota; }
static void acctShow(SettingsPage* page) {
	(void)page;
	acctLoad();
	acctRows();
	snprintf(quota, sizeof(quota), "%s", "…");
	if (ss_user[0] && ss_pass[0]) {
		pthread_t t;
		if (pthread_create(&t, NULL, quotaMain, NULL) == 0)
			pthread_detach(t);
	}
}

// ---- the main page ----
static SettingItem main_items[MAIN_FIXED + MAX_CONS + MAIN_TAIL];
static SettingsPage art_page;
static char main_sub[96], queue_val[32], missing_val[32], kinds_val[32];
static char con_vals[MAX_CONS][24];
static char count_text[32];

static void queueAllOf(int ci) {
	ArtConsole* c = &cons[ci];
	if (c->sid < 0)
		return;
	// the console's games without art (walked again when its games page is not the one open)
	if (cur_con == ci) {
		for (int i = 0; i < ngames; i++)
			if (!games[i].art)
				queueGame(c, &games[i].g);
	} else {
		ArtGame* keep = games;
		int kn = ngames, kc = games_cap;
		games = NULL, ngames = 0, games_cap = 0;
		Scan_walk(c->path, gameCb, NULL);
		for (int i = 0; i < ngames; i++)
			if (!games[i].art)
				queueGame(c, &games[i].g);
		free(games);
		games = keep, ngames = kn, games_cap = kc;
	}
	startWorker();
	qdirty = 1;
}

static void queueAllMissing(void) {
	for (int i = 0; i < ncons; i++)
		if (cons[i].sid >= 0 && cons[i].games > cons[i].with_art)
			queueAllOf(i);
}

static void openQueue(void) {
	memset(&queue_page, 0, sizeof(queue_page));
	queue_page.title = "Settings | Downloading cover art";
	queue_page.items = q_items;
	queue_page.dynamic_start = -1;
	queue_page.max_items = QMAX + 1;
	queue_page.on_show = queueShow;
	queue_page.on_tick = queueTick;
	queueBuild(&queue_page);
	settings_menu_push(&queue_page);
}

static volatile int job_done, job_busy;
static char opt_status[] = "/tmp/bos_art_optimize";
static void* optimizeMain(void* arg) {
	(void)arg;
	char cmd[MAX_PATH * 2];
	snprintf(cmd, sizeof(cmd), "\"%s/scraper.elf\" --optimize %s >/dev/null 2>&1", ART_PAK, opt_status);
	system(cmd);
	job_done = 1;
	job_busy = 0;
	return NULL;
}
static void* resetMain(void* arg) {
	(void)arg;
	Scraper_deleteAllArtwork(ROMS_PATH);
	job_done = 1;
	job_busy = 0;
	return NULL;
}

static void artRefresh(void);

static void doOptimize(void) {
	if (SettingsArt_busy()) {
		settings_confirm("Cover art is downloading", "Wait for the queue to finish first.");
		return;
	}
	if (!settings_confirm("Make the pictures smaller?",
						  "Saves the downloaded pictures with fewer colours: about a third of the space."))
		return;
	char stop[64];
	snprintf(stop, sizeof(stop), "%s.stop", opt_status);
	unlink(stop);
	job_done = 0;
	settings_run_async(settings_screen(), I18N_text("Making the pictures smaller…"), optimizeMain, NULL, &job_done, &job_busy);
	if (!job_done)
		putFile(stop, "1"); // B: stop after the picture in hand
}

static void doReset(void) {
	if (SettingsArt_busy()) {
		settings_confirm("Cover art is downloading", "Wait for the queue to finish first.");
		return;
	}
	if (!settings_confirm("Delete the downloaded pictures?",
						  "Only pictures this page downloaded. Your own pictures in .media are kept."))
		return;
	job_done = 0;
	settings_run_async(settings_screen(), I18N_text("Deleting…"), resetMain, NULL, &job_done, &job_busy);
	scanConsoles();
	artRefresh();
}

static void artRefresh(void) {
	int done = 0, total = 0, failed = 0;
	pthread_mutex_lock(&qlock);
	ScrapeQueue_stats(&queue, &done, &total, &failed);
	pthread_mutex_unlock(&qlock);
	SettingItem* q = &main_items[0];
	q->visible = total > 0;
	snprintf(queue_val, sizeof(queue_val), "%d/%d", done, total);
	q->bar_pct = 1 + (total ? done * 100 / total : 0);
	snprintf(missing_val, sizeof(missing_val), I18N_text("%d games"), totalMissing());
	snprintf(main_sub, sizeof(main_sub), I18N_text("Has cover art: %d of %d games"), totalArt(), totalGames());
	main_items[0].header = q->visible ? main_sub : NULL;
	main_items[1].header = q->visible ? NULL : main_sub;
	for (int i = 0; i < ncons; i++) {
		SettingItem* it = &main_items[MAIN_FIXED + i];
		if (cons[i].sid < 0)
			snprintf(con_vals[i], sizeof(con_vals[i]), "%s", I18N_text("Not supported"));
		else
			snprintf(con_vals[i], sizeof(con_vals[i]), "%d/%d", cons[i].with_art, cons[i].games);
		snprintf(it->display_text, sizeof(it->display_text), "%s", con_vals[i]);
	}
	snprintf(kinds_val, sizeof(kinds_val), I18N_text("%d kinds"), nKinds());
}

static void artBuild(void) {
	int n = 0;
	main_items[n] = (SettingItem)ITEM_BUTTON_INIT("Queue", "", openQueue);
	main_items[n].chevron = 1;
	main_items[n].get_display = NULL;
	snprintf(main_items[n].display_text, sizeof(main_items[n].display_text), "%s", "");
	n++;
	main_items[n] = (SettingItem)ITEM_BUTTON_INIT("Download missing pictures",
												  "Every game without cover art, on every console.", queueAllMissing);
	main_items[n].chevron = 1;
	main_items[n].a_label = "Download";
	n++;
	for (int i = 0; i < ncons; i++, n++) {
		main_items[n] = (SettingItem)ITEM_BUTTON_INIT(cons[i].name, "", openConsole);
		main_items[n].chevron = 1;
		main_items[n].user_data = (void*)(intptr_t)i;
		main_items[n].header = i == 0 ? "Console list" : NULL;
	}
	int first_tail = n;
	main_items[n] = (SettingItem)ITEM_SUBMENU_INIT("Pictures to download", "", &kinds_page);
	n++;
	main_items[n] = (SettingItem)ITEM_SUBMENU_INIT("ScreenScraper account", "", &acct_page);
	main_items[n].visible = devkey == 1;
	n++;
	main_items[n] = (SettingItem)ITEM_BUTTON_INIT("Make pictures smaller",
												  "Saves the downloaded pictures with fewer colours to free space.",
												  doOptimize);
	n++;
	main_items[n] = (SettingItem)ITEM_BUTTON_INIT("Delete downloaded pictures",
												  "Your own pictures in .media are kept.", doReset);
	main_items[n].name_rgb = BOS_C_DANGER, main_items[n].name_rgb_hot = HOT_DANGER;
	main_items[n].a_label = "Delete";
	n++;
	main_items[first_tail].header = "Settings";
	art_page.item_count = n;
	// value pointers that never move
	main_items[1].get_display = NULL;
	artRefresh();
}

static const char* queueVal(void) { return queue_val; }
static const char* missingVal(void) { return missing_val; }
static const char* kindsVal(void) { return kinds_val; }

static void artShow(SettingsPage* page) {
	(void)page;
	if (devkey < 0) {
		FILE* p = popen("\"" ART_PAK "/scraper.elf\" --caps 2>/dev/null", "r");
		char line[32] = "";
		if (p) {
			if (!fgets(line, sizeof(line), p))
				line[0] = 0;
			pclose(p);
		}
		devkey = strstr(line, "devkey=1") ? 1 : 0;
	}
	if (!counted)
		scanConsoles();
	artBuild();
	main_items[0].get_display = queueVal;
	main_items[1].get_display = missingVal;
	main_items[ncons + MAIN_FIXED].get_display = kindsVal;
	if (art_page.selected >= settings_page_visible_count(&art_page))
		art_page.selected = 0;
}

// the queue's news (games finished): counts, rows. True when something changed.
static bool artPoll(void) {
	if (!qdirty)
		return false;
	qdirty = 0;
	// finished games: their console's count and, when open, their row
	for (;;) {
		pthread_mutex_lock(&qlock);
		char* path = ScrapeQueue_popFinished(&queue);
		pthread_mutex_unlock(&qlock);
		if (!path)
			break;
		for (int i = 0; i < ncons; i++) {
			size_t l = strlen(cons[i].path);
			if (!strncmp(path, cons[i].path, l) && path[l] == '/') {
				if (cur_con == i) {
					for (int g = 0; g < ngames; g++)
						if (!strcmp(games[g].g.path, path) && !games[g].art) {
							games[g].art = true;
							cons[i].with_art++;
							games[g].thumb_tried = false;
						}
				} else
					cons[i].with_art++;
				if (cons[i].with_art > cons[i].games)
					cons[i].with_art = cons[i].games;
				break;
			}
		}
		free(path);
	}
	for (int g = 0; g < ngames && cur_con >= 0 && game_items; g++)
		gameRow(g);
	if (cur_con >= 0)
		gamesSub();
	artRefresh();
	return true;
}

static void artTick(SettingsPage* page) {
	page->needs_layout = artPoll();
}

SettingsPage* art_page_create(void) {
	memset(&art_page, 0, sizeof(art_page));
	art_page.title = "Settings | Cover art";
	art_page.items = main_items;
	art_page.dynamic_start = -1;
	art_page.max_items = MAIN_FIXED + MAX_CONS + MAIN_TAIL;
	art_page.on_show = artShow;
	art_page.on_tick = artTick;

	memset(&kinds_page, 0, sizeof(kinds_page));
	kinds_page.title = "Settings | Pictures to download";
	ScraperPrefs_load(&prefs);
	kind_items[0] = (SettingItem)ITEM_STATIC_INIT("Screenshot and 3D box art", "Always downloaded.", always);
	kind_items[1] = (SettingItem)ITEM_CYCLE_INIT("2D box art", "Also download the flat box art.", ONOFF, 2, NULL,
												 getBox2d, setBox2d, NULL);
	kind_items[2] = (SettingItem)ITEM_CYCLE_INIT("Game logo", "Also download the game's logo", ONOFF, 2, NULL, getWheel,
												 setWheel, NULL);
	kind_items[3] = (SettingItem)ITEM_CYCLE_INIT(
		"Mix",
		"Screenshot, box and logo in one picture, for the list view. Downloading is slower and uses more space; turns the game logo on.",
		ONOFF, 2, NULL, getMix, setMix, NULL);
	kinds_page.items = kind_items;
	kinds_page.item_count = 4;
	kinds_page.dynamic_start = -1;

	memset(&acct_page, 0, sizeof(acct_page));
	acct_page.title = "Settings | ScreenScraper account";
	static const char* ss_desc =
		"ScreenScraper is a website of game cover art. It works without an account but slower. Sign up free at screenscraper.fr";
	acct_items[0] = (SettingItem)ITEM_TEXT_INPUT_INIT("Username", ss_desc, getUser, setUser);
	acct_items[1] = (SettingItem)ITEM_TEXT_INPUT_INIT("Password", ss_desc, getPass, setPass);
	acct_items[2] = (SettingItem)ITEM_STATIC_INIT("Used today", "", getQuota);
	acct_items[2].header = "Account";
	acct_items[3] = (SettingItem)ITEM_BUTTON_INIT("Log out", "", logout);
	acct_items[3].name_rgb = BOS_C_DANGER, acct_items[3].name_rgb_hot = HOT_DANGER;
	acct_page.items = acct_items;
	acct_page.item_count = 4;
	acct_page.dynamic_start = -1;
	acct_page.on_show = acctShow;
	return &art_page;
}

const char* art_count_text(void) { // "312/540" on หน้าแรกและเกม's row
	if (!counted)
		return "";
	snprintf(count_text, sizeof(count_text), "%d/%d", totalArt(), totalGames());
	return count_text;
}
