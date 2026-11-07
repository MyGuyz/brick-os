// Brick OS YouTube engine (yt_engine.h).
#define _GNU_SOURCE
#include "yt_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "defines.h"
#include "api.h"
#include "wget_fetch.h"

#define DATA_DIR SHARED_USERDATA_PATH "/youtube"
#define CHANNELS_FILE DATA_DIR "/channels.txt"
#define HISTORY_FILE DATA_DIR "/history.txt"
#define POSITIONS_FILE DATA_DIR "/positions.txt"
#define TMP_OUT "/tmp/yt_out.txt"
#define TMP_ERR "/tmp/yt_err.txt"
#define FEED_MAX (256 * 1024)
#define HISTORY_MAX 50
#define SEARCH_COUNT 20

static char ytdlp[512]; // <pak>/bin/yt-dlp/yt-dlp_linux_aarch64
static char qjs[512];	// <pak>/bin/qjs
static char updater[512];

void Yt_init(const char* pak_dir) {
	snprintf(ytdlp, sizeof(ytdlp), "%s/bin/yt-dlp/yt-dlp_linux_aarch64", pak_dir);
	snprintf(qjs, sizeof(qjs), "%s/bin/qjs", pak_dir);
	snprintf(updater, sizeof(updater), "%s/update-ytdlp.sh", pak_dir);
	mkdir(DATA_DIR, 0755);
}

bool Yt_engineInstalled(void) {
	return access(ytdlp, X_OK) == 0 && access(qjs, X_OK) == 0;
}

void Yt_formatDuration(int sec, char* out, int n) {
	if (sec < 0) {
		if (n > 0)
			out[0] = '\0';
		return;
	}
	if (sec >= 3600)
		snprintf(out, n, "%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
	else
		snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}

// ---- shell helpers --------------------------------------------------------

// Keep a user string safe inside single quotes: drop the characters that could end the quoting.
static void shell_clean(const char* in, char* out, int n) {
	int j = 0;
	for (int i = 0; in[i] && j < n - 1; i++) {
		char c = in[i];
		if (c == '\'' || c == '"' || c == '`' || c == '$' || c == '\\' || c == '\n')
			continue;
		out[j++] = c;
	}
	out[j] = '\0';
}

static void first_error_line(char* out, int n) {
	out[0] = '\0';
	FILE* f = fopen(TMP_ERR, "r");
	if (!f)
		return;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "ERROR")) {
			line[strcspn(line, "\n")] = '\0';
			snprintf(out, n, "%s", line);
			break;
		}
	}
	fclose(f);
}

static void friendly_error(YtOp* op, const char* fallback) {
	char err[400];
	first_error_line(err, sizeof(err));
	if (strstr(err, "resolve") || strstr(err, "Network") || strstr(err, "network"))
		snprintf(op->error, sizeof(op->error), "No internet connection");
	else if (strstr(err, "timed out"))
		snprintf(op->error, sizeof(op->error), "Connection timed out");
	else if (strstr(err, "Sign in") || strstr(err, "bot"))
		snprintf(op->error, sizeof(op->error), "YouTube blocked this request. Try again later");
	else
		snprintf(op->error, sizeof(op->error), "%s", fallback);
}

// ---- search ---------------------------------------------------------------

static int parse_tsv(const char* path, YtList* list) {
	list->count = 0;
	FILE* f = fopen(path, "r");
	if (!f)
		return 0;
	char line[2048];
	while (fgets(line, sizeof(line), f) && list->count < YT_MAX_ITEMS) {
		line[strcspn(line, "\n")] = '\0';
		char* fields[5] = {0};
		int nf = 0;
		char* p = line;
		fields[nf++] = p;
		while (nf < 5 && (p = strchr(p, '\t'))) {
			*p++ = '\0';
			fields[nf++] = p;
		}
		if (nf < 5 || !fields[0][0] || !strcmp(fields[0], "NA"))
			continue;
		YtVideo* v = &list->items[list->count++];
		memset(v, 0, sizeof(*v));
		snprintf(v->id, sizeof(v->id), "%s", fields[0]);
		snprintf(v->title, sizeof(v->title), "%s", fields[1]);
		snprintf(v->channel, sizeof(v->channel), "%s", strcmp(fields[2], "NA") ? fields[2] : "");
		snprintf(v->channel_id, sizeof(v->channel_id), "%s", strcmp(fields[3], "NA") ? fields[3] : "");
		v->duration = strcmp(fields[4], "NA") ? atoi(fields[4]) : -1;
	}
	fclose(f);
	return list->count;
}

static void* search_thread(void* arg) {
	YtOp* op = arg;
	char q[512];
	shell_clean(op->arg, q, sizeof(q));
	char cmd[2048];
	snprintf(cmd, sizeof(cmd),
			 "'%s' --no-cache-dir --no-warnings --socket-timeout 15 --flat-playlist"
			 " 'ytsearch%d:%s' --print '%%(id)s\t%%(title)s\t%%(channel)s\t%%(channel_id)s\t%%(duration)s'"
			 " > " TMP_OUT " 2> " TMP_ERR,
			 ytdlp, SEARCH_COUNT, q);
	LOG_info("yt search: %s\n", q);
	system(cmd);
	if (op->cancel) {
		op->state = YT_IDLE;
		return NULL;
	}
	if (parse_tsv(TMP_OUT, &op->list) > 0)
		op->state = YT_DONE;
	else {
		friendly_error(op, "No results found");
		op->state = YT_ERROR;
	}
	return NULL;
}

// ---- stream URL -----------------------------------------------------------

static bool resolve_with(const char* video_id, const char* client_args, char* url, int n) {
	char cmd[1536];
	snprintf(cmd, sizeof(cmd),
			 "'%s' --no-cache-dir --no-warnings --socket-timeout 15 %s"
			 " -f '18/best[height<=480][vcodec!=none][acodec!=none]' -g"
			 " 'https://www.youtube.com/watch?v=%s' > " TMP_OUT " 2> " TMP_ERR,
			 ytdlp, client_args, video_id);
	system(cmd);
	url[0] = '\0';
	FILE* f = fopen(TMP_OUT, "r");
	if (f) {
		if (fgets(url, n, f))
			url[strcspn(url, "\n")] = '\0';
		fclose(f);
	}
	return strncmp(url, "http", 4) == 0;
}

static void* resolve_thread(void* arg) {
	YtOp* op = arg;
	char id[YT_ID];
	shell_clean(op->arg, id, sizeof(id));
	// The android_vr client answers without the JavaScript challenge (fast on this CPU);
	// the web client with QuickJS solving it is the fallback.
	bool ok = resolve_with(id, "--js-runtimes none --extractor-args youtube:player_client=android_vr", op->url, sizeof(op->url));
	if (!ok && !op->cancel) {
		char js[640];
		snprintf(js, sizeof(js), "--js-runtimes 'quickjs:%s'", qjs);
		ok = resolve_with(id, js, op->url, sizeof(op->url));
	}
	if (op->cancel)
		op->state = YT_IDLE;
	else if (ok)
		op->state = YT_DONE;
	else {
		friendly_error(op, "Could not get the video");
		op->state = YT_ERROR;
	}
	return NULL;
}

// ---- channel feeds (RSS, no yt-dlp) ---------------------------------------

static void xml_unescape(char* s) {
	static const struct {
		const char* ent;
		char ch;
	} ents[] = {{"&amp;", '&'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}};
	char* w = s;
	for (char* r = s; *r;) {
		bool hit = false;
		for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
			size_t l = strlen(ents[i].ent);
			if (!strncmp(r, ents[i].ent, l)) {
				*w++ = ents[i].ch;
				r += l;
				hit = true;
				break;
			}
		}
		if (!hit)
			*w++ = *r++;
	}
	*w = '\0';
}

// Copy the text of the first <tag>...</tag> inside [from, end) into out.
static bool xml_get(const char* from, const char* end, const char* tag, char* out, int n) {
	char open[48], close[48];
	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close, sizeof(close), "</%s>", tag);
	const char* a = strstr(from, open);
	if (!a || a >= end)
		return false;
	a += strlen(open);
	const char* b = strstr(a, close);
	if (!b || b > end)
		return false;
	int len = (int)(b - a);
	if (len >= n)
		len = n - 1;
	memcpy(out, a, len);
	out[len] = '\0';
	xml_unescape(out);
	return true;
}

static long parse_iso8601(const char* s) {
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	if (!strptime(s, "%Y-%m-%dT%H:%M:%S", &tm))
		return 0;
	return (long)timegm(&tm);
}

static void feed_add_channel(const YtChannel* ch, uint8_t* buf, YtList* all) {
	char url[256];
	snprintf(url, sizeof(url), "https://www.youtube.com/feeds/videos.xml?channel_id=%s", ch->id);
	int n = wget_fetch(url, buf, FEED_MAX - 1);
	if (n <= 0)
		return;
	buf[n] = '\0';
	const char* p = (const char*)buf;
	while ((p = strstr(p, "<entry>")) && all->count < YT_MAX_ITEMS * 4) {
		const char* end = strstr(p, "</entry>");
		if (!end)
			break;
		YtVideo v;
		memset(&v, 0, sizeof(v));
		v.duration = -1;
		char pub[64] = "";
		if (xml_get(p, end, "yt:videoId", v.id, sizeof(v.id)) && xml_get(p, end, "title", v.title, sizeof(v.title))) {
			xml_get(p, end, "published", pub, sizeof(pub));
			v.published = parse_iso8601(pub);
			snprintf(v.channel, sizeof(v.channel), "%s", ch->name);
			snprintf(v.channel_id, sizeof(v.channel_id), "%s", ch->id);
			if (all->count < YT_MAX_ITEMS)
				all->items[all->count++] = v;
			else {
				// keep the newest: replace the oldest entry if this one is newer
				int oldest = 0;
				for (int i = 1; i < all->count; i++)
					if (all->items[i].published < all->items[oldest].published)
						oldest = i;
				if (v.published > all->items[oldest].published)
					all->items[oldest] = v;
			}
		}
		p = end;
	}
}

static int by_published_desc(const void* a, const void* b) {
	long pa = ((const YtVideo*)a)->published, pb = ((const YtVideo*)b)->published;
	return pa < pb ? 1 : pa > pb ? -1 : 0;
}

static void* feed_thread(void* arg) {
	YtOp* op = arg;
	YtChannel chans[64];
	int nc = Yt_channels(chans, 64);
	op->list.count = 0;
	uint8_t* buf = malloc(FEED_MAX);
	if (!buf) {
		snprintf(op->error, sizeof(op->error), "Out of memory.");
		op->state = YT_ERROR;
		return NULL;
	}
	for (int i = 0; i < nc && !op->cancel; i++)
		feed_add_channel(&chans[i], buf, &op->list);
	free(buf);
	qsort(op->list.items, op->list.count, sizeof(YtVideo), by_published_desc);
	if (op->cancel)
		op->state = YT_IDLE;
	else if (op->list.count > 0 || nc == 0)
		op->state = YT_DONE;
	else {
		snprintf(op->error, sizeof(op->error), "Couldn't load - check Wi-Fi");
		op->state = YT_ERROR;
	}
	return NULL;
}

// ---- yt-dlp install / update ----------------------------------------------

static void* update_thread(void* arg) {
	YtOp* op = arg;
	char cmd[700];
	snprintf(cmd, sizeof(cmd), "sh '%s' > " TMP_OUT " 2> " TMP_ERR, updater);
	int rc = system(cmd);
	if (rc == 0 && Yt_engineInstalled())
		op->state = YT_DONE;
	else {
		snprintf(op->error, sizeof(op->error), "Download failed (check WiFi)");
		op->state = YT_ERROR;
	}
	return NULL;
}

// ---- async plumbing -------------------------------------------------------

static void start(YtOp* op, const char* arg, void* (*fn)(void*)) {
	Yt_cancel(op);
	memset(op, 0, sizeof(*op));
	if (arg)
		snprintf(op->arg, sizeof(op->arg), "%s", arg);
	op->state = YT_RUNNING;
	pthread_create(&op->thread, NULL, fn, op);
}

void Yt_searchAsync(YtOp* op, const char* query) { start(op, query, search_thread); }
void Yt_resolveAsync(YtOp* op, const char* video_id) { start(op, video_id, resolve_thread); }
void Yt_feedAsync(YtOp* op) { start(op, NULL, feed_thread); }
void Yt_updateAsync(YtOp* op) { start(op, NULL, update_thread); }

void Yt_cancel(YtOp* op) {
	if (op->state != YT_RUNNING)
		return;
	op->cancel = true;
	system("killall yt-dlp_linux_aarch64 qjs 2>/dev/null");
	pthread_join(op->thread, NULL);
	op->state = YT_IDLE;
	op->cancel = false;
}

// ---- followed channels ----------------------------------------------------

int Yt_channels(YtChannel* out, int max) {
	int n = 0;
	FILE* f = fopen(CHANNELS_FILE, "r");
	if (!f)
		return 0;
	char line[256];
	while (n < max && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		char* tab = strchr(line, '\t');
		if (!tab || tab == line)
			continue;
		*tab = '\0';
		snprintf(out[n].id, sizeof(out[n].id), "%s", line);
		snprintf(out[n].name, sizeof(out[n].name), "%s", tab + 1);
		n++;
	}
	fclose(f);
	return n;
}

bool Yt_isFollowing(const char* channel_id) {
	YtChannel c[64];
	int n = Yt_channels(c, 64);
	for (int i = 0; i < n; i++)
		if (!strcmp(c[i].id, channel_id))
			return true;
	return false;
}

void Yt_follow(const char* channel_id, const char* name) {
	if (!channel_id[0] || Yt_isFollowing(channel_id))
		return;
	FILE* f = fopen(CHANNELS_FILE, "a");
	if (!f)
		return;
	fprintf(f, "%s\t%s\n", channel_id, name[0] ? name : channel_id);
	fclose(f);
}

void Yt_unfollow(const char* channel_id) {
	YtChannel c[64];
	int n = Yt_channels(c, 64);
	FILE* f = fopen(CHANNELS_FILE, "w");
	if (!f)
		return;
	for (int i = 0; i < n; i++)
		if (strcmp(c[i].id, channel_id))
			fprintf(f, "%s\t%s\n", c[i].id, c[i].name);
	fclose(f);
}

// ---- history and positions ------------------------------------------------

// history.txt: one line per play, oldest first: id, title, channel, channel_id, duration, time.
int Yt_history(YtList* out) {
	out->count = 0;
	FILE* f = fopen(HISTORY_FILE, "r");
	if (!f)
		return 0;
	static YtVideo all[HISTORY_MAX * 4];
	int n = 0;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		char* fld[6] = {0};
		int nf = 0;
		char* p = line;
		fld[nf++] = p;
		while (nf < 6 && (p = strchr(p, '\t'))) {
			*p++ = '\0';
			fld[nf++] = p;
		}
		if (nf < 6)
			continue;
		YtVideo* v = &all[n % (HISTORY_MAX * 4)];
		memset(v, 0, sizeof(*v));
		snprintf(v->id, sizeof(v->id), "%s", fld[0]);
		snprintf(v->title, sizeof(v->title), "%s", fld[1]);
		snprintf(v->channel, sizeof(v->channel), "%s", fld[2]);
		snprintf(v->channel_id, sizeof(v->channel_id), "%s", fld[3]);
		v->duration = atoi(fld[4]);
		v->published = atol(fld[5]);
		n++;
	}
	fclose(f);
	int have = n < HISTORY_MAX * 4 ? n : HISTORY_MAX * 4;
	// newest first, one row per video
	for (int k = 0; k < have && out->count < YT_MAX_ITEMS; k++) {
		YtVideo* v = &all[(n - 1 - k) % (HISTORY_MAX * 4)];
		bool dup = false;
		for (int i = 0; i < out->count; i++)
			if (!strcmp(out->items[i].id, v->id))
				dup = true;
		if (!dup)
			out->items[out->count++] = *v;
	}
	return out->count;
}

static void strip_tabs(const char* in, char* out, int n) {
	snprintf(out, n, "%s", in);
	for (char* p = out; *p; p++)
		if (*p == '\t' || *p == '\n')
			*p = ' ';
}

void Yt_addHistory(const YtVideo* v) {
	FILE* f = fopen(HISTORY_FILE, "a");
	if (!f)
		return;
	char t[YT_TITLE], c[YT_CHANNEL];
	strip_tabs(v->title, t, sizeof(t));
	strip_tabs(v->channel, c, sizeof(c));
	fprintf(f, "%s\t%s\t%s\t%s\t%d\t%ld\n", v->id, t, c, v->channel_id, v->duration, (long)time(NULL));
	fclose(f);
}

int Yt_position(const char* video_id) {
	FILE* f = fopen(POSITIONS_FILE, "r");
	if (!f)
		return 0;
	char line[128];
	int pos = 0;
	while (fgets(line, sizeof(line), f)) {
		char id[YT_ID];
		int sec;
		if (sscanf(line, "%15s %d", id, &sec) == 2 && !strcmp(id, video_id))
			pos = sec; // the last line for this id wins
	}
	fclose(f);
	return pos;
}

void Yt_setPosition(const char* video_id, int sec) {
	FILE* f = fopen(POSITIONS_FILE, "a");
	if (!f)
		return;
	fprintf(f, "%s %d\n", video_id, sec);
	fclose(f);
}
