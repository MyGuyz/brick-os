// Own server and free music pages (DESIGN.md 7.2; boards v2-MusicServerSetup, -Server, -ServerAlbum, -Free).
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL2/SDL_image.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "module_online.h"
#include "module_player.h"
#include "online.h"
#include "playlist.h"
#include "music_format.h"
#include "ui_listview.h"
#include "ui_keyboard.h"
#include "ui_toast.h"
#include "ui_draw.h"
#include "display_helper.h"
#include "i18n.h"
#include "wget_fetch.h"

void BOS_uiLoading(SDL_Surface* s, const char* title, const char* subtitle);

static void busy(SDL_Surface* s, const char* what) {
	BOS_uiLoading(s, I18N_text(what), NULL);
	GFX_flip(s);
}

static char toast[160];
static uint32_t toast_at;
static void say(const char* t) {
	snprintf(toast, sizeof(toast), "%s", I18N_text(t));
	toast_at = SDL_GetTicks();
}

// ================= downloads: one list at a time on a thread =================
#define DL_MAX 200
static struct {
	char url[DL_MAX][1024];
	char path[DL_MAX][640];
	int n, done, failed;
	char tag[160];
	volatile bool run, stop;
	WgetDownloadJob job;
} dl;
static pthread_mutex_t dl_mu = PTHREAD_MUTEX_INITIALIZER;

static void* dlThread(void* arg) {
	(void)arg;
	for (int i = 0; i < dl.n && !dl.stop; i++) {
		if (access(dl.path[i], R_OK) == 0) {
			dl.done++;
			continue;
		}
		char dir[640], tmp[660];
		snprintf(dir, sizeof(dir), "%s", dl.path[i]);
		char* sl = strrchr(dir, '/');
		if (sl) {
			*sl = 0;
			Online_mkdirs(dir);
		}
		snprintf(tmp, sizeof(tmp), "%s.part", dl.path[i]);
		dl.job.progress = 0;
		int r = wget_download_file(dl.url[i], tmp, &dl.job.progress, &dl.stop, &dl.job.speed, &dl.job.eta);
		pthread_mutex_lock(&dl_mu);
		if (r > 0 && !dl.stop) {
			rename(tmp, dl.path[i]);
			dl.done++;
		} else {
			unlink(tmp);
			dl.failed++;
		}
		pthread_mutex_unlock(&dl_mu);
	}
	dl.run = false;
	return NULL;
}

static bool dlStart(const char* tag, char (*urls)[1024], char (*paths)[640], int n) {
	if (dl.run)
		return false;
	memset(dl.tag, 0, sizeof(dl.tag));
	snprintf(dl.tag, sizeof(dl.tag), "%s", tag);
	dl.n = n > DL_MAX ? DL_MAX : n, dl.done = dl.failed = 0, dl.stop = false;
	for (int i = 0; i < dl.n; i++) {
		snprintf(dl.url[i], sizeof(dl.url[i]), "%s", urls[i]);
		snprintf(dl.path[i], sizeof(dl.path[i]), "%s", paths[i]);
	}
	dl.run = true;
	pthread_t t;
	if (pthread_create(&t, NULL, dlThread, NULL) != 0) {
		dl.run = false;
		return false;
	}
	pthread_detach(t);
	return true;
}

// a file being fetched now (its percent), else -1
static int dlProgressOf(const char* path) {
	if (!dl.run)
		return -1;
	int i = dl.done + dl.failed;
	if (i < dl.n && strcmp(dl.path[i], path) == 0)
		return dl.job.progress > 0 ? dl.job.progress : 1;
	return -1;
}

// fetch one file now, the screen saying so; true when it is there
static bool fetchNow(SDL_Surface* s, const char* url, const char* path, const char* name) {
	if (access(path, R_OK) == 0)
		return true;
	char dir[640], tmp[660];
	snprintf(dir, sizeof(dir), "%s", path);
	char* sl = strrchr(dir, '/');
	if (sl) {
		*sl = 0;
		Online_mkdirs(dir);
	}
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	WgetDownloadJob job;
	if (!wget_job_start(&job, url, tmp, 0))
		return false;
	int last = -2;
	while (!job.done) {
		PAD_poll();
		if (PAD_justPressed(BTN_B))
			job.cancel = true;
		if (job.progress != last) {
			last = job.progress;
			char sub[96];
			snprintf(sub, sizeof(sub), I18N_text("Downloading · %d%%"), job.progress > 0 ? job.progress : 0);
			BOS_uiLoading(s, name, sub);
			BosHint h[1] = {{"B", I18N_text("Cancel")}};
			BOS_hintBar(s, h, 1, NULL, 0);
			GFX_flip(s);
		}
		SDL_Delay(50);
	}
	wget_job_join(&job);
	if (job.result > 0 && !job.cancel) {
		rename(tmp, path);
		return true;
	}
	unlink(tmp);
	return false;
}

static void playFiles(SDL_Surface** sp, char (*paths)[640], int n, int start, bool shuffle) {
	static PlaylistTrack tracks[DL_MAX];
	int k = 0, at = 0;
	for (int i = 0; i < n && k < DL_MAX; i++) {
		if (access(paths[i], R_OK) != 0)
			continue;
		if (i == start)
			at = k;
		snprintf(tracks[k].path, sizeof(tracks[k].path), "%s", paths[i]);
		const char* sl = strrchr(paths[i], '/');
		snprintf(tracks[k].name, sizeof(tracks[k].name), "%s", sl ? sl + 1 : paths[i]);
		tracks[k].format = MusicFormat_detect(paths[i]);
		k++;
	}
	if (k == 0) {
		say("Download first: X downloads it all");
		return;
	}
	if (shuffle)
		at = rand() % k;
	PlayerModule_runWithPlaylist(*sp, tracks, k, at);
	SDL_Surface* ns = DisplayHelper_getReinitScreen();
	if (ns)
		*sp = ns;
}

// ================= covers (raw pictures, a few kept) =================
#define COVER_SLOTS 24
static struct {
	char path[640];
	SDL_Surface* pic;
	uint32_t used;
} covers[COVER_SLOTS];
static uint32_t cover_clock;
static SDL_Surface* coverPic(const char* path) {
	if (access(path, R_OK) != 0)
		return NULL;
	cover_clock++;
	int old = 0;
	for (int i = 0; i < COVER_SLOTS; i++) {
		if (covers[i].pic && !strcmp(covers[i].path, path)) {
			covers[i].used = cover_clock;
			return covers[i].pic;
		}
		if (covers[i].used < covers[old].used)
			old = i;
	}
	if (covers[old].pic) {
		BOS_picForget(covers[old].pic);
		SDL_FreeSurface(covers[old].pic);
	}
	snprintf(covers[old].path, sizeof(covers[old].path), "%s", path);
	covers[old].pic = IMG_Load(path);
	covers[old].used = cover_clock;
	return covers[old].pic;
}

static SDL_Surface* serverCover(const char* cover) {
	if (!cover || !cover[0])
		return NULL;
	char path[640], url[1024], safe[120];
	Online_safeName(cover, safe, sizeof(safe));
	snprintf(path, sizeof(path), SDCARD_PATH "/.cache/music-server/%s.jpg", safe);
	if (access(path, R_OK) != 0) {
		Server_coverUrl(cover, 300, url, sizeof(url));
		Online_cover(url, path);
		return NULL;
	}
	return coverPic(path);
}

static void drawCover(SDL_Surface* s, SDL_Surface* raw, const char* name, int x, int y, int size, int r) {
	SDL_Surface* p = raw ? BOS_picRounded(raw, size, size, r) : NULL;
	if (p) {
		BOS_blit(p, s, x, y, 1);
		return;
	}
	static const uint32_t pal[] = {0x5E5CE6, 0xFF375F, 0x30B0C7, 0xFF9F0A, 0x30D158, 0xBF5AF2};
	unsigned h = 0;
	for (const char* q = name ? name : ""; *q; q++)
		h = h * 31 + (unsigned char)*q;
	BOS_rrect(s, x, y, size, size, r, pal[h % 6]);
	int ic = size / 2;
	BOS_icon(s, "music", 0xFFFFFF, 0.9f, x + (size - ic) / 2, y + (size - ic) / 2, ic);
}

// ================= own server: setup =================
static int setup_sel;
static BosGlide su_pill, su_scroll;
static char su_status[160];
static bool su_ok;
static void setupRow(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	ServerCfg* c = Server_cfg();
	static char masked[64];
	switch (i) {
	case 0:
		r->label = I18N_text("Server address"), r->sq_icon = "link", r->sq_color = 0x30B0C7;
		r->annotation = c->url[0] ? c->url : I18N_text("Not set"), r->chevron = true;
		break;
	case 1:
		r->label = I18N_text("User name"), r->sq_icon = "user", r->sq_color = 0x8E8E93;
		r->annotation = c->user[0] ? c->user : I18N_text("Not set"), r->chevron = true;
		break;
	case 2: {
		int n = (int)strlen(c->pass);
		n = n > 12 ? 12 : n;
		for (int k = 0; k < n; k++)
			memcpy(masked + k * 3, "\xE2\x80\xA2", 3);
		masked[n * 3] = 0;
		r->label = I18N_text("Password"), r->sq_icon = "key", r->sq_color = 0xFF9F0A;
		r->annotation = n ? masked : I18N_text("Not set"), r->chevron = true;
		break;
	}
	default:
		r->label = I18N_text("Test the connection"), r->sq_icon = "cloud", r->sq_color = 0x30D158;
		r->annotation = su_status[0] ? su_status : NULL;
		break;
	}
}

static void setupPage(SDL_Surface** sp) {
	bool dirty = true;
	IndicatorType show = INDICATOR_NONE;
	while (1) {
		GFX_startFrame();
		PAD_poll();
		SDL_Surface* s = *sp;
		if (PAD_justPressed(BTN_B))
			return;
		if (PAD_wrapNav(&setup_sel, 4, BTN_UP, BTN_DOWN))
			dirty = 1;
		else if (PAD_justPressed(BTN_A)) {
			ServerCfg* c = Server_cfg();
			if (setup_sel < 3) {
				static const char* prompts[] = {"Server address (e.g. 192.168.1.20:4533)", "User name", "Password"};
				char* fields[] = {c->url, c->user, c->pass};
				int sizes[] = {sizeof(c->url), sizeof(c->user), sizeof(c->pass)};
				char* v = UIKeyboard_openWith(I18N_text(prompts[setup_sel]), fields[setup_sel], 0);
				PAD_poll();
				PAD_reset();
				SDL_Surface* ns = DisplayHelper_getReinitScreen();
				if (ns)
					*sp = s = ns;
				if (v) {
					snprintf(fields[setup_sel], sizes[setup_sel], "%s", v);
					free(v);
					Server_save();
					su_status[0] = 0;
				}
			} else {
				busy(s, "Connecting...");
				char err[200];
				int songs = -1;
				su_ok = Server_ping(err, sizeof(err), &songs) == 0;
				if (su_ok && songs >= 0)
					snprintf(su_status, sizeof(su_status), I18N_text("Connected · %d songs"), songs);
				else if (su_ok)
					snprintf(su_status, sizeof(su_status), "%s", I18N_text("Connected"));
				else
					snprintf(su_status, sizeof(su_status), "%s", I18N_text(err));
			}
			dirty = 1;
		}
		ModuleCommon_PWR_update(&dirty, &show);
		if (BOS_glideMoving(&su_pill))
			dirty = 1;
		if (!dirty) {
			GFX_sync();
			continue;
		}
		dirty = 0;
		SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
		BOS_statusBar(s, I18N_text("Music server"));
		int top = BOS_px(BOS_BAR_H);
		int y = top + BOS_largeTitle(s, top, I18N_text("Music server"), I18N_text("Play your whole library from your computer or NAS over WiFi"));
		SDL_Rect a = {BOS_px(40), y, s->w - BOS_px(80), BOS_px(96) * 4};
		int sc = 0;
		BOS_groupRows(s, &a, BOS_px(96), 4, setup_sel, true, &sc, &su_pill, &su_scroll, false, setupRow, NULL);
		BOS_textWrap(s, I18N_text("Works with Navidrome, Subsonic, Airsonic and Gonic (install one on your computer first). The password stays on this device."),
					 26, 400, BOS_C_TEXTOFF, 1, BOS_px(68), y + BOS_px(96) * 4 + BOS_px(16), s->w - BOS_px(136), 2);
		BosHint h[2] = {{"A", I18N_text(setup_sel < 3 ? "Edit" : "Test")}, {"B", I18N_text("Back")}};
		BOS_hintBar(s, h, 2, NULL, 0);
		GFX_flip(s);
	}
}

// ================= own server: an album or a playlist =================
static SrvSong al_songs[DL_MAX];
static int al_n;
static SrvAlbum al_info;
static char al_paths[DL_MAX][640];
static char al_urls[DL_MAX][1024];
static void albumRow(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	SrvSong* so = &al_songs[i];
	r->label = so->title;
	static char num[8], dur[16];
	snprintf(num, sizeof(num), "%02d", so->track > 0 ? so->track : i + 1);
	r->sq_text = num, r->sq_color = 0x2C2C30, r->sq_raw = true;
	if (so->duration > 0) {
		snprintf(dur, sizeof(dur), "%d:%02d", so->duration / 60, so->duration % 60);
		r->annotation = dur;
	}
	int p = dlProgressOf(al_paths[i]);
	if (p >= 0)
		r->progress = p / 100.0f;
	else if (access(al_paths[i], R_OK) == 0)
		r->right_icon = "checkc", r->right_icon_colour = BOS_C_OK;
}

static void albumPage(SDL_Surface** sp, const SrvAlbum* al, bool playlist) {
	busy(*sp, "Loading...");
	al_n = playlist ? Server_playlist(al->id, al_songs, DL_MAX, &al_info) : Server_album(al->id, al_songs, DL_MAX, &al_info);
	if (al_n < 0) {
		say(Server_lastError());
		return;
	}
	if (!al_info.name[0])
		al_info = *al;
	for (int i = 0; i < al_n; i++) {
		SrvAlbum own = al_info;
		if (playlist) // a playlist's songs keep to their own albums
			snprintf(own.artist, sizeof(own.artist), "%s", al_songs[i].artist), snprintf(own.name, sizeof(own.name), "%s", al_songs[i].album);
		Server_songPath(&own, &al_songs[i], al_paths[i], sizeof(al_paths[i]));
		Server_downloadUrl(al_songs[i].id, al_urls[i], sizeof(al_urls[i]));
	}
	int sel = 0, scroll = 0;
	BosGlide pill = {0}, scr = {0};
	bool dirty = true;
	IndicatorType show = INDICATOR_NONE;
	uint32_t last_tick = 0;
	while (1) {
		GFX_startFrame();
		PAD_poll();
		SDL_Surface* s = *sp;
		if (PAD_justPressed(BTN_B))
			return;
		if (PAD_wrapNav(&sel, al_n, BTN_UP, BTN_DOWN) || PAD_pageNav(&sel, al_n, 5) > 0)
			dirty = 1;
		else if (PAD_justPressed(BTN_A) && al_n > 0) {
			if (fetchNow(s, al_urls[sel], al_paths[sel], al_songs[sel].title))
				playFiles(sp, al_paths, al_n, sel, false);
			else
				say("Download failed");
			dirty = 1;
		} else if (PAD_justPressed(BTN_X) && al_n > 0) {
			if (dlStart(al_info.id, al_urls, al_paths, al_n))
				say("Downloading the album");
			else
				say("Another download is running");
			dirty = 1;
		} else if (PAD_justPressed(BTN_Y) && al_n > 0) {
			playFiles(sp, al_paths, al_n, 0, true);
			dirty = 1;
		}
		ModuleCommon_PWR_update(&dirty, &show);
		if (BOS_glideMoving(&pill) || BOS_glideMoving(&scr) || BOS_marqueeActive())
			dirty = 1;
		if ((dl.run || toast[0]) && SDL_GetTicks() - last_tick > 500)
			last_tick = SDL_GetTicks(), dirty = 1;
		if (!dirty) {
			GFX_sync();
			continue;
		}
		dirty = 0;
		s = *sp;
		SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
		BOS_statusBar(s, I18N_text("Music server"));
		int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H), lx = BOS_px(40), y = top + BOS_px(8);
		int big = BOS_px(300);
		drawCover(s, serverCover(al_info.cover[0] ? al_info.cover : al->cover), al_info.name, lx, y, big, BOS_px(26));
		char t[200];
		int ty = y + big + BOS_px(18);
		BOS_fit(al_info.name, 40, 700, big, t, sizeof(t));
		BOS_text(s, t, 40, 700, BOS_C_TEXT, 1, lx, ty);
		ty += BOS_lineH(40);
		char line[200];
		if (al_info.year > 0)
			snprintf(line, sizeof(line), "%s · %d", al_info.artist, al_info.year);
		else
			snprintf(line, sizeof(line), "%s", al_info.artist);
		BOS_fit(line, 28, 400, big, t, sizeof(t));
		BOS_text(s, t, 28, 400, BOS_C_TEXT2, 1, lx, ty);
		ty += BOS_lineH(28);
		int have = 0;
		for (int i = 0; i < al_n; i++)
			have += access(al_paths[i], R_OK) == 0;
		snprintf(line, sizeof(line), I18N_text("Downloaded %d / %d"), have, al_n);
		int ph = BOS_px(40), pw = BOS_textW(line, 24, 600) + BOS_px(52);
		BOS_rrect(s, lx, ty + BOS_px(10), pw, ph, ph / 2, BOS_theme()->surface2);
		BOS_rrect(s, lx + BOS_px(16), ty + BOS_px(10) + ph / 2 - BOS_px(6), BOS_px(12), BOS_px(12), BOS_px(6), have == al_n && al_n ? BOS_C_OK : BOS_C_TEXTOFF);
		BOS_textMid(s, line, 24, 600, BOS_C_TEXT, 1, lx + BOS_px(36), ty + BOS_px(10) + ph / 2);
		int rx = lx + big + BOS_px(36);
		SDL_Rect a = {rx, y, s->w - BOS_px(40) - rx, bottom - BOS_px(12) - y};
		if (al_n > 0)
			BOS_groupRows(s, &a, BOS_px(84), al_n, sel, true, &scroll, &pill, &scr, false, albumRow, NULL);
		BosHint h[4] = {{"A", I18N_text("Play")}, {"B", I18N_text("Back")}, {"X", I18N_text("Download all")}, {"Y", I18N_text("Shuffle")}};
		BOS_hintBar(s, al_n > 0 ? h : h + 1, al_n > 0 ? 4 : 1, NULL, 0);
		UI_renderToast(s, toast, toast_at);
		if (toast[0] && SDL_GetTicks() - toast_at > 2500)
			toast[0] = 0, UI_clearToast();
		GFX_flip(s);
	}
}

// ================= own server: the library (tabs) =================
#define LIB_MAX 300
static SrvAlbum lib_albums[LIB_MAX];
static int lib_nalbums = -2;
static SrvArtist lib_artists[LIB_MAX];
static int lib_nartists = -2;
static SrvAlbum lib_lists[LIB_MAX];
static int lib_nlists = -2;

static void artistRow(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	static char sub[48];
	r->label = lib_artists[i].name;
	r->sq_text = lib_artists[i].name, r->sq_raw = true;
	snprintf(sub, sizeof(sub), I18N_text("%d albums"), lib_artists[i].albums);
	r->sub = sub, r->chevron = true;
}
static void listRow(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	static char sub[48];
	r->label = lib_lists[i].name;
	r->sq_icon = "list", r->sq_color = 0xFF375F;
	snprintf(sub, sizeof(sub), I18N_text("%d songs"), lib_lists[i].songs);
	r->sub = sub, r->chevron = true;
}

// albums as a grid of covers, 4 across (board v2-MusicServer)
static BosGlide gr_scroll;
static void albumGrid(SDL_Surface* s, const SrvAlbum* al, int n, int sel, int y0, int bottom, int* scroll_px) {
	const BosTheme* th = BOS_theme();
	int cols = 4, size = BOS_px(200), gap = BOS_px(28), x0 = BOS_px(40) + BOS_px(6);
	int rowh = size + BOS_px(12) + BOS_lineH(26) + BOS_lineH(22) + BOS_px(18);
	int sr = sel / cols, view = bottom - y0 - BOS_px(10);
	if (sr * rowh < *scroll_px)
		*scroll_px = sr * rowh;
	if ((sr + 1) * rowh > *scroll_px + view)
		*scroll_px = (sr + 1) * rowh - view;
	float fsc = BOS_glideTo(&gr_scroll, (float)*scroll_px, 180, false);
	SDL_Rect clip = {0, y0, s->w, bottom - y0};
	SDL_SetClipRect(s, &clip);
	for (int i = 0; i < n; i++) {
		int x = x0 + (i % cols) * (size + gap), y = y0 + BOS_px(10) + (i / cols) * rowh - (int)fsc;
		if (y + rowh < y0 || y > bottom)
			continue;
		bool hot = i == sel;
		if (hot)
			BOS_rrect(s, x - BOS_px(5), y - BOS_px(5), size + BOS_px(10), size + BOS_px(10), BOS_px(26), th->focus);
		drawCover(s, serverCover(al[i].cover), al[i].name, x, y, size, BOS_px(22));
		char t[200];
		BOS_fit(al[i].name, 26, hot ? 600 : 500, size, t, sizeof(t));
		BOS_text(s, t, 26, hot ? 600 : 500, BOS_C_TEXT, 1, x, y + size + BOS_px(12));
		BOS_fit(al[i].artist, 22, 400, size, t, sizeof(t));
		BOS_text(s, t, 22, 400, BOS_C_TEXT2, 1, x, y + size + BOS_px(12) + BOS_lineH(26));
	}
	SDL_SetClipRect(s, NULL);
}

static void loadTab(SDL_Surface* s, int tab) {
	if (tab == 1 && lib_nalbums == -2) {
		busy(s, "Loading...");
		lib_nalbums = Server_albums(lib_albums, LIB_MAX);
	} else if (tab == 0 && lib_nartists == -2) {
		busy(s, "Loading...");
		lib_nartists = Server_artists(lib_artists, LIB_MAX);
	} else if (tab == 2 && lib_nlists == -2) {
		busy(s, "Loading...");
		lib_nlists = Server_playlists(lib_lists, LIB_MAX);
	}
}

ModuleExitReason ServerModule_run(SDL_Surface* screen) {
	if (!Server_configured()) {
		setupPage(&screen);
		if (!Server_configured())
			return MODULE_EXIT_TO_MENU;
	}
	static int tab = 1;
	int sel[4] = {0}, scroll[4] = {0};
	BosGlide pill = {0}, scr = {0};
	lib_nalbums = lib_nartists = lib_nlists = -2;
	bool dirty = true;
	IndicatorType show = INDICATOR_NONE;
	// a sub-list (an artist's albums) shown in the albums grid
	static SrvAlbum sub_albums[LIB_MAX];
	int sub_n = -1, sub_sel = 0, sub_scroll = 0;
	char sub_title[160] = "";
	loadTab(screen, tab);
	while (1) {
		GFX_startFrame();
		PAD_poll();
		ContextMenuItem ctx[2];
		int nctx = 0;
		ContextMenu_addItem(ctx, &nctx, "Server settings", 1);
		ContextMenu_addItem(ctx, &nctx, "Quit App", CTX_ID_QUIT);
		GlobalInputResult g = ModuleCommon_handleGlobalInput(screen, &show, 0, ctx, nctx);
		if (g.should_quit)
			return MODULE_EXIT_QUIT;
		if (g.context_id == 1) {
			setupPage(&screen);
			lib_nalbums = lib_nartists = lib_nlists = -2;
			loadTab(screen, tab);
			dirty = 1;
		}
		if (g.input_consumed) {
			if (g.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}
		bool grid = sub_n >= 0 || tab == 1;
		int n = sub_n >= 0 ? sub_n : tab == 0 ? lib_nartists : tab == 1 ? lib_nalbums : tab == 2 ? lib_nlists : 1;
		if (n < 0)
			n = 0;
		int* cur = sub_n >= 0 ? &sub_sel : &sel[tab];
		int old = *cur;
		if (PAD_justPressed(BTN_B)) {
			if (sub_n >= 0) {
				sub_n = -1, dirty = 1;
				continue;
			}
			return MODULE_EXIT_TO_MENU;
		}
		if ((PAD_justPressed(BTN_L1) || PAD_justPressed(BTN_R1)) && sub_n < 0) {
			tab = (tab + (PAD_justPressed(BTN_R1) ? 1 : 3)) % 4;
			loadTab(screen, tab);
			dirty = 1;
			continue;
		}
		if (grid) {
			PAD_gridNav(cur, n, 4);
		} else
			PAD_wrapNav(cur, n, BTN_UP, BTN_DOWN);
		PAD_pageNav(cur, n, grid ? 8 : 5); // L2 / R2: a screen (DESIGN 7.7)
		if (PAD_justPressed(BTN_A)) {
			if (tab == 3 && sub_n < 0) { // ดาวน์โหลดแล้ว: the downloaded files
				Online_mkdirs(ONLINE_DIR);
				ModuleExitReason r = PlayerModule_runAt(screen, ONLINE_DIR);
				if (r == MODULE_EXIT_QUIT)
					return r;
			} else if (n > 0) {
				if (sub_n >= 0)
					albumPage(&screen, &sub_albums[*cur], false);
				else if (tab == 1)
					albumPage(&screen, &lib_albums[*cur], false);
				else if (tab == 2)
					albumPage(&screen, &lib_lists[*cur], true);
				else if (tab == 0) {
					busy(screen, "Loading...");
					sub_n = Server_artistAlbums(lib_artists[*cur].id, sub_albums, LIB_MAX);
					snprintf(sub_title, sizeof(sub_title), "%s", lib_artists[*cur].name);
					if (sub_n < 0)
						say(Server_lastError()), sub_n = -1;
					sub_sel = 0, sub_scroll = 0;
				}
			}
			dirty = 1;
		}
		if (*cur != old)
			dirty = 1;
		ModuleCommon_PWR_update(&dirty, &show);
		static uint32_t tick;
		if (BOS_glideMoving(&pill) || BOS_glideMoving(&scr) || BOS_glideMoving(&gr_scroll) || BOS_marqueeActive())
			dirty = 1;
		if (SDL_GetTicks() - tick > 700) // covers arrive in the background
			tick = SDL_GetTicks(), dirty = 1;
		if (!dirty) {
			GFX_sync();
			continue;
		}
		dirty = 0;
		SDL_Surface* s = screen;
		SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
		BOS_statusBar(s, I18N_text("Music server"));
		int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
		char sub[200];
		const char* host = strstr(Server_cfg()->url, "://") ? strstr(Server_cfg()->url, "://") + 3 : Server_cfg()->url;
		if (sub_n >= 0)
			snprintf(sub, sizeof(sub), I18N_text("%d albums"), sub_n);
		else
			snprintf(sub, sizeof(sub), I18N_text("%s · %d albums · newest first"), host, lib_nalbums > 0 ? lib_nalbums : 0);
		int y = top + BOS_largeTitle(s, top, sub_n >= 0 ? sub_title : I18N_text("Music server"), sub);
		if (sub_n < 0) { // the tabs
			static const char* tabs[] = {"Artists", "Albums", "Playlists", "Downloaded"};
			int x = BOS_px(40), h = BOS_px(52);
			for (int i = 0; i < 4; i++) {
				const char* t = I18N_text(tabs[i]);
				int w = BOS_textW(t, 28, 600) + BOS_px(48);
				BOS_rrect(s, x, y, w, h, h / 2, i == tab ? BOS_theme()->focus : BOS_theme()->surface);
				BOS_textMid(s, t, 28, 600, i == tab ? BOS_theme()->onfocus : BOS_C_TEXT2, 1, x + BOS_px(24), y + h / 2);
				x += w + BOS_px(12);
			}
			y += h + BOS_px(16);
		}
		if (grid) {
			const SrvAlbum* list = sub_n >= 0 ? sub_albums : lib_albums;
			if (n > 0)
				albumGrid(s, list, n, *cur, y, bottom - BOS_px(4), sub_n >= 0 ? &sub_scroll : &scroll[1]);
			else
				BOS_textWrapCentre(s, I18N_text(lib_nalbums == -1 ? Server_lastError() : "No albums"), 32, 600, BOS_C_TEXT2, 1,
								   BOS_px(80), y + BOS_px(80), s->w - BOS_px(160), 2);
		} else if (tab == 0 || tab == 2) {
			SDL_Rect a = {BOS_px(40), y, s->w - BOS_px(80), bottom - BOS_px(12) - y};
			if (n > 0)
				BOS_groupRows(s, &a, BOS_px(96), n, *cur, true, &scroll[tab], &pill, &scr, false,
							  tab == 0 ? artistRow : listRow, NULL);
			else
				BOS_textWrapCentre(s, I18N_text(tab == 0 ? "No artists" : "No playlists"), 32, 600, BOS_C_TEXT2, 1, BOS_px(80),
								   y + BOS_px(80), s->w - BOS_px(160), 2);
		} else { // ดาวน์โหลดแล้ว
			BOS_textWrap(s, I18N_text("Songs you downloaded are in Music files › ดาวน์โหลด and play without the internet. A opens them."),
						 30, 400, BOS_C_TEXT2, 1, BOS_px(48), y + BOS_px(20), s->w - BOS_px(96), 3);
		}
		BosHint h[2] = {{"A", I18N_text("Open")}, {"B", I18N_text("Back")}}, l[1] = {{"L1 R1", I18N_text("Tabs")}};
		BOS_hintBar(s, h, 2, sub_n < 0 ? l : NULL, sub_n < 0 ? 1 : 0);
		UI_renderToast(s, toast, toast_at);
		if (toast[0] && SDL_GetTicks() - toast_at > 2500)
			toast[0] = 0, UI_clearToast();
		GFX_flip(s);
	}
}

// ================= free music (Internet Archive) =================
#define ARC_MAX 60
static ArcItem arc[ARC_MAX];
static int arc_n = -2;
static const char* genres[][2] = {{"All", ""}, {"Lo-fi", "lofi"}, {"Jazz", "jazz"}, {"Classical", "classical"},
								  {"Rock", "rock"}, {"Electronic", "electronic"}};
#define NGENRES 6

static void arcRow(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	ArcItem* it = &arc[i];
	r->label = it->title;
	r->sub = it->creator;
	r->sq_text = it->title, r->sq_raw = true;
	if (it->license[0])
		r->pill = I18N_text(it->license);
	char dir[640];
	Archive_itemDir(it, dir, sizeof(dir));
	if (dl.run && !strcmp(dl.tag, it->id))
		r->progress = dl.n ? (dl.done + (dl.job.progress / 100.0f)) / dl.n : 0.02f;
	else if (access(dir, F_OK) == 0)
		r->right_icon = "checkc", r->right_icon_colour = BOS_C_OK;
}

static int arcFiles(const ArcItem* it, char (*urls)[1024], char (*paths)[640]) {
	static char names[DL_MAX][200];
	int n = Archive_mp3s(it->id, names, DL_MAX);
	char dir[640];
	Archive_itemDir(it, dir, sizeof(dir));
	for (int i = 0; i < n; i++) {
		Archive_fileUrl(it->id, names[i], urls[i], 1024);
		char safe[200];
		Online_safeName(names[i], safe, sizeof(safe));
		snprintf(paths[i], 640, "%s/%s", dir, safe);
	}
	return n;
}

ModuleExitReason FreeModule_run(SDL_Surface* screen) {
	static int genre = 0;
	static char query[120];
	int sel = 0, scroll = 0;
	BosGlide pill = {0}, scr = {0};
	bool dirty = true, reload = true;
	IndicatorType show = INDICATOR_NONE;
	static char urls[DL_MAX][1024], paths[DL_MAX][640];
	while (1) {
		if (reload) {
			reload = false;
			busy(screen, "Loading...");
			arc_n = Archive_search(genres[genre][1], query, arc, ARC_MAX);
			sel = 0, scroll = 0;
			dirty = 1;
		}
		GFX_startFrame();
		PAD_poll();
		ContextMenuItem ctx[1];
		int nctx = 0;
		ContextMenu_addItem(ctx, &nctx, "Quit App", CTX_ID_QUIT);
		GlobalInputResult g = ModuleCommon_handleGlobalInput(screen, &show, 0, ctx, nctx);
		if (g.should_quit)
			return MODULE_EXIT_QUIT;
		if (g.input_consumed) {
			if (g.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}
		int n = arc_n > 0 ? arc_n : 0, old = sel;
		if (PAD_justPressed(BTN_B)) {
			if (query[0]) {
				query[0] = 0, reload = true;
				continue;
			}
			return MODULE_EXIT_TO_MENU;
		}
		if (PAD_justPressed(BTN_L1) || PAD_justPressed(BTN_R1)) {
			genre = (genre + (PAD_justPressed(BTN_R1) ? 1 : NGENRES - 1)) % NGENRES;
			reload = true;
			continue;
		}
		if (PAD_wrapNav(&sel, n, BTN_UP, BTN_DOWN) || PAD_pageNav(&sel, n, 5) > 0)
			;
		else if (PAD_justPressed(BTN_Y)) {
			char* v = UIKeyboard_openWith(I18N_text("Search free music"), query, 0);
			PAD_poll();
			PAD_reset();
			SDL_Surface* ns = DisplayHelper_getReinitScreen();
			if (ns)
				screen = ns;
			if (v) {
				snprintf(query, sizeof(query), "%s", v);
				free(v);
				reload = true;
				continue;
			}
			dirty = 1;
		} else if (PAD_justPressed(BTN_A) && n > 0) {
			busy(screen, "Loading...");
			int k = arcFiles(&arc[sel], urls, paths);
			if (k <= 0)
				say("No songs to play in this one");
			else {
				int first = 0;
				for (int i = 0; i < k; i++)
					if (access(paths[i], R_OK) == 0) {
						first = i;
						break;
					}
				if (access(paths[first], R_OK) == 0 || fetchNow(screen, urls[first], paths[first], arc[sel].title))
					playFiles(&screen, paths, k, first, false);
				else
					say("Download failed");
			}
			dirty = 1;
		} else if (PAD_justPressed(BTN_X) && n > 0) {
			busy(screen, "Loading...");
			int k = arcFiles(&arc[sel], urls, paths);
			if (k > 0 && dlStart(arc[sel].id, urls, paths, k))
				say("Downloading");
			else
				say(k > 0 ? "Another download is running" : "No songs to play in this one");
			dirty = 1;
		}
		if (sel != old)
			dirty = 1;
		ModuleCommon_PWR_update(&dirty, &show);
		static uint32_t tick;
		if (BOS_glideMoving(&pill) || BOS_glideMoving(&scr) || BOS_marqueeActive())
			dirty = 1;
		if ((dl.run || toast[0]) && SDL_GetTicks() - tick > 500)
			tick = SDL_GetTicks(), dirty = 1;
		if (!dirty) {
			GFX_sync();
			continue;
		}
		dirty = 0;
		SDL_Surface* s = screen;
		SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
		BOS_statusBar(s, I18N_text("Free music"));
		int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
		char sub[200];
		if (query[0])
			snprintf(sub, sizeof(sub), I18N_text("Results for \"%s\""), query);
		else
			snprintf(sub, sizeof(sub), "%s", I18N_text("Free to play and keep · mostly independent artists"));
		int y = top + BOS_largeTitle(s, top, I18N_text("Free music"), sub);
		{ // the source, then the genres (L1 R1)
			int x = BOS_px(40), h = BOS_px(52);
			const char* src = "Internet Archive";
			int w = BOS_textW(src, 28, 600) + BOS_px(48);
			BOS_rrect(s, x, y, w, h, h / 2, BOS_theme()->surface2);
			BOS_textMid(s, src, 28, 600, BOS_C_TEXT, 1, x + BOS_px(24), y + h / 2);
			x += w + BOS_px(10);
			SDL_FillRect(s, &(SDL_Rect){x + BOS_px(6), y + BOS_px(9), BOS_px(2), h - BOS_px(18)}, BOS_map(s, BOS_C_SEPARATOR));
			x += BOS_px(22);
			for (int i = 0; i < NGENRES && x < s->w - BOS_px(40); i++) {
				const char* t = I18N_text(genres[i][0]);
				int gw = BOS_textW(t, 24, 600) + BOS_px(32), gh = BOS_px(40);
				if (x + gw > s->w - BOS_px(40))
					break;
				BOS_rrect(s, x, y + (h - gh) / 2, gw, gh, gh / 2, i == genre ? BOS_theme()->focus : BOS_theme()->surface);
				BOS_textMid(s, t, 24, 600, i == genre ? BOS_theme()->onfocus : BOS_C_TEXT2, 1, x + BOS_px(16), y + h / 2);
				x += gw + BOS_px(10);
			}
			y += h + BOS_px(16);
		}
		if (n > 0) {
			SDL_Rect a = {BOS_px(40), y, s->w - BOS_px(80), bottom - BOS_px(12) - y};
			BOS_groupRows(s, &a, BOS_px(96), n, sel, true, &scroll, &pill, &scr, false, arcRow, NULL);
		} else
			BOS_textWrapCentre(s, I18N_text(arc_n < 0 ? Server_lastError() : "No results found"), 32, 600, BOS_C_TEXT2, 1,
							   BOS_px(80), y + BOS_px(80), s->w - BOS_px(160), 2);
		BosHint h[4] = {{"A", I18N_text("Listen")}, {"B", I18N_text("Back")}, {"X", I18N_text("Download")}, {"Y", I18N_text("Search")}},
				l[1] = {{"L1 R1", I18N_text("Genre")}};
		BOS_hintBar(s, n > 0 ? h : h + 1, n > 0 ? 4 : 3, l, 1);
		UI_renderToast(s, toast, toast_at);
		if (toast[0] && SDL_GetTicks() - toast_at > 2500)
			toast[0] = 0, UI_clearToast();
		GFX_flip(s);
	}
}
