// Files: the Brick OS file manager (DESIGN 7.9, boards v2-Files, -FilesSheet, -FilesPaste, -FilesCopy).
// Replaces NextCommander. One list of the folder (folders first), where you are along the top, X picks rows,
// SELECT (or MENU) opens the actions sheet: copy, move, rename, new folder, details, delete. Copy and move keep
// what was picked and wait: go to the folder it goes to and press START. Everything long shows the progress page.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <msettings.h>

#include "api.h"
#include "bos_ui.h"
#include "defines.h"
#include "i18n.h"
#include "ui_confirmdialog.h"
#include "ui_contextmenu.h"
#include "ui_downloadprogress.h"
#include "ui_draw.h"
#include "ui_keyboard.h"
#include "ui_listview.h"
#include "ui_menubar.h"
#include "ui_splash.h"
#include "ui_toast.h"
#include "utils.h"

#define ROOT "/mnt/SDCARD"
#define FPATH 1024
#define MAX_ENTRIES 4096
#define MAX_CLIP 256

enum { ACT_COPY = 1, ACT_MOVE, ACT_RENAME, ACT_NEWDIR, ACT_INFO, ACT_DELETE };

typedef struct {
	char name[256];
	bool dir;
	off_t size;
	time_t mtime;
	int children; // -1 not counted yet
	bool picked;
} Entry;

static volatile sig_atomic_t quit = false;
static SDL_Surface* screen;
static char cur[FPATH] = ROOT;
static Entry* ents;
static int nents;
static ListView view;
static BosGlide g_pill, g_scroll;
static char toast[96];
static uint32_t toast_at;

// what copy / move carries
static char clip[MAX_CLIP][FPATH];
static int nclip;
static int clip_op; // 0 none, ACT_COPY, ACT_MOVE

static void sigHandler(int sig) {
	if (sig == SIGINT || sig == SIGTERM)
		quit = true;
}

static void say(const char* m) {
	snprintf(toast, sizeof(toast), "%s", I18N_text(m));
	toast_at = SDL_GetTicks();
}

static void join(char* out, size_t n, const char* a, const char* b) {
	snprintf(out, n, "%s/%s", a, b);
}

// ---- the folder ----
static int cmp(const void* a, const void* b) {
	const Entry *x = a, *y = b;
	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	return strcasecmp(x->name, y->name);
}

static void load(const char* path, const char* select) {
	if (path != cur)
		snprintf(cur, sizeof(cur), "%s", path);
	nents = 0;
	DIR* d = opendir(cur);
	if (d) {
		struct dirent* de;
		while ((de = readdir(d)) && nents < MAX_ENTRIES) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			Entry* e = &ents[nents];
			memset(e, 0, sizeof(*e));
			snprintf(e->name, sizeof(e->name), "%s", de->d_name);
			char p[FPATH];
			join(p, sizeof(p), cur, e->name);
			struct stat st;
			if (stat(p, &st) == 0) {
				e->dir = S_ISDIR(st.st_mode);
				e->size = st.st_size;
				e->mtime = st.st_mtime;
			}
			e->children = -1;
			nents++;
		}
		closedir(d);
	}
	qsort(ents, nents, sizeof(Entry), cmp);
	UI_listViewReset(&view, nents, ents);
	if (select)
		for (int i = 0; i < nents; i++)
			if (!strcmp(ents[i].name, select)) {
				view.selected = i;
				break;
			}
	GFX_clearLayers(LAYER_SCROLLTEXT);
}

static int picked_count(void) {
	int n = 0;
	for (int i = 0; i < nents; i++)
		n += ents[i].picked;
	return n;
}

static int count_children(const char* path) {
	DIR* d = opendir(path);
	if (!d)
		return 0;
	int n = 0;
	struct dirent* de;
	while ((de = readdir(d)))
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
			n++;
	closedir(d);
	return n;
}

// ---- how a row reads ----
static void size_text(off_t b, char* out, int n) {
	if (b >= 1024LL * 1024 * 1024)
		snprintf(out, n, "%.1f GB", b / (1024.0 * 1024 * 1024));
	else if (b >= 1024 * 1024)
		snprintf(out, n, "%.1f MB", b / (1024.0 * 1024));
	else if (b >= 1024)
		snprintf(out, n, "%lld KB", (long long)(b / 1024));
	else
		snprintf(out, n, "%lld B", (long long)b);
}

static void date_text(time_t t, char* out, int n) {
	static const char* mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	struct tm tm;
	localtime_r(&t, &tm);
	snprintf(out, n, "%d %s %d", tm.tm_mday, I18N_text(mon[tm.tm_mon]), tm.tm_year + 1900);
}

static const char* ext_of(const char* name) {
	const char* d = strrchr(name, '.');
	return d && d != name ? d + 1 : "";
}

static void kind(const Entry* e, const char** icon, uint32_t* colour) {
	static const char* zips[] = {"zip", "7z", "rar", "gz", "tar", NULL};
	static const char* pics[] = {"png", "jpg", "jpeg", "bmp", "gif", "webp", NULL};
	static const char* songs[] = {"mp3", "flac", "ogg", "wav", "m4a", "opus", NULL};
	static const char* films[] = {"mp4", "mkv", "avi", "webm", "mov", NULL};
	static const char* texts[] = {"txt", "cfg", "ini", "json", "xml", "log", "md", "sh", NULL};
	static const char* saves[] = {"sav", "srm", "state", "st0", "st1", "st2", "st3", NULL};
	const char* x = ext_of(e->name);
	struct {
		const char** list;
		const char* icon;
		uint32_t colour;
	} k[] = {{zips, "zip", 0xFF9F0A},	{pics, "image", 0xBF5AF2}, {songs, "music", 0xFF375F},
			 {films, "film", 0x30B0C7}, {texts, "text", 0x8E8E93}, {saves, "save", 0x8E8E93}};
	if (e->dir) {
		*icon = "folder", *colour = BOS_C_PRIMARY;
		return;
	}
	for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); i++)
		for (const char** p = k[i].list; *p; p++)
			if (!strcasecmp(x, *p)) {
				*icon = k[i].icon, *colour = k[i].colour;
				return;
			}
	*icon = "file", *colour = 0x5E5CE6; // a game or anything else
}

static void row(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	static char sb[96], sz[32];
	Entry* e = &ents[i];
	const char* ic;
	uint32_t c;
	kind(e, &ic, &c);
	*r = (ListViewRow){.label = e->name, .sq_icon = ic, .sq_color = c, .chevron = e->dir};
	if (e->dir) {
		if (e->children < 0) {
			char p[FPATH];
			join(p, sizeof(p), cur, e->name);
			e->children = count_children(p);
		}
		snprintf(sb, sizeof(sb), I18N_text("Folder · %d items"), e->children);
	} else {
		date_text(e->mtime, sb, sizeof(sb));
		size_text(e->size, sz, sizeof(sz));
		r->annotation = sz;
	}
	r->sub = sb;
	if (e->picked)
		r->right_icon = "checkc", r->right_icon_colour = BOS_C_OK;
}

// ---- the page ----
// where you are: an SD card icon, "การ์ด SD", then each folder with a chevron between; the last one in text colour.
// When it does not fit, the first folders give way to "…".
static int crumb(int y) {
	int x = BOS_px(40), maxx = screen->w - BOS_px(40), ic = BOS_px(30), ch = BOS_px(22), cy = y + BOS_lineH(30) / 2;
	char parts[32][256];
	int n = 0;
	snprintf(parts[n++], 256, "%s", I18N_text("SD card"));
	const char* p = cur + strlen(ROOT);
	while (*p == '/')
		p++;
	while (*p && n < 32) {
		const char* s = strchr(p, '/');
		int len = s ? (int)(s - p) : (int)strlen(p);
		snprintf(parts[n++], 256, "%.*s", len, p);
		p += len;
		while (*p == '/')
			p++;
	}
	if (nclip && clip_op == 0) // a pill on the right: how many are picked
		;
	int first = 1, total;
	for (;;) { // drop folders from the front until the rest fits
		total = ic + BOS_px(10) + BOS_textW(parts[0], 30, 400);
		if (first > 1)
			total += BOS_px(10) + ch + BOS_px(10) + BOS_textW("…", 30, 400);
		for (int i = first; i < n; i++)
			total += BOS_px(10) + ch + BOS_px(10) + BOS_textW(parts[i], 30, i == n - 1 ? 600 : 400);
		if (total <= maxx - x || first >= n - 1)
			break;
		first++;
	}
	BOS_icon(screen, "sd", BOS_C_TEXT2, 1, x, cy - ic / 2, ic);
	x += ic + BOS_px(10);
	BOS_textMid(screen, parts[0], 30, 400, n == 1 ? BOS_C_TEXT : BOS_C_TEXT2, 1, x, cy);
	x += BOS_textW(parts[0], 30, 400);
	for (int i = first > 1 ? first - 1 : first; i < n; i++) {
		x += BOS_px(10);
		BOS_icon(screen, "chev", BOS_C_TEXTOFF, 1, x, cy - ch / 2, ch);
		x += ch + BOS_px(10);
		const char* t = (first > 1 && i == first - 1) ? "…" : parts[i];
		bool last = i == n - 1;
		char f[256];
		BOS_fit(t, 30, last ? 600 : 400, maxx - x, f, sizeof(f));
		BOS_textMid(screen, f, 30, last ? 600 : 400, last ? BOS_C_TEXT : BOS_C_TEXT2, 1, x, cy);
		x += BOS_textW(f, 30, last ? 600 : 400);
	}
	return BOS_lineH(30) + BOS_px(14);
}

static void render(void) {
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("Files"));
	int top = BOS_px(BOS_BAR_H), gx = BOS_px(40), gw = screen->w - 2 * gx;
	int y = top + BOS_px(4);
	int np = picked_count();
	if (np && !clip_op) { // "เลือก 2 รายการ" pill, top right of the page
		char t[64];
		snprintf(t, sizeof(t), I18N_text("%d picked"), np);
		int ph = BOS_px(40), pw = BOS_textW(t, 24, 600) + BOS_px(52);
		int px = screen->w - BOS_px(40) - pw;
		BOS_rrect(screen, px, y + BOS_px(2), pw, ph, ph / 2, BOS_theme()->surface2);
		BOS_rrect(screen, px + BOS_px(16), y + BOS_px(2) + ph / 2 - BOS_px(6), BOS_px(12), BOS_px(12), BOS_px(6), BOS_C_OK);
		BOS_textMid(screen, t, 24, 600, BOS_C_TEXT, 1, px + BOS_px(36), y + BOS_px(2) + ph / 2);
	}
	y += crumb(y);
	if (clip_op) { // where the copy / move will land
		int bh = BOS_px(100), sq = BOS_px(56);
		BOS_rrect(screen, gx, y, gw, bh, BOS_px(24), BOS_theme()->surface);
		uint32_t c = clip_op == ACT_COPY ? BOS_C_PRIMARY : 0xFF9F0A;
		BOS_rrect(screen, gx + BOS_px(24), y + (bh - sq) / 2, sq, sq, sq * 26 / 100, c);
		int ic = BOS_px(36);
		BOS_icon(screen, clip_op == ACT_COPY ? "copy" : "move", 0xFFFFFF, 1, gx + BOS_px(24) + (sq - ic) / 2, y + (bh - ic) / 2, ic);
		char t[128];
		snprintf(t, sizeof(t), I18N_text(clip_op == ACT_COPY ? "Copy %d items here?" : "Move %d items here?"), nclip);
		int tx = gx + BOS_px(24) + sq + BOS_px(20);
		BOS_textMid(screen, t, 32, 600, BOS_C_TEXT, 1, tx, y + bh / 2 - BOS_px(16));
		BOS_textMid(screen, I18N_text("Open the folder it goes to, then press START"), 26, 400, BOS_C_TEXT2, 1, tx,
					y + bh / 2 + BOS_px(20));
		y += bh + BOS_px(16);
	}
	int bottom = screen->h - BOS_px(BOS_BAR_H) - BOS_px(12);
	if (nents > 0) {
		BOS_groupRows(screen, &(SDL_Rect){gx, y, gw, bottom - y}, BOS_px(BOS_ROW_H), nents, view.selected, true, &view.scroll,
					  &g_pill, &g_scroll, false, row, NULL);
		view.last_visible = (bottom - y) / BOS_px(BOS_ROW_H);
	} else {
		int cw = BOS_px(140), ic = BOS_px(76), cx = screen->w / 2, cy = y + (bottom - y) / 2 - BOS_px(60);
		BOS_rrect(screen, cx - cw / 2, cy - cw / 2, cw, cw, BOS_px(36), BOS_theme()->surface);
		BOS_icon(screen, "folder", BOS_C_TEXT2, 1, cx - ic / 2, cy - ic / 2, ic);
		const char* t = I18N_text("This folder is empty");
		BOS_text(screen, t, 40, 700, BOS_C_TEXT, 1, cx - BOS_textW(t, 40, 700) / 2, cy + cw / 2 + BOS_px(24));
	}
	view.input_pending = false;
	bool root = !strcmp(cur, ROOT);
	if (clip_op) {
		BosHint h[] = {{"START", I18N_text("Paste here")}, {"A", I18N_text("Open")}, {"B", I18N_text(root ? "Exit" : "Up")}};
		BosHint l[] = {{"SELECT", I18N_text("Cancel")}};
		BOS_hintBar(screen, h, 3, l, 1);
	} else {
		BosHint h[] = {{"A", I18N_text("Open")}, {"B", I18N_text(root ? "Exit" : "Up")}, {"X", I18N_text("Pick")}};
		BosHint l[] = {{"SELECT", I18N_text("Options")}};
		BOS_hintBar(screen, h, nents ? 3 : 2, l, 1);
	}
	UI_renderToast(screen, toast, toast_at);
	if (ContextMenu_isOpen())
		ContextMenu_render(screen);
}

// ---- long jobs: the progress page, B stops ----
typedef struct {
	const char* status; // "Copying"
	long long done, total;
	int files_done, files_total;
	char now[256];
	bool stop;
	uint32_t drawn;
} Job;

static void job_draw(Job* j, bool force) {
	uint32_t t = SDL_GetTicks();
	if (!force && t - j->drawn < 100)
		return;
	j->drawn = t;
	PAD_poll();
	if (PAD_justPressed(BTN_B))
		j->stop = true;
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("Files"));
	char sb[320], dt[96], a[32], b[32];
	snprintf(sb, sizeof(sb), I18N_text("File %d / %d · %s"), j->files_done + 1 > j->files_total ? j->files_total : j->files_done + 1,
			 j->files_total, j->now);
	size_text(j->done, a, sizeof(a));
	size_text(j->total, b, sizeof(b));
	snprintf(dt, sizeof(dt), "%s / %s", a, b);
	UI_renderDownloadProgress(screen, &(UIDownloadProgress){.status = j->status, .sub = sb, .detail = dt, .show_bar = true,
															 .progress = j->total > 0 ? (int)(j->done * 100 / j->total) : 0,
															 .icon = !strcmp(j->status, "Moving") ? "move" : !strcmp(j->status, "Deleting") ? "trash" : "copy",
															 .colour = !strcmp(j->status, "Moving") ? 0xFF9F0A : !strcmp(j->status, "Deleting") ? BOS_C_DANGER : 0});
	BosHint h[] = {{"B", I18N_text("Stop")}};
	BOS_hintBar(screen, h, 1, NULL, 0);
	GFX_flip(screen);
}

static void measure(const char* p, Job* j) {
	struct stat st;
	if (lstat(p, &st) != 0)
		return;
	if (!S_ISDIR(st.st_mode)) {
		j->total += st.st_size;
		j->files_total++;
		return;
	}
	DIR* d = opendir(p);
	if (!d)
		return;
	struct dirent* de;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char q[FPATH];
		join(q, sizeof(q), p, de->d_name);
		measure(q, j);
	}
	closedir(d);
}

static bool copy_file(const char* src, const char* dst, Job* j) {
	int in = open(src, O_RDONLY);
	if (in < 0)
		return false;
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		close(in);
		return false;
	}
	const char* base = strrchr(src, '/');
	snprintf(j->now, sizeof(j->now), "%s", base ? base + 1 : src);
	static char buf[256 * 1024];
	bool ok = true;
	for (;;) {
		ssize_t n = read(in, buf, sizeof(buf));
		if (n == 0)
			break;
		if (n < 0 || write(out, buf, n) != n) {
			ok = false;
			break;
		}
		j->done += n;
		job_draw(j, false);
		if (j->stop) {
			ok = false;
			break;
		}
	}
	close(in);
	if (close(out) != 0)
		ok = false;
	if (!ok)
		unlink(dst);
	j->files_done++;
	return ok;
}

static bool copy_tree(const char* src, const char* dst, Job* j) {
	struct stat st;
	if (lstat(src, &st) != 0)
		return false;
	if (!S_ISDIR(st.st_mode))
		return copy_file(src, dst, j);
	if (mkdir(dst, 0755) != 0 && errno != EEXIST)
		return false;
	DIR* d = opendir(src);
	if (!d)
		return false;
	bool ok = true;
	struct dirent* de;
	while (ok && !j->stop && (de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char a[FPATH], b[FPATH];
		join(a, sizeof(a), src, de->d_name);
		join(b, sizeof(b), dst, de->d_name);
		ok = copy_tree(a, b, j);
	}
	closedir(d);
	return ok && !j->stop;
}

static bool delete_tree(const char* p, Job* j) {
	struct stat st;
	if (lstat(p, &st) != 0)
		return false;
	if (S_ISDIR(st.st_mode)) {
		DIR* d = opendir(p);
		if (d) {
			struct dirent* de;
			while ((de = readdir(d))) {
				if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
					continue;
				char q[FPATH];
				join(q, sizeof(q), p, de->d_name);
				delete_tree(q, j);
			}
			closedir(d);
		}
		return rmdir(p) == 0;
	}
	if (j) {
		const char* base = strrchr(p, '/');
		snprintf(j->now, sizeof(j->now), "%s", base ? base + 1 : p);
		j->done += st.st_size;
		j->files_done++;
		job_draw(j, false);
	}
	return unlink(p) == 0;
}

// a name not taken in dir: "name", then "name (2)", "name (3)" ...
static void free_name(const char* dir, const char* name, char* out, size_t n) {
	join(out, n, dir, name);
	if (access(out, F_OK) != 0)
		return;
	const char* dot = strrchr(name, '.');
	int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
	for (int k = 2; k < 1000; k++) {
		char nm[300];
		snprintf(nm, sizeof(nm), "%.*s (%d)%s", stem, name, k, name + stem);
		join(out, n, dir, nm);
		if (access(out, F_OK) != 0)
			return;
	}
}

static void paste_here(void) {
	// a folder cannot go inside itself
	for (int i = 0; i < nclip; i++) {
		size_t l = strlen(clip[i]);
		if (!strncmp(cur, clip[i], l) && (cur[l] == 0 || cur[l] == '/')) {
			say("A folder cannot go inside itself");
			return;
		}
	}
	Job j = {.status = clip_op == ACT_COPY ? "Copying" : "Moving"};
	for (int i = 0; i < nclip; i++)
		measure(clip[i], &j);
	job_draw(&j, true);
	bool ok = true;
	char last[256] = "";
	for (int i = 0; i < nclip && ok && !j.stop; i++) {
		const char* base = strrchr(clip[i], '/');
		base = base ? base + 1 : clip[i];
		char dst[FPATH];
		char dir_of[FPATH];
		snprintf(dir_of, sizeof(dir_of), "%.*s", (int)(base - clip[i] - 1), clip[i]);
		if (clip_op == ACT_MOVE && !strcmp(dir_of, cur))
			continue; // already here
		free_name(cur, base, dst, sizeof(dst));
		if (clip_op == ACT_MOVE && rename(clip[i], dst) == 0) { // one card: a rename is the whole move
			Job m = {0};
			measure(dst, &m);
			j.done += m.total, j.files_done += m.files_total;
			job_draw(&j, true);
		} else {
			ok = copy_tree(clip[i], dst, &j);
			if (ok && clip_op == ACT_MOVE)
				delete_tree(clip[i], NULL);
			else if (!ok)
				delete_tree(dst, NULL); // nothing half-copied stays
		}
		const char* b2 = strrchr(dst, '/');
		snprintf(last, sizeof(last), "%s", b2 ? b2 + 1 : dst);
	}
	PAD_reset();
	if (j.stop)
		say("Stopped");
	else if (!ok)
		say(clip_op == ACT_COPY ? "Copy failed" : "Move failed");
	else
		say(clip_op == ACT_COPY ? "Copied" : "Moved");
	clip_op = 0, nclip = 0;
	load(cur, last[0] ? last : NULL);
}

// the rows the actions work on: the picked ones, else the focused one
static int targets(char out[][FPATH], int max) {
	int n = 0;
	for (int i = 0; i < nents && n < max; i++)
		if (ents[i].picked)
			join(out[n++], FPATH, cur, ents[i].name);
	if (!n && view.selected >= 0 && view.selected < nents)
		join(out[n++], FPATH, cur, ents[view.selected].name);
	return n;
}

static void do_delete(void) {
	static char t[MAX_CLIP][FPATH];
	int n = targets(t, MAX_CLIP);
	if (!n)
		return;
	char title[320], sub[160];
	if (n == 1) {
		const char* b = strrchr(t[0], '/');
		snprintf(title, sizeof(title), I18N_text("Delete \"%s\"?"), b ? b + 1 : t[0]);
	} else
		snprintf(title, sizeof(title), I18N_text("Delete %d items?"), n);
	snprintf(sub, sizeof(sub), "%s", I18N_text("They are removed from the SD card and cannot come back"));
	if (!UI_confirmModal(screen, title, sub, NULL, true, true))
		return;
	Job j = {.status = "Deleting"};
	for (int i = 0; i < n; i++)
		measure(t[i], &j);
	job_draw(&j, true);
	bool ok = true;
	for (int i = 0; i < n; i++)
		ok = delete_tree(t[i], &j) && ok;
	PAD_reset();
	say(ok ? "Deleted" : "Some could not be deleted");
	int keep = view.selected;
	load(cur, NULL);
	view.selected = keep < nents ? keep : nents - 1;
}

static void do_rename(void) {
	if (view.selected < 0 || view.selected >= nents)
		return;
	Entry* e = &ents[view.selected];
	char prompt[320];
	snprintf(prompt, sizeof(prompt), I18N_text("Rename \"%s\""), e->name);
	char* nn = UIKeyboard_open(prompt);
	if (!nn || !nn[0] || strchr(nn, '/')) {
		free(nn);
		return;
	}
	char a[FPATH], b[FPATH];
	join(a, sizeof(a), cur, e->name);
	join(b, sizeof(b), cur, nn);
	if (access(b, F_OK) == 0)
		say("That name is already used");
	else if (rename(a, b) != 0)
		say("Rename failed");
	else
		load(cur, nn);
	free(nn);
}

static void do_newdir(void) {
	char* nn = UIKeyboard_open(I18N_text("New folder name"));
	if (!nn || !nn[0] || strchr(nn, '/')) {
		free(nn);
		return;
	}
	char p[FPATH];
	join(p, sizeof(p), cur, nn);
	if (access(p, F_OK) == 0)
		say("That name is already used");
	else if (mkdir(p, 0755) != 0)
		say("Could not make the folder");
	else
		load(cur, nn);
	free(nn);
}

static void do_info(void) {
	if (view.selected < 0 || view.selected >= nents)
		return;
	Entry* e = &ents[view.selected];
	char p[FPATH];
	join(p, sizeof(p), cur, e->name);
	Job m = {0};
	measure(p, &m);
	char sz[32], dt[64], text[256];
	size_text(e->dir ? m.total : e->size, sz, sizeof(sz));
	date_text(e->mtime, dt, sizeof(dt));
	if (e->dir)
		snprintf(text, sizeof(text), I18N_text("%s · %d files\nChanged %s"), sz, m.files_total, dt);
	else
		snprintf(text, sizeof(text), I18N_text("%s\nChanged %s"), sz, dt);
	const char* ic;
	uint32_t c;
	kind(e, &ic, &c);
	PAD_reset();
	bool dirty = true;
	IndicatorType ind = INDICATOR_NONE;
	while (!quit) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_B) || PAD_justPressed(BTN_A))
			break;
		PWR_update(&dirty, &ind, NULL, NULL);
		if (dirty) {
			SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
			BOS_statusBar(screen, I18N_text("Files"));
			BOS_resultCard(screen, ic, c, e->name, text);
			BosHint h[] = {{"B", I18N_text("Back")}};
			BOS_hintBar(screen, h, 1, NULL, 0);
			GFX_flip(screen);
			dirty = false;
		} else
			GFX_sync();
	}
	PAD_reset();
}

static void open_sheet(void) {
	int np = picked_count();
	ContextMenuItem it[6];
	int n = 0;
	if (nents > 0) {
		snprintf(it[n].label, sizeof(it[n].label), "%s", "Copy"), it[n++].id = ACT_COPY;
		snprintf(it[n].label, sizeof(it[n].label), "%s", "Move"), it[n++].id = ACT_MOVE;
		if (np <= 1)
			snprintf(it[n].label, sizeof(it[n].label), "%s", "Rename"), it[n++].id = ACT_RENAME;
	}
	snprintf(it[n].label, sizeof(it[n].label), "%s", "New folder"), it[n++].id = ACT_NEWDIR;
	if (nents > 0 && np <= 1)
		snprintf(it[n].label, sizeof(it[n].label), "%s", "Details"), it[n++].id = ACT_INFO;
	if (nents > 0)
		snprintf(it[n].label, sizeof(it[n].label), "%s", "Delete"), it[n++].id = ACT_DELETE;
	ContextMenu_open(it, n);
	GFX_clearLayers(LAYER_SCROLLTEXT);
}

static void act(int id) {
	switch (id) {
	case ACT_COPY:
	case ACT_MOVE:
		nclip = targets(clip, MAX_CLIP);
		clip_op = nclip ? id : 0;
		for (int i = 0; i < nents; i++)
			ents[i].picked = false;
		break;
	case ACT_RENAME:
		do_rename();
		break;
	case ACT_NEWDIR:
		do_newdir();
		break;
	case ACT_INFO:
		do_info();
		break;
	case ACT_DELETE:
		do_delete();
		break;
	}
}

static void go_up(void) {
	char parent[FPATH], here[256];
	snprintf(parent, sizeof(parent), "%s", cur);
	char* s = strrchr(parent, '/');
	if (!s)
		return;
	snprintf(here, sizeof(here), "%s", s + 1);
	*s = 0;
	load(parent, here);
}

int main(int argc, char* argv[]) {
	(void)argc, (void)argv;
	UI_bos = true;
	screen = GFX_init(MODE_MAIN);
	PWR_pinToCores(CPU_CORE_EFFICIENCY);
	UI_showSplashScreen(screen, "Files");
	InitSettings();
	PAD_init();
	PWR_init();
	signal(SIGINT, sigHandler);
	signal(SIGTERM, sigHandler);

	ents = calloc(MAX_ENTRIES, sizeof(Entry));
	if (!ents)
		return EXIT_FAILURE;
	view.get_row = NULL;
	load(ROOT, NULL);

	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;
	while (!quit) {
		GFX_startFrame();
		PAD_poll();
		if (ContextMenu_isOpen()) {
			ContextMenuResult r = ContextMenu_handleInput();
			if (r.action == CONTEXTMENU_SELECTED)
				act(r.id);
			dirty = true; // the sheet moves its own highlight: draw every frame while it is up
		} else if (PAD_justPressed(BTN_SELECT) && clip_op) {
			clip_op = 0, nclip = 0;
			say("Cancelled");
			dirty = true;
		} else if (PAD_justPressed(BTN_SELECT)) {
			open_sheet();
			dirty = true;
		} else if (PAD_justPressed(BTN_START) && clip_op) {
			paste_here();
			dirty = true;
		} else {
			view.count = nents;
			ListViewAction a = UI_listViewHandleInput(&view);
			switch (a.type) {
			case LISTVIEW_ACTIVATED:
				if (a.index >= 0 && a.index < nents) {
					Entry* e = &ents[a.index];
					if (e->dir) {
						char p[FPATH];
						join(p, sizeof(p), cur, e->name);
						load(p, NULL);
					} else
						do_info();
				}
				dirty = true;
				break;
			case LISTVIEW_BACK:
				if (strcmp(cur, ROOT))
					go_up();
				else if (BOS_askQuit(screen, "Files"))
					quit = true;
				dirty = true;
				break;
			case LISTVIEW_MENU:
				open_sheet();
				dirty = true;
				break;
			case LISTVIEW_BUTTON:
				if (a.btn == BTN_X && a.index >= 0 && a.index < nents && !clip_op) {
					ents[a.index].picked = !ents[a.index].picked;
					dirty = true;
				}
				break;
			default:
				break;
			}
			if (view.input_pending)
				dirty = true;
		}
		UI_tickToast(toast, toast_at, &dirty);
		PWR_update(&dirty, &show_setting, NULL, NULL);
		if (UI_statusBarChanged() || BOS_glideMoving(&g_pill) || BOS_glideMoving(&g_scroll) || BOS_marqueeActive())
			dirty = true;
		if (dirty) {
			render();
			GFX_flip(screen);
			dirty = false;
		} else
			GFX_sync();
	}
	free(ents);
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return EXIT_SUCCESS;
}
