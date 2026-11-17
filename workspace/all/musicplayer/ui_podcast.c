#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <pthread.h>

#include "platform.h"
#include "../common/defines.h"
#include "../common/utils.h"
#include "../common/api.h"
#include "../common/sdl.h"
#include "../common/ui_list_layout.h"
#include "../common/ui/ui_buttonhintbar.h"
#include "../common/ui/ui_emptystate.h"
#include "../common/ui/ui_menubar.h"
#include "podcast.h"
#include "music_client.h"
#include "ui_podcast.h"
#include "../common/ui/ui_list.h"
#include "../common/ui/ui_toast.h"
#include "ui_icons.h"
#include "ui_album_art.h"
#include "../common/ui/ui_image.h"
#include "../common/wget_fetch.h"
#include <unistd.h>
#include "../common/ui/ui_draw.h"
#include "../common/ui/ui_listview.h"
#include "i18n.h"
#include "music_bos.h"

// Brick OS pages (ui_podcast_bos part, at the end of this file)
static void bos_main(SDL_Surface* s, int selected, const char* toast, uint32_t toast_time);
static void bos_manage(SDL_Surface* s, int sel);
static void bos_shows(SDL_Surface* s, bool charts, int sel, const char* toast, uint32_t toast_time);
static void bos_episodes(SDL_Surface* s, int feed_index, int sel, int* scroll, const char* toast, uint32_t toast_time);
static void bos_queue(SDL_Surface* s, int sel, int* scroll, const char* toast, uint32_t toast_time);
static void bos_playing(SDL_Surface* s, int feed_index, int episode_index);
static int bos_play_second;
bool UIPodcast_bosMoving(void);
bool UIPodcast_bosListMoving(void);
void BOS_uiLoading(SDL_Surface* s, const char* title, const char* subtitle);

// Max artwork size (1MB to match radio album art buffer)
#define PODCAST_ARTWORK_MAX_SIZE (1024 * 1024)

// Scroll state for selected item title in lists
static ScrollTextState podcast_title_scroll = {0};

// Scroll state for playing screen episode title
static ScrollTextState podcast_playing_title_scroll = {0};

// Podcast artwork state
static SDL_Surface* podcast_artwork = NULL;
static char podcast_artwork_url[512] = {0};

// Podcast progress GPU state
static int progress_bar_x = 0, progress_bar_y = 0;
static int progress_bar_w = 0, progress_bar_h = 0;
static int progress_time_y = 0;
static int progress_screen_w = 0;
static int progress_duration_ms = 0;
static int progress_last_position_sec = -1;
static bool progress_position_set = false;

// Helper to convert surface to ARGB8888 for proper scaling
static SDL_Surface* convert_to_argb8888(SDL_Surface* src) {
	if (!src)
		return NULL;

	SDL_Surface* converted = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(src);
	return converted;
}

// --- Async artwork downloader ---------------------------------------------
// Podcast artwork/thumbnails used to be fetched with a blocking wget() directly
// inside the render functions, freezing the UI for the download timeout (and,
// on failure, re-fetching every single frame). This worker downloads one image
// at a time to its disk-cache path; the render side keeps its fast memory/disk
// cache lookups and just requests a download when the file isn't there yet.
#define ARTWORK_FAILED_MAX 64
static pthread_t artwork_thread;
static volatile bool artwork_thread_active = false;
static pthread_mutex_t artwork_mutex = PTHREAD_MUTEX_INITIALIZER;
static char artwork_req_url[PODCAST_MAX_URL];
static char artwork_req_path[768];
static char artwork_failed[ARTWORK_FAILED_MAX][PODCAST_MAX_URL];
static int artwork_failed_count = 0;

static bool artwork_url_failed(const char* url) {
	for (int i = 0; i < artwork_failed_count; i++)
		if (strcmp(artwork_failed[i], url) == 0)
			return true;
	return false;
}

static void artwork_mark_failed(const char* url) {
	if (artwork_url_failed(url))
		return;
	if (artwork_failed_count < ARTWORK_FAILED_MAX) {
		snprintf(artwork_failed[artwork_failed_count++], PODCAST_MAX_URL, "%s", url);
	} else {
		memmove(artwork_failed[0], artwork_failed[1],
				sizeof(artwork_failed[0]) * (ARTWORK_FAILED_MAX - 1));
		snprintf(artwork_failed[ARTWORK_FAILED_MAX - 1], PODCAST_MAX_URL, "%s", url);
	}
}

static void* artwork_fetch_thread(void* arg) {
	(void)arg;
	PWR_pinToCores(CPU_CORE_EFFICIENCY);

	char url[PODCAST_MAX_URL], path[768];
	pthread_mutex_lock(&artwork_mutex);
	snprintf(url, sizeof(url), "%s", artwork_req_url);
	snprintf(path, sizeof(path), "%s", artwork_req_path);
	pthread_mutex_unlock(&artwork_mutex);

	static uint8_t buf[PODCAST_ARTWORK_MAX_SIZE]; // only this (single) worker uses it
	int n = wget_fetch(url, buf, PODCAST_ARTWORK_MAX_SIZE);
	bool ok = (n > 0 && UI_imageDataComplete(buf, n));
	if (ok) {
		// Ensure the parent directory exists so a missing dir isn't recorded as
		// a permanent failure.
		char* slash = strrchr(path, '/');
		if (slash) {
			*slash = '\0';
			mkdir(path, 493);
			*slash = '/';
		}
		FILE* f = fopen(path, "wb");
		if (f) {
			fwrite(buf, 1, n, f);
			fclose(f);
		} else {
			ok = false;
		}
	}

	pthread_mutex_lock(&artwork_mutex);
	if (!ok)
		artwork_mark_failed(url);
	artwork_thread_active = false;
	pthread_mutex_unlock(&artwork_mutex);
	return NULL;
}

// Request an async download of `url` → `cache_path`. Returns immediately; the
// file appears when done. No-op if a download is already running or the URL
// previously failed (so we never re-hammer a dead URL every frame).
static void artwork_request_async(const char* url, const char* cache_path) {
	if (!url || !url[0] || !cache_path || !cache_path[0])
		return;
	pthread_mutex_lock(&artwork_mutex);
	if (artwork_thread_active || artwork_url_failed(url)) {
		pthread_mutex_unlock(&artwork_mutex);
		return;
	}
	snprintf(artwork_req_url, sizeof(artwork_req_url), "%s", url);
	snprintf(artwork_req_path, sizeof(artwork_req_path), "%s", cache_path);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&artwork_thread, &attr, artwork_fetch_thread, NULL) == 0)
		artwork_thread_active = true;
	pthread_attr_destroy(&attr);
	pthread_mutex_unlock(&artwork_mutex);
}

bool UIPodcast_artworkBusy(void) {
	return artwork_thread_active;
}

// Fetch podcast artwork from URL (cached in podcast folder)
// feed_id: the podcast's feed_id for storing artwork in its folder
static void podcast_fetch_artwork(const char* artwork_url, const char* feed_id) {
	if (!artwork_url || !artwork_url[0] || !feed_id || !feed_id[0])
		return;

	// Already have exactly this artwork loaded
	if (strcmp(podcast_artwork_url, artwork_url) == 0 && podcast_artwork)
		return;

	// A different episode/feed's artwork is loaded — drop it
	if (podcast_artwork) {
		SDL_FreeSurface(podcast_artwork);
		podcast_artwork = NULL;
		cleanup_album_art_background();
	}
	podcast_artwork_url[0] = '\0'; // set only once artwork is actually loaded

	// Build cache path: <podcast_data_dir>/<feed_id>/artwork.jpg
	char feed_dir[512];
	Podcast_getFeedDataPath(feed_id, feed_dir, sizeof(feed_dir));

	char cache_path[768];
	snprintf(cache_path, sizeof(cache_path), "%s/artwork.jpg", feed_dir);

	// Try the disk cache (fast, synchronous)
	FILE* f = fopen(cache_path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);

		if (size > 0 && size < PODCAST_ARTWORK_MAX_SIZE) {
			uint8_t* data = (uint8_t*)malloc(size);
			if (data && fread(data, 1, size, f) == (size_t)size) {
				if (UI_imageDataComplete(data, size)) {
					SDL_RWops* rw = SDL_RWFromConstMem(data, size);
					if (rw) {
						SDL_Surface* loaded = IMG_Load_RW(rw, 1);
						podcast_artwork = convert_to_argb8888(loaded);
					}
				}
			}
			free(data);
		}
		fclose(f);
		if (podcast_artwork) {
			snprintf(podcast_artwork_url, sizeof(podcast_artwork_url), "%s", artwork_url);
			return;
		}
		// Cached file is corrupt/incomplete — delete it so we re-fetch
		remove(cache_path);
	}

	// Not cached — download in the background (off the render thread). The disk
	// path above will pick it up on a later frame once the worker finishes.
	artwork_request_async(artwork_url, cache_path);
}

// Clear podcast artwork (call when leaving playing screen)
void Podcast_clearArtwork(void) {
	if (podcast_artwork) {
		SDL_FreeSurface(podcast_artwork);
		podcast_artwork = NULL;
	}
	podcast_artwork_url[0] = '\0';
	memset(&podcast_playing_title_scroll, 0, sizeof(podcast_playing_title_scroll));
	PodcastProgress_clear(); // Clear GPU progress layer
}

// Thumbnail cache for subscription artwork on main page
#define THUMBNAIL_CACHE_SIZE 64
typedef struct {
	// Holds a feed_id OR an itunes_id (up to 32 chars). Must be large enough to
	// store the full key — a truncated key never matches the full-string lookup,
	// causing a per-frame re-decode + FIFO thrash.
	char feed_id[64];
	SDL_Surface* thumbnail;
} ThumbnailCacheEntry;
static ThumbnailCacheEntry thumbnail_cache[THUMBNAIL_CACHE_SIZE];
static int thumbnail_cache_count = 0;

// Load image file from disk path, scale to size x size, apply circular mask
// Deletes corrupt/incomplete files so they get re-fetched
static SDL_Surface* load_circular_thumbnail(const char* path, int size) {
	SDL_Surface* raw = UI_loadValidatedImage(path, PODCAST_ARTWORK_MAX_SIZE);
	if (!raw)
		return NULL;

	SDL_Surface* result = UI_circleFromSurface(raw, size);
	SDL_FreeSurface(raw);
	return result;
}

// Add thumbnail to in-memory cache (FIFO eviction)
static void cache_thumbnail(const char* cache_key, SDL_Surface* surface) {
	if (thumbnail_cache_count >= THUMBNAIL_CACHE_SIZE) {
		SDL_FreeSurface(thumbnail_cache[0].thumbnail);
		for (int i = 0; i < THUMBNAIL_CACHE_SIZE - 1; i++) {
			thumbnail_cache[i] = thumbnail_cache[i + 1];
		}
		thumbnail_cache_count = THUMBNAIL_CACHE_SIZE - 1;
	}
	ThumbnailCacheEntry* entry = &thumbnail_cache[thumbnail_cache_count];
	strncpy(entry->feed_id, cache_key, sizeof(entry->feed_id) - 1);
	entry->feed_id[sizeof(entry->feed_id) - 1] = '\0';
	entry->thumbnail = surface;
	thumbnail_cache_count++;
}

// Look up thumbnail in memory cache
static SDL_Surface* find_cached_thumbnail(const char* cache_key) {
	for (int i = 0; i < thumbnail_cache_count; i++) {
		if (strcmp(thumbnail_cache[i].feed_id, cache_key) == 0 && thumbnail_cache[i].thumbnail) {
			return thumbnail_cache[i].thumbnail;
		}
	}
	return NULL;
}

// Lazy load one subscription thumbnail from disk (call once per frame)
// Returns true if a thumbnail was loaded (caller should redraw)
static bool subscription_thumb_load_one(const char* feed_id, int size) {
	if (!feed_id || !feed_id[0] || size <= 0)
		return false;

	// Already in memory cache
	if (find_cached_thumbnail(feed_id))
		return false;

	// Load from disk: <feed_data_dir>/artwork.jpg
	char feed_dir[512];
	Podcast_getFeedDataPath(feed_id, feed_dir, sizeof(feed_dir));
	char art_path[768];
	snprintf(art_path, sizeof(art_path), "%s/artwork.jpg", feed_dir);

	SDL_Surface* thumb = load_circular_thumbnail(art_path, size);
	if (thumb) {
		cache_thumbnail(feed_id, thumb);
		return true;
	}
	return false;
}

// Disk cache path for artwork by itunes_id
#define PODCAST_CACHE_PARENT SDCARD_PATH "/.cache"
#define PODCAST_CACHE_DIR SDCARD_PATH "/.cache/podcast"

static void get_artwork_cache_path(const char* itunes_id, char* path, int path_size) {
	snprintf(path, path_size, PODCAST_CACHE_DIR "/%s.jpg", itunes_id);
}

void Podcast_clearThumbnailCache(void) {
	for (int i = 0; i < thumbnail_cache_count; i++) {
		if (thumbnail_cache[i].thumbnail) {
			SDL_FreeSurface(thumbnail_cache[i].thumbnail);
			thumbnail_cache[i].thumbnail = NULL;
		}
		thumbnail_cache[i].feed_id[0] = '\0';
	}
	thumbnail_cache_count = 0;
}

bool Podcast_loadPendingThumbnails(void) {
	int sub_count = Podcast_getSubscriptionCount();
	if (sub_count == 0)
		return false;

	PodcastFeed* feeds = Podcast_getSubscriptions(NULL);
	int thumb_size = SCALE1(PILL_SIZE) * 3 / 2 - SCALE1(4) * 2;

	for (int i = 0; i < sub_count; i++) {
		if (subscription_thumb_load_one(feeds[i].feed_id, thumb_size))
			return true;
	}
	return false;
}

// Management menu item labels (opened from the context menu)
static const char* podcast_manage_items[] = {
	"Search",
	"Top Shows"};

// Format date as relative time or date string
static void format_date(char* buf, uint32_t timestamp) {
	if (timestamp == 0) {
		strcpy(buf, "");
		return;
	}

	time_t now = time(NULL);
	time_t pub = (time_t)timestamp;
	int days = (now - pub) / (24 * 3600);
	if (days < 0)
		days = 0; // future pub date (timezone-less feed / wrong RTC) → "Today"

	// translated: the old screens' text was translated when drawn, the Brick OS pages draw it as it is
	if (days == 0) {
		strcpy(buf, I18N_text("Today"));
	} else if (days == 1) {
		strcpy(buf, I18N_text("Yesterday"));
	} else if (days < 7) {
		sprintf(buf, I18N_text("%d days ago"), days);
	} else if (days < 30) {
		sprintf(buf, I18N_text("%d weeks ago"), days / 7);
	} else {
		static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		struct tm* tm = localtime(&pub);
		sprintf(buf, "%d %s", tm->tm_mday, I18N_text(mon[tm->tm_mon]));
	}
}

// Render redesigned podcast main page (continue listening + subscriptions with artwork)
void render_podcast_main_page(SDL_Surface* screen, MusicIndicatorType show_setting,
							  int selected, int* scroll,
							  const char* toast_message, uint32_t toast_time) {
	(void)show_setting, (void)scroll;
	bos_main(screen, selected, toast_message, toast_time);
}

// Render the podcast management menu (opened from the context menu)
void render_podcast_manage(SDL_Surface* screen, MusicIndicatorType show_setting,
						   int menu_selected, int subscription_count) {
	(void)show_setting, (void)subscription_count;
	bos_manage(screen, menu_selected);
}

// Render Top Shows list
void render_podcast_top_shows(SDL_Surface* screen, MusicIndicatorType show_setting,
							  int selected, int* scroll,
							  const char* toast_message, uint32_t toast_time) {
	(void)show_setting, (void)scroll;
	bos_shows(screen, true, selected, toast_message, toast_time);
}

// Render search results
void render_podcast_search_results(SDL_Surface* screen, MusicIndicatorType show_setting,
								   int selected, int* scroll,
								   const char* toast_message, uint32_t toast_time) {
	(void)show_setting, (void)scroll;
	bos_shows(screen, false, selected, toast_message, toast_time);
}

// Render episode list for a feed
void render_podcast_episodes(SDL_Surface* screen, MusicIndicatorType show_setting,
							 int feed_index, int selected, int* scroll,
							 const char* toast_message, uint32_t toast_time) {
	(void)show_setting;
	bos_episodes(screen, feed_index, selected, scroll, toast_message, toast_time);
}

// Render download queue view
void render_podcast_download_queue(SDL_Surface* screen, MusicIndicatorType show_setting,
								   int selected, int* scroll,
								   const char* toast_message, uint32_t toast_time) {
	(void)show_setting;
	bos_queue(screen, selected, scroll, toast_message, toast_time);
}

// Render now playing screen for podcast (matches radio/music player style)
void render_podcast_playing(SDL_Surface* screen, MusicIndicatorType show_setting,
							int feed_index, int episode_index) {
	(void)show_setting;
	bos_playing(screen, feed_index, episode_index);
}

// Render loading screen
void render_podcast_loading(SDL_Surface* screen, const char* message) {
	BOS_uiLoading(screen, message ? message : "Loading...", NULL);
}

// Check if podcast title is currently scrolling (list or playing screen)
bool Podcast_isTitleScrolling(void) {
	return false;
}

// Check if title scroll needs a render to transition from delay to active scrolling
// Returns true during the delay phase when text is wider than max_width but scrolling hasn't started
bool Podcast_titleScrollNeedsRender(void) {
	return UIPodcast_bosMoving() || UIPodcast_bosListMoving();
}

// Animate podcast title scroll only (GPU mode, no screen redraw needed)
void Podcast_animateTitleScroll(void) {
	return;
}

// Clear podcast title scroll state (call when selection changes or leaving page)
void Podcast_clearTitleScroll(void) {
	memset(&podcast_title_scroll, 0, sizeof(podcast_title_scroll));
	GFX_clearLayers(LAYER_SCROLLTEXT);
	GFX_resetScrollText(); // Also reset NextUI's internal scroll state
	PLAT_GPU_Flip();	   // Commit the layer clearing to the display
}

// === PODCAST PROGRESS GPU FUNCTIONS ===

void PodcastProgress_setPosition(int bar_x, int bar_y, int bar_w, int bar_h,
								 int time_y, int screen_w, int duration_ms) {
	progress_bar_x = bar_x;
	progress_bar_y = bar_y;
	progress_bar_w = bar_w;
	progress_bar_h = bar_h;
	progress_time_y = time_y;
	progress_screen_w = screen_w;
	progress_duration_ms = duration_ms;
	progress_position_set = true;
}

void PodcastProgress_clear(void) {
	progress_position_set = false;
	progress_last_position_sec = -1;
	PLAT_clearLayers(LAYER_PODCAST_PROGRESS);
}

bool PodcastProgress_needsRefresh(void) {
 // the page shows the time: once a second, a redraw
	const MusicSnapshotWire* snap = MusicClient_snapshot();
	return snap->state == MUSIC_STATE_PLAYING && snap->position_ms / 1000 != bos_play_second;
}

void PodcastProgress_renderGPU(void) {
	return;
}

// ======================= Brick OS (DESIGN.md 7.2; boards v2-MusicPodcasts, -PodcastShow, -PodcastTop, -Downloads,
// -PodcastPlaying). Included at the end of ui_podcast.c: it uses the file's artwork and format helpers. ==========

static int bos_play_second = -1; // the playing page shows this second (declared above)

// raw artwork by key (feed_id or itunes_id), loaded once; the kit rounds and keeps its own copies
#define BOS_ART_SLOTS 24
static struct {
	char key[64];
	SDL_Surface* raw;
	uint32_t used;
} bos_art[BOS_ART_SLOTS];
static uint32_t bos_art_clock;

static SDL_Surface* bosArtLoad(const char* key, const char* path) {
	if (!key || !key[0])
		return NULL;
	bos_art_clock++;
	int old = 0;
	for (int i = 0; i < BOS_ART_SLOTS; i++) {
		if (strcmp(bos_art[i].key, key) == 0) {
			if (!bos_art[i].raw && path) { // not there last time: try the file again now and then
				if (bos_art_clock - bos_art[i].used < 120)
					return NULL;
				bos_art[i].raw = UI_loadValidatedImage(path, PODCAST_ARTWORK_MAX_SIZE);
			}
			bos_art[i].used = bos_art_clock;
			return bos_art[i].raw;
		}
		if (bos_art[i].used < bos_art[old].used)
			old = i;
	}
	if (bos_art[old].raw) {
		BOS_picForget(bos_art[old].raw);
		SDL_FreeSurface(bos_art[old].raw);
	}
	snprintf(bos_art[old].key, sizeof(bos_art[old].key), "%s", key);
	bos_art[old].raw = path ? UI_loadValidatedImage(path, PODCAST_ARTWORK_MAX_SIZE) : NULL;
	bos_art[old].used = bos_art_clock;
	return bos_art[old].raw;
}

static SDL_Surface* bosFeedArt(const char* feed_id) {
	char dir[512], p[768];
	Podcast_getFeedDataPath(feed_id, dir, sizeof(dir));
	snprintf(p, sizeof(p), "%s/artwork.jpg", dir);
	return bosArtLoad(feed_id, p);
}

// a chart / search show: the cached file, or fetch it in the background (a later frame shows it)
static SDL_Surface* bosShowArt(const char* itunes_id, const char* url) {
	char p[768];
	get_artwork_cache_path(itunes_id, p, sizeof(p));
	SDL_Surface* s = bosArtLoad(itunes_id, p);
	if (!s && url && url[0] && access(p, F_OK) != 0) {
		mkdir(PODCAST_CACHE_PARENT, 493);
		mkdir(PODCAST_CACHE_DIR, 493);
		artwork_request_async(url, p);
	}
	return s;
}

static void bosCover(SDL_Surface* s, SDL_Surface* art, const char* title, int x, int y, int size, int r) {
	SDL_Surface* p = art ? BOS_picRounded(art, size, size, r) : NULL;
	if (p)
		BOS_blit(p, s, x, y, 1);
	else {
		BOS_rrect(s, x, y, size, size, r, 0xBF5AF2);
		int ic = size / 2;
		BOS_icon(s, "mic", 0xFFFFFF, 1, x + (size - ic) / 2, y + (size - ic) / 2, ic);
	}
	(void)title;
}

static void bosPill(SDL_Surface* s, int x, int y, const char* t, uint32_t bg, uint32_t fg) {
	int h = BOS_px(40), w = BOS_textW(t, 24, 600) + BOS_px(32);
	BOS_rrect(s, x, y, w, h, h / 2, bg);
	BOS_textMid(s, t, 24, 600, fg, 1, x + BOS_px(16), y + h / 2);
}

static int bosPillW(const char* t) {
	return BOS_textW(t, 24, 600) + BOS_px(32);
}

// ---- a grid of shows (subscriptions, top shows, results): 5 across, the name under each ----
#define BOS_GRID_COLS 5
static BosGlide g_scroll, g_fx, g_fy;
typedef struct {
	SDL_Surface* art;
	const char* name;
	const char* sub;
	const char* pill; // ใหม่ N
	bool tick;		  // followed
	const char* icon; // a tile with an icon instead of a cover (ดาวน์โหลด)
	const char* feed_id;   // the cover, loaded when drawn (the cache is small): a followed show's
	const char* itunes_id; //   or a chart / search show's,
	const char* art_url;   //   fetched from here when missing
} BosShow;

static int bosGrid(SDL_Surface* s, int y0, int bottom, const BosShow* shows, int n, int sel, int* scroll_px) {
	const BosTheme* th = BOS_theme();
	int size = BOS_px(168), gapx = BOS_px(22), x0 = BOS_px(40), rowh = size + BOS_px(12) + BOS_lineH(24) + BOS_lineH(22) + BOS_px(16);
	if (n <= 0)
		return y0;
	// scroll so the focused row shows
	int sr = sel >= 0 ? sel / BOS_GRID_COLS : 0, view = bottom - y0 - BOS_px(14);
	int sc = scroll_px ? *scroll_px : 0;
	if (sr * rowh < sc)
		sc = sr * rowh;
	if ((sr + 1) * rowh > sc + view)
		sc = (sr + 1) * rowh - view;
	if (sc < 0)
		sc = 0;
	if (scroll_px)
		*scroll_px = sc;
	float fsc = BOS_glideTo(&g_scroll, (float)sc, 180, false);
	SDL_Rect clip = {0, y0, s->w, bottom - y0};
	SDL_SetClipRect(s, &clip);
	for (int i = 0; i < n; i++) {
		int x = x0 + (i % BOS_GRID_COLS) * (size + gapx), y = y0 + BOS_px(14) + (i / BOS_GRID_COLS) * rowh - (int)fsc; // room for the ring
		if (y + rowh < y0 || y > bottom)
			continue;
		const BosShow* sh = &shows[i];
		bool hot = i == sel;
		int grow = hot ? size * 8 / 100 / 2 : 0;
		if (hot) // the ring (§5.3)
			BOS_rrect(s, x - grow - BOS_px(4), y - grow - BOS_px(4), size + 2 * grow + BOS_px(8), size + 2 * grow + BOS_px(8),
					  BOS_px(26), th->focus);
		if (sh->icon) {
			BOS_rrect(s, x - grow, y - grow, size + 2 * grow, size + 2 * grow, BOS_px(22), th->surface2);
			int ic = size / 2;
			BOS_icon(s, sh->icon, BOS_C_TEXT, 1, x + (size - ic) / 2, y + (size - ic) / 2, ic);
		} else {
			SDL_Surface* raw = sh->feed_id ? bosFeedArt(sh->feed_id) : sh->itunes_id ? bosShowArt(sh->itunes_id, sh->art_url) : NULL;
			SDL_Surface* p = raw ? BOS_picRounded(raw, size + 2 * grow, size + 2 * grow, BOS_px(22)) : NULL;
			if (p)
				BOS_blit(p, s, x - grow, y - grow, 1);
			else
				bosCover(s, NULL, sh->name, x - grow, y - grow, size + 2 * grow, BOS_px(22));
		}
		if (sh->pill)
			bosPill(s, x + size - BOS_px(8) - bosPillW(sh->pill), y + BOS_px(8), sh->pill, th->primary, 0xFFFFFF);
		if (sh->tick) {
			int t = BOS_px(34);
			BOS_rrect(s, x + size - BOS_px(8) - t, y + BOS_px(8), t, t, t / 2, 0xFFFFFF);
			BOS_icon(s, "checkc", BOS_C_OK, 1, x + size - BOS_px(8) - t, y + BOS_px(8), t);
		}
		char t[160];
		int ty = y + size + grow + BOS_px(12);
		BOS_fit(sh->name ? sh->name : "", 24, hot ? 600 : 500, size, t, sizeof(t));
		BOS_text(s, t, 24, hot ? 600 : 500, BOS_C_TEXT, 1, x + (size - BOS_textW(t, 24, hot ? 600 : 500)) / 2, ty);
		if (sh->sub && sh->sub[0]) {
			BOS_fit(sh->sub, 22, 400, size, t, sizeof(t));
			BOS_text(s, t, 22, 400, BOS_C_TEXT2, 1, x + (size - BOS_textW(t, 22, 400)) / 2, ty + BOS_lineH(24));
		}
	}
	SDL_SetClipRect(s, NULL);
	return y0 + ((n + BOS_GRID_COLS - 1) / BOS_GRID_COLS) * rowh;
}

bool UIPodcast_bosMoving(void) {
	return BOS_glideMoving(&g_scroll) || BOS_marqueeActive();
}

// ---- the main page: ฟังต่อ cards, then the followed shows (and ดาวน์โหลด) ----
static void bos_main(SDL_Surface* s, int selected, const char* toast, uint32_t toast_time) {
	const BosTheme* th = BOS_theme();
	SDL_FillRect(s, NULL, BOS_map(s, th->bg));
	BOS_statusBar(s, I18N_text("Podcasts"));
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H), y = top + BOS_px(4);
	int cl_raw = Podcast_getContinueListeningCount();
	int cl = cl_raw > PODCAST_CONTINUE_LISTENING_DISPLAY ? PODCAST_CONTINUE_LISTENING_DISPLAY : cl_raw;
	int subn = Podcast_getSubscriptionCount(), dln = 0;
	Podcast_getDownloadQueue(&dln);
	if (cl + subn + (dln > 0) == 0) { // nothing followed yet
		void BOS_uiEmpty(SDL_Surface* s, const char* message, const char* subtitle, char** pairs);
		BOS_uiEmpty(s, "No podcasts subscribed", "Press A to manage podcasts", (char*[]){"B", "BACK", "A", "MANAGE", NULL});
		UI_renderToast(s, toast, toast_time);
		return;
	}
	bool on_cards = selected < cl;
	static int grid_scroll;
	if (cl > 0 && (on_cards || grid_scroll == 0)) {
		BOS_text(s, I18N_text("Continue listening"), 30, 600, BOS_C_TEXT2, 1, BOS_px(40), y);
		y += BOS_lineH(30) + BOS_px(10);
		int cw = BOS_px(300), ch = BOS_px(124), x = BOS_px(40);
		for (int i = 0; i < cl; i++) {
			ContinueListeningEntry* e = Podcast_getContinueListening(i);
			if (!e)
				continue;
			bool hot = i == selected;
			int g = hot ? cw * 4 / 100 / 2 : 0, gy = hot ? ch * 4 / 100 / 2 : 0;
			BOS_rrect(s, x - g, y - gy, cw + 2 * g, ch + 2 * gy, BOS_px(22), hot ? th->focus : th->surface);
			int a = BOS_px(64);
			bosCover(s, bosFeedArt(e->feed_id), e->feed_title, x + BOS_px(16), y + BOS_px(16), a, BOS_px(12));
			int tx = x + BOS_px(16) + a + BOS_px(14), room = x + cw - BOS_px(14) - tx;
			char t[200];
			BOS_fit(e->episode_title, 26, 600, room, t, sizeof(t));
			BOS_text(s, t, 26, 600, hot ? th->onfocus : BOS_C_TEXT, 1, tx, y + BOS_px(18));
			BOS_fit(e->feed_title, 22, 400, room, t, sizeof(t));
			BOS_text(s, t, 22, 400, hot ? 0x3A3A3C : BOS_C_TEXT2, 1, tx, y + BOS_px(18) + BOS_lineH(26));
			x += cw + BOS_px(20);
		}
		y += ch + BOS_px(24);
	}
	BOS_text(s, I18N_text("Following"), 30, 600, BOS_C_TEXT2, 1, BOS_px(40), y);
	y += BOS_lineH(30) + BOS_px(10);
	static BosShow shows[PODCAST_MAX_SUBSCRIPTIONS + 1];
	static char pills[PODCAST_MAX_SUBSCRIPTIONS][24];
	int n = 0;
	PodcastFeed* feeds = Podcast_getSubscriptions(NULL);
	for (int i = 0; i < subn && n < PODCAST_MAX_SUBSCRIPTIONS; i++, n++) {
		shows[n] = (BosShow){NULL, feeds[i].title, NULL, NULL, false, NULL, feeds[i].feed_id, NULL, NULL};
		if (feeds[i].new_episode_count > 0) {
			snprintf(pills[n], sizeof(pills[n]), I18N_text("New %d"), feeds[i].new_episode_count);
			shows[n].pill = pills[n];
		}
	}
	if (dln > 0) {
		static char dl[48];
		snprintf(dl, sizeof(dl), I18N_text("%d in the queue"), dln);
		shows[n++] = (BosShow){NULL, I18N_text("Downloads"), dl, NULL, false, "download", NULL, NULL, NULL};
	}
	bosGrid(s, y, bottom - BOS_px(8), shows, n, on_cards ? -1 : selected - cl, &grid_scroll);
	if (on_cards)
		grid_scroll = 0;
	static BosHint hr[2], hl[1];
	hr[0] = (BosHint){"A", I18N_text(on_cards ? "Continue" : "Open")};
	hr[1] = (BosHint){"B", I18N_text("Back")};
	hl[0] = (BosHint){"MENU", I18N_text("Options")};
	BOS_hintBar(s, hr, 2, hl, 1);
	UI_renderToast(s, toast, toast_time);
}

// left/right/up/down on the main page (cards, then a 5-across grid); returns the new selection
int UIPodcast_bosMainNav(int sel, int cl, int grid_n) {
	int n = cl + grid_n, C = BOS_GRID_COLS;
	if (n <= 0)
		return sel;
	if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT)) { // the reading order, round at the ends
		PAD_wrapNav(&sel, n, BTN_LEFT, BTN_RIGHT);
		return sel;
	}
	// rows: the cards (one row), then the grid's; Up/Down keep the column and wrap
	int top = cl > 0 ? 1 : 0, rows = top + (grid_n + C - 1) / C;
	int r = sel < cl ? 0 : top + (sel - cl) / C, c = sel < cl ? sel : (sel - cl) % C;
	if (PAD_wrapNav(&r, rows, BTN_UP, BTN_DOWN) <= 0)
		return sel;
	if (top && r == 0)
		return c < cl ? c : cl - 1;
	int g = (r - top) * C + c;
	return cl + (g < grid_n ? g : grid_n - 1);
}

// a grid page's own moves (top shows, results)
int UIPodcast_bosGridNav(int sel, int n) {
	if (n > 0)
		PAD_gridNav(&sel, n, BOS_GRID_COLS);
	return sel;
}

// ---- manage: Search, Top Shows ----
static void bos_manage_row(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	r->label = I18N_text(podcast_manage_items[i]);
	r->sq_icon = i == 0 ? "search" : "star", r->sq_color = i == 0 ? 0x0A84FF : 0xFF9F0A, r->chevron = true;
}
static BosGlide m_pill, m_scroll;
static void bos_manage(SDL_Surface* s, int sel) {
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_statusBar(s, I18N_text("Podcasts"));
	int y = BOS_px(BOS_BAR_H) + BOS_largeTitle(s, BOS_px(BOS_BAR_H), I18N_text("Find podcasts"), NULL);
	SDL_Rect a = {BOS_px(40), y, s->w - BOS_px(80), BOS_px(96) * PODCAST_MANAGE_COUNT};
	int sc = 0;
	BOS_groupRows(s, &a, BOS_px(96), PODCAST_MANAGE_COUNT, sel, true, &sc, &m_pill, &m_scroll, false, bos_manage_row, NULL);
	BosHint hr[2] = {{"A", I18N_text("Open")}, {"B", I18N_text("Back")}};
	BOS_hintBar(s, hr, 2, NULL, 0);
}

// ---- top shows / search results: the grid ----
static void bos_shows(SDL_Surface* s, bool charts, int sel, const char* toast, uint32_t toast_time) {
	const BosTheme* th = BOS_theme();
	SDL_FillRect(s, NULL, BOS_map(s, th->bg));
	BOS_statusBar(s, I18N_text("Podcasts"));
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	int n = 0;
	static BosShow shows[64];
	bool loading = false;
	const char* err = NULL;
	if (charts) {
		const PodcastChartsStatus* st = Podcast_getChartsStatus();
		loading = st->loading, err = st->error_message[0] ? st->error_message : NULL;
		PodcastChartItem* it = loading ? NULL : Podcast_getTopShows(&n);
		for (int i = 0; i < n && i < 64; i++)
			shows[i] = (BosShow){NULL, it[i].title, it[i].genre, NULL, Podcast_isSubscribedByItunesId(it[i].itunes_id),
								 NULL, NULL, it[i].itunes_id, it[i].artwork_url};
	} else {
		const PodcastSearchStatus* st = Podcast_getSearchStatus();
		loading = st->searching, err = st->error_message[0] ? st->error_message : NULL;
		PodcastSearchResult* it = loading ? NULL : Podcast_getSearchResults(&n);
		for (int i = 0; i < n && i < 64; i++)
			shows[i] = (BosShow){NULL, it[i].title, it[i].genre, NULL, Podcast_isSubscribedByItunesId(it[i].itunes_id),
								 NULL, NULL, it[i].itunes_id, it[i].artwork_url};
	}
	if (n > 64)
		n = 64;
	int y = top + BOS_largeTitle(s, top, I18N_text(charts ? "Top shows" : "Search results"),
								 I18N_text(loading ? "Loading..." : "Green tick: following"));
	if (!loading && n == 0) {
		BOS_textWrapCentre(s, I18N_text(err ? err : charts ? "No shows available" : "No results found"), 32, 600,
						   BOS_C_TEXT2, 1, BOS_px(80), (y + bottom) / 2 - BOS_px(20), s->w - BOS_px(160), 2);
	} else {
		static int sc[2];
		bosGrid(s, y, bottom - BOS_px(8), shows, n, sel, &sc[charts ? 0 : 1]);
	}
	bool followed = sel >= 0 && sel < n && shows[sel].tick;
	BosHint hr[2] = {{"A", I18N_text(followed ? "Unfollow" : "Follow")}, {"B", I18N_text("Back")}};
	BOS_hintBar(s, n > 0 ? hr : hr + 1, n > 0 ? 2 : 1, NULL, 0);
	UI_renderToast(s, toast, toast_time);
}

// ---- a show's episodes ----
typedef struct {
	PodcastFeed* feed;
	int index;
} EpCtx;
static char ep_sub[256];
static void bos_episode_row(void* ctx, int i, bool hot, ListViewRow* r) {
	EpCtx* c = ctx;
	(void)hot;
	static PodcastEpisode ep;
	if (!Podcast_getEpisode(c->index, i, &ep)) {
		r->label = "";
		return;
	}
	static char title[PODCAST_MAX_TITLE];
	snprintf(title, sizeof(title), "%s", ep.title);
	r->label = title;
	char date[64] = "", dur[32] = "";
	format_date(date, ep.pub_date);
	if (ep.duration_sec > 0)
		snprintf(dur, sizeof(dur), I18N_text("%d min"), (ep.duration_sec + 59) / 60);
	snprintf(ep_sub, sizeof(ep_sub), "%s%s%s", date, date[0] && dur[0] ? " · " : "", dur);
	r->sub = ep_sub;
	int prog = 0;
	int dl = Podcast_getEpisodeDownloadStatus(c->feed->feed_url, ep.guid, &prog);
	bool have = Podcast_episodeFileExists(c->feed, i);
	static char left[48];
	if (dl == PODCAST_DOWNLOAD_DOWNLOADING)
		r->progress = prog > 0 ? prog / 100.0f : 0.02f;
	else if (dl == PODCAST_DOWNLOAD_PENDING)
		r->annotation = I18N_text("Queued");
	else if (ep.is_new)
		r->pill = I18N_text("New"), r->pill_bg = BOS_theme()->primary, r->pill_fg = 0xFFFFFF;
	else if (ep.progress_sec > 0 && ep.duration_sec > 0) {
		snprintf(left, sizeof(left), I18N_text("%d min left"), (ep.duration_sec - ep.progress_sec + 59) / 60);
		r->annotation = left;
	} else if (ep.progress_sec == -1)
		r->right_icon = "checkc", r->right_icon_colour = BOS_C_TEXTOFF;
	if (have && dl != PODCAST_DOWNLOAD_DOWNLOADING && !r->right_icon)
		r->right_icon = "download", r->right_icon_colour = BOS_C_OK;
}
static BosGlide e_pill, e_scroll;
static void bos_episodes(SDL_Surface* s, int feed_index, int sel, int* scroll, const char* toast, uint32_t toast_time) {
	const BosTheme* th = BOS_theme();
	SDL_FillRect(s, NULL, BOS_map(s, th->bg));
	BOS_statusBar(s, I18N_text("Podcasts"));
	PodcastFeed* feed = Podcast_getSubscription(feed_index);
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	if (!feed) {
		BosHint hb[1] = {{"B", I18N_text("Back")}};
		BOS_hintBar(s, hb, 1, NULL, 0);
		return;
	}
	int a = BOS_px(112), x = BOS_px(40), y = top + BOS_px(4);
	bosCover(s, bosFeedArt(feed->feed_id), feed->title, x, y, a, BOS_px(20));
	char t[256];
	BOS_fit(feed->title, 48, 700, s->w - x - a - BOS_px(64), t, sizeof(t));
	BOS_text(s, t, 48, 700, BOS_C_TEXT, 1, x + a + BOS_px(24), y + BOS_px(10));
	char sub[300];
	snprintf(sub, sizeof(sub), I18N_text("%s · %d episodes · following"), feed->author, feed->episode_count);
	BOS_fit(sub, 28, 400, s->w - x - a - BOS_px(64), t, sizeof(t));
	BOS_text(s, t, 28, 400, BOS_C_TEXT2, 1, x + a + BOS_px(24), y + BOS_px(10) + BOS_lineH(48));
	y += a + BOS_px(16);
	EpCtx c = {feed, feed_index};
	if (feed->episode_count > 0) {
		SDL_Rect area = {BOS_px(40), y, s->w - BOS_px(80), bottom - BOS_px(12) - y};
		BOS_groupRows(s, &area, BOS_px(96), feed->episode_count, sel, true, scroll, &e_pill, &e_scroll, false,
					  bos_episode_row, &c);
	} else
		BOS_textWrapCentre(s, I18N_text("No episodes available"), 32, 600, BOS_C_TEXT2, 1, BOS_px(80), y + BOS_px(80),
						   s->w - BOS_px(160), 2);
	// what A does on the focused one
	const char* act = "Download";
	PodcastEpisode ep;
	if (feed->episode_count > 0 && Podcast_getEpisode(feed_index, sel, &ep)) {
		int prog = 0, dl = Podcast_getEpisodeDownloadStatus(feed->feed_url, ep.guid, &prog);
		if (dl == PODCAST_DOWNLOAD_PENDING || dl == PODCAST_DOWNLOAD_DOWNLOADING)
			act = "Cancel";
		else if (Podcast_episodeFileExists(feed, sel))
			act = ep.progress_sec > 0 ? "Continue" : "Listen";
	}
	BosHint hr[2] = {{"A", I18N_text(act)}, {"B", I18N_text("Back")}}, hl[1] = {{"MENU", I18N_text("Options")}};
	BOS_hintBar(s, feed->episode_count > 0 ? hr : hr + 1, feed->episode_count > 0 ? 2 : 1, hl, 1);
	UI_renderToast(s, toast, toast_time);
}

// ---- the download queue ----
static PodcastDownloadItem bos_q[PODCAST_MAX_DOWNLOAD_QUEUE];
static int bos_qn;
static BosGlide q_pill, q_scroll;
static char q_sub[160];
static void bos_queue_row(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	PodcastDownloadItem* it = &bos_q[i];
	r->label = it->episode_title;
	const PodcastDownloadProgress* pr = Podcast_getDownloadProgress();
	int pct = it->progress_percent;
	if (it->status == PODCAST_DOWNLOAD_DOWNLOADING && strcmp(pr->current_guid, it->episode_guid) == 0)
		pct = pr->current_percent;
	switch (it->status) {
	case PODCAST_DOWNLOAD_DOWNLOADING:
		snprintf(q_sub, sizeof(q_sub), I18N_text("Downloading · %d%%"), pct);
		r->progress = pct > 0 ? pct / 100.0f : 0.02f;
		break;
	case PODCAST_DOWNLOAD_COMPLETE:
		snprintf(q_sub, sizeof(q_sub), "%s · %s", it->feed_title, I18N_text("Done"));
		r->right_icon = "checkc", r->right_icon_colour = BOS_C_OK;
		break;
	case PODCAST_DOWNLOAD_FAILED:
		snprintf(q_sub, sizeof(q_sub), "%s · %s", it->feed_title, I18N_text("Download failed"));
		r->right_icon = "warn", r->right_icon_colour = BOS_C_WARN;
		break;
	default:
		snprintf(q_sub, sizeof(q_sub), "%s · %s", it->feed_title, I18N_text("Queued"));
	}
	r->sub = q_sub;
	r->sq_icon = "mic", r->sq_color = 0xBF5AF2;
}
static void bos_queue(SDL_Surface* s, int sel, int* scroll, const char* toast, uint32_t toast_time) {
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_statusBar(s, I18N_text("Podcasts"));
	bos_qn = Podcast_copyDownloadQueue(bos_q, PODCAST_MAX_DOWNLOAD_QUEUE);
	const PodcastDownloadProgress* pr = Podcast_getDownloadProgress();
	char sub[96];
	snprintf(sub, sizeof(sub), I18N_text("%d of %d done"), pr->completed_count, pr->total_items);
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	int y = top + BOS_largeTitle(s, top, I18N_text("Downloads"), bos_qn > 0 ? sub : NULL);
	if (bos_qn > 0) {
		SDL_Rect a = {BOS_px(40), y, s->w - BOS_px(80), bottom - BOS_px(12) - y};
		BOS_groupRows(s, &a, BOS_px(96), bos_qn, sel, true, scroll, &q_pill, &q_scroll, false, bos_queue_row, NULL);
	} else
		BOS_textWrapCentre(s, I18N_text("No downloads"), 32, 600, BOS_C_TEXT2, 1, BOS_px(80), y + BOS_px(80),
						   s->w - BOS_px(160), 1);
	BosHint hr[2] = {{"A", I18N_text("Cancel")}, {"B", I18N_text("Back")}};
	BOS_hintBar(s, bos_qn > 0 ? hr : hr + 1, bos_qn > 0 ? 2 : 1, NULL, 0);
	UI_renderToast(s, toast, toast_time);
}

// ---- the player: as music's, −10 / +30, the speed ----
static void bos_playing(SDL_Surface* s, int feed_index, int episode_index) {
	PodcastFeed* feed = Podcast_getSubscription(feed_index);
	PodcastEpisode ep;
	bool have = Podcast_getEpisode(feed_index, episode_index, &ep);
	const MusicSnapshotWire* snap = MusicClient_snapshot();
	MusicPlayView v = {0};
	v.status = I18N_text("Podcasts");
	if (feed && feed->artwork_url[0] && feed->feed_id[0]) {
		podcast_fetch_artwork(feed->artwork_url, feed->feed_id);
		v.art = podcast_artwork;
	}
	v.icon = "mic", v.icon_colour = 0xBF5AF2;
	static char speed[32], title[PODCAST_MAX_TITLE], line[300];
	float sp = MusicClient_speed();
	snprintf(speed, sizeof(speed), I18N_text("Speed %.2gx"), sp > 0 ? sp : 1.0f);
	v.pills[0] = speed;
	v.pills[1] = I18N_text("Podcast");
	snprintf(title, sizeof(title), "%s", have ? ep.title : snap->title);
	v.title = title;
	v.artist = feed ? feed->title : snap->album;
	if (have) {
		char date[64] = "";
		format_date(date, ep.pub_date);
		snprintf(line, sizeof(line), "%s%s%s", date, have && Podcast_episodeFileExists(feed, episode_index) ? " · " : "",
				 have && Podcast_episodeFileExists(feed, episode_index) ? I18N_text("Downloaded") : "");
		v.album = line;
	}
	v.pos_ms = snap->position_ms, v.dur_ms = snap->duration_ms;
	v.paused = snap->state != MUSIC_STATE_PLAYING;
	v.back_icon = "rew10", v.fwd_icon = "fwd30";
	static BosHint hr[2], hl[1];
	hr[0] = (BosHint){"A", I18N_text(v.paused ? "Play" : "Pause")};
	hr[1] = (BosHint){"B", I18N_text("Back")};
	hl[0] = (BosHint){"↑↓", I18N_text("Speed")};
	v.hints = hr, v.nhints = 2, v.left = hl, v.nleft = 1;
	GFX_clearLayers(LAYER_SCROLLTEXT);
	MusicBos_playing(s, &v, NULL);
	bos_play_second = snap->position_ms / 1000;
}

bool UIPodcast_bosListMoving(void) {
	return BOS_glideMoving(&e_pill) || BOS_glideMoving(&e_scroll) || BOS_glideMoving(&m_pill) ||
		   BOS_glideMoving(&q_pill) || BOS_glideMoving(&q_scroll) || BOS_marqueeActive();
}
