// Unified list widget: render + state + input for the OS's pill lists,
// composed from the ui_list.c primitives (glide core, marquee, pill
// helpers). Spec: docs/superpowers/specs/2026-08-10-unified-listview-design.md
#include <stdlib.h>
#include <string.h>
#include "ui_listview.h"
#include "ui_draw.h"
#include "initial_jump.h"
#include "list_nav.h"
#include "api.h"
#include "defines.h"
#include "ui_buttonhintbar.h"
#include "ui_menubar.h"
#include "ui_emptystate.h"
#include "../ui_list_layout.h"

#define LISTVIEW_ICON_SIZE 24 // logical px, SCALE1'd at draw
#define LISTVIEW_ICON_SPACING 6
#define LISTVIEW_MAX_VISIBLE 32 // list_h / (item_h*3/4) tops out well below this

static void get_row_safe(ListView* v, int i, bool selected, ListViewRow* out) {
	memset(out, 0, sizeof(*out));
	// Provider may not be wired yet: full-mode callers UI_listViewReset on
	// module entry, but get_row/ctx are only assigned by the first render -
	// input runs first that frame.
	if (v->get_row)
		v->get_row(v->ctx, i, selected, out);
	if (!out->label)
		out->label = ""; // normalizes both the provider and no-provider paths
}

static bool row_is_header(ListView* v, int i) {
	ListViewRow row;
	get_row_safe(v, i, false, &row);
	return row.is_header;
}

static bool row_is_header_cb(void* v, int i) {
	return row_is_header(v, i);
}

// Selectable-row steps over the headers (ListNav_step, ListNav_nearest, ListNav_adjacent).
static int step_selectable(ListView* v, int from, int dir) {
	return ListNav_step(v->count, v->no_wrap, from, dir, row_is_header_cb, v);
}

static int nearest_selectable(ListView* v, int target, int dir) {
	return ListNav_nearest(v->count, target, dir, row_is_header_cb, v);
}

ListViewAction UI_listViewHandleInput(ListView* v) {
	ListViewAction a = {LISTVIEW_NONE, v->count > 0 ? v->selected : -1, 0};

	if (v->count > 0) {
		int page = v->last_visible > 0 ? v->last_visible : 1;
		int prev = v->selected;
		if (PAD_justRepeated(BTN_UP)) {
			v->selected = step_selectable(v, v->selected, -1);
		} else if (PAD_justRepeated(BTN_DOWN)) {
			v->selected = step_selectable(v, v->selected, +1);
		} else if (!v->no_lr_paging && PAD_justRepeated(BTN_LEFT)) {
			v->selected = nearest_selectable(v, v->selected - page, -1);
		} else if (!v->no_lr_paging && PAD_justRepeated(BTN_RIGHT)) {
			v->selected = nearest_selectable(v, v->selected + page, +1);
		} else if (PAD_justRepeated(BTN_L2)) { // DESIGN 7.7: L2/R2 a page up / down, stopping at the ends
			v->selected = nearest_selectable(v, v->selected - page, -1);
		} else if (PAD_justRepeated(BTN_R2)) {
			v->selected = nearest_selectable(v, v->selected + page, +1);
		}
		if (v->selected != prev) {
			v->input_pending = true;
			a.index = v->selected;
		}
	}

	if (PAD_justPressed(BTN_A) && v->count > 0 &&
		!row_is_header(v, v->selected)) {
		a.type = LISTVIEW_ACTIVATED;
		return a;
	}
	if (PAD_justPressed(BTN_A) && v->count == 0) {
		// Empty list: report A as a button so empty-state actions (e.g. an
		// advertised A/NEW) can fire; index is already -1.
		a.type = LISTVIEW_BUTTON;
		a.btn = BTN_A;
		return a;
	}
	if (PAD_justPressed(BTN_B)) {
		a.type = LISTVIEW_BACK;
		return a;
	}
	if (PAD_tappedMenu(SDL_GetTicks())) {
		a.type = LISTVIEW_MENU;
		return a;
	}
	if (PAD_justPressed(BTN_X)) {
		a.type = LISTVIEW_BUTTON;
		a.btn = BTN_X;
		return a;
	}
	if (PAD_justPressed(BTN_Y)) {
		a.type = LISTVIEW_BUTTON;
		a.btn = BTN_Y;
		return a;
	}
	return a;
}

// First letter of row i's label, uppercased ('a'-'z' -> 'A'-'Z'); leading
// spaces skipped. Used only by the initial-jump navigation below.
static char row_initial(void* ctx, int i) {
	ListViewRow row;
	get_row_safe((ListView*)ctx, i, false, &row);
	return InitialJump_labelInitial(row.label);
}

bool UI_listViewJumpInitial(ListView* v, int dir) {
	if (!v || v->count <= 1)
		return false;
	int n = v->count;
	int target = InitialJump_target(n, v->selected, dir, row_initial, v);
	if (target < 0)
		return false; // already in the first/last letter group
	if (row_is_header(v, target))
		target = nearest_selectable(v, target, dir > 0 ? +1 : -1);
	if (target < 0 || target >= n || target == v->selected)
		return false;
	v->selected = target;
	v->input_pending = true;
	return true;
}

void UI_listViewReset(ListView* v, int count, const void* list_id) {
	v->count = count;
	v->list_id = list_id;
	v->selected = 0;
	v->scroll = 0;
	v->drawn_id = list_id;
	v->drawn_count = count;
	v->prev_selected = -1;
	v->input_pending = true;
	GFX_clearLayers(LAYER_SCROLLTEXT);
	ScrollText_clear(&v->marquee);
	// glide: the list_id change (or same-id relocation's own travel) is
	// handled by UI_listGlideDrawAtY on the next render.
}

// The Brick OS look lives in the kit (bos_list.c); apps without the kit never set bos
void BOS_listViewRender(ListView* v, SDL_Surface* screen) __attribute__((weak));
bool BOS_listViewBusy(ListView* v) __attribute__((weak));

bool UI_listViewBusy(ListView* v) {
	return BOS_listViewBusy(v);
}

void UI_listViewTickIdle(ListView* v) {
	if (v->bos || (UI_bos && BOS_listViewRender))
		return;
	ScrollText_activateAfterDelay(&v->marquee);
	if (ScrollText_isScrolling(&v->marquee))
		ScrollText_animateOnly(&v->marquee);
}

void UI_listViewRender(ListView* v, SDL_Surface* screen) {
	BOS_listViewRender(v, screen);
}
