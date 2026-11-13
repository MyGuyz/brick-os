// osdbright — the quick panel's brightness widget (Brick OS, DESIGN.md 5.9).
//
// trimui_osdd draws its own sliders from pictures only: no number. This widget is an "app" widget instead (the
// protocol of osdmusic.c): the daemon runs widgets/app_backlight/launch.sh once, opens the command FIFO and copies the
// canvas file into the panel at "canvasfps". It writes "enable" when A enters the widget, "left"/"right" while in it,
// "disable"/"key_b" when it leaves. The tile looks like the slider it replaces (sun, 14 px track, white fill; the fill
// takes the accent while entered) with the level after the bar.
//
// Brightness is set with msettings (as keymon's keys do); the settings file is written once, 800 ms after the last
// step (SetSettingsSaveDeferred), not on every step.

#if defined(PLATFORM_TG5050)
#include "../../tg5050/libmsettings/msettings.h"
#else
#include "../../tg5040/libmsettings/msettings.h"
#endif
// deferred saving is the tg5040 library's (keymon); elsewhere every step is saved as it is set
void SetSettingsSaveDeferred(int on) __attribute__((weak));
void FlushSettings(void) __attribute__((weak));

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_PATH "/tmp/trimui_bright"
#define CMD_PATH DIR_PATH "/widget_cmd"
#define CANVAS_PATH DIR_PATH "/vfb_osd"
#define READY_PATH DIR_PATH "/widget_ready"
#define LOCK_PATH DIR_PATH "/widget.lock"
#define CONFIG_PATH "/usr/trimui/osd/widgets/app_backlight/config.json"
#define ICON_PATH "/usr/trimui/osd/widgets/app_backlight/icon-brightness.png"
#define TILE_PATH "/usr/trimui/osd/block4x1.png"
#define FONT_PATH "/mnt/SDCARD/.system/res/bos/Inter-SemiBold.ttf"
#define FONT_FALLBACK "/usr/trimui/osd/regular.ttf"
#define THEME_SETTINGS_PATH "/mnt/SDCARD/.userdata/shared/minuisettings.txt"
#define OSD_SHOW_FLAG "/tmp/trimui_osd/osdd_show_up"
#define SAVE_AFTER_MS 800
#define POLL_MS 250

static int W = 556, H = 124;
static volatile sig_atomic_t quit;
static void on_signal(int sig) {
	(void)sig;
	quit = 1;
}

static int config_int(const char* json, const char* key, int def) {
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char* p = strstr(json, pat);
	if (!p || !(p = strchr(p + strlen(pat), ':')))
		return def;
	int v = atoi(p + 1);
	return v > 0 ? v : def;
}

static SDL_Color accent = {0xF2, 0xF2, 0xF7, 255};
static void load_accent(void) { // the theme's color2 (as osdmusic): the fill while the widget is entered
	FILE* f = fopen(THEME_SETTINGS_PATH, "r");
	if (!f)
		return;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		unsigned v;
		if (sscanf(line, "color2=0x%x", &v) == 1 || sscanf(line, "color2=%x", &v) == 1) {
			if (v == 0x002222ffU)
				v = 0x006666ffU;
			accent = (SDL_Color){(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, 255};
		}
	}
	fclose(f);
}

typedef struct {
	SDL_Surface* frame;
	SDL_Surface* tile;
	SDL_Surface* icon;
	TTF_Font* font;
	Uint8* canvas;
	int cmd_fd;
	char input[256];
	size_t input_len;
	bool entered;
} Widget;

static void rrect(SDL_Surface* s, int x, int y, int w, int h, int r, Uint32 argb) {
	// a filled rounded rectangle with 1 px anti-aliased corners (r = h / 2: a pill)
	Uint8 ca = argb >> 24;
	for (int j = 0; j < h; j++) {
		Uint32* row = (Uint32*)((Uint8*)s->pixels + (y + j) * s->pitch);
		for (int i = 0; i < w; i++) {
			float dx = 0, dy = 0;
			if (i < r)
				dx = r - i - 0.5f;
			else if (i >= w - r)
				dx = i - (w - r) + 0.5f;
			if (j < r)
				dy = r - j - 0.5f;
			else if (j >= h - r)
				dy = j - (h - r) + 0.5f;
			float cov = 1;
			if (dx > 0 && dy > 0) {
				float d = SDL_sqrtf(dx * dx + dy * dy);
				cov = d <= r - 0.5f ? 1 : d >= r + 0.5f ? 0 : r + 0.5f - d;
			}
			if (cov <= 0)
				continue;
			Uint32 sa = (Uint32)(ca * cov), dpx = row[x + i], da = dpx >> 24;
			Uint32 oa = sa + da * (255 - sa) / 255;
			if (!oa)
				continue;
			Uint32 out = oa << 24;
			for (int sh = 0; sh < 24; sh += 8) {
				Uint32 sc = (argb >> sh) & 255, dc = (dpx >> sh) & 255;
				out |= ((sc * sa + dc * da * (255 - sa) / 255) / oa) << sh;
			}
			row[x + i] = out;
		}
	}
}

static void render(Widget* w) {
	SDL_Surface* f = w->frame;
	SDL_FillRect(f, NULL, 0);
	if (w->tile)
		SDL_BlitSurface(w->tile, NULL, f, NULL);
	int cy = H / 2;
	if (w->icon) // the sun, where the daemon's slider had it
		SDL_BlitSurface(w->icon, NULL, f, &(SDL_Rect){65 - w->icon->w / 2, cy - w->icon->h / 2});
	int level = GetBrightness();
	if (level < 0)
		level = 0;
	if (level > 10)
		level = 10;
	char num[8];
	snprintf(num, sizeof(num), "%d", level);
	int nw = 0, widest = 0;
	if (w->font) {
		TTF_SizeUTF8(w->font, num, &nw, NULL);
		TTF_SizeUTF8(w->font, "10", &widest, NULL);
	}
	int right = W - 32, bx = 110, bw = right - widest - 22 - bx, bh = 14;
	rrect(f, bx, cy - bh / 2, bw, bh, bh / 2, 0xFF3A3A3E);
	int fw = bw * level / 10;
	Uint32 fill = w->entered ? 0xFF000000u | accent.r << 16 | accent.g << 8 | accent.b : 0xFFF2F2F7;
	if (fw > 0)
		rrect(f, bx, cy - bh / 2, fw < bh ? bh : fw, bh, bh / 2, fill);
	if (w->font) { // the level, right-aligned: the bar never moves
		SDL_Surface* t = TTF_RenderUTF8_Blended(w->font, num, (SDL_Color){255, 255, 255, 255});
		if (t) {
			SDL_SetSurfaceBlendMode(t, SDL_BLENDMODE_BLEND);
			SDL_BlitSurface(t, NULL, f, &(SDL_Rect){right - nw, cy - t->h / 2});
			SDL_FreeSurface(t);
		}
	}
	memcpy(w->canvas, f->pixels, (size_t)W * H * 4);
}

static int open_fifo(void) {
	struct stat st;
	if (lstat(CMD_PATH, &st) == 0 && !S_ISFIFO(st.st_mode))
		unlink(CMD_PATH);
	if (mkfifo(CMD_PATH, 0600) != 0 && errno != EEXIST)
		return -1;
	return open(CMD_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

static Uint32 save_at;
static void handle(Widget* w, const char* cmd) {
	if (!strcmp(cmd, "enable"))
		w->entered = true;
	else if (!strcmp(cmd, "disable") || !strcmp(cmd, "key_b"))
		w->entered = false;
	else if (w->entered && (!strcmp(cmd, "left") || !strcmp(cmd, "right"))) {
		int v = GetBrightness() + (cmd[0] == 'l' ? -1 : 1);
		if (v >= 0 && v <= 10) {
			SetBrightness(v);
			save_at = SDL_GetTicks() + SAVE_AFTER_MS;
		}
	}
}

static void read_commands(Widget* w) {
	for (;;) {
		if (w->input_len >= sizeof(w->input) - 1)
			w->input_len = 0;
		ssize_t n = read(w->cmd_fd, w->input + w->input_len, sizeof(w->input) - 1 - w->input_len);
		if (n > 0) {
			w->input_len += (size_t)n;
			w->input[w->input_len] = 0;
			char* nl;
			while ((nl = memchr(w->input, '\n', w->input_len))) {
				*nl = 0;
				handle(w, w->input);
				size_t used = (size_t)(nl - w->input) + 1;
				memmove(w->input, w->input + used, w->input_len - used);
				w->input_len -= used;
			}
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			return;
		close(w->cmd_fd); // EOF: the daemon closed it; reopen for its next open
		w->cmd_fd = open_fifo();
		w->input_len = 0;
		return;
	}
}

static SDL_Surface* load(const char* path, int w, int h) {
	SDL_Surface* l = IMG_Load(path);
	if (!l)
		return NULL;
	SDL_Surface* c = SDL_ConvertSurfaceFormat(l, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(l);
	if (c && w > 0 && (c->w != w || c->h != h)) {
		SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (s) {
			SDL_SetSurfaceBlendMode(c, SDL_BLENDMODE_NONE);
			SDL_BlitScaled(c, NULL, s, NULL);
		}
		SDL_FreeSurface(c);
		c = s;
	}
	if (c)
		SDL_SetSurfaceBlendMode(c, SDL_BLENDMODE_BLEND);
	return c;
}

int main(void) {
	Widget w;
	memset(&w, 0, sizeof(w));
	w.cmd_fd = -1;
	FILE* cf = fopen(CONFIG_PATH, "r");
	if (cf) {
		char json[2048];
		size_t n = fread(json, 1, sizeof(json) - 1, cf);
		json[n] = 0;
		fclose(cf);
		W = config_int(json, "canvaswidth", W);
		H = config_int(json, "canvasheight", H);
	}
	mkdir(DIR_PATH, 0700);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	int lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT, 0600);
	if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
		return 0; // another instance owns the widget
	unlink(READY_PATH);
	if (SDL_Init(0) != 0 || TTF_Init() != 0 || !(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG))
		return 1;
	InitSettingsNoApply();
	if (SetSettingsSaveDeferred)
		SetSettingsSaveDeferred(1);
	load_accent();
	w.font = TTF_OpenFont(FONT_PATH, 30);
	if (!w.font)
		w.font = TTF_OpenFont(FONT_FALLBACK, 30);
	w.tile = load(TILE_PATH, W, H);
	w.icon = load(ICON_PATH, 0, 0);
	w.frame = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
	int fd = open(CANVAS_PATH, O_RDWR | O_CREAT, 0600);
	if (fd < 0 || ftruncate(fd, (off_t)W * H * 4) != 0 || !w.frame)
		return 1;
	w.canvas = mmap(NULL, (size_t)W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (w.canvas == MAP_FAILED)
		return 1;
	w.cmd_fd = open_fifo();
	render(&w);
	FILE* rf = fopen(READY_PATH, "w");
	if (rf) {
		fprintf(rf, "%ld\n", (long)getpid());
		fclose(rf);
	}
	int shown = -1;
	bool was_visible = false;
	while (!quit) {
		bool visible = access(OSD_SHOW_FLAG, F_OK) == 0;
		if (visible && !was_visible) { // the panel opened: the level may have changed (keys, Settings)
			load_accent();
			w.entered = false;
		}
		was_visible = visible;
		if (w.cmd_fd < 0) {
			usleep(POLL_MS * 1000);
			w.cmd_fd = open_fifo();
			continue;
		}
		int timeout = visible ? POLL_MS : 1000;
		if (save_at) {
			Sint32 left = (Sint32)(save_at - SDL_GetTicks());
			timeout = left < 0 ? 0 : left < timeout ? left : timeout;
		}
		struct pollfd p = {.fd = w.cmd_fd, .events = POLLIN};
		bool got = poll(&p, 1, timeout) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR));
		if (got)
			read_commands(&w);
		if (save_at && (Sint32)(SDL_GetTicks() - save_at) >= 0) {
			if (FlushSettings)
				FlushSettings();
			save_at = 0;
		}
		int now = GetBrightness() * 2 + w.entered;
		if (visible && (got || now != shown)) {
			render(&w);
			shown = now;
		}
	}
	if (FlushSettings)
		FlushSettings();
	unlink(READY_PATH);
	return 0;
}
