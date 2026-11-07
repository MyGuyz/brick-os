#ifndef __YT_ENGINE_H__
#define __YT_ENGINE_H__

// Brick OS YouTube: yt-dlp calls (search, stream URL), channel RSS feeds, and
// the app's small text stores (followed channels, history, resume positions).
// Every network call runs on a worker thread; the UI polls YtOp.state.

#include <pthread.h>
#include <stdbool.h>

#define YT_MAX_ITEMS 50
#define YT_ID 16
#define YT_CH_ID 32
#define YT_TITLE 256
#define YT_CHANNEL 128
#define YT_URL 2048

typedef struct {
	char id[YT_ID];
	char title[YT_TITLE];
	char channel[YT_CHANNEL];
	char channel_id[YT_CH_ID];
	int duration;	 // seconds, -1 unknown
	long published;	 // unix time (feed/history), 0 unknown
} YtVideo;

typedef struct {
	YtVideo items[YT_MAX_ITEMS];
	int count;
} YtList;

typedef enum { YT_IDLE, YT_RUNNING, YT_DONE, YT_ERROR } YtState;

typedef struct {
	volatile YtState state;
	volatile bool cancel;
	char error[160];
	char arg[512];		// query, video id
	YtList list;		// search / feed results
	char url[YT_URL];	// resolved stream URL
	pthread_t thread;
} YtOp;

typedef struct {
	char id[YT_CH_ID];
	char name[YT_CHANNEL];
} YtChannel;

// Paths: the pak folder (for bin/) and the data folder are set once at start.
void Yt_init(const char* pak_dir);
bool Yt_engineInstalled(void);
// Install or update yt-dlp + Node.js on a worker thread.
void Yt_updateAsync(YtOp* op);

// Playback, in two steps: Yt_resolveAsync has yt-dlp solve and save the video's
// info (slow; behind a loading screen), then Yt_streamStart has yt-dlp download
// from that info into a FIFO that ffplay reads (YouTube refuses a stream URL
// fetched by another client). Yt_streamStart returns the FIFO path to hand to
// ffplay; Yt_streamStop ends the download.
const char* Yt_streamStart(void);
void Yt_streamStop(void);

void Yt_searchAsync(YtOp* op, const char* query);
void Yt_resolveAsync(YtOp* op, const char* video_id);
void Yt_feedAsync(YtOp* op);
void Yt_cancel(YtOp* op);

// Followed channels
int Yt_channels(YtChannel* out, int max);
bool Yt_isFollowing(const char* channel_id);
void Yt_follow(const char* channel_id, const char* name);
void Yt_unfollow(const char* channel_id);

// History (most recent first) and resume positions
int Yt_history(YtList* out);
void Yt_addHistory(const YtVideo* v);
int Yt_position(const char* video_id);
void Yt_setPosition(const char* video_id, int sec);

void Yt_formatDuration(int sec, char* out, int n);

#endif
