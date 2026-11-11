#ifndef BOS_KEYS_H
#define BOS_KEYS_H

// Brick OS ปุ่มพิเศษ (DESIGN.md §6.9b): what HOME, F1 and F2 do on a tap and on a hold.
// Header-only and SDL-free: keymon, the launcher, minarch and Settings all read the same file.
//
// .userdata/shared/bos_keys.txt holds "key=value" lines: btnhome, btnhomehold, btnf1, btnf1hold, btnf2, btnf2hold,
// and "<key>.label" (the name Settings shows, taken when the value was picked). A missing key means its default.
// Values: sys:quick | sys:switcher | sys:home | sys:mute, app:<pak folder name> (or app:games / app:ports),
// console:<Roms dir path>, game:<rom path>, or empty (ว่าง).

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef BOS_KEYS_PATH
#define BOS_KEYS_PATH "/mnt/SDCARD/.userdata/shared/bos_keys.txt"
#endif
// keymon -> the app in front: the action to run (launcher), or to save/close and leave for the launcher
#define BOS_ACTION_PATH "/tmp/bos_action"
// Settings -> launcher: "<slot>\t<kind>\t<title>" (kind app | console | game), the launcher shows its screen in pick mode
#define BOS_PICK_PATH "/tmp/bos_pick"
// launcher -> Settings: "<slot>\tset\t<value>\t<label>" or "<slot>\tcancel"
#define BOS_PICK_RESULT_PATH "/tmp/bos_pick_result"

#define BOS_KEY_VAL 512

enum { BOSKEY_HOME, BOSKEY_F1, BOSKEY_F2, BOSKEY_COUNT };

static inline const char* BosKeys_slot(int key, int hold) {
	static const char* names[BOSKEY_COUNT][2] = {
		{"btnhome", "btnhomehold"}, {"btnf1", "btnf1hold"}, {"btnf2", "btnf2hold"}};
	return key >= 0 && key < BOSKEY_COUNT ? names[key][hold ? 1 : 0] : "";
}

// the value stored for name ("btnf1", "btnf1.label"); false (out = "") when the file has no such line
static inline bool BosKeys_read(const char* name, char* out, size_t size) {
	out[0] = '\0';
	FILE* f = fopen(BOS_KEYS_PATH, "r");
	if (!f)
		return false;
	char line[BOS_KEY_VAL + 64];
	size_t n = strlen(name);
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, name, n) && line[n] == '=') {
			snprintf(out, size, "%s", line + n + 1);
			found = true;
		}
	}
	fclose(f);
	return found;
}

static inline bool BosKeys_has(const char* name) {
	char v[BOS_KEY_VAL];
	return BosKeys_read(name, v, sizeof(v));
}

// sets name=value (value NULL removes the line); the file is rewritten through a temp file
static inline void BosKeys_write(const char* name, const char* value) {
	char* old = NULL;
	size_t len = 0;
	FILE* f = fopen(BOS_KEYS_PATH, "r");
	if (f) {
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz > 0 && (old = (char*)malloc((size_t)sz + 1))) {
			len = fread(old, 1, (size_t)sz, f);
			old[len] = '\0';
		}
		fclose(f);
	}
	char tmp[] = BOS_KEYS_PATH ".tmp";
	FILE* o = fopen(tmp, "w");
	if (!o) {
		free(old);
		return;
	}
	size_t n = strlen(name);
	if (old) {
		char* save = NULL;
		for (char* line = strtok_r(old, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			line[strcspn(line, "\r")] = '\0';
			if (!line[0] || (!strncmp(line, name, n) && line[n] == '='))
				continue;
			fprintf(o, "%s\n", line);
		}
	}
	if (value)
		fprintf(o, "%s=%s\n", name, value);
	fflush(o);
	fsync(fileno(o));
	fclose(o);
	free(old);
	rename(tmp, BOS_KEYS_PATH);
}

// what the key does now: the stored value, or the default (HOME: ตั้งค่าด่วน / สลับเกม; F1, F2: ว่าง)
static inline void BosKeys_value(int key, int hold, char* out, size_t size) {
	if (BosKeys_read(BosKeys_slot(key, hold), out, size))
		return;
	snprintf(out, size, "%s", key == BOSKEY_HOME ? (hold ? "sys:switcher" : "sys:quick") : "");
}

static inline void BosKeys_set(int key, int hold, const char* value, const char* label) {
	char lab[64];
	snprintf(lab, sizeof(lab), "%s.label", BosKeys_slot(key, hold));
	BosKeys_write(BosKeys_slot(key, hold), value ? value : "");
	BosKeys_write(lab, label && label[0] ? label : NULL);
}

// back to the default: the lines go away
static inline void BosKeys_reset(int key, int hold) {
	char lab[64];
	snprintf(lab, sizeof(lab), "%s.label", BosKeys_slot(key, hold));
	BosKeys_write(BosKeys_slot(key, hold), NULL);
	BosKeys_write(lab, NULL);
}

// The old F1/F2 tool setting (fn1tool / fn2tool, an SD-relative pak path) becomes btnf1 / btnf2 once
static inline void BosKeys_migrate(const char* fn1tool, const char* fn2tool) {
	const char* tools[2] = {fn1tool, fn2tool};
	for (int i = 0; i < 2; i++) {
		int key = i == 0 ? BOSKEY_F1 : BOSKEY_F2;
		if (BosKeys_has(BosKeys_slot(key, 0)))
			continue;
		const char* t = tools[i];
		if (!t || !t[0]) {
			BosKeys_set(key, 0, "", NULL);
			continue;
		}
		const char* base = strrchr(t, '/');
		base = base ? base + 1 : t;
		char id[128];
		snprintf(id, sizeof(id), "%s", base);
		char* dot = strrchr(id, '.');
		if (dot && !strcmp(dot, ".pak"))
			*dot = '\0';
		char v[160];
		snprintf(v, sizeof(v), "app:%s", id);
		BosKeys_set(key, 0, v, id);
	}
}

// one line from a small file, "" when there is none
static inline void BosKeys_readFile(const char* path, char* out, size_t size) {
	out[0] = '\0';
	FILE* f = fopen(path, "r");
	if (!f)
		return;
	if (!fgets(out, (int)size, f))
		out[0] = '\0';
	fclose(f);
	out[strcspn(out, "\r\n")] = '\0';
}

static inline void BosKeys_writeFile(const char* path, const char* text) {
	FILE* f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%s\n", text);
	fclose(f);
}

#endif
