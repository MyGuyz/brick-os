// Brick OS: UI translation table (i18n.h). Built as part of text_shape.c.
#include "i18n.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef I18N_NO_DEFINES
#include "defines.h"
#define I18N_LANG_SETTING SHARED_USERDATA_PATH "/lang"
#define I18N_TABLE_DIR RES_PATH "/lang"
#endif

typedef struct {
	char* key;
	char* val;
} I18nEntry;

static I18nEntry* i18n_slots = NULL; // open addressing, power-of-two size
static uint32_t i18n_cap = 0;
static int i18n_state = 0; // 0 not loaded, 1 loaded (possibly empty)

static uint32_t i18n_hash(const char* s) {
	uint32_t h = 2166136261u;
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

static void i18n_free(void) {
	for (uint32_t i = 0; i < i18n_cap; i++) {
		free(i18n_slots[i].key);
		free(i18n_slots[i].val);
	}
	free(i18n_slots);
	i18n_slots = NULL;
	i18n_cap = 0;
}

// "\n" -> newline, in place
static void i18n_unescape(char* s) {
	char* w = s;
	for (; *s; s++) {
		if (s[0] == 0x5C && s[1] == 'n') { // backslash
			*w++ = '\n';
			s++;
		} else
			*w++ = *s;
	}
	*w = '\0';
}

static void i18n_put(char* key, char* val) {
	uint32_t i = i18n_hash(key) & (i18n_cap - 1);
	while (i18n_slots[i].key) {
		if (!strcmp(i18n_slots[i].key, key)) { // a later line wins
			free(i18n_slots[i].val);
			free(key);
			i18n_slots[i].val = val;
			return;
		}
		i = (i + 1) & (i18n_cap - 1);
	}
	i18n_slots[i].key = key;
	i18n_slots[i].val = val;
}

int I18N_loadFile(const char* path) {
	i18n_free();
	i18n_state = 1;
	FILE* f = path ? fopen(path, "r") : NULL;
	if (!f)
		return 0;
	int lines = 0;
	char line[1024];
	while (fgets(line, sizeof(line), f))
		lines++;
	i18n_cap = 64;
	while (i18n_cap < (uint32_t)lines * 2)
		i18n_cap <<= 1;
	i18n_slots = calloc(i18n_cap, sizeof(I18nEntry));
	if (!i18n_slots) {
		fclose(f);
		i18n_cap = 0;
		return 0;
	}
	rewind(f);
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char* tab = strchr(line, '\t');
		if (line[0] == '#' || !tab || tab == line || !tab[1])
			continue;
		*tab = '\0';
		char* key = strdup(line);
		char* val = strdup(tab + 1);
		if (!key || !val) {
			free(key);
			free(val);
			continue;
		}
		i18n_unescape(key);
		i18n_unescape(val);
		i18n_put(key, val);
		n++;
	}
	fclose(f);
	return n;
}

#ifdef I18N_LANG_SETTING
static void i18n_load_current(void) {
	char lang[16] = "th";
	FILE* f = fopen(I18N_LANG_SETTING, "r");
	if (f) {
		if (fscanf(f, "%15s", lang) != 1)
			strcpy(lang, "th");
		fclose(f);
	}
	if (!strcmp(lang, "en")) {
		i18n_free();
		i18n_state = 1;
		return;
	}
	char path[256];
	snprintf(path, sizeof(path), "%s/%s.txt", I18N_TABLE_DIR, lang);
	I18N_loadFile(path);
}
#endif

void I18N_reload(void) {
	i18n_free();
	i18n_state = 0;
}

const char* I18N_text(const char* s) {
	if (!s || !*s)
		return s;
#ifdef I18N_LANG_SETTING
	if (!i18n_state)
		i18n_load_current();
#endif
	if (!i18n_cap)
		return s;
	uint32_t i = i18n_hash(s) & (i18n_cap - 1);
	while (i18n_slots[i].key) {
		if (!strcmp(i18n_slots[i].key, s))
			return i18n_slots[i].val;
		i = (i + 1) & (i18n_cap - 1);
	}
	return s;
}
