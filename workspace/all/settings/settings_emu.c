// ตั้งค่า › ในเกม › ตั้งค่าแยกเครื่อง (DESIGN §6.7c): the console-wide emulator options, drawn by Settings in place
// of the Emulator Settings app. The files and rules are options.elf's: the pak's options.sh --paths refreshes the
// schema cache and says where things are; opts_minarch_load / opts_minarch_save read and write the layered cfgs.
// The text is the core's (English); res/lang/th-coreopts.txt gives the Thai, keyed by option key.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#include "defines.h"
#include "emu_overlay_cfg.h"
#include "i18n.h"
#include "opts_minarch.h"
#include "opts_override.h"
#include "settings_menu.h"
#include "utils.h"

#define MAX_CONSOLES 64

// ---- the Thai table: "<key>", "<key>.desc", "<key>=<value>" -> Thai ----
typedef struct {
	char* k;
	char* v;
} Tr;
static Tr* tr;
static int ntr;
static int trCmp(const void* a, const void* b) {
	return strcmp(((const Tr*)a)->k, ((const Tr*)b)->k);
}
static void trLoad(void) {
	if (tr)
		return;
	if (!strcmp(I18N_text("Settings"), "Settings")) // not Thai: the cores' own English
		return;
	FILE* f = fopen(RES_PATH "/lang/th-coreopts.txt", "r");
	if (!f)
		return;
	int cap = 1024;
	tr = malloc(sizeof(Tr) * cap);
	char line[2048];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char* t = strchr(line, '\t');
		if (!t || line[0] == '#')
			continue;
		*t++ = 0;
		if (ntr == cap)
			tr = realloc(tr, sizeof(Tr) * (cap *= 2));
		tr[ntr].k = strdup(line), tr[ntr].v = strdup(t), ntr++;
	}
	fclose(f);
	qsort(tr, ntr, sizeof(Tr), trCmp);
}
static const char* trGet(const char* key) {
	if (!ntr)
		return NULL;
	Tr q = {(char*)key, NULL};
	Tr* r = bsearch(&q, tr, ntr, sizeof(Tr), trCmp);
	return r ? r->v : NULL;
}

// ---- consoles that have games and an options.sh ----
typedef struct {
	char name[128];	  // the console as the launcher names it
	char emu[64];	  // the core, from the pak's "# NAME: Console (core)"
	char script[MAX_PATH];
} Console;
static Console consoles[MAX_CONSOLES];
static int nconsoles;
static SettingItem console_items[MAX_CONSOLES];
static SettingsPage list_page;
static char count_text[32];

static bool scriptFor(const char* tag, char* out, size_t size) {
	snprintf(out, size, "%s/Emus/%s.pak/options.sh", SDCARD_PATH, tag); // an SD pak wins, as getEmuPath
	if (exists(out))
		return true;
	snprintf(out, size, "%s/paks/Emus/%s.pak/options.sh", SYSTEM_PATH, tag);
	return exists(out);
}

static void emuName(const char* script, char* out, size_t size) {
	out[0] = 0;
	FILE* f = fopen(script, "r");
	if (!f)
		return;
	char line[256];
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "# NAME: ", 8)) {
			char* o = strrchr(line, '(');
			char* c = o ? strchr(o, ')') : NULL;
			if (o && c)
				snprintf(out, size, "%.*s", (int)(c - o - 1), o + 1);
			break;
		}
	fclose(f);
}

static int byConsole(const void* a, const void* b) {
	return strcasecmp(((const Console*)a)->name, ((const Console*)b)->name);
}

// the launcher's console list cache: "<Roms dir>\t<name>\t<games>"
static void scanConsoles(void) {
	nconsoles = 0;
	FILE* f = fopen(USERDATA_PATH "/emulist_cache.txt", "r");
	if (!f)
		return;
	char line[1024];
	while (nconsoles < MAX_CONSOLES && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#')
			continue;
		char* name = strchr(line, '\t');
		char* count = name ? strchr(name + 1, '\t') : NULL;
		if (!count || atoi(count + 1) <= 0)
			continue;
		*name++ = 0, *count = 0;
		char* close = strrchr(line, ')');
		char* open = close ? strrchr(line, '(') : NULL;
		if (!open || close < open)
			continue;
		char tag[32];
		snprintf(tag, sizeof(tag), "%.*s", (int)(close - open - 1), open + 1);
		if (!strcmp(tag, "PORTS"))
			continue;
		Console* c = &consoles[nconsoles];
		if (!scriptFor(tag, c->script, sizeof(c->script)))
			continue;
		snprintf(c->name, sizeof(c->name), "%s", name);
		emuName(c->script, c->emu, sizeof(c->emu));
		nconsoles++;
	}
	fclose(f);
	qsort(consoles, nconsoles, sizeof(Console), byConsole);
}

// ---- one console's options ----
static EmuOvlConfig cfg;
static OptsMinarchState mst;
static OptsOverrideState ost;
static bool loaded;
static SettingItem* opt_items;
static int nopt;
static SettingsPage opt_page;
static char opt_title[160], opt_sub[160], opt_desc_none[8];
static const char* ONOFF[] = {"Off", "On"};
static int ONOFF_V[] = {0, 1};

typedef struct { // what a row edits
	int s, i;
	int* values;
	const char** labels;
	char** owned; // labels this page made (freed on close)
	int n;
} Row;
static Row* rows;

static void freeOptions(void) {
	for (int r = 0; r < nopt; r++) {
		if (rows[r].owned) {
			for (int k = 0; k < rows[r].n; k++)
				free(rows[r].owned[k]);
			free(rows[r].owned);
		}
		if (rows[r].values != ONOFF_V)
			free(rows[r].values);
		if (rows[r].labels != ONOFF)
			free(rows[r].labels);
	}
	free(rows), rows = NULL;
	free(opt_items), opt_items = NULL;
	nopt = 0;
	if (loaded) {
		opts_minarch_free(&mst);
		emu_ovl_cfg_free(&cfg);
		loaded = false;
	}
}

static void optClear(void) { // X: the focused option back to its default
	SettingsPage* p = settings_menu_current();
	SettingItem* it = p ? settings_page_visible_item(p, p->selected) : NULL;
	if (!it || !it->user_data)
		return;
	Row* r = it->user_data;
	int def = cfg.sections[r->s].items[r->i].default_value;
	for (int k = 0; k < r->n; k++)
		if (r->values[k] == def)
			it->current_idx = k;
}

static void optHide(SettingsPage* page) {
	(void)page;
	if (!loaded)
		return;
	for (int r = 0; r < nopt; r++) {
		SettingItem* it = &opt_items[r];
		Row* w = &rows[r];
		if (it->current_idx >= 0 && it->current_idx < w->n)
			cfg.sections[w->s].items[w->i].staged_value = w->values[it->current_idx];
	}
	if (opts_minarch_save(&cfg, &mst, &ost) < 0)
		fprintf(stderr, "emu options: save failed\n");
	freeOptions();
}

// the row's values and labels, Thai where the table has them
static void rowValues(Row* r, const EmuOvlItem* item) {
	if (item->type == EMU_OVL_TYPE_BOOL) {
		r->values = ONOFF_V, r->labels = ONOFF, r->n = 2;
		return;
	}
	int n = item->value_count;
	bool range = item->type == EMU_OVL_TYPE_INT && n <= 0 && item->int_step > 0 &&
				 (item->int_max - item->int_min) / item->int_step < 200;
	if (range)
		n = (item->int_max - item->int_min) / item->int_step + 1;
	if (n <= 0)
		n = 1;
	r->n = n;
	r->values = malloc(sizeof(int) * n);
	r->labels = malloc(sizeof(char*) * n);
	r->owned = calloc(n, sizeof(char*));
	for (int k = 0; k < n; k++) {
		char buf[160], key[EMU_OVL_MAX_STR * 2 + 2];
		if (range) {
			r->values[k] = item->int_min + k * item->int_step;
			snprintf(buf, sizeof(buf), "%d", r->values[k]);
		} else if (item->value_count > 0) {
			r->values[k] = item->values[k];
			const char* sv = item->svalues ? item->svalues[k] : NULL;
			snprintf(key, sizeof(key), "%s=%s", item->key, sv ? sv : "");
			const char* th = trGet(key);
			snprintf(buf, sizeof(buf), "%s", th ? th : item->labels[k][0] ? item->labels[k] : sv ? sv : "");
		} else {
			r->values[k] = item->staged_value;
			emu_ovl_cfg_format_value(item, item->staged_value, buf, sizeof(buf));
		}
		r->owned[k] = strdup(buf);
		r->labels[k] = r->owned[k];
	}
}

static bool loadConsole(const Console* c) {
	char cmd[MAX_PATH + 32], paths[4][MAX_PATH] = {{0}};
	snprintf(cmd, sizeof(cmd), "sh \"%s\" --paths 2>/dev/null", c->script);
	FILE* p = popen(cmd, "r");
	if (!p)
		return false;
	for (int k = 0; k < 4 && fgets(paths[k], sizeof(paths[k]), p); k++)
		paths[k][strcspn(paths[k], "\r\n")] = 0;
	pclose(p);
	if (!paths[0][0] || !paths[3][0])
		return false;
	memset(&cfg, 0, sizeof(cfg));
	memset(&mst, 0, sizeof(mst));
	memset(&ost, 0, sizeof(ost));
	if (emu_ovl_cfg_load(&cfg, paths[0]) != 0)
		return false;
	if (opts_minarch_load(&cfg, &mst, &ost, paths[1], paths[2], paths[3], NULL) != 0) {
		emu_ovl_cfg_free(&cfg);
		return false;
	}
	loaded = true;
	return true;
}

static void openConsole(void) {
	SettingsPage* lp = settings_menu_current();
	SettingItem* sel = lp ? settings_page_visible_item(lp, lp->selected) : NULL;
	if (!sel)
		return;
	const Console* c = &consoles[(int)(intptr_t)sel->user_data];
	trLoad();
	freeOptions();
	snprintf(opt_title, sizeof(opt_title), "Settings | %s", c->name);
	bool ok = loadConsole(c);
	int total = 0;
	if (ok)
		for (int s = 0; s < cfg.section_count; s++)
			if (emu_ovl_cfg_section_visible(&cfg, s))
				total += cfg.sections[s].item_count;
	opt_items = calloc(total + 1, sizeof(SettingItem));
	rows = calloc(total + 1, sizeof(Row));
	snprintf(opt_sub, sizeof(opt_sub), "%s \xC2\xB7 %s", c->emu, I18N_text("Applies the next time a game starts"));
	if (!ok || !total) { // FBNeo before its first game: no schema yet
		opt_items[0] = (SettingItem)ITEM_STATIC_INIT("Can't be set yet", "Start one game of this console first", NULL);
		opt_items[0].get_display = NULL;
		nopt = 0;
		memset(&opt_page, 0, sizeof(opt_page));
		opt_page.title = opt_title;
		opt_page.items = opt_items;
		opt_page.item_count = 1;
		opt_page.dynamic_start = -1;
		settings_menu_push(&opt_page);
		return;
	}
	for (int s = 0; s < cfg.section_count; s++) {
		if (!emu_ovl_cfg_section_visible(&cfg, s))
			continue;
		bool first = true;
		for (int i = 0; i < cfg.sections[s].item_count; i++) {
			EmuOvlItem* item = &cfg.sections[s].items[i];
			Row* r = &rows[nopt];
			r->s = s, r->i = i;
			rowValues(r, item);
			SettingItem* it = &opt_items[nopt];
			char key[EMU_OVL_MAX_STR + 8];
			const char* name = trGet(item->key);
			snprintf(key, sizeof(key), "%s.desc", item->key);
			const char* desc = trGet(key);
			*it = (SettingItem)ITEM_CYCLE_INIT(name ? name : item->label, desc ? desc : item->description,
											   r->labels, r->n, NULL, NULL, NULL, NULL);
			it->user_data = r;
			it->on_clear = optClear;
			it->clear_label = "Restore default";
			for (int k = 0; k < r->n; k++)
				if (r->values[k] == item->staged_value)
					it->current_idx = k;
			if (first) // the first row carries the subtitle line (and the section name when there are several)
				it->header = cfg.section_count > 1 ? cfg.sections[s].name : (nopt == 0 ? opt_sub : NULL);
			first = false;
			nopt++;
		}
	}
	memset(&opt_page, 0, sizeof(opt_page));
	opt_page.title = opt_title;
	opt_page.items = opt_items;
	opt_page.item_count = nopt;
	opt_page.dynamic_start = -1;
	opt_page.on_hide = optHide;
	settings_menu_push(&opt_page);
}

static void listShow(SettingsPage* page) {
	(void)page;
	scanConsoles();
	for (int i = 0; i < nconsoles; i++) {
		SettingItem* it = &console_items[i];
		*it = (SettingItem)ITEM_BUTTON_INIT(consoles[i].name, "", openConsole);
		snprintf(it->display_text, sizeof(it->display_text), "%s", consoles[i].emu);
		it->chevron = 1;
		it->user_data = (void*)(intptr_t)i;
		it->header = i == 0 ? "Consoles with games · A–Z" : NULL;
	}
	list_page.item_count = nconsoles;
}

SettingsPage* emu_page_create(void) {
	memset(&list_page, 0, sizeof(list_page));
	list_page.title = "Settings | Per-console settings";
	list_page.items = console_items;
	list_page.dynamic_start = -1;
	list_page.max_items = MAX_CONSOLES;
	list_page.on_show = listShow;
	scanConsoles();
	return &list_page;
}

const char* emu_count_text(void) { // "8 เครื่อง" on ในเกม's row
	snprintf(count_text, sizeof(count_text), I18N_text("%d consoles"), nconsoles);
	return count_text;
}
