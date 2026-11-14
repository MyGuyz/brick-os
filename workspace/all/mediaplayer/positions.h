#ifndef __POSITIONS_H__
#define __POSITIONS_H__

// Per-video playback position store, persisted to positions.cfg.
// Most-recently-updated entries first; capped, oldest dropped.

// (Re)load store from disk. Missing/corrupt file -> empty store.
void Positions_init(void);

// Saved position in seconds for a video path, 0 if none.
int Positions_get(const char* path);

// Insert or update an entry (moves it to the front) and save.
void Positions_set(const char* path, int sec);

// Remove an entry (no-op if absent) and save.
void Positions_remove(const char* path);

// With the video's length (dur 0: keep what is known). Newest first: count and the i-th (its path; NULL past the end).
void Positions_setFull(const char* path, int sec, int dur);
int Positions_count(void);
const char* Positions_at(int i, int* sec, int* dur);

#endif
