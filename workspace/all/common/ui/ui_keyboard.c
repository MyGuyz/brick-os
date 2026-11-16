// In-process on-screen keyboard (DESIGN.md 7.6). Rows line up in columns (English 10 keys, Thai Kedmanee 12),
// with ⇧ and ⌫ on the fourth row and ◀ ▶ Space #+= ก/A OK at the bottom. A types, B deletes (on an empty field it
// closes), X space, Y Thai/English, START confirms, L1 shift, L2/R2 move the caret.
// Apps with the Brick OS kit draw it the Brick OS way (BOS_uiKeyboard); the others get the plain grid below.
#include "ui_keyboard.h"
#include "ui_draw.h"
#include "ui_buttonhintbar.h"
#include "kb_utf8.h"
#include "api.h"
#include "config.h"
#include "defines.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KB_MAX_INPUT 128

void BOS_uiKeyboard(SDL_Surface* s, const UIKeyboardView* v) __attribute__((weak));

#define BOTTOM_ROW {UIKB_LEFT, UIKB_RIGHT, UIKB_SPACE, UIKB_SYM, UIKB_LANG, UIKB_GO}
static const char* kb_en[UIKB_ROWS][UIKB_COLS] = {
	{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
	{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
	{"a", "s", "d", "f", "g", "h", "j", "k", "l", "."},
	{UIKB_SHIFT, "z", "x", "c", "v", "b", "n", "m", ",", UIKB_DEL},
	BOTTOM_ROW};
static const char* kb_en_shift[UIKB_ROWS][UIKB_COLS] = {
	{"!", "@", "#", "$", "%", "^", "&", "*", "(", ")"},
	{"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
	{"A", "S", "D", "F", "G", "H", "J", "K", "L", ":"},
	{UIKB_SHIFT, "Z", "X", "C", "V", "B", "N", "M", "?", UIKB_DEL},
	BOTTOM_ROW};
// the symbols page (#+=): what passwords and names need beyond the letters
static const char* kb_sym[UIKB_ROWS][UIKB_COLS] = {
	{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
	{"-", "_", "=", "+", "[", "]", "{", "}", "\\", "|"},
	{"`", "~", ";", ":", "'", "\"", "<", ">", "/", "@"},
	{UIKB_SHIFT, "!", "#", "$", "%", "^", "&", "*", "?", UIKB_DEL},
	BOTTOM_ROW};
// Thai on the standard Kedmanee layout (TIS 820-2538), as on Thai PC and phone keyboards; marks alone on their keys
static const char* kb_th[UIKB_ROWS][UIKB_COLS] = {
	{"ๅ", "/", "-", "ภ", "ถ", "ุ", "ึ", "ค", "ต", "จ", "ข", "ช"},
	{"ๆ", "ไ", "ำ", "พ", "ะ", "ั", "ี", "ร", "น", "ย", "บ", "ล"},
	{"ฟ", "ห", "ก", "ด", "เ", "้", "่", "า", "ส", "ว", "ง", "ฃ"},
	{UIKB_SHIFT, "ผ", "ป", "แ", "อ", "ิ", "ื", "ท", "ม", "ใ", "ฝ", UIKB_DEL},
	BOTTOM_ROW};
static const char* kb_th_shift[UIKB_ROWS][UIKB_COLS] = {
	{"+", "๑", "๒", "๓", "๔", "ู", "฿", "๕", "๖", "๗", "๘", "๙"},
	{"๐", "\"", "ฎ", "ฑ", "ธ", "ํ", "๊", "ณ", "ฯ", "ญ", "ฐ", ","},
	{"ฤ", "ฆ", "ฏ", "โ", "ฌ", "็", "๋", "ษ", "ศ", "ซ", ".", "ฅ"},
	{UIKB_SHIFT, "(", ")", "ฉ", "ฮ", "ฺ", "์", "?", "ฒ", "ฬ", "ฦ", UIKB_DEL},
	BOTTOM_ROW};

typedef const char* KbLayout[UIKB_ROWS][UIKB_COLS];
static int kb_thai_mode = 0; // the last language used, kept for the next keyboard this run

static KbLayout* kb_layout(int shift, int sym) {
	if (sym)
		return &kb_sym;
	if (kb_thai_mode)
		return shift ? &kb_th_shift : &kb_th;
	return shift ? &kb_en_shift : &kb_en;
}

static int kb_row_len(KbLayout* l, int row) {
	int n = 0;
	while (n < UIKB_COLS && (*l)[row][n])
		n++;
	return n;
}

static int kb_units(KbLayout* l) { return kb_row_len(l, 0); }

// the bottom row: Space takes what the five other keys leave, OK two
static int kb_span(KbLayout* l, int row, int col) {
	const char* k = (*l)[row][col];
	if (row != UIKB_ROWS - 1)
		return 1;
	if (strcmp(k, UIKB_SPACE) == 0)
		return kb_units(l) - 6;
	if (strcmp(k, UIKB_GO) == 0)
		return 2;
	return 1;
}

static float kb_center(KbLayout* l, int row, int col) {
	float x = 0;
	for (int c = 0; c < col; c++)
		x += kb_span(l, row, c);
	return x + kb_span(l, row, col) / 2.0f;
}

static int kb_col_at(KbLayout* l, int row, float x) {
	float acc = 0;
	int n = kb_row_len(l, row);
	for (int c = 0; c < n; c++) {
		acc += kb_span(l, row, c);
		if (x < acc)
			return c;
	}
	return n - 1;
}

static void kb_view(UIKeyboardView* v, KbLayout* l) {
	memset(v->keys, 0, sizeof(v->keys));
	for (int r = 0; r < UIKB_ROWS; r++)
		for (int c = 0; c < kb_row_len(l, r); c++) {
			v->keys[r][c] = (*l)[r][c];
			v->span[r][c] = kb_span(l, r, c);
		}
	v->units = kb_units(l);
}

// The plain grid for apps without the Brick OS kit: the prompt, the field, the keys as glyphs, the focused key on
// the theme colour, the standard hint bar.
static void kb_draw_plain(SDL_Surface* screen, const UIKeyboardView* v) {
	SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
	SDL_Color focused_color = uintToColour(THEME_COLOR5_255);
	int y = SCALE1(PADDING);
	if (v->prompt && v->prompt[0]) {
		SDL_Surface* t = GFX_renderText(font.small, v->prompt, COLOR_GRAY);
		if (t) {
			SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){SCALE1(PADDING), y});
			y += t->h + SCALE1(4);
			SDL_FreeSurface(t);
		}
	}
	int input_h = TTF_FontHeight(font.large);
	SDL_Surface* t = v->input[0] ? GFX_renderText(font.large, v->input, COLOR_WHITE) : NULL;
	int caret_x = 0;
	if (v->caret > 0) {
		char head[KB_MAX_INPUT + 1];
		snprintf(head, sizeof(head), "%.*s", v->caret, v->input);
		GFX_measureText(font.large, head, &caret_x, NULL);
	}
	int max_w = screen->w - SCALE1(PADDING * 2), src_x = caret_x > max_w ? caret_x - max_w : 0;
	if (t) {
		SDL_BlitSurface(t, &(SDL_Rect){src_x, 0, max_w, t->h}, screen, &(SDL_Rect){SCALE1(PADDING), y});
		SDL_FreeSurface(t);
	}
	SDL_FillRect(screen, &(SDL_Rect){SCALE1(PADDING) + caret_x - src_x, y, SCALE1(2), input_h},
				 SDL_MapRGB(screen->format, 255, 255, 255));
	int rows_top = y + input_h + SCALE1(8), avail = screen->h - BAR_HEIGHT - rows_top;
	int cell_h = avail / UIKB_ROWS, cell_w = (screen->w - SCALE1(PADDING * 2)) / v->units;
	for (int r = 0; r < UIKB_ROWS; r++) {
		int x = SCALE1(PADDING), cy = rows_top + r * cell_h + cell_h / 2;
		for (int c = 0; c < UIKB_COLS && v->keys[r][c]; c++) {
			int w = cell_w * v->span[r][c];
			bool focused = r == v->row && c == v->col;
			if (focused)
				UI_fillRoundedRect(screen, x + SCALE1(1), cy - cell_h / 2 + SCALE1(1), w - SCALE1(2), cell_h - SCALE1(2),
								   SCALE1(6), THEME_COLOR1);
			const char* k = v->keys[r][c];
			const char* label = strcmp(k, UIKB_GO) == 0 ? (v->search ? "Search" : "OK")
							  : strcmp(k, UIKB_SYM) == 0 && v->sym ? "abc" : k;
			SDL_Surface* g = GFX_renderText(font.large, label, focused ? focused_color : COLOR_WHITE);
			if (g) {
				SDL_BlitSurface(g, NULL, screen, &(SDL_Rect){x + (w - g->w) / 2, cy - g->h / 2});
				SDL_FreeSurface(g);
			}
			x += w;
		}
	}
	UI_renderButtonHintBar(screen, (char*[]){"Y", "TH/EN", "X", "SPACE", "B", "DELETE", "A", "TYPE", NULL});
	GFX_flip(screen);
}

char* UIKeyboard_open(const char* prompt) {
	return UIKeyboard_openEx(prompt, 0);
}

char* UIKeyboard_openEx(const char* prompt, int flags) {
	return UIKeyboard_openWith(prompt, NULL, flags);
}

// Insert a key's whole UTF-8 text (Thai keys are 3 bytes); false when it doesn't fit.
static bool kb_insert_str(char* input, int* len, int* caret, const char* key) {
	int n = (int)strlen(key);
	if (*len + n > KB_MAX_INPUT)
		return false;
	memmove(input + *caret + n, input + *caret, *len - *caret + 1);
	memcpy(input + *caret, key, n);
	*len += n;
	*caret += n;
	return true;
}

static bool kb_delete(char* input, int* len, int* caret) {
	if (*caret <= 0)
		return false;
	int start = KbUtf8_prev(input, *caret);
	memmove(input + start, input + *caret, *len - *caret + 1);
	*len -= *caret - start;
	*caret = start;
	return true;
}

static bool kb_searchy(const char* prompt) {
	return prompt && (strstr(prompt, "Search") || strstr(prompt, "search") || strstr(prompt, "ค้นหา"));
}

char* UIKeyboard_openWith(const char* prompt, const char* initial, int flags) {
	(void)flags;
	SDL_Surface* screen = GFX_getScreen();
	if (!screen) {
		LOG_error("UIKeyboard_open: no screen (GFX_init not called?)\n");
		return NULL;
	}

	char input[KB_MAX_INPUT + 1] = {0};
	int len = 0;
	if (initial) {
		snprintf(input, sizeof(input), "%s", initial);
		len = KbUtf8_boundary(input, (int)strlen(input));
		input[len] = '\0';
	}
	int caret = len; // byte offset of the edit position, always on a char boundary
	int row = 1, col = 0;
	int shift = 0, sym = 0, caps = 0; // caps: L1 twice quickly, until L1 again
	uint32_t l1_at = 0;
	bool dirty = true;

	// The keyboard takes over the whole screen, but hardware overlay layers
	// (game-list thumbnail art, marquee scroll text) composite ABOVE the
	// surface the keyboard paints — left uncleared they float over the keys.
	// Callers repaint from scratch after the keyboard closes anyway.
	GFX_clearLayers(LAYER_ALL);

	// Don't let the button press that opened the keyboard leak in.
	PAD_reset();

	while (1) {
		GFX_startFrame();
		PAD_poll();

		KbLayout* l = kb_layout(shift, sym);
		bool confirm = false;

		// BTN_UP/DOWN/LEFT/RIGHT are composites (BTN_DPAD_* | BTN_ANALOG_*), so the left stick navigates too.
		// Up/Down keep the same spot across rows of different widths; the ends wrap on a fresh press (DESIGN 5.7).
		if (PAD_justRepeated(BTN_UP) || PAD_justRepeated(BTN_DOWN)) {
			int to = row;
			if (PAD_wrapNav(&to, UIKB_ROWS, BTN_UP, BTN_DOWN) > 0) {
				col = kb_col_at(l, to, kb_center(l, row, col));
				row = to;
			}
			dirty = true;
		} else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT)) {
			PAD_wrapNav(&col, kb_row_len(l, row), BTN_LEFT, BTN_RIGHT);
			dirty = true;
		} else if (PAD_justRepeated(BTN_B)) {
			if (len == 0) { // nothing to delete: a fresh B closes; a held B that emptied the field stops there
				if (PAD_justPressed(BTN_B)) {
					PAD_reset();
					return NULL;
				}
			} else dirty = kb_delete(input, &len, &caret);
		} else if (PAD_justRepeated(BTN_X)) {
			dirty = kb_insert_str(input, &len, &caret, " ");
		} else if (PAD_justPressed(BTN_Y)) {
			kb_thai_mode = !kb_thai_mode;
			shift = sym = caps = 0;
			dirty = true;
		} else if (PAD_justPressed(BTN_L1)) { // one-shot shift; twice quickly = caps lock; again = off (DESIGN 7.7)
			uint32_t now = SDL_GetTicks();
			if (caps)
				caps = shift = 0;
			else if (shift && now - l1_at < 400)
				caps = 1;
			else
				shift = !shift;
			l1_at = now;
			sym = 0;
			dirty = true;
		} else if (PAD_justPressed(BTN_R1)) { // the #+= page / back to abc
			sym = !sym;
			shift = caps = 0;
			dirty = true;
		} else if (PAD_justPressed(BTN_SELECT)) { // close, as B on an empty field
			PAD_reset();
			return NULL;
		} else if (PAD_justRepeated(BTN_L2)) {
			if (caret > 0) {
				caret = KbUtf8_prev(input, caret);
				dirty = true;
			}
		} else if (PAD_justRepeated(BTN_R2)) {
			if (caret < len) {
				caret = KbUtf8_next(input, caret, len);
				dirty = true;
			}
		} else if (PAD_justPressed(BTN_START)) {
			confirm = true;
		} else if (PAD_justRepeated(BTN_A)) {
			const char* key = (*l)[row][col];
			if (strcmp(key, UIKB_SPACE) == 0)
				dirty = kb_insert_str(input, &len, &caret, " ");
			else if (strcmp(key, UIKB_GO) == 0)
				confirm = true;
			else if (strcmp(key, UIKB_LANG) == 0) {
				kb_thai_mode = !kb_thai_mode;
				shift = sym = caps = 0;
				dirty = true;
			} else if (strcmp(key, UIKB_SYM) == 0) {
				sym = !sym;
				shift = caps = 0;
				dirty = true;
			} else if (strcmp(key, UIKB_SHIFT) == 0) {
				shift = !shift;
				sym = caps = 0;
				dirty = true;
			} else if (strcmp(key, UIKB_DEL) == 0)
				dirty = kb_delete(input, &len, &caret);
			else if (strcmp(key, UIKB_LEFT) == 0) {
				if (caret > 0)
					caret = KbUtf8_prev(input, caret);
				dirty = true;
			} else if (strcmp(key, UIKB_RIGHT) == 0) {
				if (caret < len)
					caret = KbUtf8_next(input, caret, len);
				dirty = true;
			} else {
				dirty = kb_insert_str(input, &len, &caret, key);
				if (!caps)
					shift = 0; // one-shot, as on phones (and YouTube's keyboard); caps lock stays
			}
		}
		if (confirm) {
			PAD_reset();
			if (len == 0)
				return NULL; // empty input = cancel (matches old wrapper)
			return strdup(input);
		}
		// a page with fewer keys in this row (shift / symbols / language changed)
		l = kb_layout(shift, sym);
		if (col >= kb_row_len(l, row))
			col = kb_row_len(l, row) - 1;

		PWR_update(&dirty, NULL, NULL, NULL);

		if (dirty) {
			UIKeyboardView v = {prompt, input, caret};
			kb_view(&v, l);
			v.row = row;
			v.col = col;
			v.shift = caps ? 2 : shift; // 2: caps lock
			v.sym = sym;
			v.search = kb_searchy(prompt);
			if (BOS_uiKeyboard) {
				BOS_uiKeyboard(screen, &v);
				GFX_flip(screen);
			} else
				kb_draw_plain(screen, &v);
			dirty = false;
		} else {
			GFX_sync();
		}
	}
}
