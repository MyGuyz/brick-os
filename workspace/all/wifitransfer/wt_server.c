// WiFi Transfer: a small HTTP/1.1 server, one thread per connection, "Connection: close" on every answer.
//
// Files are named by a "ref": their path under the card ("Roms/Sony PlayStation (PS)/Crash.chd"). Only refs
// under the folders below are served; nothing outside them is read or written.
#include "wt_server.h"

#include "wt_rules.h"
#include "wt_zip.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONN 16
#define HDR_MAX 16384
#define RECENT_MAX 12
#define SPACE_SPARE (8ull << 20) // never fill the card to the last byte

static WtConfig cfg;
static char sd[256], web_dir[512];
static int listen_fd = -1;
static volatile int running;
static pthread_t accept_thread;
static volatile int conns;

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static char pin[8], token[40];
static int bad_pins;
static time_t locked_until;
static WtRecent recent[RECENT_MAX];
static int nrecent;
static volatile int changed;
static int open_fds[MAX_CONN + 4]; // the connections now, so stopping can cut them

static void fd_track(int fd, bool on) {
	pthread_mutex_lock(&mx);
	for (int i = 0; i < MAX_CONN + 4; i++)
		if (on ? open_fds[i] == 0 : open_fds[i] == fd + 1) {
			open_fds[i] = on ? fd + 1 : 0;
			break;
		}
	pthread_mutex_unlock(&mx);
}

// ---------- small strings ----------
typedef struct {
	char* p;
	size_t n, cap;
} Buf;

static void b_add(Buf* b, const char* s, size_t n) {
	if (b->n + n + 1 > b->cap) {
		size_t c = b->cap ? b->cap * 2 : 4096;
		while (c < b->n + n + 1)
			c *= 2;
		char* q = realloc(b->p, c);
		if (!q)
			return;
		b->p = q;
		b->cap = c;
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = '\0';
}
static void b_str(Buf* b, const char* s) { b_add(b, s, strlen(s)); }
static void b_fmt(Buf* b, const char* f, ...) {
	char t[512];
	va_list ap;
	va_start(ap, f);
	int n = vsnprintf(t, sizeof(t), f, ap);
	va_end(ap);
	if (n > 0)
		b_add(b, t, (size_t)n < sizeof(t) ? (size_t)n : sizeof(t) - 1);
}
static void b_json(Buf* b, const char* s) { // a JSON string with its quotes
	b_add(b, "\"", 1);
	for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			char e[2] = {'\\', (char)*p};
			b_add(b, e, 2);
		} else if (*p < 0x20)
			b_fmt(b, "\\u%04x", *p);
		else
			b_add(b, (const char*)p, 1);
	}
	b_add(b, "\"", 1);
}

static int hexv(int c) {
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static void url_decode(const char* s, size_t n, char* out, size_t out_size) {
	size_t o = 0;
	for (size_t i = 0; i < n && o + 1 < out_size; i++) {
		if (s[i] == '%' && i + 2 < n && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
			out[o++] = (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
			i += 2;
		} else
			out[o++] = s[i] == '+' ? ' ' : s[i];
	}
	out[o] = '\0';
}
// the idx-th value of key in a "a=1&b=2" string; false when there is none
static bool param(const char* q, const char* key, int idx, char* out, size_t out_size) {
	size_t kl = strlen(key);
	for (const char* p = q; p && *p;) {
		const char* amp = strchr(p, '&');
		size_t len = amp ? (size_t)(amp - p) : strlen(p);
		if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
			if (idx-- == 0) {
				url_decode(p + kl + 1, len - kl - 1, out, out_size);
				return true;
			}
		}
		p = amp ? amp + 1 : NULL;
	}
	return false;
}

// ---------- the connection ----------
typedef struct {
	int fd;
	char method[8];
	char path[1024];
	char query[HDR_MAX];
	char cookie[512];
	long long clen;
	char* extra; // body bytes read with the header
	size_t nextra;
	char hdr[HDR_MAX + 1];
} Req;

static bool send_all(int fd, const void* buf, size_t n) {
	const char* p = buf;
	while (n) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if (w <= 0) {
			if (w < 0 && errno == EINTR)
				continue;
			return false;
		}
		p += w;
		n -= (size_t)w;
	}
	return true;
}
static bool zip_out(void* ctx, const void* b, size_t n) { return send_all(*(int*)ctx, b, n); }

static void reply(Req* r, int code, const char* type, const char* body, size_t n, const char* extra_hdr) {
	const char* st = code == 200 ? "OK" : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : code == 409 ? "Conflict"
					 : code == 507 ? "Insufficient Storage" : code == 429 ? "Too Many Requests" : "Bad Request";
	char h[1024];
	int hn = snprintf(h, sizeof(h),
					  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n",
					  code, st, type, n, extra_hdr ? extra_hdr : "");
	send_all(r->fd, h, (size_t)hn);
	if (n && strcmp(r->method, "HEAD"))
		send_all(r->fd, body, n);
}
static void reply_json(Req* r, int code, Buf* b) {
	reply(r, code, "application/json; charset=utf-8", b->p ? b->p : "{}", b->p ? b->n : 2, NULL);
}
static void reply_err(Req* r, int code, const char* err) {
	Buf b = {0};
	b_str(&b, "{\"error\":");
	b_json(&b, err);
	b_str(&b, "}");
	reply_json(r, code, &b);
	free(b.p);
}

static bool read_request(Req* r) {
	size_t n = 0;
	char* end = NULL;
	while (n < HDR_MAX) {
		ssize_t g = recv(r->fd, r->hdr + n, HDR_MAX - n, 0);
		if (g <= 0)
			return false;
		n += (size_t)g;
		r->hdr[n] = '\0';
		if ((end = strstr(r->hdr, "\r\n\r\n")))
			break;
	}
	if (!end)
		return false;
	*end = '\0';
	r->extra = end + 4;
	r->nextra = n - (size_t)(end + 4 - r->hdr);
	char target[HDR_MAX];
	if (sscanf(r->hdr, "%7s %16383s", r->method, target) != 2)
		return false;
	char* q = strchr(target, '?');
	if (q)
		*q++ = '\0';
	url_decode(target, strlen(target), r->path, sizeof(r->path));
	snprintf(r->query, sizeof(r->query), "%s", q ? q : "");
	r->clen = -1;
	r->cookie[0] = '\0';
	for (char* line = strstr(r->hdr, "\r\n"); line; line = strstr(line, "\r\n")) {
		line += 2;
		if (!strncasecmp(line, "Content-Length:", 15))
			r->clen = atoll(line + 15);
		else if (!strncasecmp(line, "Cookie:", 7)) {
			char* e = strstr(line, "\r\n");
			snprintf(r->cookie, sizeof(r->cookie), "%.*s", (int)(e ? e - line - 7 : (long)strlen(line + 7)), line + 7);
		}
	}
	return true;
}

// reads up to n body bytes (the ones that came with the header first)
static ssize_t body_read(Req* r, char* buf, size_t n) {
	if (r->nextra) {
		size_t k = r->nextra < n ? r->nextra : n;
		memcpy(buf, r->extra, k);
		r->extra += k;
		r->nextra -= k;
		return (ssize_t)k;
	}
	ssize_t g;
	do
		g = recv(r->fd, buf, n, 0);
	while (g < 0 && errno == EINTR);
	return g;
}
static char* body_all(Req* r, size_t max) {
	if (r->clen < 0 || (size_t)r->clen > max)
		return NULL;
	char* b = malloc((size_t)r->clen + 1);
	size_t got = 0;
	while (b && got < (size_t)r->clen) {
		ssize_t g = body_read(r, b + got, (size_t)r->clen - got);
		if (g <= 0) {
			free(b);
			return NULL;
		}
		got += (size_t)g;
	}
	if (b)
		b[got] = '\0';
	return b;
}

// ---------- the screen's list ----------
static int recent_add(const char* name, const char* folder, bool up, uint64_t total) {
	pthread_mutex_lock(&mx);
	if (nrecent == RECENT_MAX) { // drop the oldest that is not moving
		int k = 0;
		while (k < RECENT_MAX && recent[k].state == 0)
			k++;
		if (k == RECENT_MAX)
			k = 0;
		memmove(&recent[k], &recent[k + 1], sizeof(WtRecent) * (size_t)(RECENT_MAX - 1 - k));
		nrecent--;
	}
	WtRecent* e = &recent[nrecent];
	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", name);
	snprintf(e->folder, sizeof(e->folder), "%s", folder);
	e->up = up;
	e->total = total;
	int id = nrecent++;
	changed = 1;
	pthread_mutex_unlock(&mx);
	return id;
}
// the entries move when an old one drops: find ours again by name
static void recent_set(const char* name, uint64_t done, int state) {
	pthread_mutex_lock(&mx);
	for (int i = nrecent - 1; i >= 0; i--)
		if (recent[i].state == 0 && !strcmp(recent[i].name, name)) {
			recent[i].done = done;
			recent[i].state = state;
			break;
		}
	changed = 1;
	pthread_mutex_unlock(&mx);
}

// ---------- refs and folders ----------
static uint64_t free_bytes(void) {
	struct statvfs v;
	return statvfs(sd, &v) ? 0 : (uint64_t)v.f_bavail * v.f_frsize;
}

// every segment is a safe name, except the allowed hidden ones
static bool ref_ok(const char* ref) {
	if (!ref || !ref[0] || ref[0] == '/' || strlen(ref) > 900)
		return false;
	static const char* roots[] = {"Roms/", "Saves/", "Bios/", "Music/", "Videos/", "Images/Screenshots/", "Screenshots/",
								  ".userdata/shared/"};
	bool root = false;
	for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
		root |= !strncmp(ref, roots[i], strlen(roots[i]));
	if (!root)
		return false;
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", ref);
	int seg = 0;
	for (char *s = tmp, *e; s; s = e, seg++) {
		e = strchr(s, '/');
		if (e)
			*e++ = '\0';
		bool hidden_ok = (seg == 0 && !strcmp(s, ".userdata")) || !strcmp(s, ".media");
		if (!hidden_ok && !wt_name_safe(s))
			return false;
	}
	// .userdata/shared/<TAG>-<core>/<file>: states only
	if (!strncmp(ref, ".userdata/shared/", 17)) {
		const char* d = ref + 17;
		const char* sl = strchr(d, '/');
		if (!sl || strchr(sl + 1, '/') || !memchr(d, '-', (size_t)(sl - d)))
			return false;
		char base[512];
		if (wt_save_split(sl + 1, base, sizeof(base)) != WT_SAVE_STATE)
			return false;
	}
	return true;
}
static void full(const char* ref, char* out, size_t n) { snprintf(out, n, "%s/%s", sd, ref); }

static bool is_file(const char* p, struct stat* st) { return !stat(p, st) && S_ISREG(st->st_mode); }

// the console folder named c, its tag; false when it is not one the page offers
static bool console(const char* c, char* tag, size_t tag_size) {
	if (!c || !wt_name_safe(c) || !wt_folder_tag(c, tag, tag_size) || !wt_console_exts(tag))
		return false;
	char p[1024];
	snprintf(p, sizeof(p), "%s/Roms/%s", sd, c);
	struct stat st;
	return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static bool cover_path(const char* rom_ref, char* out, size_t n, bool any_ext) {
	char dir[1024], base[512];
	snprintf(dir, sizeof(dir), "%s/%s", sd, rom_ref);
	char* sl = strrchr(dir, '/');
	if (!sl)
		return false;
	wt_base(sl + 1, base, sizeof(base));
	*sl = '\0';
	snprintf(out, n, "%s/.media/%s.png", dir, base);
	struct stat st;
	if (!any_ext || is_file(out, &st))
		return true;
	static const char* alt[] = {"jpg", "jpeg", "webp"};
	for (int i = 0; i < 3; i++) {
		snprintf(out, n, "%s/.media/%s.%s", dir, base, alt[i]);
		if (is_file(out, &st))
			return true;
	}
	snprintf(out, n, "%s/.media/%s.png", dir, base);
	return false;
}

// a game file counted on the page: the parts of a .cue / .m3u (.bin, .img) are not games of their own
static bool counts_as_game(const char* tag, const char* name, bool has_sheet) {
	if (!wt_game_ok(tag, name))
		return false;
	char e[16];
	wt_ext(name, e, sizeof(e));
	return !(has_sheet && (!strcmp(e, "bin") || !strcmp(e, "img")));
}

static int cmp_str(const void* a, const void* b) { return strcasecmp(*(char* const*)a, *(char* const*)b); }

// the names in a folder (files only, no hidden ones), sorted; free with free_names
static int names(const char* dir, char*** out) {
	*out = NULL;
	DIR* d = opendir(dir);
	if (!d)
		return 0;
	int n = 0, cap = 0;
	struct dirent* e;
	char p[1200];
	struct stat st;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		if (!is_file(p, &st))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			char** q = realloc(*out, sizeof(char*) * (size_t)cap);
			if (!q)
				break;
			*out = q;
		}
		(*out)[n++] = strdup(e->d_name);
	}
	closedir(d);
	if (n)
		qsort(*out, (size_t)n, sizeof(char*), cmp_str);
	return n;
}
static void free_names(char** v, int n) {
	for (int i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

static bool has_sheet(char** v, int n) {
	for (int i = 0; i < n; i++) {
		char e[16];
		wt_ext(v[i], e, sizeof(e));
		if (!strcmp(e, "cue") || !strcmp(e, "m3u") || !strcmp(e, "gdi"))
			return true;
	}
	return false;
}

static void item(Buf* b, bool* first, const char* ref, const char* name, const struct stat* st) {
	b_str(b, *first ? "{\"r\":" : ",{\"r\":");
	*first = false;
	b_json(b, ref);
	b_str(b, ",\"n\":");
	b_json(b, name);
	b_fmt(b, ",\"s\":%llu,\"t\":%lld", (unsigned long long)st->st_size, (long long)st->st_mtime);
}

// the states folders of a tag: .userdata/shared/<tag>-<core>
static int states_dirs(const char* tag, char dirs[][128], int max) {
	char p[600];
	snprintf(p, sizeof(p), "%s/.userdata/shared", sd);
	DIR* d = opendir(p);
	if (!d)
		return 0;
	int n = 0;
	size_t tl = strlen(tag);
	struct dirent* e;
	while (n < max && (e = readdir(d)))
		if (!strncmp(e->d_name, tag, tl) && e->d_name[tl] == '-' && e->d_name[tl + 1] && wt_name_safe(e->d_name))
			snprintf(dirs[n++], 128, "%s", e->d_name);
	closedir(d);
	return n;
}

// the folder new states of a tag go to: the one used last, else <tag>-<EMU_EXE> from its pak
static bool states_dir_for(const char* tag, char* out, size_t n) {
	char dirs[8][128];
	int k = states_dirs(tag, dirs, 8);
	time_t best = -1;
	for (int i = 0; i < k; i++) {
		char p[700];
		struct stat st;
		snprintf(p, sizeof(p), "%s/.userdata/shared/%s", sd, dirs[i]);
		if (!stat(p, &st) && st.st_mtime > best) {
			best = st.st_mtime;
			snprintf(out, n, "%s", dirs[i]);
		}
	}
	if (best >= 0)
		return true;
	static const char* paks[] = {"%s/.system/paks/Emus/%s.pak/launch.sh", "%s/Emus/tg5040/%s.pak/launch.sh"};
	for (int i = 0; i < 2; i++) {
		char p[700], line[256], core[64] = "";
		snprintf(p, sizeof(p), paks[i], sd, tag);
		FILE* f = fopen(p, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			char* q = strstr(line, "EMU_EXE=");
			if (q && sscanf(q + 8, "%63[a-z0-9_]", core) != 1)
				core[0] = '\0'; // the last one wins
		}
		fclose(f);
		if (core[0]) {
			snprintf(out, n, "%s-%s", tag, core);
			return true;
		}
	}
	return false;
}

// ---------- the API ----------
static bool authed(Req* r) {
	char want[64];
	pthread_mutex_lock(&mx);
	snprintf(want, sizeof(want), "wt=%s", token);
	pthread_mutex_unlock(&mx);
	return strstr(r->cookie, want) != NULL;
}

static void api_login(Req* r) {
	char* body = body_all(r, 256);
	char got[16] = "";
	if (!param(r->query, "pin", 0, got, sizeof(got)) && body)
		param(body, "pin", 0, got, sizeof(got));
	free(body);
	pthread_mutex_lock(&mx);
	time_t now = time(NULL);
	if (now < locked_until) {
		int wait = (int)(locked_until - now);
		pthread_mutex_unlock(&mx);
		char e[32];
		snprintf(e, sizeof(e), "wait %d", wait);
		reply_err(r, 429, e);
		return;
	}
	bool ok = got[0] && !strcmp(got, pin);
	if (!ok && ++bad_pins >= 5) {
		bad_pins = 0;
		locked_until = now + 30;
	}
	if (ok)
		bad_pins = 0;
	char cookie[128];
	snprintf(cookie, sizeof(cookie), "Set-Cookie: wt=%s; Path=/; HttpOnly; SameSite=Strict\r\n", token);
	pthread_mutex_unlock(&mx);
	if (ok)
		reply(r, 200, "application/json", "{\"ok\":true}", 11, cookie);
	else
		reply_err(r, 401, "pin");
}

static void api_info(Req* r) {
	Buf b = {0};
	struct statvfs v;
	uint64_t total = statvfs(sd, &v) ? 0 : (uint64_t)v.f_blocks * v.f_frsize;
	b_fmt(&b, "{\"free\":%llu,\"total\":%llu,\"consoles\":[", (unsigned long long)free_bytes(), (unsigned long long)total);
	char p[700];
	snprintf(p, sizeof(p), "%s/Roms", sd);
	DIR* d = opendir(p);
	char** dirs = NULL;
	int nd = 0, cap = 0;
	struct dirent* e;
	while (d && (e = readdir(d))) {
		char tag[32];
		if (e->d_name[0] == '.' || !console(e->d_name, tag, sizeof(tag)))
			continue;
		if (nd == cap) {
			cap = cap ? cap * 2 : 64;
			char** q = realloc(dirs, sizeof(char*) * (size_t)cap);
			if (!q)
				break;
			dirs = q;
		}
		dirs[nd++] = strdup(e->d_name);
	}
	if (d)
		closedir(d);
	if (nd)
		qsort(dirs, (size_t)nd, sizeof(char*), cmp_str);
	for (int i = 0; i < nd; i++) {
		char tag[32], gdir[1100];
		console(dirs[i], tag, sizeof(tag));
		snprintf(gdir, sizeof(gdir), "%s/Roms/%s", sd, dirs[i]);
		char** v2;
		int n = names(gdir, &v2), games = 0;
		bool sheet = has_sheet(v2, n);
		for (int k = 0; k < n; k++)
			games += counts_as_game(tag, v2[k], sheet);
		free_names(v2, n);
		char name[256];
		snprintf(name, sizeof(name), "%s", dirs[i]);
		char* open = strrchr(name, '(');
		if (open && open > name) {
			open[-1] == ' ' ? (open[-1] = '\0') : (*open = '\0');
		}
		b_str(&b, i ? ",{\"c\":" : "{\"c\":");
		b_json(&b, dirs[i]);
		b_str(&b, ",\"name\":");
		b_json(&b, name);
		b_str(&b, ",\"tag\":");
		b_json(&b, tag);
		b_str(&b, ",\"exts\":");
		b_json(&b, wt_console_exts(tag));
		b_fmt(&b, ",\"zip\":%s,\"saves\":%s,\"games\":%d}", wt_console_archives(tag) ? "true" : "false",
			  wt_console_saves(tag) ? "true" : "false", games);
		free(dirs[i]);
	}
	free(dirs);
	b_str(&b, "]}");
	reply_json(r, 200, &b);
	free(b.p);
}

// files of one folder (ref prefix rel) that pass ok(); with covers for games
static void list_folder(Buf* b, bool* first, const char* rel, bool (*ok)(const char*), const char* tag, bool games) {
	char dir[1100];
	snprintf(dir, sizeof(dir), "%s/%s", sd, rel);
	char** v;
	int n = names(dir, &v);
	bool sheet = games && has_sheet(v, n);
	for (int i = 0; i < n; i++) {
		if (games ? !counts_as_game(tag, v[i], sheet) && !wt_game_ok(tag, v[i]) : !ok(v[i]))
			continue;
		char ref[1400], p[1500];
		struct stat st;
		snprintf(ref, sizeof(ref), "%s/%s", rel, v[i]);
		full(ref, p, sizeof(p));
		if (!is_file(p, &st))
			continue;
		item(b, first, ref, v[i], &st);
		if (games) {
			char cp[1600];
			struct stat cs;
			bool has = cover_path(ref, cp, sizeof(cp), true) && is_file(cp, &cs);
			b_fmt(b, ",\"cover\":%s,\"part\":%s", has ? "true" : "false",
				  counts_as_game(tag, v[i], sheet) ? "false" : "true");
			if (has)
				b_fmt(b, ",\"ct\":%lld", (long long)cs.st_mtime);
		}
		b_str(b, "}");
	}
	free_names(v, n);
}

// music / videos: two folder levels down
static void list_tree(Buf* b, bool* first, const char* rel, bool (*ok)(const char*), int depth) {
	list_folder(b, first, rel, ok, NULL, false);
	if (!depth)
		return;
	char dir[1100];
	snprintf(dir, sizeof(dir), "%s/%s", sd, rel);
	DIR* d = opendir(dir);
	struct dirent* e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.' || !wt_name_safe(e->d_name))
			continue;
		char sub[1400], p[1500];
		struct stat st;
		snprintf(sub, sizeof(sub), "%s/%s", rel, e->d_name);
		full(sub, p, sizeof(p));
		if (!stat(p, &st) && S_ISDIR(st.st_mode))
			list_tree(b, first, sub, ok, depth - 1);
	}
	if (d)
		closedir(d);
}

static bool any_save(const char* n) {
	char base[512];
	return wt_save_split(n, base, sizeof(base)) != WT_SAVE_NONE;
}
static bool any_file(const char* n) { return n[0] != '.'; }

static void api_list(Req* r) {
	char k[16] = "", c[300] = "", tag[32] = "";
	param(r->query, "k", 0, k, sizeof(k));
	param(r->query, "c", 0, c, sizeof(c));
	bool need_console = !strcmp(k, "games") || !strcmp(k, "saves") || !strcmp(k, "covers") || !strcmp(k, "bios");
	if (need_console && !console(c, tag, sizeof(tag))) {
		reply_err(r, 404, "console");
		return;
	}
	Buf b = {0};
	bool first = true;
	char rel[400];
	b_str(&b, "{\"items\":[");
	if (!strcmp(k, "games") || !strcmp(k, "covers")) {
		snprintf(rel, sizeof(rel), "Roms/%s", c);
		list_folder(&b, &first, rel, NULL, tag, true);
	} else if (!strcmp(k, "saves")) {
		snprintf(rel, sizeof(rel), "Saves/%s", tag);
		list_folder(&b, &first, rel, any_save, NULL, false);
		char dirs[8][128];
		int n = states_dirs(tag, dirs, 8);
		for (int i = 0; i < n; i++) {
			snprintf(rel, sizeof(rel), ".userdata/shared/%s", dirs[i]);
			list_folder(&b, &first, rel, any_save, NULL, false);
		}
		b_str(&b, "],\"games\":[");
		first = true;
		snprintf(rel, sizeof(rel), "Roms/%s", c);
		list_folder(&b, &first, rel, NULL, tag, true);
	} else if (!strcmp(k, "bios")) {
		snprintf(rel, sizeof(rel), "Bios/%s", tag);
		list_folder(&b, &first, rel, any_file, NULL, false);
	} else if (!strcmp(k, "music"))
		list_tree(&b, &first, "Music", wt_music_ok, 2);
	else if (!strcmp(k, "videos"))
		list_tree(&b, &first, "Videos", wt_video_ok, 2);
	else if (!strcmp(k, "shots")) {
		list_folder(&b, &first, "Images/Screenshots", wt_shot_ok, NULL, false);
		list_folder(&b, &first, "Screenshots", wt_shot_ok, NULL, false);
	} else {
		free(b.p);
		reply_err(r, 400, "kind");
		return;
	}
	b_str(&b, "]}");
	reply_json(r, 200, &b);
	free(b.p);
}

static const char* mime(const char* name) {
	char e[16];
	wt_ext(name, e, sizeof(e));
	return !strcmp(e, "png") ? "image/png" : !strcmp(e, "jpg") || !strcmp(e, "jpeg") ? "image/jpeg"
		   : !strcmp(e, "webp") ? "image/webp" : !strcmp(e, "mp4") ? "video/mp4" : !strcmp(e, "mp3") ? "audio/mpeg"
		   : "application/octet-stream";
}

// Content-Disposition for a UTF-8 name: an ASCII stand-in and the real one
static void disposition(const char* name, bool attach, char* out, size_t n) {
	char ascii[256], enc[1024];
	size_t a = 0, o = 0;
	for (const unsigned char* p = (const unsigned char*)name; *p && a + 1 < sizeof(ascii); p++)
		ascii[a++] = *p >= 0x20 && *p < 0x7f && *p != '"' && *p != '\\' ? (char)*p : '_';
	ascii[a] = '\0';
	for (const unsigned char* p = (const unsigned char*)name; *p && o + 4 < sizeof(enc); p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p))
			enc[o++] = (char)*p;
		else
			o += (size_t)snprintf(enc + o, sizeof(enc) - o, "%%%02X", *p);
	}
	enc[o] = '\0';
	snprintf(out, n, "Content-Disposition: %s; filename=\"%s\"; filename*=UTF-8''%s\r\n", attach ? "attachment" : "inline",
			 ascii, enc);
}

static void send_file(Req* r, const char* path, const char* name, bool attach, bool track) {
	struct stat st;
	FILE* f = is_file(path, &st) ? fopen(path, "rb") : NULL;
	if (!f) {
		reply_err(r, 404, "file");
		return;
	}
	char disp[1400], h[1800];
	disposition(name, attach, disp, sizeof(disp));
	int hn = snprintf(h, sizeof(h),
					  "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %llu\r\n%sCache-Control: no-store\r\nConnection: close\r\n\r\n",
					  mime(name), (unsigned long long)st.st_size, disp);
	bool ok = send_all(r->fd, h, (size_t)hn);
	if (track)
		recent_add(name, "", false, (uint64_t)st.st_size);
	char* buf = malloc(256 * 1024);
	uint64_t done = 0;
	size_t g;
	while (ok && buf && (g = fread(buf, 1, 256 * 1024, f)) > 0) {
		ok = send_all(r->fd, buf, g);
		done += g;
	}
	free(buf);
	fclose(f);
	if (track)
		recent_set(name, done, ok && done == (uint64_t)st.st_size ? 1 : 2);
}

static void api_dl(Req* r) {
	char ref[1024], p[1300];
	if (!param(r->query, "r", 0, ref, sizeof(ref)) || !ref_ok(ref)) {
		reply_err(r, 404, "file");
		return;
	}
	full(ref, p, sizeof(p));
	send_file(r, p, strrchr(ref, '/') + 1, true, true);
}

static void api_cover(Req* r) {
	char ref[1024], p[1300];
	if (!param(r->query, "r", 0, ref, sizeof(ref)) || strncmp(ref, "Roms/", 5) || !ref_ok(ref) ||
		!cover_path(ref, p, sizeof(p), true)) {
		reply_err(r, 404, "cover");
		return;
	}
	send_file(r, p, strrchr(p, '/') + 1, false, false);
}

static void api_delcover(Req* r) {
	char ref[1024], p[1300];
	free(body_all(r, 64));
	if (!param(r->query, "r", 0, ref, sizeof(ref)) || strncmp(ref, "Roms/", 5) || !ref_ok(ref)) {
		reply_err(r, 404, "cover");
		return;
	}
	int gone = 0;
	while (cover_path(ref, p, sizeof(p), true) && !unlink(p))
		gone++;
	gone ? reply(r, 200, "application/json", "{\"ok\":true}", 11, NULL) : reply_err(r, 404, "cover");
}

// POST f=<ref>&f=<ref>...: one stored zip
static void api_zip(Req* r) {
	char* body = body_all(r, 4u << 20);
	if (!body) {
		reply_err(r, 400, "body");
		return;
	}
	WtZipItem* items = calloc(WT_ZIP_MAX, sizeof(WtZipItem));
	char** keep = calloc(WT_ZIP_MAX * 2, sizeof(char*));
	int n = 0;
	char ref[1024], zname[64] = "BrickOS.zip";
	param(body, "z", 0, zname, sizeof(zname));
	if (!wt_name_safe(zname))
		snprintf(zname, sizeof(zname), "BrickOS.zip");
	for (int i = 0; items && keep && n < WT_ZIP_MAX && param(body, "f", i, ref, sizeof(ref)); i++) {
		char p[1300];
		struct stat st;
		if (!ref_ok(ref))
			continue;
		full(ref, p, sizeof(p));
		if (!is_file(p, &st))
			continue;
		// the name inside: the file's own, " (2)" when it is there already
		const char* bn = strrchr(ref, '/') + 1;
		char nm[600];
		snprintf(nm, sizeof(nm), "%s", bn);
		for (int k = 2, dup = 1; dup; k++) {
			dup = 0;
			for (int j = 0; j < n && !dup; j++)
				dup = !strcmp(items[j].name, nm);
			if (dup) {
				char b2[512], e2[16];
				wt_base(bn, b2, sizeof(b2));
				wt_ext(bn, e2, sizeof(e2));
				snprintf(nm, sizeof(nm), e2[0] ? "%s (%d).%s" : "%s (%d)", b2, k, strrchr(bn, '.') ? strrchr(bn, '.') + 1 : "");
			}
		}
		keep[2 * n] = strdup(p);
		keep[2 * n + 1] = strdup(nm);
		items[n] = (WtZipItem){keep[2 * n], keep[2 * n + 1], (uint64_t)st.st_size, (uint32_t)st.st_mtime};
		n++;
	}
	free(body);
	uint64_t len = n ? wt_zip_length(items, n) : 0;
	if (!len)
		reply_err(r, 400, n ? "too big" : "empty");
	else {
		char disp[400], h[800];
		disposition(zname, true, disp, sizeof(disp));
		int hn = snprintf(h, sizeof(h),
						  "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: %llu\r\n%sCache-Control: no-store\r\nConnection: close\r\n\r\n",
						  (unsigned long long)len, disp);
		char label[64];
		snprintf(label, sizeof(label), "%s", zname);
		recent_add(label, "", false, len);
		bool ok = send_all(r->fd, h, (size_t)hn) && wt_zip_write(items, n, zip_out, &r->fd);
		recent_set(label, ok ? len : 0, ok ? 1 : 2);
	}
	for (int i = 0; keep && i < 2 * n; i++)
		free(keep[i]);
	free(keep);
	free(items);
}

static bool mkdirs(const char* path) { // every folder of path (a file path: its last part is not made)
	char t[1300];
	snprintf(t, sizeof(t), "%s", path);
	for (char* p = t + strlen(sd) + 1; (p = strchr(p, '/')); p++) {
		*p = '\0';
		if (mkdir(t, 0755) && errno != EEXIST)
			return false;
		*p = '/';
	}
	return true;
}

// the old file goes to .backup/<name>.<time> next to it
static bool backup(const char* path) {
	char dir[1300], bp[1500], stamp[32];
	snprintf(dir, sizeof(dir), "%s", path);
	char* sl = strrchr(dir, '/');
	if (!sl)
		return false;
	*sl = '\0';
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &lt);
	snprintf(bp, sizeof(bp), "%s/.backup", dir);
	mkdir(bp, 0755);
	snprintf(bp, sizeof(bp), "%s/.backup/%s.%s", dir, sl + 1, stamp);
	return !rename(path, bp);
}

// PUT /api/up?k=&c=&n=<file name>&g=<game file, for saves and covers>&x=1 (replace); q=1 only answers
// whether a file is there already (nothing is read or written)
static void api_up(Req* r) {
	char k[16] = "", c[300] = "", n[300] = "", g[300] = "", x[4] = "", tag[32] = "";
	param(r->query, "k", 0, k, sizeof(k));
	param(r->query, "c", 0, c, sizeof(c));
	param(r->query, "n", 0, n, sizeof(n));
	param(r->query, "g", 0, g, sizeof(g));
	param(r->query, "x", 0, x, sizeof(x));
	bool replace = x[0] == '1';
	if (r->clen < 0) {
		reply_err(r, 400, "length");
		return;
	}
	if (!wt_name_safe(n)) {
		reply_err(r, 400, "name");
		return;
	}
	char ref[1024] = "", folder[96] = "";
	bool need_console = !strcmp(k, "games") || !strcmp(k, "saves") || !strcmp(k, "covers") || !strcmp(k, "bios");
	if (need_console && !console(c, tag, sizeof(tag))) {
		reply_err(r, 404, "console");
		return;
	}
	bool is_save = false;
	if (!strcmp(k, "games")) {
		if (!wt_game_ok(tag, n)) {
			reply_err(r, 400, "type");
			return;
		}
		snprintf(ref, sizeof(ref), "Roms/%s/%s", c, n);
		snprintf(folder, sizeof(folder), "%s", c);
	} else if (!strcmp(k, "covers") || !strcmp(k, "saves")) {
		char gref[1024], gp[1300];
		struct stat st;
		snprintf(gref, sizeof(gref), "Roms/%s/%s", c, g);
		full(gref, gp, sizeof(gp));
		if (!wt_name_safe(g) || !is_file(gp, &st)) {
			reply_err(r, 404, "game");
			return;
		}
		if (!strcmp(k, "covers")) {
			char cp[1300];
			cover_path(gref, cp, sizeof(cp), false);
			snprintf(ref, sizeof(ref), "%s", cp + strlen(sd) + 1);
			snprintf(folder, sizeof(folder), "%s", c);
		} else {
			char out[600];
			int kind = wt_save_target(tag, n, g, cfg.save_format, cfg.state_format, out, sizeof(out));
			if (kind == WT_SAVE_NONE) {
				reply_err(r, 400, "type");
				return;
			}
			if (kind == WT_SAVE_BATTERY)
				snprintf(ref, sizeof(ref), "Saves/%s/%s", tag, out);
			else {
				char sdir[128];
				if (!states_dir_for(tag, sdir, sizeof(sdir))) {
					reply_err(r, 400, "nostates");
					return;
				}
				snprintf(ref, sizeof(ref), ".userdata/shared/%s/%s", sdir, out);
			}
			snprintf(folder, sizeof(folder), "Saves/%s", tag);
			is_save = true;
		}
	} else if (!strcmp(k, "music") || !strcmp(k, "videos")) {
		bool music = k[0] == 'm';
		if (!(music ? wt_music_ok(n) : wt_video_ok(n))) {
			reply_err(r, 400, "type");
			return;
		}
		snprintf(ref, sizeof(ref), "%s/%s", music ? "Music" : "Videos", n);
		snprintf(folder, sizeof(folder), "%s", music ? "Music" : "Videos");
	} else if (!strcmp(k, "bios")) {
		if (!wt_bios_ok(n)) {
			reply_err(r, 400, "type");
			return;
		}
		snprintf(ref, sizeof(ref), "Bios/%s/%s", tag, n);
		snprintf(folder, sizeof(folder), "Bios/%s", tag);
	} else {
		reply_err(r, 400, "kind");
		return;
	}
	char path[1300], part[1400];
	full(ref, path, sizeof(path));
	struct stat st;
	bool exists = is_file(path, &st);
	char q[4] = "";
	if (param(r->query, "q", 0, q, sizeof(q)) && q[0] == '1') { // only asking: would it replace one?
		Buf b = {0};
		b_fmt(&b, "{\"ok\":true,\"exists\":%s,\"to\":", exists ? "true" : "false");
		b_json(&b, strrchr(ref, '/') + 1);
		b_str(&b, "}");
		reply_json(r, 200, &b);
		free(b.p);
		return;
	}
	if (exists && !replace) {
		Buf b = {0};
		b_fmt(&b, "{\"error\":\"exists\",\"s\":%llu,\"t\":%lld,\"to\":", (unsigned long long)st.st_size, (long long)st.st_mtime);
		b_json(&b, strrchr(ref, '/') + 1);
		b_str(&b, "}");
		reply_json(r, 409, &b);
		free(b.p);
		return;
	}
	uint64_t room = free_bytes();
	if ((uint64_t)r->clen + SPACE_SPARE > room + (exists ? (uint64_t)st.st_size : 0)) {
		char e[64];
		snprintf(e, sizeof(e), "space %llu", (unsigned long long)((uint64_t)r->clen + SPACE_SPARE - room));
		reply_err(r, 507, e);
		return;
	}
	if (!mkdirs(path)) {
		reply_err(r, 400, "folder");
		return;
	}
	snprintf(part, sizeof(part), "%s.wtpart", path);
	int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		reply_err(r, 400, "open");
		return;
	}
	const char* shown = strrchr(ref, '/') + 1;
	recent_add(shown, folder, true, (uint64_t)r->clen);
	char* buf = malloc(256 * 1024);
	uint64_t got = 0, last_mark = 0;
	bool ok = buf != NULL;
	unsigned char head[8] = {0};
	while (ok && got < (uint64_t)r->clen) {
		size_t want = (uint64_t)r->clen - got < 256 * 1024 ? (size_t)((uint64_t)r->clen - got) : 256 * 1024;
		ssize_t g2 = body_read(r, buf, want);
		if (g2 <= 0) {
			ok = false;
			break;
		}
		if (got < 8)
			memcpy(head + got, buf, (size_t)(8 - got) < (size_t)g2 ? (size_t)(8 - got) : (size_t)g2);
		ok = write(fd, buf, (size_t)g2) == g2;
		got += (uint64_t)g2;
		if (got - last_mark > (1u << 20)) {
			recent_set(shown, got, 0);
			last_mark = got;
		}
	}
	free(buf);
	static const unsigned char png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	bool bad_png = ok && !strcmp(k, "covers") && memcmp(head, png, 8);
	ok = ok && !bad_png && fsync(fd) == 0;
	close(fd);
	if (ok && exists && is_save)
		ok = backup(path);
	ok = ok && !rename(part, path);
	if (!ok)
		unlink(part);
	recent_set(shown, got, ok ? 1 : 2);
	if (!ok) {
		reply_err(r, 400, bad_png ? "png" : "write");
		return;
	}
	if (!strcmp(k, "games")) { // the game lists are built again
		char p[700];
		snprintf(p, sizeof(p), "%s/.userdata/tg5040/emulist_cache.txt", sd);
		unlink(p);
		snprintf(p, sizeof(p), "%s/.userdata/tg5040/romindex_cache.txt", sd);
		unlink(p);
	}
	Buf b = {0};
	b_str(&b, "{\"ok\":true,\"r\":");
	b_json(&b, ref);
	b_str(&b, "}");
	reply_json(r, 200, &b);
	free(b.p);
}

static void serve(Req* r) {
	if (!strcmp(r->path, "/") || !strcmp(r->path, "/index.html")) {
		char p[600];
		snprintf(p, sizeof(p), "%s/index.html", web_dir);
		struct stat st;
		FILE* f = is_file(p, &st) ? fopen(p, "rb") : NULL;
		char* b = f ? malloc((size_t)st.st_size) : NULL;
		size_t n = b ? fread(b, 1, (size_t)st.st_size, f) : 0;
		if (f)
			fclose(f);
		reply(r, b ? 200 : 404, "text/html; charset=utf-8", b ? b : "", n, NULL);
		free(b);
		return;
	}
	if (!strcmp(r->path, "/api/login")) {
		api_login(r);
		return;
	}
	if (strncmp(r->path, "/api/", 5)) {
		reply_err(r, 404, "page");
		return;
	}
	if (!authed(r)) {
		reply_err(r, 401, "login");
		return;
	}
	const char* a = r->path + 5;
	bool get = !strcmp(r->method, "GET") || !strcmp(r->method, "HEAD");
	if (get && !strcmp(a, "info"))
		api_info(r);
	else if (get && !strcmp(a, "list"))
		api_list(r);
	else if (get && !strcmp(a, "dl"))
		api_dl(r);
	else if (get && !strcmp(a, "cover"))
		api_cover(r);
	else if (!strcmp(r->method, "POST") && !strcmp(a, "zip"))
		api_zip(r);
	else if (!strcmp(r->method, "POST") && !strcmp(a, "delcover"))
		api_delcover(r);
	else if (!strcmp(r->method, "PUT") && !strcmp(a, "up"))
		api_up(r);
	else
		reply_err(r, 404, "api");
}

static void* conn_main(void* arg) {
	Req* r = arg;
	fd_track(r->fd, true);
	struct timeval tv = {60, 0};
	setsockopt(r->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(r->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (read_request(r))
		serve(r);
	shutdown(r->fd, SHUT_WR);
	fd_track(r->fd, false);
	close(r->fd);
	free(r);
	__sync_fetch_and_sub(&conns, 1);
	return NULL;
}

static void* accept_main(void* arg) {
	(void)arg;
	while (running) {
		struct pollfd p = {listen_fd, POLLIN, 0};
		if (poll(&p, 1, 250) <= 0)
			continue;
		int fd = accept(listen_fd, NULL, NULL);
		if (fd < 0)
			continue;
		if (conns >= MAX_CONN) {
			close(fd);
			continue;
		}
		Req* r = malloc(sizeof(Req));
		if (!r) {
			close(fd);
			continue;
		}
		memset(r, 0, offsetof(Req, hdr));
		r->fd = fd;
		__sync_fetch_and_add(&conns, 1);
		pthread_t t;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&at, 256 * 1024);
		if (pthread_create(&t, &at, conn_main, r)) {
			close(fd);
			free(r);
			__sync_fetch_and_sub(&conns, 1);
		}
		pthread_attr_destroy(&at);
	}
	return NULL;
}

static void new_token(void) {
	unsigned char b[16];
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0 || read(fd, b, sizeof(b)) != (ssize_t)sizeof(b))
		for (size_t i = 0; i < sizeof(b); i++)
			b[i] = (unsigned char)(rand() ^ (int)time(NULL));
	if (fd >= 0)
		close(fd);
	for (int i = 0; i < 16; i++)
		sprintf(token + 2 * i, "%02x", b[i]);
}

void wt_server_set_pin(const char* p) {
	pthread_mutex_lock(&mx);
	snprintf(pin, sizeof(pin), "%s", p);
	new_token();
	bad_pins = 0;
	locked_until = 0;
	pthread_mutex_unlock(&mx);
}

bool wt_server_start(const WtConfig* c) {
	cfg = *c;
	snprintf(sd, sizeof(sd), "%s", c->sd);
	snprintf(web_dir, sizeof(web_dir), "%s", c->web_dir);
	if (!pin[0])
		wt_server_set_pin("0000");
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
		return false;
	int one = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_port = htons(WT_PORT);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listen_fd, (struct sockaddr*)&a, sizeof(a)) || listen(listen_fd, 16)) {
		close(listen_fd);
		listen_fd = -1;
		return false;
	}
	running = 1;
	if (pthread_create(&accept_thread, NULL, accept_main, NULL)) {
		running = 0;
		close(listen_fd);
		listen_fd = -1;
		return false;
	}
	return true;
}

void wt_server_stop(void) {
	if (listen_fd < 0)
		return;
	running = 0;
	pthread_join(accept_thread, NULL);
	close(listen_fd);
	listen_fd = -1;
	// cut the open connections: a file still coming in stops, and its .wtpart is removed by its own thread
	pthread_mutex_lock(&mx);
	for (int i = 0; i < MAX_CONN + 4; i++)
		if (open_fds[i])
			shutdown(open_fds[i] - 1, SHUT_RDWR);
	pthread_mutex_unlock(&mx);
	for (int i = 0; i < 60 && conns > 0; i++)
		usleep(50 * 1000);
}

int wt_server_recent(WtRecent* out, int max) {
	pthread_mutex_lock(&mx);
	int n = 0;
	for (int i = nrecent - 1; i >= 0 && n < max; i--)
		out[n++] = recent[i];
	pthread_mutex_unlock(&mx);
	return n;
}

int wt_server_moving(void) {
	pthread_mutex_lock(&mx);
	int n = 0;
	for (int i = 0; i < nrecent; i++)
		n += recent[i].state == 0;
	pthread_mutex_unlock(&mx);
	return n;
}

bool wt_server_changed(void) { return __sync_lock_test_and_set(&changed, 0) != 0; }
