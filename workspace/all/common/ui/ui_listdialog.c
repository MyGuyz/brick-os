#include "ui_listdialog.h"
#include "ui_list.h"
#include "list_nav.h"
#include "ui_accent.h"
#include "ui_buttonhintbar.h"
#include "ui_menubar.h"
#include "ui_draw.h"
#include "api.h"
#include "defines.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static char dialog_title[128];
// a copy of the caller's rows, any number of them (LISTDIALOG_MAX_ITEMS is only a size for callers' own arrays)
static ListDialogItem* dialog_items = NULL;
static int dialog_count;
static int dialog_selected;
static int dialog_scroll;

void ListDialog_init(const char* title) {
	strncpy(dialog_title, title, sizeof(dialog_title) - 1);
	dialog_title[sizeof(dialog_title) - 1] = '\0';
	dialog_count = 0;
	dialog_selected = 0;
	dialog_scroll = 0;
	free(dialog_items);
	dialog_items = NULL;
}

void ListDialog_setItems(ListDialogItem* items, int count) {
	if (items == NULL || count < 0)
		count = 0;
	if (count > 0) {
		ListDialogItem* copy = realloc(dialog_items, (size_t)count * sizeof(ListDialogItem));
		if (copy) {
			dialog_items = copy;
			memcpy(dialog_items, items, (size_t)count * sizeof(ListDialogItem));
		} else {
			count = 0; // out of memory: no rows
		}
	}
	dialog_count = count;
	ListNav_clampSelection(dialog_count, &dialog_selected, &dialog_scroll);
}

ListDialogResult ListDialog_handleInput(void) {
	ListDialogResult result = {LISTDIALOG_NONE, -1};

	if (PAD_justPressed(BTN_B)) {
		result.action = LISTDIALOG_CANCEL;
		return result;
	}

	if (dialog_count == 0)
		return result;

	if (PAD_justPressed(BTN_A)) {
		result.action = LISTDIALOG_SELECTED;
		result.index = dialog_selected;
		return result;
	}

	PAD_navigateMenu(&dialog_selected, dialog_count);

	return result;
}

void BOS_uiListDialog(SDL_Surface* s, const char* title, ListDialogItem* items, int count, int sel, int* scroll);
void ListDialog_render(SDL_Surface* screen) {
	GFX_clearLayers(LAYER_SCROLLTEXT);
	// Brick OS (ui_draw.h)
	BOS_uiListDialog(screen, dialog_title, dialog_items, dialog_count, dialog_selected, &dialog_scroll);
}

void ListDialog_quit(void) {
	free(dialog_items);
	dialog_items = NULL;
	dialog_count = 0;
	dialog_selected = 0;
	dialog_scroll = 0;
	dialog_title[0] = '\0';
}
