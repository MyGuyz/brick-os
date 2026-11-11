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
#include "bos_keys.h"
#include <unistd.h>

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
	{"Updates", "update", 0x0A84FF, 6},
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
static const uint32_t SWATCHES[12] = {0xF2F2F7, 0x64B5FF, 0x5AC8FA, 0x5EE08A, 0xD4F25A, 0xFFD60A,
									  0xFFB04A, 0xFF7A6B, 0xFF8FC0, 0xC79BFF, 0x9E9EFF, 0xAC8E68};

void SettingsBos_preset(int i, uint32_t* focus, uint32_t* primary) {
	BOS_preset(i, focus, primary);
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
	if (it->type == ITEM_BUTTON || it->type == ITEM_SUBMENU) // a page row's value ("6 แอป", a console's emulator)
		return it->get_display ? it->get_display() : it->display_text;
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

// a key-cap value (DESIGN §6.9): "MENU+R1" -> caps in surface2 joined by "+"; "MENU|Hold" adds a word after the
// caps; the D-pad names show as arrows. Returns the width; draws only when s is set
static const char* capText(const char* k) {
	if (!strcmp(k, "UP"))
		return "\xE2\x86\x91";
	if (!strcmp(k, "DOWN"))
		return "\xE2\x86\x93";
	if (!strcmp(k, "LEFT"))
		return "\xE2\x86\x90";
	if (!strcmp(k, "RIGHT"))
		return "\xE2\x86\x92";
	return k;
}
static int keyCaps(SDL_Surface* s, const char* v, int x, int cy, float a, uint32_t sub) {
	char buf[64];
	snprintf(buf, sizeof(buf), "%s", v);
	char* tail = strchr(buf, '|');
	if (tail)
		*tail++ = 0;
	int ch = BOS_px(40), pad = BOS_px(12), gap = BOS_px(6), x0 = x;
	uint32_t cap = BOS_theme()->surface2;
	char* save = NULL;
	bool first = true;
	for (char* k = strtok_r(buf, "+", &save); k; k = strtok_r(NULL, "+", &save)) {
		if (!first) {
			x += gap;
			if (s)
				BOS_textMid(s, "+", 24, 500, sub, a, x, cy);
			x += BOS_textW("+", 24, 500) + gap;
		}
		first = false;
		const char* t = capText(k);
		bool round = strlen(k) == 1 || t != k; // A B X Y and the arrows: a circle
		int tw = BOS_textW(t, 22, 700), w = round ? ch : tw + 2 * pad;
		if (w < ch)
			w = ch;
		if (s) {
			BOS_rrect(s, x, cy - ch / 2, w, ch, round ? ch / 2 : BOS_px(12), mix(cap, BOS_theme()->surface, a));
			BOS_textMid(s, t, 22, 700, BOS_C_TEXT, a, x + (w - tw) / 2, cy);
		}
		x += w;
	}
	if (tail && tail[0]) {
		const char* t = I18N_text(tail);
		x += BOS_px(10);
		if (s)
			BOS_textMid(s, t, 26, 400, sub, a, x, cy);
		x += BOS_textW(t, 26, 400);
	}
	return x - x0;
}

static void keyText(int key, int hold, char* out, size_t size);
static int lt_want; // the lights this frame wants: 0 the profile, 1 the LED pages' preview, 2 + key: that key blinks
static void lightsApply(void);
static void keyRowDraw(SDL_Surface* s, const char* name, const char* value, bool tag, int mark, int x, int y, int w,
					   int h, bool hot);
static void itemRow(SDL_Surface* s, SettingItem* it, int x, int y, int w, int h, bool hot) {
	if (it->keybtn) {
		if (hot)
			lt_want = 2 + it->keybtn - 1;
		char v[160], hv[8];
		keyText(it->keybtn - 1, 0, v, sizeof(v));
		BosKeys_value(it->keybtn - 1, 1, hv, sizeof(hv));
		keyRowDraw(s, I18N_text(it->name), v, hv[0] != '\0', 1, x, y, w, h, hot);
		return;
	}
	if (hot) {
		int gw = (int)(w * BOS_ROW_SCALE) - w, gh = (int)(h * BOS_ROW_SCALE) - h;
		x -= gw / 2, w += gw, y -= gh / 2, h += gh;
		if (it->lifted) // จัดเรียง: picked up, as Home's (DESIGN §6.1)
			y -= BOS_px(12);
		BOS_rrect(s, x, y, w, h, BOS_px(BOS_RADIUS_ROW), BOS_theme()->focus);
	}
	float a = hot ? 1.0f : BOS_DIM;
	uint32_t fg = hot ? BOS_theme()->onfocus : BOS_C_TEXT, sub = hot ? C_ON_SUB : BOS_C_TEXT2;
	int cy = y + h / 2, lx = x + BOS_px(28), right = x + w - BOS_px(24);
	int weight = hot ? 600 : 500;
	const char* name = I18N_text(it->name);
	if (it->lead_icon) { // an app's tile: its icon on surface2
		int t = BOS_px(60), ic = BOS_px(44);
		lx = x + BOS_px(20);
		BOS_rrect(s, lx, cy - t / 2, t, t, BOS_px(16), hot ? BOS_theme()->surface2 : mix(BOS_theme()->surface2, BOS_theme()->surface, a));
		BOS_icon(s, it->lead_icon, it->lead_color, hot ? 1 : a, lx + (t - ic) / 2, cy - ic / 2, ic);
		lx += t + BOS_px(22);
	}
	if (it->lifted && hot) { // ▲▼ in place of the value
		int ar = BOS_px(30);
		BOS_icon(s, "chevu", sub, 1, right - ar, cy - ar - BOS_px(1), ar);
		BOS_icon(s, "chevd", sub, 1, right - ar, cy + BOS_px(1), ar);
		int nw = it->note[0] ? BOS_textW(it->note, BOS_FS_SMALL, 400) : 0;
		if (nw)
			BOS_textMid(s, it->note, BOS_FS_SMALL, 400, sub, 1, right - ar - BOS_px(18) - nw, cy);
		BOS_textMarquee(s, name, BOS_FS_BODY, weight, fg, a, lx, cy, right - ar - BOS_px(40) - nw - lx);
		return;
	}

	// the value: a switch, a slider, a colour dot, or text; a Left/Right row in focus gets ‹ › round it
	enum { V_NONE, V_TEXT, V_SLIDER, V_DOT, V_TOGGLE, V_CAPS } k = V_NONE;
	int ts = toggleState(it), cw = 0;
	uint32_t dotc = 0;
	char val[160] = "";
	if (it->color_slot) // ธีมสี: the colour set now
		k = V_DOT, cw = BOS_px(32), dotc = slotColor(it->color_slot - 1);
	else if (it->dot_color) { // ไฟ LED: the effect (if given), then the colour
		k = V_DOT, cw = BOS_px(32), dotc = it->dot_color() & 0xFFFFFF;
		if (it->get_display && it->get_display()[0]) {
			snprintf(val, sizeof(val), "%s", I18N_text(it->get_display()));
			cw += BOS_textW(val, BOS_FS_SMALL, 400) + BOS_px(14);
		}
	}
	else if (it->keycaps) { // ปุ่มลัด: the buttons, or "ไม่ได้ตั้ง"
		const char* v = itemValue(it);
		if (!v || !v[0] || !strcmp(v, "NONE"))
			snprintf(val, sizeof(val), "%s", I18N_text("Not set")), k = V_TEXT, cw = BOS_textW(val, BOS_FS_SMALL, 400);
		else
			snprintf(val, sizeof(val), "%s", v), k = V_CAPS, cw = keyCaps(NULL, val, 0, 0, 1, 0);
	}
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
	bool chev = it->type == ITEM_SUBMENU || it->color_slot || it->dot_color || it->chevron;
	int chevw = chev ? BOS_px(28 + 10) : 0;
	if (it->pill && it->pill[0]) { // a primary pill before the chevron (อัปเดต's "ใหม่")
		const char* pt = I18N_text(it->pill);
		int ph = BOS_px(34), pw = BOS_textW(pt, 24, 700) + BOS_px(28);
		BOS_rrect(s, right - chevw - pw, cy - ph / 2, pw, ph, ph / 2, BOS_theme()->primary);
		BOS_textMid(s, pt, 24, 700, BOS_theme()->onprimary, 1, right - chevw - pw + BOS_px(14), cy);
		chevw += pw + BOS_px(12);
	}
	// a small text before the value (a pin's place), kept with the value
	int notew = it->note[0] ? BOS_textW(it->note, BOS_FS_SMALL, 400) + BOS_px(18) : 0;
	int room = right - chevw - notew - lx; // the label's width with nothing beside it
	// the name is never cut for its value: when both don't fit on one line the value goes under the name. Decided
	// from the widest choice, the focused weight and the arrows, so the row never jumps between one and two lines
	// when its value changes or it gains focus (DESIGN §6.6)
	int widest = cw;
	if (it->keycaps) { // the widest binding there is
		int ww = keyCaps(NULL, "MENU+SELECT", 0, 0, 1, 0);
		if (ww > widest)
			widest = ww;
	} else if (k == V_TEXT && (it->type == ITEM_CYCLE || it->type == ITEM_COLOR) && it->labels)
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

	if (chev)
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
	if (notew && !two)
		BOS_textMid(s, it->note, BOS_FS_SMALL, 400, sub, a, vx - notew, vy);
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
	case V_CAPS:
		keyCaps(s, val, vx, vy, a, sub);
		break;
	case V_DOT: {
		if (val[0]) { // ไฟ LED: the effect's name before the dot
			int tw = BOS_textW(val, BOS_FS_SMALL, 400);
			BOS_textMid(s, val, BOS_FS_SMALL, 400, sub, a, vx, vy);
			vx += tw + BOS_px(14), cw = BOS_px(32);
		}
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
		// no description line here: the page scrolls, and a line under the view sat on the rows (DESIGN §6.8)
		freeColumn(s, page, focused, rx, rw, list_y, bottom, hot_item);
		BOS_text(s, head, BOS_FS_TITLE, 700, BOS_C_TEXT, 1, rx + BOS_px(8), top);
		if (has_lock)
			pthread_rwlock_unlock(&page->lock);
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


// ---- ธีมสี › ธีมสำเร็จรูป: six mini screens (128×92) with their names, 3 a row. The D-pad moves the ring
// (it->cursor), A uses the card; the card in use has the check, none while กำหนดเอง is on ----
void SettingsBos_themeCards(SDL_Surface* s, SettingItem* it, int x, int y, int w, int h, int hot) {
	(void)h;
	int cur = CFG_getTheme(THEME_CUSTOM) ? -1 : CFG_getTheme(THEME_PRESET);
	int cw = BOS_px(128), ch = BOS_px(92), name_h = BOS_px(14) + BOS_lineH(26), gap_y = BOS_px(14);
	int pad = BOS_px(20), gap_x = (w - 2 * pad - 3 * cw) / 2;
	uint32_t bg = BOS_theme()->bg;
	for (int i = 0; i < 6; i++) {
		int cx = x + pad + (i % 3) * (cw + gap_x), cy = y + BOS_px(10) + (i / 3) * (ch + name_h + gap_y);
		bool focus = hot && i == it->cursor;
		float a = focus || i == cur ? 1.0f : BOS_DIM;
		uint32_t pf, pp;
		BOS_preset(i, &pf, &pp);
		if (focus) { // the picture ring (§4): 4 px, 4 px off the card
			int o = BOS_px(BOS_RING) * 2;
			BOS_rrect(s, cx - o, cy - o, cw + 2 * o, ch + 2 * o, BOS_px(16) + o, BOS_theme()->focus);
			o = BOS_px(BOS_RING);
			BOS_rrect(s, cx - o, cy - o, cw + 2 * o, ch + 2 * o, BOS_px(16) + o, bg);
		}
		BOS_rrect(s, cx, cy, cw, ch, BOS_px(16), mix(0x38383A, bg, a));
		BOS_rrect(s, cx + 2, cy + 2, cw - 4, ch - 4, BOS_px(14), BOS_C_BG);
		int ix = cx + BOS_px(12), iw = cw - 2 * BOS_px(12);
		BOS_rrect(s, ix, cy + BOS_px(12), iw, BOS_px(22), BOS_px(8), mix(pf, BOS_C_BG, a));
		int ry = cy + BOS_px(12 + 22 + 8) + BOS_px(10);
		BOS_rrect(s, ix, ry - BOS_px(4), BOS_px(50), BOS_px(8), BOS_px(4), mix(0x48484A, BOS_C_BG, a));
		int tw = BOS_px(34), th = BOS_px(20);
		BOS_rrect(s, ix + iw - tw, ry - th / 2, tw, th, th / 2, mix(pp, BOS_C_BG, a));
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

// ---- the colour sheet (§6.8 v3): two tabs, สีพื้น | สีตัวหนังสือ (L1/R1); 12 swatches, a custom hue, the colour code
// (#RRGGBB, a digit at a time) and a preview with the readability verdict. A saves both tabs, B drops both ----
static const uint32_t TEXT_SWATCHES[12] = {0, 0xFFFFFF, 0x0B0B0D, 0x3A3A3E, 0x0A2A66, 0x0F3D22,
										   0x5C1020, 0x34145C, 0xFFD60A, 0x64B5FF, 0xFF8FC0, 0x5EE08A}; // [0] อัตโนมัติ
static int cs_slot = -1, cs_tab, cs_row, cs_col, cs_text, cs_text_was, cs_edit, cs_digit, cs_code_text;
static uint32_t cs_color, cs_was, cs_code_was;
static float cs_hue;
static const char* cs_title;
#define CS_SAT (cs_led ? 1.0f : 0.55f) // the hue bar's colours: as soft as the swatches; an LED's are pure
// ไฟ LED (DESIGN §6.10): the same sheet with no tabs and pure colours; the light follows the colour live
static int cs_led;
static uint32_t cs_live_at;
static void (*cs_live)(uint32_t), (*cs_set)(uint32_t);
static const uint32_t LED_SWATCHES[12] = {0xFFFFFF, 0xFF0000, 0xFF5A00, 0xFFB000, 0xFFFF00, 0x80FF00,
										   0x00FF40, 0x00FFC0, 0x00C0FF, 0x0040FF, 0x8000FF, 0xFF4FA0};
#define BG_SW (cs_led ? LED_SWATCHES : SWATCHES)

static int textValue(int v) { // stored text colour -> 0 auto or THEME_TEXT_RGB|rgb (old configs: 1 white, 2 black)
	if (v == 1)
		return (int)(THEME_TEXT_RGB | 0xFFFFFF);
	if (v == 2)
		return (int)(THEME_TEXT_RGB | BOS_C_ONFOCUS);
	return v;
}
static uint32_t tabColor(void) { // the colour the tab edits now (auto text: the colour auto picks)
	return cs_tab ? BOS_textOn(cs_color, cs_text) : cs_color;
}
static void setTabColor(uint32_t c) {
	if (cs_tab)
		cs_text = (int)(THEME_TEXT_RGB | (c & 0xFFFFFF));
	else
		cs_color = c & 0xFFFFFF;
}
static bool swatchIs(int i, bool was) { // the swatch holds the tab's value (now, or when the sheet opened)
	if (cs_tab) {
		int v = was ? cs_text_was : cs_text;
		return i == 0 ? v == 0 : v == (int)(THEME_TEXT_RGB | TEXT_SWATCHES[i]);
	}
	return BG_SW[i] == (was ? cs_was : cs_color);
}
static void pickSwatch(void) {
	int i = cs_row * 6 + cs_col;
	if (cs_tab)
		cs_text = i == 0 ? 0 : (int)(THEME_TEXT_RGB | TEXT_SWATCHES[i]);
	else
		cs_color = BG_SW[i];
	cs_hue = hueOf(tabColor());
}
static void seekTab(void) { // land on the swatch holding the tab's value, else on กำหนดเอง
	cs_row = 2, cs_col = 0;
	for (int i = 0; i < 12; i++)
		if (swatchIs(i, false)) {
			cs_row = i / 6, cs_col = i % 6;
			break;
		}
	cs_hue = hueOf(tabColor());
}

void SettingsBos_ledColorOpen(const char* title, uint32_t color, void (*live)(uint32_t), void (*set)(uint32_t)) {
	cs_slot = 99, cs_led = 1, cs_title = title, cs_tab = 0, cs_edit = 0, cs_text = cs_text_was = 0;
	cs_color = cs_was = cs_live_at = color & 0xFFFFFF;
	cs_live = live, cs_set = set;
	seekTab();
}

void SettingsBos_colorSheetOpen(int slot, const char* title) {
	cs_slot = slot, cs_led = 0, cs_title = title, cs_tab = 0, cs_edit = 0;
	cs_color = cs_was = slotColor(slot);
	cs_text = cs_text_was = textValue(CFG_getTheme(slot + 1));
	seekTab();
}

static void sheetInput(bool* dirty) {
	if (PAD_justPressed(BTN_A)) { // ใช้สีนี้ (on the code row, not yet editing: edit it)
		if (cs_row == 3 && !cs_edit) {
			cs_edit = 1, cs_digit = 0, cs_code_was = cs_color, cs_code_text = cs_text;
		} else if (cs_led) {
			cs_set(cs_color);
			cs_slot = -1, cs_led = 0;
		} else {
			CFG_setTheme(cs_slot, (int)cs_color);
			CFG_setTheme(cs_slot + 1, cs_text);
			cs_slot = -1;
		}
		*dirty = true;
	} else if (PAD_justPressed(BTN_B)) {
		if (cs_edit) // leave the code as it was before editing
			cs_color = cs_code_was, cs_text = cs_code_text, cs_hue = hueOf(tabColor()), cs_edit = 0;
		else if (cs_led) { // the light goes back to the colour it had
			cs_live(cs_was);
			cs_slot = -1, cs_led = 0;
		} else
			cs_slot = -1;
		*dirty = true;
	} else if (!cs_led && (PAD_justPressed(BTN_L1) || PAD_justPressed(BTN_R1))) {
		cs_tab = !cs_tab;
		seekTab();
		if (cs_edit)
			cs_row = 3;
		*dirty = true;
	} else if (cs_edit) { // the colour code: Left/Right picks the digit, Up/Down steps it 0-F
		if (PAD_justRepeated(BTN_LEFT) && cs_digit > 0)
			cs_digit--, *dirty = true;
		else if (PAD_justRepeated(BTN_RIGHT) && cs_digit < 5)
			cs_digit++, *dirty = true;
		else if (PAD_justRepeated(BTN_UP) || PAD_justRepeated(BTN_DOWN)) {
			int sh = 4 * (5 - cs_digit);
			uint32_t c = tabColor(), n = (((c >> sh) & 15) + (PAD_justRepeated(BTN_UP) ? 1 : 15)) & 15;
			setTabColor((c & ~(15u << sh)) | (n << sh));
			cs_hue = hueOf(tabColor());
			*dirty = true;
		}
	} else if (PAD_justRepeated(BTN_UP) && cs_row > 0) {
		if (--cs_row < 2)
			pickSwatch();
		*dirty = true;
	} else if (PAD_justRepeated(BTN_DOWN) && cs_row < 3) {
		if (++cs_row < 2)
			pickSwatch();
		*dirty = true;
	} else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT)) {
		int d = PAD_justRepeated(BTN_RIGHT) ? 1 : -1;
		if (cs_row < 2) {
			cs_col = (cs_col + d + 6) % 6;
			pickSwatch();
		} else if (cs_row == 2) {
			cs_hue = fmodf(cs_hue + d * 6 + 360, 360);
			setTabColor(hsv(cs_hue, CS_SAT, 1));
		}
		*dirty = true;
	}
	if (cs_led && cs_slot >= 0 && cs_color != cs_live_at) // the light shows the colour as it is picked
		cs_live(cs_color), cs_live_at = cs_color;
}

static void triangle(SDL_Surface* s, int cx, int y, int hgt, bool up, uint32_t c) {
	for (int r = 0; r < hgt; r++) {
		int half = up ? r : hgt - 1 - r;
		SDL_FillRect(s, &(SDL_Rect){cx - half, y + r, 2 * half + 1, 1}, BOS_map(s, c));
	}
}

static void sheetRender(SDL_Surface* s) {
	const BosTheme* th = BOS_theme();
	int bar = BOS_px(BOS_BAR_H), sh = BOS_px(590), top = s->h - bar - sh, px = BOS_px(48);
	BOS_rrect(s, 0, top, s->w, sh + bar, BOS_px(32), th->surface);
	int y = top + BOS_px(24);
	BOS_text(s, I18N_text(cs_title), 44, 700, BOS_C_TEXT, 1, px, y);
	// the tabs: สีพื้น | สีตัวหนังสือ, right of the title
	if (!cs_led) {
		const char* tl[2] = {I18N_text("Background colour"), I18N_text("Text colour")};
		int pad = BOS_px(24), segh = BOS_px(52), tw[2], tot = BOS_px(8) + BOS_px(4);
		for (int i = 0; i < 2; i++)
			tw[i] = BOS_textW(tl[i], BOS_FS_SMALL, 600) + 2 * pad, tot += tw[i];
		int sx = s->w - px - tot, cy = y + BOS_lineH(44) / 2;
		BOS_rrect(s, sx, cy - segh / 2 - BOS_px(4), tot, segh + BOS_px(8), BOS_px(18), th->surface2);
		sx += BOS_px(4);
		for (int i = 0; i < 2; i++) {
			bool on = i == cs_tab;
			if (on)
				BOS_rrect(s, sx, cy - segh / 2, tw[i], segh, BOS_px(14), th->focus);
			int w = BOS_textW(tl[i], BOS_FS_SMALL, on ? 600 : 400);
			BOS_textMid(s, tl[i], BOS_FS_SMALL, on ? 600 : 400, on ? th->onfocus : BOS_C_TEXT2, 1, sx + (tw[i] - w) / 2, cy);
			sx += tw[i] + BOS_px(4);
		}
	}
	y += BOS_lineH(44) + BOS_px(16);
	// swatches: the one in use has a check, the focused one a ring and 1.08; the text tab's first is อัตโนมัติ
	int d = BOS_px(92), gx = (s->w - 2 * px - 6 * d) / 5;
	for (int i = 0; i < 12; i++) {
		uint32_t c = cs_tab ? TEXT_SWATCHES[i] : BG_SW[i];
		bool autosw = cs_tab && i == 0;
		int cx = px + (i % 6) * (d + gx) + d / 2, cy = y + (i / 6) * (d + BOS_px(20)) + d / 2;
		bool focus = !cs_edit && cs_row == i / 6 && cs_col == i % 6;
		int dd = focus ? (int)(d * 1.08f) : d;
		if (focus) {
			int o = BOS_px(5) + BOS_px(4);
			BOS_rrect(s, cx - dd / 2 - o, cy - dd / 2 - o, dd + 2 * o, dd + 2 * o, dd / 2 + o, 0xFFFFFF);
			o = BOS_px(5);
			BOS_rrect(s, cx - dd / 2 - o, cy - dd / 2 - o, dd + 2 * o, dd + 2 * o, dd / 2 + o, th->surface);
		}
		if (autosw || BOS_contrast(c, th->surface) < 1.6) { // a dark swatch gets a rim so it shows on the sheet
			int o = BOS_px(2);
			BOS_rrect(s, cx - dd / 2 - o, cy - dd / 2 - o, dd + 2 * o, dd + 2 * o, dd / 2 + o, 0x48484A);
		}
		if (autosw) { // half white, half black: auto picks one of them
			BOS_rrect(s, cx - dd / 2, cy - dd / 2, dd, dd, dd / 2, 0xFFFFFF);
			// rounded fills ignore the clip: draw a black disc on the sheet colour apart, then copy its right half
			SDL_Surface* tmp = SDL_CreateRGBSurfaceWithFormat(0, dd, dd, 32, s->format->format);
			if (tmp) {
				SDL_Rect box = {cx - dd / 2, cy - dd / 2, dd, dd};
				SDL_BlitSurface(s, &box, tmp, NULL); // what is there now (the rim, the sheet), so only the disc changes
				BOS_rrect(tmp, 0, 0, dd, dd, dd / 2, BOS_C_ONFOCUS);
				SDL_Rect src = {dd / 2, 0, dd - dd / 2, dd}, dst = {cx, cy - dd / 2, 0, 0};
				SDL_BlitSurface(tmp, &src, s, &dst);
				SDL_FreeSurface(tmp);
			}
		} else
			BOS_rrect(s, cx - dd / 2, cy - dd / 2, dd, dd, dd / 2, c);
		if (swatchIs(i, true)) {
			int ic = BOS_px(44);
			BOS_icon(s, "check", autosw ? th->primary : BOS_textOn(c, 0), 1, cx - ic / 2, cy - ic / 2, ic);
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
	// รหัสสี: #RRGGBB; A edits it a digit at a time
	int ch = BOS_px(60);
	cy = y + ch / 2;
	BOS_textMid(s, I18N_text("Colour code"), BOS_FS_SMALL, cs_row == 3 ? 600 : 400, cs_row == 3 ? BOS_C_TEXT : BOS_C_TEXT2, 1, px, cy);
	uint32_t tc = tabColor();
	char hex[8];
	snprintf(hex, sizeof(hex), "%06X", (unsigned)(tc & 0xFFFFFF));
	if (cs_edit) {
		int cw = BOS_px(48), g = BOS_px(8), x = bx + BOS_px(4);
		BOS_textMid(s, "#", 36, 700, BOS_C_TEXTOFF, 1, x, cy);
		x += BOS_textW("#", 36, 700) + BOS_px(10);
		for (int i = 0; i < 6; i++) {
			bool on = i == cs_digit;
			BOS_rrect(s, x, cy - ch / 2, cw, ch, BOS_px(12), on ? th->focus : th->surface2);
			char dg[2] = {hex[i], 0};
			int w = BOS_textW(dg, 36, 700);
			BOS_textMid(s, dg, 36, 700, on ? th->onfocus : BOS_C_TEXT, 1, x + (cw - w) / 2, cy);
			if (on) {
				triangle(s, x + cw / 2, cy - ch / 2 - BOS_px(16), BOS_px(10), true, th->focus);
				triangle(s, x + cw / 2, cy + ch / 2 + BOS_px(6), BOS_px(10), false, th->focus);
			}
			x += cw + g;
		}
		int sw = ch;
		BOS_rrect(s, s->w - px - sw, cy - sw / 2, sw, sw, BOS_px(16), 0x48484A);
		BOS_rrect(s, s->w - px - sw + BOS_px(2), cy - sw / 2 + BOS_px(2), sw - BOS_px(4), sw - BOS_px(4), BOS_px(14), tc);
	} else {
		if (cs_row == 3)
			BOS_rrect(s, bx - BOS_px(4), cy - ch / 2 - BOS_px(4), bw + BOS_px(8), ch + BOS_px(8), BOS_px(20), th->focus);
		BOS_rrect(s, bx, cy - ch / 2, bw, ch, BOS_px(16), th->surface2);
		int sw = BOS_px(32), x = bx + BOS_px(20);
		BOS_rrect(s, x, cy - sw / 2, sw, sw, BOS_px(9), 0x48484A);
		BOS_rrect(s, x + BOS_px(2), cy - sw / 2 + BOS_px(2), sw - BOS_px(4), sw - BOS_px(4), BOS_px(7), tc);
		x += sw + BOS_px(16);
		char shown[16];
		snprintf(shown, sizeof(shown), "#%s", hex);
		BOS_textMid(s, cs_tab && cs_text == 0 ? I18N_text("Auto") : shown, 34, 600, BOS_C_TEXT, 1, x, cy);
		if (cs_row == 3) {
			const char* h = I18N_text("Press A to edit");
			BOS_textMid(s, h, 26, 400, BOS_C_TEXTOFF, 1, bx + bw - BOS_px(20) - BOS_textW(h, 26, 400), cy);
		}
	}
	y += ch + BOS_px(20);
	if (cs_led) { // the light itself is the preview: a lit pill and what is happening
		int ph = BOS_px(84), pw = BOS_px(56), pht = BOS_px(28);
		BOS_rrect(s, px, y, s->w - 2 * px, ph, BOS_px(20), BOS_C_BG);
		BOS_rrect(s, px + BOS_px(24) - BOS_px(6), y + (ph - pht) / 2 - BOS_px(6), pw + BOS_px(12), pht + BOS_px(12), (pht + BOS_px(12)) / 2, mix(cs_color, BOS_C_BG, 0.35f));
		BOS_rrect(s, px + BOS_px(24), y + (ph - pht) / 2, pw, pht, pht / 2, cs_color);
		BOS_textMid(s, I18N_text("The light shows this colour now \xC2\xB7 B cancels"), BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, px + BOS_px(24) + pw + BOS_px(20), y + ph / 2);
	} else {
	// the preview: this colour with its text colour, and whether it reads well (4.5:1)
	uint32_t fg = BOS_textOn(cs_color, cs_text);
	bool easy = BOS_contrast(cs_color, fg) >= 4.5;
	int ph = BOS_px(84);
	BOS_rrect(s, px, y, s->w - 2 * px, ph, BOS_px(20), cs_color);
	BOS_textMid(s, I18N_text("Preview"), BOS_FS_BODY, 600, fg, 1, px + BOS_px(24), y + ph / 2);
	const char* verdict = I18N_text(easy ? "Easy to read" : "Hard to read");
	int vw = BOS_textW(verdict, 26, 600), ic = BOS_px(28), pw = vw + BOS_px(24) + (easy ? ic + BOS_px(6) : 0);
	int vx = s->w - px - BOS_px(16) - pw, vh = BOS_px(44);
	BOS_rrect(s, vx, y + (ph - vh) / 2, pw, vh, vh / 2, BOS_C_BG); // a dark pill: the verdict reads on any colour
	BOS_textMid(s, verdict, 26, 600, easy ? BOS_C_OK : BOS_C_WARN, 1, vx + BOS_px(12), y + ph / 2);
	if (easy)
		BOS_icon(s, "check", BOS_C_OK, 1, vx + BOS_px(12) + vw + BOS_px(6), y + (ph - ic) / 2, ic);
	}
	if (cs_edit) {
		BosHint r[] = {{"A", I18N_text("Use this colour")}, {"B", I18N_text("Cancel")}};
		BosHint l[] = {{"\xE2\x86\x90 \xE2\x86\x92", I18N_text("Pick digit")}, {"\xE2\x86\x91 \xE2\x86\x93", I18N_text("Change value")}};
		BOS_hintBar(s, r, 2, l, 2);
	} else {
		BosHint r[] = {{"A", I18N_text(cs_row == 3 ? "Edit code" : "Use this colour")}, {"B", I18N_text("Cancel")}};
		BosHint l[] = {{"L1 R1", I18N_text("Background/Text")}};
		BOS_hintBar(s, r, 2, l, cs_led ? 0 : 1);
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

static int ks_key = -1;
static void keySheetInput(bool* dirty);
static void keySheetRender(SDL_Surface* s);

bool SettingsBos_pickerActive(void) {
	return pk_item != NULL || cs_slot >= 0 || ks_key >= 0;
}

void SettingsBos_pickerInput(bool* dirty) {
	if (ks_key >= 0) {
		keySheetInput(dirty);
		return;
	}
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

static void shadeScreen(SDL_Surface* s) { // the page under a sheet dims to 60 %
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
}

// ---- ปุ่มลัด: "กดปุ่มที่ต้องการ…", a §5.7 sheet; the shortcut's name, the ask, a note, the countdown ----
void SettingsBos_captureSheet(SDL_Surface* s, const char* name, const char* note, int secs) {
	shadeScreen(s);
	const BosTheme* th = BOS_theme();
	int bar = BOS_px(BOS_BAR_H), sh = BOS_px(330), top = s->h - bar - sh, px = BOS_px(48);
	BOS_rrect(s, 0, top, s->w, sh + bar, BOS_px(32), th->surface);
	int y = top + BOS_px(24);
	BOS_text(s, I18N_text(name), 44, 700, BOS_C_TEXT, 1, px, y);
	char n[16];
	snprintf(n, sizeof(n), "%d", secs);
	int nw = BOS_textW(n, 44, 700), d = BOS_px(64);
	BOS_rrect(s, s->w - px - d, y + (BOS_lineH(44) - d) / 2, d, d, d / 2, th->surface2);
	BOS_text(s, n, 44, 700, BOS_C_TEXT2, 1, s->w - px - d + (d - nw) / 2, y + (BOS_lineH(44) - BOS_lineH(44)) / 2);
	y += BOS_lineH(44) + BOS_px(28);
	BOS_text(s, I18N_text("Press the button you want..."), BOS_FS_BODY, 600, BOS_C_TEXT, 1, px, y);
	y += BOS_lineH(BOS_FS_BODY) + BOS_px(14);
	BOS_textWrap(s, note, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, px, y, s->w - 2 * px, 2);
	BosHint r[] = {{"B", I18N_text("Cancel")}};
	BOS_hintBar(s, r, 1, NULL, 0);
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
	lt_want = 0;
	rightColumn(s, page, cat, right_focused, top, bottom, &hot);
	if (right_focused && page && page->led_preview)
		lt_want = 1;

	// drawn after the columns: a scrolled category's squircle can't show through it
	BOS_statusBar(s, I18N_text("Settings"));

	// hint bar: A opens (เปิด/ปิด on a toggle, เลือก on a value row), ← → changes a value, X clears, B back
	BosHint r[3], l[1];
	int nr = 0, nl = 0;
	SettingItem* it = right_focused ? hot : cat;
	if (it && right_focused && it->lhint_keys)
		l[nl++] = (BosHint){it->lhint_keys, I18N_text(it->lhint_label)};
	if (it) {
		bool cyc = it->type == ITEM_CYCLE || it->type == ITEM_COLOR;
		if (right_focused && it->a_label) {
			if (it->a_label[0])
				r[nr++] = (BosHint){"A", I18N_text(it->a_label)};
		} else if (it->keybtn)
			r[nr++] = (BosHint){"A", I18N_text("Configure")};
		else if (it->color_slot || (it->dot_color && it->on_press))
			r[nr++] = (BosHint){"A", I18N_text("Choose")};
		else if (it->keycaps && it->on_press)
			r[nr++] = (BosHint){"A", I18N_text("Set")};
		else if (it->keycaps)
			; // read only (ปุ่มสลับเกม)
		else if (it->custom_draw)
			r[nr++] = (BosHint){"A", I18N_text("Use this theme")}; // the theme cards
		else if (!cyc && it->type != ITEM_STATIC)
			r[nr++] = (BosHint){"A", I18N_text("Open")};
		else if (cyc && (it->a_cycles || toggleState(it) >= 0))
			r[nr++] = (BosHint){"A", I18N_text("On/Off")};
		else if (cyc && it->on_press)
			r[nr++] = (BosHint){"A", I18N_text("Set")};
		else if (SettingsBos_isPickable(it))
			r[nr++] = (BosHint){"A", I18N_text("Choose")};
		if (cyc && toggleState(it) < 0 && !it->keycaps && !nl)
			l[nl++] = (BosHint){"\xE2\x86\x90 \xE2\x86\x92", I18N_text("Change")};
	}
	r[nr++] = (BosHint){"B", I18N_text(it && right_focused && it->b_label ? it->b_label : "Back")};
	if (it && right_focused && it->on_clear)
		r[nr++] = (BosHint){"X", I18N_text(it->clear_label ? it->clear_label : "Clear")};
	BOS_hintBar(s, r, nr, l, nl);
	if (pk_item || cs_slot >= 0 || ks_key >= 0) { // a sheet over the page, which dims to 60 %
		shadeScreen(s);
		if (ks_key >= 0)
			keySheetRender(s);
		else if (cs_slot >= 0)
			sheetRender(s);
		else
			pickerRender(s);
	}
	if (ks_key >= 0)
		lt_want = 2 + ks_key;
	lightsApply();
}

// ---- ปุ่มพิเศษ (DESIGN §6.9b) ----
// step 1 lists กดคลิก / กดค้าง; step 2 replaces it with what the slot can do. The three "…" rows hand over to the
// launcher's real screens in pick mode (BOS_PICK_PATH), which answers through BOS_PICK_RESULT_PATH.
static const struct {
	const char* name;
	const char* value; // a sys: value, or "pick:<kind>"
} KS_OPTS[] = {
	{"Open app...", "pick:app"},	   {"Open console...", "pick:console"}, {"Open game...", "pick:game"},
	{"Quick settings", "sys:quick"},  {"Switch games", "sys:switcher"},   {"Back to Home", "sys:home"},
	{"Mute / unmute", "sys:mute"},	   {"Nothing", ""},
};
#define KS_NOPTS ((int)(sizeof(KS_OPTS) / sizeof(KS_OPTS[0])))
#define KS_ROWS2 5 // step 2 rows in view
static int ks_step, ks_hold, ks_sel, ks_top;
static bool ks_quit; // Settings closes (a pick, or an action for the launcher)

static const char* keyName(int key) {
	return key == BOSKEY_HOME ? "HOME button" : key == BOSKEY_F1 ? "F1 button" : "F2 button";
}

// what a slot does, as Settings shows it
static void keyText(int key, int hold, char* out, size_t size) {
	char v[BOS_KEY_VAL];
	BosKeys_value(key, hold, v, sizeof(v));
	if (!v[0]) {
		snprintf(out, size, "%s", I18N_text("Nothing"));
		return;
	}
	for (int i = 3; i < KS_NOPTS; i++)
		if (!strcmp(v, KS_OPTS[i].value)) {
			snprintf(out, size, "%s", I18N_text(KS_OPTS[i].name));
			return;
		}
	char lab[64], l[160];
	snprintf(lab, sizeof(lab), "%s.label", BosKeys_slot(key, hold));
	if (BosKeys_read(lab, l, sizeof(l)) && l[0]) {
		snprintf(out, size, "%s", l);
		return;
	}
	const char* c = strchr(v, ':');
	const char* b = strrchr(v, '/');
	snprintf(out, size, "%s", b ? b + 1 : c ? c + 1 : v);
}

// the key's light (Brick, Brick Pro: one under F1 and one under F2) blinks fast in the colour the user gave that
// light (ไฟ LED), white when it has none. Every other light is off so the key stands out; LEDS_refresh puts the
// profile back.
static void keyBlink(int key) {
#if MAX_LIGHTS > 0
	if ((key != BOSKEY_F1 && key != BOSKEY_F2) || !(is_brick || is_brickpro))
		return;
	int zone = key == BOSKEY_F1 ? 0 : 1;
	for (int z = 0; z < (is_brickpro ? 5 : 4) && z < MAX_LIGHTS; z++)
		if (z != zone) {
			LightSettings off = lightsDefault[z]; // effect 0 freezes a light mid-animation (lit or dark): static black
			off.color1 = 0;
			off.effect = 4;
			PLAT_setLedColor(&off);
			PLAT_setLedEffect(&off);
		}
	LightSettings on = lightsDefault[zone], scale = lightsDefault[0];
	if (scale.brightness < 40) // F1 and F2 share one brightness node (written via f1): dim or off would hide the blink
		scale.brightness = 60, PLAT_setLedBrightness(&scale);
	if (!(on.color1 & 0xFFFFFF))
		on.color1 = 0xFFFFFF;
	on.effect = 5;	 // Blink 1
	on.speed = 150; // ms per blink
	on.cycles = -1;
	PLAT_setLedEffectCycles(&on);
	PLAT_setLedEffectSpeed(&on);
	PLAT_setLedColor(&on);
	PLAT_setLedEffect(&on);
#else
	(void)key;
#endif
}

// the lights Settings draws itself, decided on every drawn frame (lt_want): a ปุ่มพิเศษ row or popup in focus blinks
// that key; the ไฟ LED pages show the user's own settings (even while charging); anything else gives the profile back
static int lt_now;
static void (*lt_preview)(void);
void SettingsBos_setLedPreview(void (*preview)(void)) {
	lt_preview = preview;
}
static void lightsApply(void) {
#if MAX_LIGHTS > 0
	if (lt_want == lt_now)
		return;
	lt_now = lt_want;
	if (lt_now == 1 && lt_preview)
		lt_preview();
	else
		LEDS_refresh();
	if (lt_now >= 2)
		keyBlink(lt_now - 2);
#endif
}
void SettingsBos_lightsDone(void) { // Settings is closing: the user's lights back
	lt_want = 0;
	lightsApply();
}

static void keySaved(void) {
}

void SettingsBos_keyOpen(int key) {
	ks_key = key;
	ks_step = 1;
	ks_hold = 0;
	ks_sel = 0;
}

void SettingsBos_keyReset(int key) {
	BosKeys_reset(key, 0);
	BosKeys_reset(key, 1);
	if (key != BOSKEY_HOME) { // F1/F2's default is ว่าง; a missing line would be migrated again from fn1tool
		BosKeys_set(key, 0, "", NULL);
		BosKeys_set(key, 1, "", NULL);
	}
}

static void keyClose(void) {
	ks_key = -1;
}

static int keyOptIndex(void) { // step 2: the row holding the slot's value (-1: an app / console / game)
	char v[BOS_KEY_VAL];
	BosKeys_value(ks_key, ks_hold, v, sizeof(v));
	for (int i = 3; i < KS_NOPTS; i++)
		if (!strcmp(v, KS_OPTS[i].value))
			return i;
	return -1;
}

static void keySheetInput(bool* dirty) {
	if (PAD_anyPressed()) // nothing moves on the popup otherwise: no redraw (it ran at 60 fps and warmed the device)
		*dirty = true;
	if (ks_step == 1) {
		if (PAD_justRepeated(BTN_UP) || PAD_justRepeated(BTN_DOWN))
			ks_sel = !ks_sel;
		else if (PAD_justPressed(BTN_B))
			keyClose();
		else if (PAD_justPressed(BTN_X)) { // ล้าง (HOME: คืนค่าเดิม)
			if (ks_key == BOSKEY_HOME)
				BosKeys_reset(ks_key, ks_sel);
			else
				BosKeys_set(ks_key, ks_sel, "", NULL);
			keySaved();
		} else if (PAD_justPressed(BTN_A)) {
			ks_hold = ks_sel;
			ks_step = 2;
			int i = keyOptIndex();
			ks_sel = i >= 0 ? i : 0;
			ks_top = ks_sel - KS_ROWS2 + 1 > 0 ? ks_sel - KS_ROWS2 + 1 : 0;
		}
		return;
	}
	if (PAD_justRepeated(BTN_UP) && ks_sel > 0)
		ks_sel--;
	else if (PAD_justRepeated(BTN_DOWN) && ks_sel < KS_NOPTS - 1)
		ks_sel++;
	else if (PAD_justPressed(BTN_B)) {
		ks_step = 1;
		ks_sel = ks_hold;
	} else if (PAD_justPressed(BTN_A)) {
		const char* v = KS_OPTS[ks_sel].value;
		if (!strncmp(v, "pick:", 5)) { // the launcher's real screen; it brings us back here
			char title[160], line[256];
			snprintf(title, sizeof(title), "%s · %s", I18N_text(keyName(ks_key)), I18N_text(ks_hold ? "Hold" : "Tap"));
			snprintf(line, sizeof(line), "%s\t%s\t%s", BosKeys_slot(ks_key, ks_hold), v + 5, title);
			BosKeys_writeFile(BOS_PICK_PATH, line);
			unlink(BOS_PICK_RESULT_PATH);
			SettingsBos_lightsDone();
			ks_quit = true;
			return;
		}
		BosKeys_set(ks_key, ks_hold, v, NULL);
		ks_step = 1;
		ks_sel = ks_hold;
		keySaved();
	}
	if (ks_sel < ks_top)
		ks_top = ks_sel;
	if (ks_sel >= ks_top + KS_ROWS2)
		ks_top = ks_sel - KS_ROWS2 + 1;
}

// one row: the name, then the value (or nothing), the "+ค้าง" tag, and a chevron (mark 1) or a ✓ (mark 2)
static void keyRowDraw(SDL_Surface* s, const char* name, const char* value, bool tag, int mark, int x, int y, int w,
					   int h, bool hot) {
	if (hot) {
		int gw = (int)(w * BOS_ROW_SCALE) - w, gh = (int)(h * BOS_ROW_SCALE) - h;
		x -= gw / 2, w += gw, y -= gh / 2, h += gh;
		BOS_rrect(s, x, y, w, h, BOS_px(BOS_RADIUS_ROW), BOS_theme()->focus);
	}
	float a = hot ? 1.0f : BOS_DIM;
	uint32_t fg = hot ? BOS_theme()->onfocus : BOS_C_TEXT, sub = hot ? C_ON_SUB : BOS_C_TEXT2;
	int cy = y + h / 2, lx = x + BOS_px(28), rx = x + w - BOS_px(24);
	if (mark == 1) {
		BOS_icon(s, "chev", hot ? C_ON_SUB : BOS_C_TEXTOFF, a, rx - BOS_px(28), cy - BOS_px(14), BOS_px(28));
		rx -= BOS_px(28 + 10);
	} else if (mark == 2) {
		int ck = BOS_px(36);
		BOS_icon(s, "check", hot ? BOS_theme()->onfocus : BOS_theme()->primary, 1, rx - ck, cy - ck / 2, ck);
		rx -= ck + BOS_px(10);
	}
	if (tag) { // "+ค้าง": 26 in a 2 px outline, radius 10, padding 0 8
		const char* t = I18N_text("+hold");
		int tw = BOS_textW(t, 26, 500) + 2 * BOS_px(8), th = BOS_px(36);
		rx -= tw;
		BOS_rrect(s, rx, cy - th / 2, tw, th, BOS_px(10), sub);
		BOS_rrect(s, rx + BOS_px(2), cy - th / 2 + BOS_px(2), tw - BOS_px(4), th - BOS_px(4), BOS_px(8),
				  hot ? BOS_theme()->focus : BOS_theme()->surface);
		BOS_textMid(s, t, 26, 500, sub, a, rx + BOS_px(8), cy);
		rx -= BOS_px(10);
	}
	int vw = 0;
	char v[160] = "";
	if (value && value[0]) {
		BOS_fit(value, BOS_FS_SMALL, 400, (rx - lx) / 2, v, sizeof(v));
		vw = BOS_textW(v, BOS_FS_SMALL, 400);
		BOS_textMid(s, v, BOS_FS_SMALL, 400, sub, a, rx - vw, cy);
	}
	char l[160];
	BOS_fit(name, BOS_FS_BODY, hot ? 600 : 500, rx - vw - BOS_px(16) - lx, l, sizeof(l));
	BOS_textMid(s, l, BOS_FS_BODY, hot ? 600 : 500, fg, a, lx, cy);
}

// the popup: a §5.7 sheet from the bottom (330 tall at step 1, 600 at step 2), above everything on the page
static void keySheetRender(SDL_Surface* s) {
	const BosTheme* th = BOS_theme();
	// step 1 fits its title, two rows and the note line; step 2 is 600 tall
	int bar = BOS_px(BOS_BAR_H), px = BOS_px(48);
	int sh = ks_step == 1 ? BOS_px(24 + 16 + 2 * 96 + 14 + 22) + BOS_lineH(44) + BOS_lineH(26) : BOS_px(600);
	int top = s->h - bar - sh;
	BOS_rrect(s, 0, top, s->w, sh + bar, BOS_px(32), th->surface);
	int y = top + BOS_px(24);
	char title[160];
	if (ks_step == 1)
		snprintf(title, sizeof(title), "%s", I18N_text(keyName(ks_key)));
	else
		snprintf(title, sizeof(title), "%s · %s", I18N_text(keyName(ks_key)), I18N_text(ks_hold ? "Hold" : "Tap"));
	BOS_text(s, title, 44, 700, BOS_C_TEXT, 1, px, y);
	y += BOS_lineH(44) + BOS_px(16);
	int rh = BOS_px(96), gx = px, gw = s->w - 2 * px;
	if (ks_step == 1) {
		BOS_rrect(s, gx, y, gw, 2 * rh, BOS_px(24), th->surface2);
		for (int r = 0; r < 2; r++) {
			char v[160], hv[8];
			keyText(ks_key, r, v, sizeof(v));
			(void)hv;
			keyRowDraw(s, I18N_text(r ? "Hold" : "Tap"), v, false, 1, gx, y + r * rh, gw, rh, ks_sel == r);
		}
		y += 2 * rh + BOS_px(14);
		char note[256];
		if (ks_key == BOSKEY_HOME)
			snprintf(note, sizeof(note), "%s", I18N_text("Works everywhere, in games too"));
		else
			snprintf(note, sizeof(note), I18N_text("The light under %s blinks = the key being set · works everywhere, in games too"),
					 ks_key == BOSKEY_F1 ? "F1" : "F2");
		BOS_text(s, note, 26, 400, BOS_C_TEXT2, 1, px + BOS_px(28), y);
		BosHint r[] = {{"A", I18N_text("Choose")}, {"B", I18N_text("Close")},
					   {"X", I18N_text(ks_key == BOSKEY_HOME ? "Restore default" : "Clear")}};
		BOS_hintBar(s, r, 3, NULL, 0);
		return;
	}
	int rows = KS_NOPTS < KS_ROWS2 ? KS_NOPTS : KS_ROWS2, cur = keyOptIndex();
	SDL_SetClipRect(s, &(SDL_Rect){0, y, s->w, s->h - bar - y});
	BOS_rrect(s, gx, y, gw, rows * rh, BOS_px(24), th->surface2);
	for (int r = 0; r < rows; r++) {
		int i = ks_top + r;
		bool pick = !strncmp(KS_OPTS[i].value, "pick:", 5);
		keyRowDraw(s, I18N_text(KS_OPTS[i].name), NULL, false, pick ? 1 : i == cur ? 2 : 0, gx, y + r * rh, gw, rh,
				   i == ks_sel);
	}
	SDL_SetClipRect(s, NULL);
	if (KS_NOPTS > rows) { // a scroll mark inside the sheet
		int tt = rows * rh, mh = tt * rows / KS_NOPTS, my = y + (tt - mh) * ks_top / (KS_NOPTS - rows);
		BOS_rrect(s, s->w - BOS_px(18), my, BOS_px(6), mh, BOS_px(3), BOS_C_TEXTOFF);
	}
	BosHint r[] = {{"A", I18N_text("Choose")}, {"B", I18N_text("Back")}};
	BOS_hintBar(s, r, 2, NULL, 0);
}

int SettingsBos_keyResume(void) {
	if (access(BOS_PICK_RESULT_PATH, F_OK) != 0)
		return -1;
	char line[BOS_KEY_VAL + 256];
	BosKeys_readFile(BOS_PICK_RESULT_PATH, line, sizeof(line));
	unlink(BOS_PICK_RESULT_PATH);
	char* f[4] = {line, NULL, NULL, NULL};
	for (int i = 1; i < 4; i++) {
		char* t = f[i - 1] ? strchr(f[i - 1], '\t') : NULL;
		if (t) {
			*t = '\0';
			f[i] = t + 1;
		}
	}
	int key = -1, hold = 0;
	for (int k = 0; k < BOSKEY_COUNT; k++)
		for (int h = 0; h < 2; h++)
			if (!strcmp(f[0], BosKeys_slot(k, h)))
				key = k, hold = h;
	if (key < 0)
		return -1;
	SettingsBos_keyOpen(key);
	ks_sel = hold;
	if (f[1] && !strcmp(f[1], "set") && f[2]) {
		BosKeys_set(key, hold, f[2], f[3]);
		keySaved();
	}
	return key;
}

bool SettingsBos_keyTick(bool* quit) {
	bool d = false;
	static int n = 0;
	if (++n >= 15) { // an action keymon left for the launcher: Settings gets out of its way (the popup ignores them)
		n = 0;
		if (access(BOS_ACTION_PATH, F_OK) == 0) {
			if (ks_key >= 0)
				unlink(BOS_ACTION_PATH);
			else
				ks_quit = true;
		}
	}
	if (ks_quit) {
		*quit = true;
		d = true;
	}
	return d;
}
