// Brick OS ตั้งค่า (DESIGN.md 6.6, board v2-SettingsIPad): the categories in iOS groups on the left (the root page),
// the highlighted category's page on the right (the page open now once one is pushed). Drawn from the shared kit
// (bos_ui.h); input, pages and items stay the menu's own.

#include "settings_bos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "bos_ui.h"
#include "defines.h"
#include "i18n.h"

#define C_SEP 0x38383A	  // row separator inside a group
#define C_ON_SUB 0x3A3A3C // a value / chevron on the focused (white) row
#define C_SLIDER_ON 0xC7C7CC

// ---- categories (§6.7): squircle icon and colour, and the group each sits in ----
typedef struct {
	const char* name; // the root item's (English) name
	const char* icon;
	uint32_t color;
	int group;
} Cat;
static const Cat CATS[] = {
	{"Network", "wifi", 0x0A84FF, 1},
	{"Bluetooth", "bt", 0x0A84FF, 1},
	{"Display", "display", 0x5E5CE6, 2},
	{"Audio", "sound", 0xFF375F, 2},
	{"Battery", "battery", 0x30D158, 2},
	{"Appearance", "palette", 0xBF5AF2, 3},
	{"Home & games", "home", 0xFF9F0A, 3},
	{"Shortcuts", "keyboard", 0x40C8E0, 4},
	{"Controls", "pad", 0x8E8E93, 4},
	{"In-game", "game", 0x30D158, 5},
	{"LED Control", "led", 0xFFD60A, 5},
	{"System", "cpu", 0x8E8E93, 6},
	{"About", "info", 0x8E8E93, 6},
};
#define NCATS ((int)(sizeof(CATS) / sizeof(CATS[0])))

static const Cat* catOf(const char* name) {
	static const Cat other = {"", "settings", 0x8E8E93, 7};
	for (int i = 0; i < NCATS; i++)
		if (name && !strcmp(name, CATS[i].name))
			return &CATS[i];
	return &other;
}

int SettingsBos_order(const char* name) {
	for (int i = 0; i < NCATS; i++)
		if (name && !strcmp(name, CATS[i].name))
			return i;
	return NCATS;
}

// a colour drawn at `t` over the group's surface (a dimmed row's squircle, toggle, slider)
static uint32_t mix(uint32_t c, uint32_t under, float t) {
	uint32_t o = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		float a = (c >> sh) & 255, b = (under >> sh) & 255;
		o |= (uint32_t)(b + (a - b) * t + 0.5f) << sh;
	}
	return o;
}

static const char* itemValue(SettingItem* it) {
	if ((it->type == ITEM_CYCLE || it->type == ITEM_COLOR) && it->labels && it->current_idx >= 0 &&
		it->current_idx < it->label_count)
		return it->labels[it->current_idx];
	if (it->type == ITEM_STATIC)
		return it->get_display ? it->get_display() : it->display_text;
	if (it->type == ITEM_TEXT_INPUT)
		return it->get_text ? it->get_text() : it->text_value;
	return "";
}

static bool numericRange(SettingItem* it) { // a 0..N cycle drawn as a slider
	if (it->type != ITEM_CYCLE || !it->labels || it->label_count < 5)
		return false;
	for (int i = 0; i < it->label_count; i++) {
		const char* l = it->labels[i];
		if (!l || !l[0])
			return false;
		for (const char* p = l; *p; p++)
			if ((*p < '0' || *p > '9') && *p != '%')
				return false;
	}
	return true;
}

// an Off/On cycle drawn as a toggle: 1 = on, 0 = off, -1 = not a toggle
static int toggleState(SettingItem* it) {
	if (it->type != ITEM_CYCLE || !it->labels || it->label_count != 2 || !it->labels[0] || !it->labels[1])
		return -1;
	static const char* pairs[][2] = {{"Off", "On"}, {"Hide", "Show"}};
	for (int p = 0; p < 2; p++) {
		int off = !strcmp(it->labels[0], pairs[p][0]) ? 0 : !strcmp(it->labels[1], pairs[p][0]) ? 1 : -1;
		int on = !strcmp(it->labels[0], pairs[p][1]) ? 0 : !strcmp(it->labels[1], pairs[p][1]) ? 1 : -1;
		if (off >= 0 && on >= 0)
			return it->current_idx == on;
	}
	return -1;
}

bool SettingsBos_isToggle(SettingItem* it) {
	return it && toggleState(it) >= 0;
}

// a Left/Right value row (not a switch, a two-value flip or a button-capture row): A opens its value list
bool SettingsBos_isPickable(SettingItem* it) {
	return it && (it->type == ITEM_CYCLE || it->type == ITEM_COLOR) && it->labels && it->label_count > 1 &&
		   toggleState(it) < 0 && !it->a_cycles && !it->on_press;
}

static const char* pageTitle(SettingsPage* page, SettingItem* cat) {
	const char* title = page && page->title ? page->title : (cat ? cat->name : "");
	const char* bar = strrchr(title, '|'); // "Settings | Display": the page's own name
	if (bar) {
		title = bar + 1;
		while (*title == ' ')
			title++;
	}
	return title;
}

// ---- left column: the categories, iOS groups of rows 84 / label 34, a squircle each ----
static int lscroll; // content px scrolled off the top

static void leftColumn(SDL_Surface* s, SettingsPage* root, bool focused, int top, int bottom) {
	int lx = BOS_px(24), lw = BOS_px(380), rh = BOS_px(84), gap = BOS_px(14), r = BOS_px(22);
	int n = settings_page_visible_count(root);
	if (n <= 0)
		return;
	int ry[64];
	if (n > 64)
		n = 64;
	int y = BOS_px(6), prev = -1; // room for the focused row's 1.04 scale under the status bar
	for (int i = 0; i < n; i++) {
		SettingItem* it = settings_page_visible_item(root, i);
		int g = catOf(it ? it->name : NULL)->group;
		if (i > 0 && g != prev)
			y += gap;
		prev = g;
		ry[i] = y;
		y += rh;
	}
	int view = bottom - top, sel = root->selected < 0 ? 0 : root->selected >= n ? n - 1 : root->selected;
	if (ry[sel] - lscroll < BOS_px(6))
		lscroll = ry[sel] - BOS_px(6);
	if (ry[sel] + rh - lscroll > view)
		lscroll = ry[sel] + rh - view;
	if (lscroll > y + BOS_px(6) - view)
		lscroll = y + BOS_px(6) - view;
	if (lscroll < 0)
		lscroll = 0;
	SDL_Rect clip = {0, top, lx + lw + BOS_px(14), view};
	SDL_SetClipRect(s, &clip);

	// group boxes, then separators, then the rows
	for (int i = 0; i < n;) {
		int g = catOf(settings_page_visible_item(root, i)->name)->group, j = i;
		while (j + 1 < n && catOf(settings_page_visible_item(root, j + 1)->name)->group == g)
			j++;
		BOS_rrect(s, lx, top + ry[i] - lscroll, lw, ry[j] + rh - ry[i], r, BOS_C_SURFACE);
		for (int k = i; k < j; k++)
			if (k != sel && k + 1 != sel)
				SDL_FillRect(s, &(SDL_Rect){lx + BOS_px(96), top + ry[k] + rh - lscroll - 1, lw - BOS_px(96), 1}, BOS_map(s, C_SEP));
		i = j + 1;
	}
	for (int i = 0; i < n; i++) {
		SettingItem* it = settings_page_visible_item(root, i);
		if (!it)
			continue;
		const Cat* c = catOf(it->name);
		int x = lx, w = lw, h = rh, yy = top + ry[i] - lscroll;
		bool hot = i == sel && focused, open = i == sel && !focused;
		if (hot) { // focus: white fill, x1.04
			int gw = (int)(w * BOS_ROW_SCALE) - w, gh = (int)(h * BOS_ROW_SCALE) - h;
			x -= gw / 2, w += gw, yy -= gh / 2, h += gh;
			BOS_rrect(s, x, yy, w, h, BOS_px(BOS_RADIUS_ROW), BOS_theme()->focus);
		} else if (open)
			BOS_rrect(s, x, yy, w, h, BOS_px(BOS_RADIUS_ROW), BOS_C_SURFACE2);
		float a = hot || open ? 1.0f : BOS_DIM;
		int sq = BOS_px(56), sx = x + BOS_px(20), sy = yy + (h - sq) / 2;
		BOS_rrect(s, sx, sy, sq, sq, BOS_px(15), mix(c->color, BOS_C_SURFACE, a));
		int ic = BOS_px(36);
		BOS_icon(s, c->icon, 0xFFFFFF, a, sx + (sq - ic) / 2, sy + (sq - ic) / 2, ic);
		char name[128];
		int nx = sx + sq + BOS_px(22);
		if (hot)
			BOS_textMarquee(s, I18N_text(it->name), 34, 600, BOS_theme()->onfocus, a, nx, yy + h / 2, x + w - BOS_px(20) - nx);
		else {
			BOS_fit(I18N_text(it->name), 34, 500, x + w - BOS_px(20) - nx, name, sizeof(name));
			BOS_textMid(s, name, 34, 500, BOS_C_TEXT, a, nx, yy + h / 2);
		}
	}
	SDL_SetClipRect(s, NULL);
}

// ---- right column: the page's large title and its group of rows 96 ----
static void toggle(SDL_Surface* s, int x, int cy, bool on, bool hot, float a) {
	int w = BOS_px(80), h = BOS_px(46), k = BOS_px(38), p = BOS_px(4);
	uint32_t track = on ? BOS_theme()->primary : hot ? BOS_C_TEXTOFF : BOS_C_SURFACE2;
	BOS_rrect(s, x, cy - h / 2, w, h, h / 2, hot ? track : mix(track, BOS_C_SURFACE, a));
	BOS_rrect(s, on ? x + w - p - k : x + p, cy - k / 2, k, k, k / 2, hot ? 0xFFFFFF : mix(0xFFFFFF, BOS_C_SURFACE, a));
}

static void slider(SDL_Surface* s, int x, int w, int cy, float frac, bool hot, float a) {
	int h = BOS_px(12);
	BOS_rrect(s, x, cy - h / 2, w, h, h / 2, hot ? C_SLIDER_ON : mix(BOS_C_SURFACE2, BOS_C_SURFACE, a));
	int fw = (int)(w * frac);
	if (fw > 0)
		BOS_rrect(s, x, cy - h / 2, fw < h ? h : fw, h, h / 2, hot ? BOS_theme()->onfocus : mix(BOS_C_TEXT, BOS_C_SURFACE, a));
}

static void itemRow(SDL_Surface* s, SettingItem* it, int x, int y, int w, int h, bool hot) {
	if (hot) {
		int gw = (int)(w * BOS_ROW_SCALE) - w, gh = (int)(h * BOS_ROW_SCALE) - h;
		x -= gw / 2, w += gw, y -= gh / 2, h += gh;
		BOS_rrect(s, x, y, w, h, BOS_px(BOS_RADIUS_ROW), BOS_theme()->focus);
	}
	float a = hot ? 1.0f : BOS_DIM;
	uint32_t fg = hot ? BOS_theme()->onfocus : BOS_C_TEXT, sub = hot ? C_ON_SUB : BOS_C_TEXT2;
	int cy = y + h / 2, lx = x + BOS_px(28), right = x + w - BOS_px(24);
	int weight = hot ? 600 : 500;
	const char* name = I18N_text(it->name);

	// the value: a switch, a slider, a colour dot, or text; a Left/Right row in focus gets ‹ › round it
	enum { V_NONE, V_TEXT, V_SLIDER, V_DOT, V_TOGGLE } k = V_NONE;
	int ts = toggleState(it), cw = 0;
	char val[160] = "";
	if (ts >= 0)
		k = V_TOGGLE, cw = BOS_px(80);
	else if (numericRange(it))
		k = V_SLIDER, cw = BOS_px(240);
	else if (it->type == ITEM_COLOR && it->values && it->current_idx >= 0 && it->current_idx < it->label_count)
		k = V_DOT, cw = BOS_px(32);
	else {
		const char* v = itemValue(it);
		if (v && v[0]) {
			snprintf(val, sizeof(val), "%s", I18N_text(v));
			k = V_TEXT, cw = BOS_textW(val, BOS_FS_SMALL, 400);
		}
	}
	bool arrows = hot && SettingsBos_isPickable(it);
	int ar = BOS_px(28), ag = BOS_px(4), arw = arrows ? 2 * (ar + ag) : 0;
	int chevw = it->type == ITEM_SUBMENU ? BOS_px(28 + 10) : 0;
	int room = right - chevw - lx; // the label's width with nothing beside it
	// the name is never cut for its value: when both don't fit on one line the value goes under the name. Decided
	// from the widest choice, the focused weight and the arrows, so the row never jumps between one and two lines
	// when its value changes or it gains focus (DESIGN §6.6)
	int widest = cw;
	if (k == V_TEXT && (it->type == ITEM_CYCLE || it->type == ITEM_COLOR) && it->labels)
		for (int i = 0; i < it->label_count; i++)
			if (it->labels[i]) {
				int lw = BOS_textW(I18N_text(it->labels[i]), BOS_FS_SMALL, 400);
				if (lw > widest)
					widest = lw;
			}
	int arw_max = SettingsBos_isPickable(it) ? 2 * (ar + ag) : 0;
	bool two = k != V_NONE && k != V_TOGGLE && BOS_textW(name, BOS_FS_BODY, 600) + BOS_px(16) + widest + arw_max > room;
	if (two && k == V_TEXT && cw + arw > room && hot)
		cw = room - arw; // the whole value, scrolling in that room
	else if (two && k == V_TEXT && cw + arw > room)
		BOS_fit(I18N_text(itemValue(it)), BOS_FS_SMALL, 400, room - arw, val, sizeof(val)), cw = BOS_textW(val, BOS_FS_SMALL, 400);
	if (two && k == V_SLIDER && cw + arw > room)
		cw = room - arw;

	if (chevw)
		BOS_icon(s, "chev", hot ? C_ON_SUB : BOS_C_TEXTOFF, a, right - BOS_px(28), cy - BOS_px(14), BOS_px(28));
	int ly = two ? y + h * 32 / 100 : cy, vy = two ? y + h * 73 / 100 : cy;
	char label[160];
	int label_room = two || k == V_NONE ? room : k == V_TOGGLE ? room - cw - BOS_px(16) : room - cw - arw - BOS_px(16);
	if (it->name_check) { // ✓ right after the name (the connected network), always in view
		int ck = BOS_px(34), gap = BOS_px(10);
		label_room -= ck + gap;
		int nw = BOS_textW(name, BOS_FS_BODY, weight);
		BOS_icon(s, "check", hot ? fg : BOS_theme()->primary, 1, lx + (nw < label_room ? nw : label_room) + gap,
				 ly - ck / 2, ck);
	}
	if (hot)
		BOS_textMarquee(s, name, BOS_FS_BODY, weight, fg, a, lx, ly, label_room);
	else {
		BOS_fit(name, BOS_FS_BODY, weight, label_room, label, sizeof(label));
		BOS_textMid(s, label, BOS_FS_BODY, weight, fg, a, lx, ly);
	}
	if (k == V_NONE)
		return;

	int vx = two ? lx : right - chevw - cw - arw;
	if (arrows) {
		BOS_icon(s, "chevl", sub, 1, vx, vy - ar / 2, ar);
		vx += ar + ag;
	}
	switch (k) {
	case V_TOGGLE:
		toggle(s, vx, vy, ts == 1, hot, a);
		break;
	case V_SLIDER:
		slider(s, vx, cw, vy, it->label_count > 1 ? (float)it->current_idx / (it->label_count - 1) : 0, hot, a);
		break;
	case V_DOT: {
		uint32_t c = (uint32_t)it->values[it->current_idx] & 0xFFFFFF;
		BOS_rrect(s, vx, vy - cw / 2, cw, cw, cw / 2, hot ? c : mix(c, BOS_C_SURFACE, a));
		break;
	}
	default:
		BOS_textMarquee(s, val, BOS_FS_SMALL, 400, sub, a, vx, vy, cw);
	}
	if (arrows)
		BOS_icon(s, "chev", sub, 1, vx + cw + ag, vy - ar / 2, ar);
}

static void rightColumn(SDL_Surface* s, SettingsPage* page, SettingItem* cat, bool focused, int top, int bottom,
						SettingItem** hot_item) {
	int rx = BOS_px(24 + 380 + 28), rw = s->w - BOS_px(32) - rx;
	char head[160];
	BOS_fit(I18N_text(pageTitle(page, cat)), BOS_FS_TITLE, 700, rw - BOS_px(8), head, sizeof(head));
	BOS_text(s, head, BOS_FS_TITLE, 700, BOS_C_TEXT, 1, rx + BOS_px(8), top);
	int list_y = top + BOS_px(74), rh = BOS_px(BOS_ROW_H);
	int foot_h = BOS_px(8) + 2 * BOS_lineH(26);
	const char* foot = NULL;
	*hot_item = NULL;
	if (!page) {
		if (cat && cat->desc && cat->desc[0])
			foot = cat->desc;
		if (foot) {
			BOS_textWrap(s, I18N_text(foot), 26, 400, BOS_C_TEXTOFF, 1, rx + BOS_px(28), list_y, rw - BOS_px(28), 2);
		}
		return;
	}
	int has_lock = page->dynamic_start >= 0;
	if (has_lock)
		pthread_rwlock_rdlock(&page->lock);
	int n = settings_page_visible_count(page);
	int rows = (bottom - list_y - foot_h) / rh;
	if (rows > n)
		rows = n;
	int sel = focused ? page->selected : -1, first = 0;
	if (focused) {
		if (page->selected < page->scroll)
			page->scroll = page->selected;
		if (rows > 0 && page->selected >= page->scroll + rows)
			page->scroll = page->selected - rows + 1;
		first = page->scroll;
	}
	if (rows > 0) {
		BOS_rrect(s, rx, list_y, rw, rows * rh, BOS_px(24), BOS_C_SURFACE);
		for (int r = 0; r < rows - 1; r++) {
			int i = first + r;
			if (i != sel && i + 1 != sel)
				SDL_FillRect(s, &(SDL_Rect){rx + BOS_px(28), list_y + (r + 1) * rh - 1, rw - BOS_px(28), 1}, BOS_map(s, C_SEP));
		}
		int hot_r = -1;
		for (int r = 0; r < rows; r++) {
			int i = first + r;
			SettingItem* it = settings_page_visible_item(page, i);
			if (!it)
				continue;
			if (it->type == ITEM_CYCLE || it->type == ITEM_COLOR)
				settings_item_sync(it);
			if (i == sel) {
				hot_r = r;
				continue; // drawn last, over its neighbours (it is scaled up)
			}
			itemRow(s, it, rx, list_y + r * rh, rw, rh, false);
		}
		if (hot_r >= 0) {
			SettingItem* it = settings_page_visible_item(page, first + hot_r);
			itemRow(s, it, rx, list_y + hot_r * rh, rw, rh, true);
			*hot_item = it;
		}
		// a scroll mark when the page has more rows than fit
		if (n > rows && rows > 0) {
			int th = rows * rh, mh = th * rows / n, my = list_y + (th - mh) * first / (n - rows);
			BOS_rrect(s, s->w - BOS_px(14), my, BOS_px(6), mh, BOS_px(3), BOS_C_TEXTOFF);
		}
		SettingItem* fi = focused ? *hot_item : cat;
		if (page->status_msg)
			foot = page->status_msg;
		else if (fi && fi->desc && fi->desc[0])
			foot = fi->desc;
	}
	if (has_lock)
		pthread_rwlock_unlock(&page->lock);
	if (foot) {
		BOS_textWrap(s, I18N_text(foot), 26, 400, BOS_C_TEXTOFF, 1, rx + BOS_px(28), list_y + rows * rh + BOS_px(8),
					 rw - BOS_px(28), 2); // two lines, so a description is read whole
	}
}

// ---- the value list (A on a Left/Right row): every value at once, a §5.7 sheet over the page ----
static SettingItem* pk_item;
static int pk_sel, pk_top;
#define PK_ROWS 6

void SettingsBos_pickerOpen(SettingItem* it) {
	if (!SettingsBos_isPickable(it))
		return;
	pk_item = it;
	pk_sel = it->current_idx >= 0 && it->current_idx < it->label_count ? it->current_idx : 0;
	pk_top = pk_sel - PK_ROWS / 2;
	if (pk_top > it->label_count - PK_ROWS)
		pk_top = it->label_count - PK_ROWS;
	if (pk_top < 0)
		pk_top = 0;
}

bool SettingsBos_pickerActive(void) {
	return pk_item != NULL;
}

void SettingsBos_pickerInput(bool* dirty) {
	if (!pk_item)
		return;
	int n = pk_item->label_count;
	if (PAD_justRepeated(BTN_UP) && pk_sel > 0)
		pk_sel--, *dirty = true;
	else if (PAD_justRepeated(BTN_DOWN) && pk_sel < n - 1)
		pk_sel++, *dirty = true;
	else if (PAD_justRepeated(BTN_L1))
		pk_sel = pk_sel > PK_ROWS ? pk_sel - PK_ROWS : 0, *dirty = true;
	else if (PAD_justRepeated(BTN_R1))
		pk_sel = pk_sel + PK_ROWS < n ? pk_sel + PK_ROWS : n - 1, *dirty = true;
	else if (PAD_justPressed(BTN_A)) {
		pk_item->current_idx = pk_sel;
		int v = pk_item->values ? pk_item->values[pk_sel] : pk_sel;
		if (pk_item->set_value)
			pk_item->set_value(v);
		settings_item_sync(pk_item); // the setter may refuse
		pk_item = NULL, *dirty = true;
	} else if (PAD_justPressed(BTN_B))
		pk_item = NULL, *dirty = true;
	if (pk_sel < pk_top)
		pk_top = pk_sel;
	if (pk_sel >= pk_top + PK_ROWS)
		pk_top = pk_sel - PK_ROWS + 1;
}

static void pickerRender(SDL_Surface* s) {
	static SDL_Surface* shade = NULL; // 60 % black over the page
	if (!shade || shade->w != s->w || shade->h != s->h) {
		if (shade)
			SDL_FreeSurface(shade);
		shade = SDL_CreateRGBSurfaceWithFormat(0, s->w, s->h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (shade) {
			SDL_FillRect(shade, NULL, SDL_MapRGBA(shade->format, 0, 0, 0, 153));
			SDL_SetSurfaceBlendMode(shade, SDL_BLENDMODE_BLEND);
		}
	}
	if (shade)
		SDL_BlitSurface(shade, NULL, s, NULL);
	int n = pk_item->label_count, rows = n < PK_ROWS ? n : PK_ROWS;
	int rh = BOS_px(84), pad = BOS_px(24), w = BOS_px(600), th = BOS_px(44 * 1.2f);
	int h = pad + th + BOS_px(12) + rows * rh + pad;
	int x = (s->w - w) / 2, y = BOS_px(BOS_BAR_H) + (s->h - 2 * BOS_px(BOS_BAR_H) - h) / 2;
	BOS_rrect(s, x, y, w, h, BOS_px(28), BOS_C_SURFACE);
	char t[160];
	BOS_fit(I18N_text(pk_item->name), 44, 700, w - 2 * pad, t, sizeof(t));
	BOS_text(s, t, 44, 700, BOS_C_TEXT, 1, x + pad, y + pad);
	int ry = y + pad + th + BOS_px(12);
	for (int r = 0; r < rows; r++) {
		int i = pk_top + r, yy = ry + r * rh, xx = x + BOS_px(12), ww = w - 2 * BOS_px(12), hh = rh;
		bool hot = i == pk_sel;
		if (hot) {
			int gw = (int)(ww * BOS_ROW_SCALE) - ww, gh = (int)(hh * BOS_ROW_SCALE) - hh;
			xx -= gw / 2, ww += gw, yy -= gh / 2, hh += gh;
			BOS_rrect(s, xx, yy, ww, hh, BOS_px(BOS_RADIUS_ROW), BOS_theme()->focus);
		}
		uint32_t fg = hot ? BOS_theme()->onfocus : BOS_C_TEXT;
		float a = hot ? 1.0f : BOS_DIM;
		int cyy = yy + hh / 2, cx = xx + ww - BOS_px(20) - BOS_px(32);
		if (i == pk_item->current_idx) // the value set now
			BOS_icon(s, "check", hot ? BOS_theme()->onfocus : BOS_theme()->primary, 1, cx, cyy - BOS_px(16), BOS_px(32));
		if (pk_item->type == ITEM_COLOR && pk_item->values) {
			int d = BOS_px(32);
			BOS_rrect(s, xx + BOS_px(20), cyy - d / 2, d, d, d / 2, (uint32_t)pk_item->values[i] & 0xFFFFFF);
		}
		int lx = xx + BOS_px(20) + (pk_item->type == ITEM_COLOR && pk_item->values ? BOS_px(48) : 0);
		char l[160];
		BOS_fit(I18N_text(pk_item->labels[i] ? pk_item->labels[i] : ""), BOS_FS_BODY, hot ? 600 : 500, cx - BOS_px(12) - lx, l,
				sizeof(l));
		if (hot)
			BOS_textMarquee(s, I18N_text(pk_item->labels[i] ? pk_item->labels[i] : ""), BOS_FS_BODY, 600, fg, a, lx, cyy,
							cx - BOS_px(12) - lx);
		else
			BOS_textMid(s, l, BOS_FS_BODY, 500, fg, a, lx, cyy);
	}
	if (n > rows) { // a scroll mark inside the sheet
		int tt = rows * rh, mh = tt * rows / n, my = ry + (tt - mh) * pk_top / (n - rows);
		BOS_rrect(s, x + w - BOS_px(10), my, BOS_px(5), mh, BOS_px(2), BOS_C_TEXTOFF);
	}
	BosHint r[] = {{"A", I18N_text("Choose")}, {"B", I18N_text("Cancel")}};
	BOS_hintBar(s, r, 2, NULL, 0);
}

void SettingsBos_render(SDL_Surface* s, SettingsPage** stack, int depth) {
	if (depth <= 0)
		return;
	SettingsPage* root = stack[0];
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	int top = BOS_px(BOS_BAR_H), bottom = s->h - BOS_px(BOS_BAR_H);
	bool right_focused = depth > 1;
	leftColumn(s, root, !right_focused, top, bottom);

	SettingItem* cat = settings_page_visible_item(root, root->selected);
	SettingsPage* page = right_focused ? stack[depth - 1] : (cat && cat->type == ITEM_SUBMENU ? cat->submenu : NULL);
	SettingItem* hot = NULL;
	rightColumn(s, page, cat, right_focused, top, bottom, &hot);

	// drawn after the columns: a scrolled category's squircle can't show through it
	BOS_statusBar(s, I18N_text("Settings"));

	// hint bar: A opens (เปิด/ปิด on a toggle, เลือก on a value row), ← → changes a value, X clears, B back
	BosHint r[3], l[1];
	int nr = 0, nl = 0;
	SettingItem* it = right_focused ? hot : cat;
	if (it) {
		bool cyc = it->type == ITEM_CYCLE || it->type == ITEM_COLOR;
		if (!cyc && it->type != ITEM_STATIC)
			r[nr++] = (BosHint){"A", I18N_text("Open")};
		else if (cyc && (it->a_cycles || toggleState(it) >= 0))
			r[nr++] = (BosHint){"A", I18N_text("On/Off")};
		else if (cyc && it->on_press)
			r[nr++] = (BosHint){"A", I18N_text("Set")};
		else if (SettingsBos_isPickable(it))
			r[nr++] = (BosHint){"A", I18N_text("Choose")};
		if (cyc && toggleState(it) < 0)
			l[nl++] = (BosHint){"\xE2\x86\x90 \xE2\x86\x92", I18N_text("Change")};
	}
	r[nr++] = (BosHint){"B", I18N_text("Back")};
	if (it && right_focused && it->on_clear)
		r[nr++] = (BosHint){"X", I18N_text("Clear")};
	BOS_hintBar(s, r, nr, l, nl);
	if (pk_item)
		pickerRender(s);
}
