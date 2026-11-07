#ifndef __BOS_FEATURE_H__
#define __BOS_FEATURE_H__

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "defines.h"

// Brick OS Optional Paks: a feature that is not in Core shows in its app only
// while its Xtras catalog entry is installed. Xtras writes
// SHARED_USERDATA_PATH/xtras/<id>.version after an install and the entry's
// uninstall.sh removes it, so that file is the switch.
#define BOS_FEATURE_IPTV "iptv"
#define BOS_FEATURE_RADIO "radio-podcasts" // Online Radio and Podcasts in the music player

static inline bool BOS_hasFeature(const char* id) {
	char path[256];
	snprintf(path, sizeof(path), "%s/xtras/%s.version", SHARED_USERDATA_PATH, id);
	return access(path, F_OK) == 0;
}

#endif // __BOS_FEATURE_H__
