// ส่งไฟล์ผ่าน WiFi (DESIGN §6.13): while this screen is open a small web server lets a phone or a PC on the
// same WiFi put files on the card and take them off. Leaving the screen stops it.
#include "api.h"
#include "config.h"
#include "defines.h"
#include "i18n.h"
#include "msettings.h"
#include "network_common.h"
#include "ui/bos_ui.h"
#include "ui/ui_menubar.h"
#include "ui/ui_quitrequest.h"
#include "ui/ui_splash.h"
#include "utils.h"
#include "wt_qr.h"
#include "wt_server.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static SDL_Surface* screen;
static volatile sig_atomic_t quit_sig;
static char ip[32], ssid[64], url[64], pin[8];
static WtQr qr;
static bool server_on, server_failed;

static void on_signal(int s) {
	(void)s;
	quit_sig = 1;
}

static void new_pin(void) {
	unsigned int v = 0;
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0 || read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
		v = (unsigned int)(time(NULL) ^ getpid());
	if (fd >= 0)
		close(fd);
	snprintf(pin, sizeof(pin), "%04u", v % 10000);
	wt_server_set_pin(pin);
}

// the WiFi address, or false when WiFi is off or not connected (the USB cable link does not count)
static bool lan(void) {
	int online = 0;
	PLAT_getNetworkStatus(&online);
	char a[32] = "", b[32] = "";
	if (!online || NET_getLanInfo(a, sizeof(a), b, sizeof(b)) != 0 || !a[0] || !strncmp(a, "10.99.0.", 8))
		return false;
	if (strcmp(a, ip)) {
		snprintf(ip, sizeof(ip), "%s", a);
		snprintf(url, sizeof(url), "http://%s:%d", ip, WT_PORT);
		wt_qr_make(url, &qr);
		struct WIFI_connection c;
		memset(&c, 0, sizeof(c));
		ssid[0] = '\0';
		if (PLAT_wifiConnection(&c) == 0 && c.valid)
			snprintf(ssid, sizeof(ssid), "%s", c.ssid);
	}
	return true;
}

static void keep_awake(bool on) {
	if (on) {
		PWR_disableSleep();
		PWR_disableAutosleep();
	} else {
		PWR_enableSleep();
		PWR_enableAutosleep();
	}
}

static void server(bool on) {
	if (on == server_on)
		return;
	if (on) {
		WtConfig c = {SDCARD_PATH, "web", CFG_getSaveFormat(), CFG_getStateFormat()};
		server_on = wt_server_start(&c);
		server_failed = !server_on;
	} else {
		wt_server_stop();
		server_on = false;
	}
}

// ---------- drawing ----------
static void draw_qr(int x, int y, int card) {
	BOS_rrect(screen, x, y, card, card, BOS_px(24), 0xFFFFFF);
	int quiet = 2, n = qr.size + 2 * quiet;
	int m = (card - BOS_px(16)) / n;
	int ox = x + (card - m * n) / 2 + quiet * m, oy = y + (card - m * n) / 2 + quiet * m;
	uint32_t black = BOS_map(screen, 0x000000);
	for (int r = 0; r < qr.size; r++)
		for (int c = 0; c < qr.size; c++)
			if (qr.m[r][c])
				SDL_FillRect(screen, &(SDL_Rect){ox + c * m, oy + r * m, m, m}, black);
}

static int step(int x, int y, int w, int num, const char* text, const char* big, int big_px) {
	const BosTheme* th = BOS_theme();
	int d = BOS_px(44);
	BOS_rrect(screen, x, y, d, d, d / 2, th->surface2);
	char n[4];
	snprintf(n, sizeof(n), "%d", num);
	BOS_textMid(screen, n, BOS_FS_SMALL, 700, BOS_C_TEXT, 1, x + (d - BOS_textW(n, BOS_FS_SMALL, 700)) / 2, y + d / 2);
	int tx = x + d + BOS_px(16), tw = w - d - BOS_px(16);
	char fit[256];
	BOS_fit(text, BOS_FS_SMALL, 500, tw, fit, sizeof(fit));
	BOS_textMid(screen, fit, BOS_FS_SMALL, 500, BOS_C_TEXT2, 1, tx, y + d / 2);
	int h = d;
	if (big) {
		BOS_fit(big, big_px, 700, tw, fit, sizeof(fit));
		BOS_text(screen, fit, big_px, 700, BOS_C_TEXT, 1, tx, y + d + BOS_px(4));
		h += BOS_px(4) + BOS_lineH(big_px);
	}
	return h;
}

static void draw_recent(int x, int y, int w, int bottom) {
	WtRecent r[8];
	int n = wt_server_recent(r, 8);
	const BosTheme* th = BOS_theme();
	BOS_text(screen, I18N_text("Recent"), BOS_FS_SMALL, 700, BOS_C_TEXT2, 1, x, y);
	y += BOS_lineH(BOS_FS_SMALL) + BOS_px(8);
	int rh = BOS_px(80);
	if (!n) {
		BOS_text(screen, I18N_text("Nothing yet. Files you send show here."), BOS_FS_SMALL, 500, BOS_C_TEXTOFF, 1, x, y + BOS_px(8));
		return;
	}
	int rows = (bottom - y) / rh;
	if (rows < 1)
		return;
	if (n > rows)
		n = rows;
	BOS_rrect(screen, x, y, w, n * rh, BOS_px(BOS_RADIUS_GROUP), th->surface);
	for (int i = 0; i < n; i++) {
		int ry = y + i * rh, ic = BOS_px(32), px = x + BOS_px(24);
		if (i)
			SDL_FillRect(screen, &(SDL_Rect){px, ry, w - 2 * BOS_px(24), 1}, BOS_map(screen, BOS_C_SEPARATOR));
		uint32_t col = r[i].state == 2 ? BOS_C_DANGER : r[i].state == 1 ? BOS_C_OK : BOS_C_TEXT2;
		BOS_icon(screen, r[i].up ? "upload" : "download", col, 1, px, ry + (rh - ic) / 2, ic);
		int tx = px + ic + BOS_px(16), right = x + w - BOS_px(24), bar = BOS_px(160);
		int tw = right - bar - BOS_px(24) - tx;
		char fit[256];
		BOS_fit(r[i].name, BOS_FS_SMALL, 500, tw, fit, sizeof(fit));
		BOS_text(screen, fit, BOS_FS_SMALL, 500, BOS_C_TEXT, 1, tx, ry + BOS_px(10));
		const char* sub = r[i].state == 2 ? I18N_text("Did not finish") : r[i].up ? r[i].folder : I18N_text("To the phone / PC");
		BOS_fit(sub, BOS_FS_BADGE, 400, tw, fit, sizeof(fit));
		BOS_text(screen, fit, BOS_FS_BADGE, 400, BOS_C_TEXT2, 1, tx, ry + BOS_px(10) + BOS_lineH(BOS_FS_SMALL));
		if (r[i].state == 1)
			BOS_icon(screen, "check", BOS_C_OK, 1, right - ic, ry + (rh - ic) / 2, ic);
		else if (r[i].state == 0) {
			int pct = r[i].total ? (int)(r[i].done * 100 / r[i].total) : 0;
			char p[8];
			snprintf(p, sizeof(p), "%d%%", pct);
			int pw = BOS_textW(p, BOS_FS_SMALL, 600);
			BOS_textMid(screen, p, BOS_FS_SMALL, 600, BOS_C_TEXT, 1, right - pw, ry + rh / 2);
			int bx = right - pw - BOS_px(16) - bar, by = ry + rh / 2 - BOS_px(4);
			BOS_rrect(screen, bx, by, bar, BOS_px(8), BOS_px(4), th->surface2);
			if (pct > 0)
				BOS_rrect(screen, bx, by, bar * pct / 100 < BOS_px(8) ? BOS_px(8) : bar * pct / 100, BOS_px(8), BOS_px(4), th->primary);
		}
	}
}

static void render(bool online) {
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("WiFi Transfer"));
	int top = BOS_px(BOS_BAR_H), gx = BOS_px(BOS_PAGE_PAD), gw = screen->w - 2 * gx;
	int bottom = screen->h - BOS_px(BOS_BAR_H) - BOS_px(12);
	if (!online || server_failed) {
		BOS_resultCard(screen, "wifi", online ? BOS_C_DANGER : BOS_C_WARN,
					   online ? "Could not start" : "Turn on WiFi first",
					   online ? "Another app is using port 8080. Close it and open this again."
							  : "Connect to WiFi in Settings > Network. This page starts by itself once it is connected.");
		BosHint h[] = {{"B", I18N_text("Close")}};
		BOS_hintBar(screen, h, 1, NULL, 0);
		GFX_flip(screen);
		return;
	}
	int y = top + BOS_largeTitle(screen, top, I18N_text("Send files over WiFi"), I18N_text("On · stops when you leave this page"));
	int card = BOS_px(260);
	draw_qr(gx, y, card);
	int sx = gx + card + BOS_px(40), sw = gx + gw - sx, sy = y;
	char t1[160];
	snprintf(t1, sizeof(t1), "%s%s%s", I18N_text("Join the same WiFi as this device"), ssid[0] ? ": " : "", ssid);
	sy += step(sx, sy, sw, 1, t1, NULL, 0) + BOS_px(20);
	sy += step(sx, sy, sw, 2, I18N_text("Scan the QR code or open this link"), url + 7, 36) + BOS_px(16);
	char spaced[16];
	snprintf(spaced, sizeof(spaced), "%c %c %c %c", pin[0], pin[1], pin[2], pin[3]);
	step(sx, sy, sw, 3, I18N_text("Enter this code"), spaced, 40);
	draw_recent(gx, y + card + BOS_px(24), gw, bottom);
	BosHint h[] = {{"B", I18N_text("Close")}, {"X", I18N_text("New code")}};
	BOS_hintBar(screen, h, 2, NULL, 0);
	GFX_flip(screen);
}

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;
	screen = GFX_init(MODE_MAIN);
	UI_showSplashScreen(screen, "WiFi Transfer");
	InitSettings();
	PAD_init();
	PWR_init();
	BOS_sfxInit();
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	keep_awake(true);
	new_pin();

	bool dirty = true, quit = false, online = false;
	IndicatorType show_setting = INDICATOR_NONE;
	uint32_t last_net = 0, last_moving_draw = 0;
	while (!quit && !quit_sig) {
		GFX_startFrame();
		PAD_poll();
		bool req = false;
		UI_handleQuitRequest(screen, &req, &dirty, "Leave WiFi Transfer?", NULL);
		PWR_update(&dirty, &show_setting, NULL, NULL);
		static time_t last_min;
		if (UI_statusBarChanged() || time(NULL) / 60 != last_min) { // the clock in the top bar
			last_min = time(NULL) / 60;
			dirty = true;
		}
		uint32_t now = SDL_GetTicks();
		if (!last_net || now - last_net > 2000) { // WiFi can come and go
			last_net = now;
			bool o = lan();
			if (o != online || (o && !server_on && !server_failed)) {
				online = o;
				server(online);
				dirty = true;
			}
		}
		if (wt_server_changed())
			dirty = true;
		if (wt_server_moving() && now - last_moving_draw > 500) { // the progress bars
			last_moving_draw = now;
			dirty = true;
		}
		if (PAD_justPressed(BTN_X) && online && server_on) {
			new_pin();
			dirty = true;
		}
		if (req || PAD_justPressed(BTN_B)) {
			if (wt_server_moving())
				quit = BOS_askLeave(screen, I18N_text("Still sending. Files not finished will be deleted. Leave?"));
			else
				quit = BOS_askLeave(screen, I18N_text("Close WiFi Transfer? The phone can't send files until you open it again."));
			dirty = true;
		}
		if (dirty) {
			render(online);
			dirty = false;
		} else
			GFX_sync();
	}
	server(false);
	keep_awake(false);
	BOS_sfxQuit(200);
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return EXIT_SUCCESS;
}
