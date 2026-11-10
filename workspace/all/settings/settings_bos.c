// Brick OS ตั้งค่า (DESIGN.md 6.6, board v2-SettingsIPad): the categories in iOS groups on the left (the root page),
// the highlighted category's page on the right (the page open now once one is pushed). Drawn from the shared kit
// (bos_ui.h); input, pages and items stay the menu's own.

#include "settings_bos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "api.h"
#include "bos_ui.h"
#include "defines.h"
#include "i18n.h"
#include "config.h"

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

// ---- ธีมสี (DESIGN §6.8): presets, swatches, the colour a slot has now ----
static const uint32_t PRESETS[6][2] = {{0xF2F2F7, 0x4A9BFF}, {0x64B5FF, 0x64B5FF}, {0xFF8FC0, 0xFF6FAE},
									   {0x5EE08A, 0x30D158}, {0xFFB04A, 0xFF9F0A}, {0xC79BFF, 0xBF5AF2}};
static const uint32_t SWATCHES[12] = {0xF2F2F7, 0x64B5FF, 0x5AC8FA, 0x5EE08A, 0xD4F25A, 0xFFD60A,
									  0xFFB04A, 0xFF7A6B, 0xFF8FC0, 0xC79BFF, 0x9E9EFF, 0xAC8E68};

void SettingsBos_preset(int i, uint32_t* focus, uint32_t* primary) {
	*focus = PRESETS[i][0], *primary = PRESETS[i][1];
}

static uint32_t slotColor(int slot) {
	const BosTheme* t = BOS_theme();
	return slot == THEME_FOCUS ? t->focus : slot == THEME_PRIMARY ? t->primary : t->hint_cap;
}

static uint32_t hsv(float h, float sat, float v) {
	float c = v * sat, hp = h / 60.0f, x = c * (1 - fabsf(fmodf(hp, 2) - 1)), r = 0, g = 0, b = 0;
	if (hp < 1) r = c, g = x;
	else if (hp < 2) r = x, g = c;
	else if (hp < 3) g = c, b = x;
	else if (hp < 4) g = x, b = c;
	else if (hp < 5) r = x, b = c;
	else r = c, b = x;
	float m = v - c;
	return ((uint32_t)((r + m) * 255 + 0.5f) << 16) | ((uint32_t)((g + m) * 255 + 0.5f) << 8) | (uint32_t)((b + m) * 255 + 0.5f);
}

static float hueOf(uint32_t c) {
	float r = ((c >> 16) & 255) / 255.0f, g = ((c >> 8) & 255) / 255.0f, b = (c & 255) / 255.0f;
	float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn, h = 0;
	if (d <= 0)
		return 0;
	if (mx == r) h = 60 * fmodf((g - b) / d, 6);
	else if (mx == g) h = 60 * ((b - r) / d + 2);
	else h = 60 * ((r - g) / d + 4);
	return h < 0 ? h + 360 : h;
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
	return it && !it->custom_draw && (it->type == ITEM_CYCLE || it->type == ITEM_COLOR) && it->labels && it->label_count > 1 &&
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
		BOS_rrect(s, lx, top + ry[i] - lscroll, lw, ry[j] + rh - ry[i], r, BOS_theme()->surface);
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
			BOS_rrect(s, x, yy, w, h, BOS_px(BOS_RADIUS_ROW), BOS_theme()->surface2);
		float a = hot || open ? 1.0f : BOS_DIM;
		int sq = BOS_px(56), sx = x + BOS_px(20), sy = yy + (h - sq) / 2;
		// สีไอคอน: the category's colour, white on grey, or the theme's primary
		int im = BOS_theme()->icon_mode;
		uint32_t sqc = im == 1 ? BOS_theme()->surface2 : im == 2 ? BOS_theme()->primary : c->color;
		uint32_t icc = im == 2 ? BOS_theme()->onprimary : 0xFFFFFF;
		BOS_rrect(s, sx, sy, sq, sq, BOS_px(15), mix(sqc, BOS_theme()->surface, a));
		int ic = BOS_px(36);
		BOS_icon(s, c->icon, icc, a, sx + (sq - ic) / 2, sy + (sq - ic) / 2, ic);
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
	uint32_t track = on ? BOS_theme()->primary : hot ? BOS_C_TEXTOFF : BOS_theme()->surface2;
	BOS_rrect(s, x, cy - h / 2, w, h, h / 2, hot ? track : mix(track, BOS_theme()->surface, a));
	BOS_rrect(s, on ? x + w - p - k : x + p, cy - k / 2, k, k, k / 2, hot ? 0xFFFFFF : mix(0xFFFFFF, BOS_theme()->surface, a));
}

static void slider(SDL_Surface* s, int x, int w, int cy, float frac, bool hot, float a) {
	int h = BOS_px(12);
	BOS_rrect(s, x, cy - h / 2, w, h, h / 2, hot ? C_SLIDER_ON : mix(BOS_theme()->surface2, BOS_theme()->surface, a));
	int fw = (int)(w * frac);
	if (fw > 0)
		BOS_rrect(s, x, cy - h / 2, fw < h ? h : fw, h, h / 2, hot ? BOS_theme()->onfocus : mix(BOS_C_TEXT, BOS_theme()->surface, a));
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
	uint32_t dotc = 0;
	char val[160] = "";
	if (it->color_slot) // ธีมสี: the colour set now
		k = V_DOT, cw = BOS_px(32), dotc = slotColor(it->color_slot - 1);
	else if (ts >= 0)
		k = V_TOGGLE, cw = BOS_px(80);
	else if (numericRange(it))
		k = V_SLIDER, cw = BOS_px(240);
	else if (it->type == ITEM_COLOR && it->values && it->current_idx >= 0 && it->current_idx < it->label_count)
		k = V_DOT, cw = BOS_px(32), dotc = (uint32_t)it->values[it->current_idx] & 0xFFFFFF;
	else {
		const char* v = itemValue(it);
		if (v && v[0]) {
			snprintf(val, sizeof(val), "%s", I18N_text(v));
			k = V_TEXT, cw = BOS_textW(val, BOS_FS_SMALL, 400);
		}
	}
	bool arrows = hot && SettingsBos_isPickable(it);
	int ar = BOS_px(28), ag = BOS_px(4), arw = arrows ? 2 * (ar + ag) : 0;
	int chevw = it->type == ITEM_SUBMENU || it->color_slot ? BOS_px(28 + 10) : 0;
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
		// a dot with a faint ring, so a dark or light colour still shows on either row
		BOS_rrect(s, vx - 2, vy - cw / 2 - 2, cw + 4, cw + 4, cw / 2 + 2, hot ? C_ON_SUB : BOS_C_TEXTOFF);
		BOS_rrect(s, vx, vy - cw / 2, cw, cw, cw / 2, hot ? dotc : mix(dotc, BOS_theme()->surface, a));
		break;
	}
	default:
		BOS_textMarquee(s, val, BOS_FS_SMALL, 400, sub, a, vx, vy, cw);
	}
	if (arrows)
		BOS_icon(s, "chev", sub, 1, vx + cw + ag, vy - ar / 2, ar);
}

// a page laid out in px: free blocks (custom_draw, outside any group) and groups of rows, each group may have a
// small header; scrolled in px so the focused item stays in view. Returns the bottom of what was drawn.
#define FREE_MAX 32
static int freeColumn(SDL_Surface* s, SettingsPage* page, bool focused, int rx, int rw, int list_y, int view_bottom,
					  SettingItem** hot_item) {
	static SettingsPage* sc_page;
	static int sc;
	int n = settings_page_visible_count(page), ys[FREE_MAX], hs[FREE_MAX], y = 0, head = BOS_px(44);
	if (n > FREE_MAX)
		n = FREE_MAX;
	for (int i = 0; i < n; i++) {
		SettingItem* it = settings_page_visible_item(page, i);
		if (it->header)
			y += (i ? BOS_px(22) : 0) + head;
		ys[i] = y, hs[i] = it->height > 0 ? BOS_px(it->height) : BOS_px(BOS_ROW_H);
		y += hs[i];
	}
	int total = y, view = view_bottom - list_y, sel = focused ? page->selected : -1;
	if (sc_page != page)
		sc_page = page, sc = 0;
	if (sel >= 0 && sel < n) {
		int t = ys[sel] - (settings_page_visible_item(page, sel)->header ? head : 0) - BOS_px(8);
		if (t < sc)
			sc = t;
		if (ys[sel] + hs[sel] + BOS_px(8) - sc > view)
			sc = ys[sel] + hs[sel] + BOS_px(8) - view;
	}
	if (sc > total - view)
		sc = total - view;
	if (sc < 0)
		sc = 0;
	SDL_Rect clip = {rx - BOS_px(24), list_y, rw + BOS_px(48), view};
	SDL_SetClipRect(s, &clip);
	// the groups: runs of normal rows, broken by a free block or a header
	for (int i = 0; i < n;) {
		SettingItem* it = settings_page_visible_item(page, i);
		if (it->height > 0) {
			i++;
			continue;
		}
		int j = i;
		while (j + 1 < n && settings_page_visible_item(page, j + 1)->height <= 0 && !settings_page_visible_item(page, j + 1)->header)
			j++;
		BOS_rrect(s, rx, list_y + ys[i] - sc, rw, ys[j] + hs[j] - ys[i], BOS_px(BOS_RADIUS_GROUP), BOS_theme()->surface);
		for (int k = i; k < j; k++)
			if (k != sel && k + 1 != sel)
				SDL_FillRect(s, &(SDL_Rect){rx + BOS_px(28), list_y + ys[k + 1] - sc - 1, rw - BOS_px(28), 1}, BOS_map(s, C_SEP));
		i = j + 1;
	}
	for (int i = 0; i < n; i++) {
		SettingItem* it = settings_page_visible_item(page, i);
		if (it->header)
			BOS_textMid(s, I18N_text(it->header), 26, 600, BOS_C_TEXT2, 1, rx + BOS_px(28), list_y + ys[i] - sc - head / 2);
		if (it->type == ITEM_CYCLE || it->type == ITEM_COLOR)
			settings_item_sync(it);
		if (it->height > 0 && it->custom_draw)
			it->custom_draw(s, it, rx, list_y + ys[i] - sc, rw, hs[i], i == sel);
		else if (i != sel)
			itemRow(s, it, rx, list_y + ys[i] - sc, rw, hs[i], false);
	}
	if (sel >= 0 && sel < n) {
		SettingItem* it = settings_page_visible_item(page, sel);
		if (it->height <= 0)
			itemRow(s, it, rx, list_y + ys[sel] - sc, rw, hs[sel], true);
		*hot_item = it;
	}
	SDL_SetClipRect(s, NULL);
	// rounded fills ignore the clip: wipe what spilled above and below the view (the title is drawn after)
	uint32_t bg = BOS_map(s, BOS_theme()->bg);
	SDL_FillRect(s, &(SDL_Rect){rx - BOS_px(24), BOS_px(BOS_BAR_H), rw + BOS_px(48), list_y - BOS_px(BOS_BAR_H)}, bg);
	SDL_FillRect(s, &(SDL_Rect){rx - BOS_px(24), view_bottom, rw + BOS_px(48), s->h - BOS_px(BOS_BAR_H) - view_bottom}, bg);
	if (total > view) { // a scroll mark
		int mh = view * view / total, my = list_y + (view - mh) * sc / (total - view);
		BOS_rrect(s, s->w - BOS_px(14), my, BOS_px(6), mh, BOS_px(3), BOS_C_TEXTOFF);
	}
	int end = list_y + total - sc;
	return end < view_bottom ? end : view_bottom;
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
	bool free_layout = false;
	for (int i = 0; i < n; i++) {
		SettingItem* it = settings_page_visible_item(page, i);
		if (it && (it->height > 0 || it->header))
			free_layout = true;
	}
	if (free_layout) { // a page with free blocks and group headers (ธีมสี): laid out in px, scrolled in px
		int end = freeColumn(s, page, focused, rx, rw, list_y, bottom - foot_h, hot_item);
		BOS_text(s, head, BOS_FS_TITLE, 700, BOS_C_TEXT, 1, rx + BOS_px(8), top);
		SettingItem* fi = focused ? *hot_item : NULL;
		foot = fi && fi->desc && fi->desc[0] ? fi->desc : NULL;
		if (has_lock)
			pthread_rwlock_unlock(&page->lock);
		if (foot)
			BOS_textWrap(s, I18N_text(foot), 26, 400, BOS_C_TEXTOFF, 1, rx + BOS_px(28), end + BOS_px(8), rw - BOS_px(28), 2);
		return;
	}
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
		BOS_rrect(s, rx, list_y, rw, rows * rh, BOS_px(24), BOS_theme()->surface);
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


// ---- ธีมสี › ธีมสำเร็จรูป: six mini screens (128×92) with their names, 3 a row; Left/Right picks one ----
void SettingsBos_themeCards(SDL_Surface* s, SettingItem* it, int x, int y, int w, int h, int hot) {
	(void)h;
	int cur = it->get_value ? it->get_value() : -1; // -1: a custom mix, no card has the check
	int cw = BOS_px(128), ch = BOS_px(92), name_h = BOS_px(14) + BOS_lineH(26), gap_y = BOS_px(14);
	int pad = BOS_px(20), gap_x = (w - 2 * pad - 3 * cw) / 2;
	uint32_t bg = BOS_theme()->bg;
	for (int i = 0; i < 6; i++) {
		int cx = x + pad + (i % 3) * (cw + gap_x), cy = y + BOS_px(10) + (i / 3) * (ch + name_h + gap_y);
		bool focus = hot && i == cur;
		float a = focus || (!hot && i == cur) ? 1.0f : BOS_DIM;
		if (focus) { // the picture ring (§4): 4 px, 4 px off the card
			int o = BOS_px(BOS_RING) * 2;
			BOS_rrect(s, cx - o, cy - o, cw + 2 * o, ch + 2 * o, BOS_px(16) + o, BOS_theme()->focus);
			o = BOS_px(BOS_RING);
			BOS_rrect(s, cx - o, cy - o, cw + 2 * o, ch + 2 * o, BOS_px(16) + o, bg);
		}
		BOS_rrect(s, cx, cy, cw, ch, BOS_px(16), mix(0x38383A, bg, a));
		BOS_rrect(s, cx + 2, cy + 2, cw - 4, ch - 4, BOS_px(14), BOS_C_BG);
		int ix = cx + BOS_px(12), iw = cw - 2 * BOS_px(12);
		BOS_rrect(s, ix, cy + BOS_px(12), iw, BOS_px(22), BOS_px(8), mix(PRESETS[i][0], BOS_C_BG, a));
		int ry = cy + BOS_px(12 + 22 + 8) + BOS_px(10);
		BOS_rrect(s, ix, ry - BOS_px(4), BOS_px(50), BOS_px(8), BOS_px(4), mix(0x48484A, BOS_C_BG, a));
		int tw = BOS_px(34), th = BOS_px(20);
		BOS_rrect(s, ix + iw - tw, ry - th / 2, tw, th, th / 2, mix(PRESETS[i][1], BOS_C_BG, a));
		BOS_rrect(s, ix + iw - BOS_px(18), ry - BOS_px(8), BOS_px(16), BOS_px(16), BOS_px(8), mix(0xFFFFFF, BOS_C_BG, a));
		if (i == cur) { // the theme in use: a primary check badge on the corner
			int b = BOS_px(36);
			BOS_rrect(s, cx + cw - b + BOS_px(8), cy - BOS_px(8), b, b, b / 2, BOS_theme()->primary);
			BOS_icon(s, "check", BOS_theme()->onprimary, 1, cx + cw - b + BOS_px(14), cy - BOS_px(2), BOS_px(24));
		}
		const char* nm = I18N_text(it->labels[i]);
		int nw = BOS_textW(nm, 26, focus ? 600 : 500);
		BOS_text(s, nm, 26, focus ? 600 : 500, focus || i == cur ? BOS_C_TEXT : BOS_C_TEXT2, 1, cx + (cw - nw) / 2,
				 cy + ch + BOS_px(14));
	}
}

// ---- the colour sheet (§6.8): 12 swatches, a custom hue, the text colour on it, a preview ----
static int cs_slot = -1, cs_row, cs_col, cs_text;
static uint32_t cs_color, cs_was;
static float cs_hue;
static const char* cs_title;
#define CS_SAT 0.55f // the hue bar's colours: as soft as the swatches

void SettingsBos_colorSheetOpen(int slot, const char* title) {
	cs_slot = slot, cs_title = title;
	cs_color = cs_was = slotColor(slot);
	cs_text = CFG_getTheme(slot + 1);
	cs_hue = hueOf(cs_color);
	cs_row = 2, cs_col = 0;
	for (int i = 0; i < 12; i++)
		if (SWATCHES[i] == cs_color)
			cs_row = i / 6, cs_col = i % 6;
}

static void sheetInput(bool* dirty) {
	if (PAD_justRepeated(BTN_UP) && cs_row > 0) {
		cs_row--, *dirty = true;
		if (cs_row < 2)
			cs_color = SWATCHES[cs_row * 6 + cs_col];
	} else if (PAD_justRepeated(BTN_DOWN) && cs_row < 3) {
		cs_row++, *dirty = true;
		if (cs_row < 2)
			cs_color = SWATCHES[cs_row * 6 + cs_col];
	} else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT)) {
		int d = PAD_justRepeated(BTN_RIGHT) ? 1 : -1;
		if (cs_row < 2) {
			cs_col = (cs_col + d + 6) % 6;
			cs_color = SWATCHES[cs_row * 6 + cs_col];
		} else if (cs_row == 2) {
			cs_hue = fmodf(cs_hue + d * 6 + 360, 360);
			cs_color = hsv(cs_hue, CS_SAT, 1);
		} else
			cs_text = (cs_text + d + 3) % 3;
		*dirty = true;
	} else if (PAD_justPressed(BTN_A)) {
		CFG_setTheme(cs_slot, (int)cs_color);
		CFG_setTheme(cs_slot + 1, cs_text);
		cs_slot = -1, *dirty = true;
	} else if (PAD_justPressed(BTN_B))
		cs_slot = -1, *dirty = true;
}

static void sheetRender(SDL_Surface* s) {
	int bar = BOS_px(BOS_BAR_H), sh = BOS_px(590), top = s->h - bar - sh, px = BOS_px(48);
	BOS_rrect(s, 0, top, s->w, sh + bar, BOS_px(32), BOS_theme()->surface);
	int y = top + BOS_px(24);
	BOS_text(s, I18N_text(cs_title), 44, 700, BOS_C_TEXT, 1, px, y);
	y += BOS_lineH(44) + BOS_px(16);
	// swatches: the one in use has a check, the focused one a ring and 1.08
	int d = BOS_px(92), gx = (s->w - 2 * px - 6 * d) / 5;
	for (int i = 0; i < 12; i++) {
		int cx = px + (i % 6) * (d + gx) + d / 2, cy = y + (i / 6) * (d + BOS_px(20)) + d / 2;
		bool focus = cs_row == i / 6 && cs_col == i % 6;
		int dd = focus ? (int)(d * 1.08f) : d;
		if (focus) {
			int o = BOS_px(5) + BOS_px(4);
			BOS_rrect(s, cx - dd / 2 - o, cy - dd / 2 - o, dd + 2 * o, dd + 2 * o, dd / 2 + o, 0xFFFFFF);
			o = BOS_px(5);
			BOS_rrect(s, cx - dd / 2 - o, cy - dd / 2 - o, dd + 2 * o, dd + 2 * o, dd / 2 + o, BOS_theme()->surface);
		}
		BOS_rrect(s, cx - dd / 2, cy - dd / 2, dd, dd, dd / 2, SWATCHES[i]);
		if (SWATCHES[i] == cs_was) {
			int ic = BOS_px(44);
			BOS_icon(s, "check", BOS_textOn(SWATCHES[i], 0), 1, cx - ic / 2, cy - ic / 2, ic);
		}
	}
	y += 2 * d + BOS_px(20) + BOS_px(24);
	// กำหนดเอง: a hue bar with a knob; Left/Right moves it
	int lw = BOS_px(170), bx = px + lw, bw = s->w - px - bx, cy = y + BOS_px(18);
	BOS_textMid(s, I18N_text("Custom"), BOS_FS_SMALL, cs_row == 2 ? 600 : 400, cs_row == 2 ? BOS_C_TEXT : BOS_C_TEXT2, 1, px, cy);
	for (int i = 0; i < bw; i++)
		SDL_FillRect(s, &(SDL_Rect){bx + i, cy - BOS_px(10), 1, BOS_px(20)}, BOS_map(s, hsv(360.0f * i / bw, CS_SAT, 1)));
	int kx = bx + (int)(bw * cs_hue / 360.0f), k = BOS_px(cs_row == 2 ? 44 : 36);
	BOS_rrect(s, kx - k / 2, cy - k / 2, k, k, k / 2, BOS_C_BG);
	BOS_rrect(s, kx - k / 2 + BOS_px(3), cy - k / 2 + BOS_px(3), k - BOS_px(6), k - BOS_px(6), k / 2, 0xFFFFFF);
	y += BOS_px(36) + BOS_px(20);
	// ตัวหนังสือ: อัตโนมัติ / ขาว / ดำ
	cy = y + BOS_px(30);
	BOS_textMid(s, I18N_text("Text"), BOS_FS_SMALL, cs_row == 3 ? 600 : 400, cs_row == 3 ? BOS_C_TEXT : BOS_C_TEXT2, 1, px, cy);
	int segh = BOS_px(60);
	if (cs_row == 3)
		BOS_rrect(s, bx - BOS_px(4), cy - segh / 2 - BOS_px(4), bw + BOS_px(8), segh + BOS_px(8), BOS_px(22), BOS_theme()->focus);
	BOS_rrect(s, bx, cy - segh / 2, bw, segh, BOS_px(18), BOS_theme()->surface2);
	static const char* TXT[] = {"Auto", "White", "Black"};
	int sw = (bw - BOS_px(8) - 2 * BOS_px(4)) / 3;
	for (int i = 0; i < 3; i++) {
		int sx = bx + BOS_px(4) + i * (sw + BOS_px(4));
		bool on = i == cs_text;
		if (on)
			BOS_rrect(s, sx, cy - BOS_px(26), sw, BOS_px(52), BOS_px(14), BOS_theme()->focus);
		const char* t = I18N_text(TXT[i]);
		int tw = BOS_textW(t, BOS_FS_SMALL, on ? 600 : 400);
		BOS_textMid(s, t, BOS_FS_SMALL, on ? 600 : 400, on ? BOS_theme()->onfocus : BOS_C_TEXT, 1, sx + (sw - tw) / 2, cy);
	}
	y += segh + BOS_px(20);
	// the preview: a focused row in this colour and text colour, and whether it reads well (4.5:1)
	uint32_t fg = BOS_textOn(cs_color, cs_text);
	bool easy = BOS_contrast(cs_color, fg) >= 4.5;
	int ph = BOS_px(84);
	BOS_rrect(s, px, y, s->w - 2 * px, ph, BOS_px(20), cs_color);
	BOS_textMid(s, I18N_text("Preview of a selected row"), BOS_FS_BODY, 600, fg, 1, px + BOS_px(24), y + ph / 2);
	const char* verdict = I18N_text(easy ? "Easy to read" : "Hard to read");
	int vw = BOS_textW(verdict, 26, 600), ic = BOS_px(28), pw = vw + BOS_px(24) + (easy ? ic + BOS_px(6) : 0);
	int vx = s->w - px - BOS_px(16) - pw, vh = BOS_px(44);
	BOS_rrect(s, vx, y + (ph - vh) / 2, pw, vh, vh / 2, BOS_C_BG); // a dark pill: the verdict reads on any colour
	BOS_textMid(s, verdict, 26, 600, easy ? BOS_C_OK : BOS_C_WARN, 1, vx + BOS_px(12), y + ph / 2);
	if (easy)
		BOS_icon(s, "check", BOS_C_OK, 1, vx + BOS_px(12) + vw + BOS_px(6), y + (ph - ic) / 2, ic);
	BosHint r[] = {{"A", I18N_text("Use this colour")}, {"B", I18N_text("Cancel")}};
	BosHint l[] = {{"\xE2\x86\x90 \xE2\x86\x92", I18N_text("Change")}};
	BOS_hintBar(s, r, 2, l, 1);
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
	return pk_item != NULL || cs_slot >= 0;
}

void SettingsBos_pickerInput(bool* dirty) {
	if (cs_slot >= 0) {
		sheetInput(dirty);
		return;
	}
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
	int n = pk_item->label_count, rows = n < PK_ROWS ? n : PK_ROWS;
	int rh = BOS_px(84), pad = BOS_px(24), w = BOS_px(600), th = BOS_px(44 * 1.2f);
	int h = pad + th + BOS_px(12) + rows * rh + pad;
	int x = (s->w - w) / 2, y = BOS_px(BOS_BAR_H) + (s->h - 2 * BOS_px(BOS_BAR_H) - h) / 2;
	BOS_rrect(s, x, y, w, h, BOS_px(28), BOS_theme()->surface);
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
		if (it->color_slot)
			r[nr++] = (BosHint){"A", I18N_text("Choose")};
		else if (it->custom_draw && cyc)
			; // the theme cards: Left/Right picks, nothing to open
		else if (!cyc && it->type != ITEM_STATIC)
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
		r[nr++] = (BosHint){"X", I18N_text(it->clear_label ? it->clear_label : "Clear")};
	BOS_hintBar(s, r, nr, l, nl);
	if (pk_item || cs_slot >= 0) { // a sheet over the page, which dims to 60 %
		static SDL_Surface* shade = NULL;
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
		if (cs_slot >= 0)
			sheetRender(s);
		else
			pickerRender(s);
	}
}
