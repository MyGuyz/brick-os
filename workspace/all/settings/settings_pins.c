// ตั้งค่า › หน้าแรกและเกม › แอปที่ปักหมุด (DESIGN §6.7b): the same pins as Home (bos_apps.c, bos_home_pins.txt).
// Pinned apps first in pin order, then the rest ก–ฮ. A pins / unpins; X picks a pinned app up (▲▼ move, A done,
// B cancel), the same as Home's จัดเรียง.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bos_apps.h"
#include "i18n.h"
#include "settings_menu.h"

static const char* pin_labels[] = {"Off", "On"};
static SettingItem pin_items[BOS_APPS_MAX];
static SettingsPage pins_page;
static bool moving;
static int saved[BOS_APPS_MAX], nsaved;
static char count_text[32];

static int byName(const void* a, const void* b) {
	return strcasecmp(I18N_text(bos_apps[*(const int*)a].name), I18N_text(bos_apps[*(const int*)b].name));
}

static int itemApp(const SettingItem* it) {
	return (int)(intptr_t)it->user_data;
}

static int pinsInput(SettingItem* it);
static void pinsClear(void) { // X on a pinned row: จัดเรียง (the hint says so; the work is in pinsInput)
}

// the rows from the pins; the focus goes to `focus_app` (-1: keep the index)
static void rebuild(int focus_app) {
	int order[BOS_APPS_MAX], n = 0;
	for (int i = 0; i < bos_npins; i++)
		order[n++] = bos_pins[i];
	int rest = n;
	for (int i = 0; i < bos_napps; i++)
		if (BosApps_pinIndex(i) < 0)
			order[n++] = i;
	qsort(order + rest, n - rest, sizeof(int), byName);

	for (int i = 0; i < n; i++) {
		BosApp* a = &bos_apps[order[i]];
		int pin = BosApps_pinIndex(order[i]);
		SettingItem* it = &pin_items[i];
		*it = (SettingItem)ITEM_CYCLE_INIT(a->name, "You can pin any number of apps. Over 6, Home's row scrolls.",
										   pin_labels, 2, NULL, NULL, NULL, NULL);
		it->current_idx = pin >= 0;
		it->user_data = (void*)(intptr_t)order[i];
		it->lead_icon = a->icon;
		it->lead_color = a->color;
		it->on_input = pinsInput;
		it->header = i == 0 && rest ? "On Home · in this order" : i == rest ? "Other apps · A–Z" : NULL;
		if (pin >= 0) {
			snprintf(it->note, sizeof(it->note), "%d", pin + 1);
			it->a_label = "Unpin";
			it->on_clear = pinsClear;
			it->clear_label = "Reorder";
		} else
			it->a_label = "Pin";
		if (moving && pin >= 0 && order[i] == focus_app) {
			it->lifted = 1;
			it->a_label = "Done";
			it->b_label = "Cancel";
			it->lhint_keys = "\xE2\x96\xB2\xE2\x96\xBC"; // ▲▼
			it->lhint_label = "Move";
			it->on_clear = NULL;
		}
		if (order[i] == focus_app)
			pins_page.selected = i;
	}
	pins_page.item_count = n;
	snprintf(count_text, sizeof(count_text), "%d", bos_npins);
}

static int pinsInput(SettingItem* it) {
	int app = itemApp(it), at = BosApps_pinIndex(app);
	if (moving) {
		if (PAD_justRepeated(BTN_UP) && at > 0) {
			int t = bos_pins[at - 1];
			bos_pins[at - 1] = app, bos_pins[at] = t;
			rebuild(app);
		} else if (PAD_justRepeated(BTN_DOWN) && at >= 0 && at < bos_npins - 1) {
			int t = bos_pins[at + 1];
			bos_pins[at + 1] = app, bos_pins[at] = t;
			rebuild(app);
		} else if (PAD_justPressed(BTN_A)) {
			moving = false;
			BosApps_savePins();
			rebuild(app);
		} else if (PAD_justPressed(BTN_B)) {
			moving = false;
			memcpy(bos_pins, saved, sizeof(int) * nsaved);
			bos_npins = nsaved;
			rebuild(app);
		}
		return 1; // nothing else moves while an app is picked up
	}
	if (PAD_justPressed(BTN_A)) {
		if (at >= 0) {
			memmove(&bos_pins[at], &bos_pins[at + 1], sizeof(int) * (bos_npins - at - 1));
			bos_npins--;
		} else if (bos_npins < BOS_APPS_MAX)
			bos_pins[bos_npins++] = app;
		BosApps_savePins();
		rebuild(app); // the row moves to its new group, the focus follows it
		return 1;
	}
	if (PAD_justPressed(BTN_X) && at >= 0) {
		moving = true;
		memcpy(saved, bos_pins, sizeof(int) * bos_npins);
		nsaved = bos_npins;
		rebuild(app);
		return 1;
	}
	if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT))
		return 1; // not a value row: Left/Right change nothing here
	return 0;
}

static void pinsShow(SettingsPage* page) {
	(void)page;
	moving = false;
	BosApps_build();
	rebuild(-1);
}

static void pinsHide(SettingsPage* page) {
	(void)page;
	if (moving) { // left with an app picked up: as B
		moving = false;
		memcpy(bos_pins, saved, sizeof(int) * nsaved);
		bos_npins = nsaved;
	}
}

SettingsPage* pins_page_create(void) {
	memset(&pins_page, 0, sizeof(pins_page));
	pins_page.title = "Settings | Pinned apps";
	pins_page.items = pin_items;
	pins_page.dynamic_start = -1;
	pins_page.max_items = BOS_APPS_MAX;
	pins_page.on_show = pinsShow;
	pins_page.on_hide = pinsHide;
	BosApps_build();
	rebuild(-1);
	return &pins_page;
}

// "6 แอป" on หน้าแรกและเกม's row
const char* pins_count_text(void) {
	static char t[48];
	snprintf(t, sizeof(t), I18N_text("%d apps"), bos_npins);
	return t;
}
