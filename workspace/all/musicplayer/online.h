#ifndef ONLINE_H
#define ONLINE_H
// Music from the network (DESIGN.md 7.2): the owner's own server (the Subsonic API: Navidrome, Subsonic, Airsonic,
// Gonic) and free music (Internet Archive). Songs are downloaded to the card, then played as files (the music
// service plays files and live radio only); they stay for offline play under Music/ดาวน์โหลด.
#include <stdbool.h>

#define ONLINE_DIR "/mnt/SDCARD/Music/ดาวน์โหลด"

// ---- own server ----
typedef struct {
	char url[256], user[96], pass[96];
} ServerCfg;
ServerCfg* Server_cfg(void); // loaded on first use
void Server_save(void);
bool Server_configured(void);
int Server_ping(char* err, int errsz, int* songs); // 0 ok

typedef struct {
	char id[64], name[160], artist[160], cover[96];
	int songs, year, duration;
} SrvAlbum;
typedef struct {
	char id[64], name[160];
	int albums;
} SrvArtist;
typedef struct {
	char id[64], title[200], artist[160], album[160], suffix[16];
	int track, duration;
} SrvSong;
int Server_albums(SrvAlbum* out, int max);
int Server_artists(SrvArtist* out, int max);
int Server_artistAlbums(const char* id, SrvAlbum* out, int max);
int Server_album(const char* id, SrvSong* out, int max, SrvAlbum* info);
int Server_playlists(SrvAlbum* out, int max);
int Server_playlist(const char* id, SrvSong* out, int max, SrvAlbum* info);
void Server_coverUrl(const char* cover, int size, char* out, int sz);
void Server_downloadUrl(const char* song_id, char* out, int sz);
// Music/ดาวน์โหลด/<artist>/<album>/<NN title>.<ext>
void Server_songPath(const SrvAlbum* al, const SrvSong* s, char* out, int sz);
void Server_albumDir(const SrvAlbum* al, char* out, int sz);
const char* Server_lastError(void);

// ---- free music (Internet Archive) ----
typedef struct {
	char id[128], title[200], creator[160], license[32];
} ArcItem;
// genre: "" all, else an archive subject; query: words to search, or ""
int Archive_search(const char* genre, const char* query, ArcItem* out, int max);
int Archive_mp3s(const char* id, char names[][200], int max); // the item's MP3 files
void Archive_fileUrl(const char* id, const char* name, char* out, int sz);
void Archive_itemDir(const ArcItem* it, char* out, int sz); // Music/ดาวน์โหลด/Internet Archive/<title>

// ---- files ----
void Online_safeName(const char* in, char* out, int sz); // no / and the like
int Online_mkdirs(const char* path);
// a cover picture fetched in the background (one at a time); true once the file is there
bool Online_cover(const char* url, const char* path);
#endif
