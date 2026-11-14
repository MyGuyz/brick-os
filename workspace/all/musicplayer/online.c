// Music from the network (DESIGN.md 7.2): own server (Subsonic API) and Internet Archive.
#include "online.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "defines.h"
#include "mbedtls/md5.h"
#include "parson/parson.h"
#include "wget_fetch.h"

#define CFG_PATH SHARED_USERDATA_PATH "/music-player/server.json"
static ServerCfg cfg;
static bool cfg_loaded;
static char last_err[256];

const char* Server_lastError(void) {
	return last_err;
}

ServerCfg* Server_cfg(void) {
	if (!cfg_loaded) {
		cfg_loaded = true;
		JSON_Value* v = json_parse_file(CFG_PATH);
		JSON_Object* o = v ? json_value_get_object(v) : NULL;
		if (o) {
			const char* s;
			if ((s = json_object_get_string(o, "url")))
				snprintf(cfg.url, sizeof(cfg.url), "%s", s);
			if ((s = json_object_get_string(o, "user")))
				snprintf(cfg.user, sizeof(cfg.user), "%s", s);
			if ((s = json_object_get_string(o, "pass")))
				snprintf(cfg.pass, sizeof(cfg.pass), "%s", s);
		}
		if (v)
			json_value_free(v);
	}
	return &cfg;
}

void Server_save(void) {
	Online_mkdirs(SHARED_USERDATA_PATH "/music-player");
	JSON_Value* v = json_value_init_object();
	JSON_Object* o = json_value_get_object(v);
	json_object_set_string(o, "url", cfg.url);
	json_object_set_string(o, "user", cfg.user);
	json_object_set_string(o, "pass", cfg.pass); // stays on this device
	json_serialize_to_file_pretty(v, CFG_PATH);
	json_value_free(v);
}

bool Server_configured(void) {
	ServerCfg* c = Server_cfg();
	return c->url[0] && c->user[0];
}

static void urlenc(const char* in, char* out, int sz) {
	static const char hex[] = "0123456789ABCDEF";
	int j = 0;
	for (const unsigned char* p = (const unsigned char*)in; *p && j < sz - 4; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p))
			out[j++] = *p;
		else
			out[j++] = '%', out[j++] = hex[*p >> 4], out[j++] = hex[*p & 15];
	}
	out[j] = 0;
}

// base + /rest/<method>.view?u=&t=&s=&v=&c=&f=json + extra
static void restUrl(const char* method, const char* extra, char* out, int sz) {
	ServerCfg* c = Server_cfg();
	char salt[17], tok[33], user[200];
	unsigned char md[16];
	snprintf(salt, sizeof(salt), "%08lx%08x", (unsigned long)time(NULL), (unsigned)rand());
	char buf[256];
	snprintf(buf, sizeof(buf), "%s%s", c->pass, salt);
	mbedtls_md5((const unsigned char*)buf, strlen(buf), md);
	for (int i = 0; i < 16; i++)
		sprintf(tok + i * 2, "%02x", md[i]);
	urlenc(c->user, user, sizeof(user));
	char base[256];
	snprintf(base, sizeof(base), "%s", c->url);
	size_t n = strlen(base);
	while (n > 0 && base[n - 1] == '/')
		base[--n] = 0;
	const char* scheme = strstr(base, "://") ? "" : "http://";
	snprintf(out, sz, "%s%s/rest/%s.view?u=%s&t=%s&s=%s&v=1.16.1&c=brickos&f=json%s%s", scheme, base, method, user, tok,
			 salt, extra && extra[0] ? "&" : "", extra ? extra : "");
}

// the "subsonic-response" object of a call, or NULL (last_err says why); free the root
static JSON_Object* call(const char* method, const char* extra, JSON_Value** root) {
	char url[1024];
	restUrl(method, extra, url, sizeof(url));
	uint8_t* body = NULL;
	int n = wget_fetch_alloc(url, 4 * 1024 * 1024, &body);
	*root = NULL;
	if (n <= 0 || !body) {
		snprintf(last_err, sizeof(last_err), "Can't reach the server");
		free(body);
		return NULL;
	}
	*root = json_parse_string((const char*)body);
	free(body);
	JSON_Object* r = *root ? json_object_get_object(json_value_get_object(*root), "subsonic-response") : NULL;
	if (!r) {
		snprintf(last_err, sizeof(last_err), "Not a music server");
		return NULL;
	}
	const char* st = json_object_get_string(r, "status");
	if (!st || strcmp(st, "ok") != 0) {
		const char* m = json_object_dotget_string(r, "error.message");
		snprintf(last_err, sizeof(last_err), "%s", m ? m : "Wrong user name or password");
		return NULL;
	}
	return r;
}

int Server_ping(char* err, int errsz, int* songs) {
	JSON_Value* root;
	JSON_Object* r = call("ping", NULL, &root);
	if (!r) {
		snprintf(err, errsz, "%s", last_err);
		if (root)
			json_value_free(root);
		return -1;
	}
	json_value_free(root);
	if (songs) {
		*songs = -1;
		r = call("getScanStatus", NULL, &root);
		if (r)
			*songs = (int)json_object_dotget_number(r, "scanStatus.count");
		if (root)
			json_value_free(root);
	}
	return 0;
}

static void str(JSON_Object* o, const char* k, char* out, int sz) {
	const char* s = json_object_get_string(o, k);
	if (s)
		snprintf(out, sz, "%s", s);
	else if (json_object_has_value_of_type(o, k, JSONNumber))
		snprintf(out, sz, "%.0f", json_object_get_number(o, k));
	else
		out[0] = 0;
}

static void albumFrom(JSON_Object* a, SrvAlbum* al) {
	memset(al, 0, sizeof(*al));
	str(a, "id", al->id, sizeof(al->id));
	str(a, "name", al->name, sizeof(al->name));
	if (!al->name[0])
		str(a, "title", al->name, sizeof(al->name));
	str(a, "artist", al->artist, sizeof(al->artist));
	str(a, "coverArt", al->cover, sizeof(al->cover));
	al->songs = (int)json_object_get_number(a, "songCount");
	al->year = (int)json_object_get_number(a, "year");
	al->duration = (int)json_object_get_number(a, "duration");
}

static int albumList(JSON_Object* r, const char* path, SrvAlbum* out, int max) {
	JSON_Array* arr = json_object_dotget_array(r, path);
	int n = 0;
	for (size_t i = 0; arr && i < json_array_get_count(arr) && n < max; i++)
		albumFrom(json_array_get_object(arr, i), &out[n++]);
	return n;
}

int Server_albums(SrvAlbum* out, int max) {
	JSON_Value* root;
	char extra[64];
	snprintf(extra, sizeof(extra), "type=newest&size=%d", max > 500 ? 500 : max);
	JSON_Object* r = call("getAlbumList2", extra, &root);
	int n = r ? albumList(r, "albumList2.album", out, max) : -1;
	if (root)
		json_value_free(root);
	return n;
}

int Server_artists(SrvArtist* out, int max) {
	JSON_Value* root;
	JSON_Object* r = call("getArtists", NULL, &root);
	int n = r ? 0 : -1;
	JSON_Array* idx = r ? json_object_dotget_array(r, "artists.index") : NULL;
	for (size_t i = 0; idx && i < json_array_get_count(idx); i++) {
		JSON_Array* as = json_object_get_array(json_array_get_object(idx, i), "artist");
		for (size_t k = 0; as && k < json_array_get_count(as) && n < max; k++) {
			JSON_Object* a = json_array_get_object(as, k);
			SrvArtist* ar = &out[n++];
			memset(ar, 0, sizeof(*ar));
			str(a, "id", ar->id, sizeof(ar->id));
			str(a, "name", ar->name, sizeof(ar->name));
			ar->albums = (int)json_object_get_number(a, "albumCount");
		}
	}
	if (root)
		json_value_free(root);
	return n;
}

int Server_artistAlbums(const char* id, SrvAlbum* out, int max) {
	JSON_Value* root;
	char extra[160], e[128];
	urlenc(id, e, sizeof(e));
	snprintf(extra, sizeof(extra), "id=%s", e);
	JSON_Object* r = call("getArtist", extra, &root);
	int n = r ? albumList(r, "artist.album", out, max) : -1;
	if (root)
		json_value_free(root);
	return n;
}

static int songList(JSON_Object* holder, SrvSong* out, int max, const char* key) {
	JSON_Array* arr = json_object_get_array(holder, key);
	int n = 0;
	for (size_t i = 0; arr && i < json_array_get_count(arr) && n < max; i++) {
		JSON_Object* s = json_array_get_object(arr, i);
		SrvSong* so = &out[n++];
		memset(so, 0, sizeof(*so));
		str(s, "id", so->id, sizeof(so->id));
		str(s, "title", so->title, sizeof(so->title));
		str(s, "artist", so->artist, sizeof(so->artist));
		str(s, "album", so->album, sizeof(so->album));
		str(s, "suffix", so->suffix, sizeof(so->suffix));
		so->track = (int)json_object_get_number(s, "track");
		so->duration = (int)json_object_get_number(s, "duration");
		if (!so->suffix[0])
			snprintf(so->suffix, sizeof(so->suffix), "mp3");
	}
	return n;
}

int Server_album(const char* id, SrvSong* out, int max, SrvAlbum* info) {
	JSON_Value* root;
	char extra[160], e[128];
	urlenc(id, e, sizeof(e));
	snprintf(extra, sizeof(extra), "id=%s", e);
	JSON_Object* r = call("getAlbum", extra, &root);
	int n = -1;
	if (r) {
		JSON_Object* a = json_object_get_object(r, "album");
		if (info && a)
			albumFrom(a, info);
		n = a ? songList(a, out, max, "song") : 0;
	}
	if (root)
		json_value_free(root);
	return n;
}

int Server_playlists(SrvAlbum* out, int max) {
	JSON_Value* root;
	JSON_Object* r = call("getPlaylists", NULL, &root);
	int n = r ? albumList(r, "playlists.playlist", out, max) : -1;
	if (root)
		json_value_free(root);
	return n;
}

int Server_playlist(const char* id, SrvSong* out, int max, SrvAlbum* info) {
	JSON_Value* root;
	char extra[160], e[128];
	urlenc(id, e, sizeof(e));
	snprintf(extra, sizeof(extra), "id=%s", e);
	JSON_Object* r = call("getPlaylist", extra, &root);
	int n = -1;
	if (r) {
		JSON_Object* p = json_object_get_object(r, "playlist");
		if (info && p)
			albumFrom(p, info);
		n = p ? songList(p, out, max, "entry") : 0;
	}
	if (root)
		json_value_free(root);
	return n;
}

void Server_coverUrl(const char* cover, int size, char* out, int sz) {
	char extra[200], e[128];
	urlenc(cover, e, sizeof(e));
	snprintf(extra, sizeof(extra), "id=%s&size=%d", e, size);
	restUrl("getCoverArt", extra, out, sz);
}

void Server_downloadUrl(const char* song_id, char* out, int sz) {
	char extra[160], e[128];
	urlenc(song_id, e, sizeof(e));
	snprintf(extra, sizeof(extra), "id=%s", e);
	restUrl("download", extra, out, sz);
}

void Online_safeName(const char* in, char* out, int sz) {
	int j = 0;
	for (const char* p = in ? in : ""; *p && j < sz - 1; p++)
		out[j++] = strchr("/\\:*?\"<>|", *p) ? '_' : *p;
	while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '.'))
		j--;
	out[j] = 0;
	if (!out[0])
		snprintf(out, sz, "_");
}

void Server_albumDir(const SrvAlbum* al, char* out, int sz) {
	char a[160], b[160];
	Online_safeName(al->artist[0] ? al->artist : "Unknown", a, sizeof(a));
	Online_safeName(al->name, b, sizeof(b));
	snprintf(out, sz, ONLINE_DIR "/%s/%s", a, b);
}

void Server_songPath(const SrvAlbum* al, const SrvSong* s, char* out, int sz) {
	char dir[400], t[200];
	Server_albumDir(al, dir, sizeof(dir));
	Online_safeName(s->title, t, sizeof(t));
	if (s->track > 0)
		snprintf(out, sz, "%s/%02d %s.%s", dir, s->track, t, s->suffix);
	else
		snprintf(out, sz, "%s/%s.%s", dir, t, s->suffix);
}

int Online_mkdirs(const char* path) {
	char p[640];
	snprintf(p, sizeof(p), "%s", path);
	for (char* q = p + 1; *q; q++)
		if (*q == '/') {
			*q = 0;
			mkdir(p, 0755);
			*q = '/';
		}
	return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

// ---- covers, one at a time on a thread ----
static pthread_mutex_t cv_mu = PTHREAD_MUTEX_INITIALIZER;
static bool cv_busy;
static char cv_url[1024], cv_path[640];
static char cv_failed[16][640];
static int cv_nfailed;

static void* cvThread(void* arg) {
	(void)arg;
	char url[1024], path[640], tmp[660];
	pthread_mutex_lock(&cv_mu);
	snprintf(url, sizeof(url), "%s", cv_url);
	snprintf(path, sizeof(path), "%s", cv_path);
	pthread_mutex_unlock(&cv_mu);
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	char dir[640];
	snprintf(dir, sizeof(dir), "%s", path);
	char* sl = strrchr(dir, '/');
	if (sl) {
		*sl = 0;
		Online_mkdirs(dir);
	}
	int n = wget_download_file(url, tmp, NULL, NULL, NULL, NULL);
	pthread_mutex_lock(&cv_mu);
	if (n > 0)
		rename(tmp, path);
	else {
		unlink(tmp);
		snprintf(cv_failed[cv_nfailed++ % 16], sizeof(cv_failed[0]), "%s", path);
	}
	cv_busy = false;
	pthread_mutex_unlock(&cv_mu);
	return NULL;
}

bool Online_cover(const char* url, const char* path) {
	if (access(path, R_OK) == 0)
		return true;
	pthread_mutex_lock(&cv_mu);
	bool failed = false;
	for (int i = 0; i < 16 && i < cv_nfailed; i++)
		failed |= strcmp(cv_failed[i], path) == 0;
	if (!cv_busy && !failed) {
		cv_busy = true;
		snprintf(cv_url, sizeof(cv_url), "%s", url);
		snprintf(cv_path, sizeof(cv_path), "%s", path);
		pthread_t t;
		if (pthread_create(&t, NULL, cvThread, NULL) == 0)
			pthread_detach(t);
		else
			cv_busy = false;
	}
	pthread_mutex_unlock(&cv_mu);
	return false;
}

// ---- Internet Archive ----
static const char* licenceOf(const char* url) {
	if (!url || !url[0])
		return "";
	if (strstr(url, "publicdomain"))
		return "Public domain";
	if (strstr(url, "/by-nc-sa"))
		return "CC BY-NC-SA";
	if (strstr(url, "/by-nc-nd"))
		return "CC BY-NC-ND";
	if (strstr(url, "/by-nc"))
		return "CC BY-NC";
	if (strstr(url, "/by-sa"))
		return "CC BY-SA";
	if (strstr(url, "/by-nd"))
		return "CC BY-ND";
	if (strstr(url, "/by"))
		return "CC BY";
	if (strstr(url, "zero"))
		return "CC0";
	return "";
}

int Archive_search(const char* genre, const char* query, ArcItem* out, int max) {
	char q[600], qe[1200], words[200] = "";
	if (query && query[0])
		snprintf(words, sizeof(words), " AND (%s)", query);
	snprintf(q, sizeof(q),
			 "mediatype:(audio) AND collection:(netlabels) AND "
			 "licenseurl:(*creativecommons* OR *publicdomain*)%s%s%s%s",
			 genre && genre[0] ? " AND subject:(" : "", genre && genre[0] ? genre : "", genre && genre[0] ? ")" : "",
			 words);
	urlenc(q, qe, sizeof(qe));
	char url[1600];
	snprintf(url, sizeof(url),
			 "https://archive.org/advancedsearch.php?q=%s&fl%%5B%%5D=identifier&fl%%5B%%5D=title&fl%%5B%%5D=creator"
			 "&fl%%5B%%5D=licenseurl&sort%%5B%%5D=downloads+desc&rows=%d&page=1&output=json",
			 qe, max > 100 ? 100 : max);
	uint8_t* body = NULL;
	int n = wget_fetch_alloc(url, 2 * 1024 * 1024, &body);
	if (n <= 0 || !body) {
		snprintf(last_err, sizeof(last_err), "Can't reach archive.org");
		free(body);
		return -1;
	}
	JSON_Value* root = json_parse_string((const char*)body);
	free(body);
	JSON_Array* docs = root ? json_object_dotget_array(json_value_get_object(root), "response.docs") : NULL;
	int k = 0;
	for (size_t i = 0; docs && i < json_array_get_count(docs) && k < max; i++) {
		JSON_Object* d = json_array_get_object(docs, i);
		ArcItem* it = &out[k];
		memset(it, 0, sizeof(*it));
		str(d, "identifier", it->id, sizeof(it->id));
		str(d, "title", it->title, sizeof(it->title));
		const char* cr = json_object_get_string(d, "creator");
		if (!cr) { // sometimes a list
			JSON_Array* ca = json_object_get_array(d, "creator");
			cr = ca && json_array_get_count(ca) ? json_array_get_string(ca, 0) : "";
		}
		snprintf(it->creator, sizeof(it->creator), "%s", cr ? cr : "");
		snprintf(it->license, sizeof(it->license), "%s", licenceOf(json_object_get_string(d, "licenseurl")));
		if (it->id[0] && it->title[0])
			k++;
	}
	if (root)
		json_value_free(root);
	return k;
}

int Archive_mp3s(const char* id, char names[][200], int max) {
	char url[400];
	snprintf(url, sizeof(url), "https://archive.org/metadata/%s/files", id);
	uint8_t* body = NULL;
	int n = wget_fetch_alloc(url, 2 * 1024 * 1024, &body);
	if (n <= 0 || !body) {
		snprintf(last_err, sizeof(last_err), "Can't reach archive.org");
		free(body);
		return -1;
	}
	JSON_Value* root = json_parse_string((const char*)body);
	free(body);
	JSON_Array* files = root ? json_object_get_array(json_value_get_object(root), "result") : NULL;
	int k = 0;
	for (size_t i = 0; files && i < json_array_get_count(files) && k < max; i++) {
		JSON_Object* f = json_array_get_object(files, i);
		const char* fmt = json_object_get_string(f, "format");
		const char* name = json_object_get_string(f, "name");
		if (fmt && name && strstr(fmt, "MP3") && !strstr(name, "/"))
			snprintf(names[k++], 200, "%s", name);
	}
	if (root)
		json_value_free(root);
	return k;
}

void Archive_fileUrl(const char* id, const char* name, char* out, int sz) {
	char e[400];
	urlenc(name, e, sizeof(e));
	snprintf(out, sz, "https://archive.org/download/%s/%s", id, e);
}

void Archive_itemDir(const ArcItem* it, char* out, int sz) {
	char t[200];
	Online_safeName(it->title, t, sizeof(t));
	snprintf(out, sz, ONLINE_DIR "/Internet Archive/%s", t);
}
