#ifndef BOS_APPS_H
#define BOS_APPS_H

// Brick OS apps (DESIGN §6.1, §6.1b, §6.7b): every Tools pak (shipped + installed) plus เกม and Ports, and the pins
// Home shows (bos_home_pins.txt). One copy for Home, แอปทั้งหมด and Settings › แอปที่ปักหมุด.

#include <stdbool.h>
#include <stdint.h>
#include "defines.h"

#define BOS_APPS_MAX 40

typedef enum { BOS_APP_KIND_GAMES, BOS_APP_KIND_PORTS, BOS_APP_KIND_PAK } BosAppKind;
typedef struct {
	char id[64];   // "games", "ports", or the pak's folder name ("Music Player")
	char name[64]; // English key for I18N_text, or the pak name as is
	const char* icon;
	uint32_t color;
	BosAppKind kind;
	char path[MAX_PATH]; // the pak or the Ports folder
	bool fresh;			 // installed since the user last opened แอปทั้งหมด (the ใหม่ pill)
} BosApp;

extern BosApp bos_apps[BOS_APPS_MAX];
extern int bos_napps;
extern int bos_pins[BOS_APPS_MAX]; // indexes into bos_apps, in the user's order
extern int bos_npins;

void BosApps_build(void); // scan the apps, then read the pins (the defaults when there is no file)
int BosApps_find(const char* id);
int BosApps_pinIndex(int app); // its place in the pins, -1 when not pinned
void BosApps_savePins(void);
void BosApps_markSeen(void); // every app is no longer "ใหม่"

#endif
