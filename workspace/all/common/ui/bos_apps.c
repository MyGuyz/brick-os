#include "bos_apps.h"

#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "bos_tokens.h"
#include "utils.h"

#define PINS_PATH USERDATA_PATH "/bos_home_pins.txt"
#define SEEN_PATH USERDATA_PATH "/bos_apps_seen.txt"

BosApp bos_apps[BOS_APPS_MAX];
int bos_napps;
int bos_pins[BOS_APPS_MAX];
int bos_npins;

static const char* DEFAULT_PINS[] = {"games", "YouTube", "Music Player", "Media Player", "ports", "Settings"};

// Paks that are no longer apps of their own: their work moved into Settings (DESIGN §6.1b, §6.7c)
static const char* MERGED[] = {"Emulator Settings", "Artwork Manager"};

static const struct {
	const char* pak;
	const char* name;
	const char* icon;
	uint32_t color;
} KNOWN[] = {
	{"YouTube", "YouTube", "youtube", BOS_APP_YOUTUBE},
	{"Music Player", "Music", "music", BOS_APP_MUSIC},
	{"Media Player", "Video", "video", BOS_APP_VIDEO},
	{"Settings", "Settings", "settings", BOS_APP_SETTINGS},
	{"Files", "Files", "folder", BOS_APP_FILES},
	{"Image Viewer", "Images", "image", BOS_APP_IMAGES},
	{"RetroAchievements", "Trophies", "trophy", BOS_APP_TROPHIES},
	{"Game Tracker", "Stats", "chart", BOS_APP_STATS},
	{"Device Sync", "Sync", "sync", BOS_APP_SYNC},
	{"Xtras", "Xtras", "spark", BOS_APP_XTRAS},
	{"Artwork Manager", "Artwork", "palette", BOS_APP_ARTWORK},
	{"PortMaster", "PortMaster", "box", BOS_APP_PORTMASTER},
};

int BosApps_find(const char* id) {
	for (int i = 0; i < bos_napps; i++)
		if (!strcmp(bos_apps[i].id, id))
			return i;
	return -1;
}

static bool lineInFile(const char* path, const char* s) {
	FILE* f = fopen(path, "r");
	if (!f)
		return false;
	char line[128];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		found = !strcmp(line, s);
	}
	fclose(f);
	return found;
}

static void addPak(const char* dir, const char* file, bool seen_known) {
	if (bos_napps >= BOS_APPS_MAX)
		return;
	size_t n = strlen(file);
	if (n < 5 || strcasecmp(file + n - 4, ".pak"))
		return;
	char id[64];
	snprintf(id, sizeof(id), "%.*s", (int)(n - 4), file);
	if (BosApps_find(id) >= 0)
		return;
	for (size_t m = 0; m < sizeof(MERGED) / sizeof(MERGED[0]); m++)
		if (!strcmp(MERGED[m], id))
			return;
	BosApp* a = &bos_apps[bos_napps];
	memset(a, 0, sizeof(*a));
	snprintf(a->id, sizeof(a->id), "%s", id);
	snprintf(a->path, sizeof(a->path), "%s/%s", dir, file);
	a->kind = BOS_APP_KIND_PAK;
	a->icon = "box";
	a->color = BOS_C_SECONDARY;
	snprintf(a->name, sizeof(a->name), "%s", id);
	for (size_t k = 0; k < sizeof(KNOWN) / sizeof(KNOWN[0]); k++)
		if (!strcmp(KNOWN[k].pak, id)) {
			snprintf(a->name, sizeof(a->name), "%s", KNOWN[k].name);
			a->icon = KNOWN[k].icon;
			a->color = KNOWN[k].color;
		}
	a->fresh = seen_known && !lineInFile(SEEN_PATH, id);
	bos_napps++;
}

static void scanPaks(const char* dir, bool seen_known) {
	DIR* d = opendir(dir);
	if (!d)
		return;
	struct dirent* e;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.')
			addPak(dir, e->d_name, seen_known);
	closedir(d);
}

void BosApps_build(void) {
	bos_napps = 0;
	BosApp* g = &bos_apps[bos_napps++];
	memset(g, 0, sizeof(*g));
	snprintf(g->id, sizeof(g->id), "games");
	snprintf(g->name, sizeof(g->name), "Games");
	g->icon = "game";
	g->color = BOS_APP_GAMES;
	g->kind = BOS_APP_KIND_GAMES;

	BosApp* p = &bos_apps[bos_napps++];
	memset(p, 0, sizeof(*p));
	snprintf(p->id, sizeof(p->id), "ports");
	snprintf(p->name, sizeof(p->name), "Ports");
	p->icon = "ports";
	p->color = BOS_APP_PORTS;
	p->kind = BOS_APP_KIND_PORTS;
	DIR* d = opendir(ROMS_PATH);
	if (d) {
		struct dirent* e;
		while ((e = readdir(d)))
			if (strstr(e->d_name, "(PORTS)")) {
				snprintf(p->path, sizeof(p->path), "%s/%s", ROMS_PATH, e->d_name);
				break;
			}
		closedir(d);
	}

	bool seen_known = exists((char*)SEEN_PATH); // the very first boot marks nothing as new
	scanPaks(SDCARD_PATH "/.system/paks/Tools", seen_known);
	scanPaks(TOOLS_PATH, seen_known); // user-installed (PortMaster …), as the Tools tab reads them

	bos_npins = 0;
	FILE* f = fopen(PINS_PATH, "r");
	if (f) {
		char line[128];
		while (bos_npins < BOS_APPS_MAX && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			int i = BosApps_find(line);
			if (i >= 0 && BosApps_pinIndex(i) < 0)
				bos_pins[bos_npins++] = i;
		}
		fclose(f);
	} else
		for (size_t i = 0; i < sizeof(DEFAULT_PINS) / sizeof(DEFAULT_PINS[0]); i++) {
			int a = BosApps_find(DEFAULT_PINS[i]);
			if (a >= 0)
				bos_pins[bos_npins++] = a;
		}
}

int BosApps_pinIndex(int app) {
	for (int i = 0; i < bos_npins; i++)
		if (bos_pins[i] == app)
			return i;
	return -1;
}

void BosApps_savePins(void) {
	FILE* f = fopen(PINS_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < bos_npins; i++)
		fprintf(f, "%s\n", bos_apps[bos_pins[i]].id);
	fclose(f);
	sync();
}

void BosApps_markSeen(void) {
	FILE* f = fopen(SEEN_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < bos_napps; i++) {
		fprintf(f, "%s\n", bos_apps[i].id);
		bos_apps[i].fresh = false;
	}
	fclose(f);
}
