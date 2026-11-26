// A zip that is sent while it is made: files stored as they are (no compression), so the device only copies
// bytes and needs no temp file. Its whole length is known before the first byte (Content-Length).
#ifndef WT_ZIP_H
#define WT_ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WT_ZIP_MAX 4000

typedef struct {
	const char* path; // on the card
	const char* name; // inside the zip (UTF-8, may have folders "PS/Game.srm")
	uint64_t size;
	uint32_t mtime;
} WtZipItem;

// The zip's length, or 0 when it would pass 4 GB or 65535 files (no zip64 here).
uint64_t wt_zip_length(const WtZipItem* items, int n);

// Writes the zip through out(ctx, buf, len) (returns false to stop: the other side went away). A file that
// changed length while being sent ends the zip early (false).
typedef bool (*WtZipOut)(void* ctx, const void* buf, size_t len);
bool wt_zip_write(const WtZipItem* items, int n, WtZipOut out, void* ctx);

#endif
