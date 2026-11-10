// Brick OS UI 2.0 shared kit (DESIGN.md §4–§5): the pieces every Brick OS screen is drawn from, so the status
// bar, hint bar, text, icons and focus look identical in every app. Sizes come from bos_tokens.h only.
// Software-drawn onto an SDL_Surface (the apps' screen); positions are device px (use BOS_px for mockup px).
//
// Apps opt in from their Makefile:  SOURCE += $(BOS_UI_SRCS)   (needs api.c and text_shape.c, as nextui/settings have)
#ifndef BOS_UI_H
#define BOS_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "sdl.h"
#include "bos_tokens.h"

int BOS_px(float mockup_px); // mockup px (1024-wide panel) -> device px

// ---- theme (ธีมสี; defaults from bos_tokens.h) ----
typedef struct {
	uint32_t focus, onfocus, primary, hint_cap, bg;
} BosTheme;
const BosTheme* BOS_theme(void);

// ---- text: Inter for Latin, IBM Plex Sans Thai for any string with Thai; weight 400/500/600/700 ----
int BOS_textW(const char* s, int px, int weight);
int BOS_lineH(int px);
// Draws `s` with its line box's top at y. alpha 0..1 (BOS_DIM for a dimmed item). Returns the width drawn.
int BOS_text(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int y);
// Draws `s` centred on the line at y (cy = vertical centre).
int BOS_textMid(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int cy);
void BOS_fit(const char* s, int px, int weight, int max_w, char* out, int size); // cut with "…"
// Draws `s` wrapped to max_w (at spaces, else between Thai clusters), up to max_lines lines; a longer last line
// scrolls (BOS_textMarquee). Returns the height drawn.
int BOS_textWrap(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int y, int max_w,
				 int max_lines);
// A focused text too long for max_w: drawn clipped to it and scrolling (hold 1.2 s, 60 px/s, hold, back), centred on
// cy; a text that fits is drawn as BOS_textMid. Unfocused text keeps BOS_fit's "…". While one scrolls,
// BOS_marqueeActive() is true: the app keeps redrawing.
int BOS_textMarquee(SDL_Surface* dst, const char* s, int px, int weight, uint32_t rgb, float alpha, int x, int cy,
					int max_w);
bool BOS_marqueeActive(void);

// ---- shapes ----
uint32_t BOS_map(SDL_Surface* s, uint32_t rgb);
void BOS_rrect(SDL_Surface* s, int x, int y, int w, int h, int r, uint32_t rgb);

// ---- icons: res/bos/icons/<name>.png (white, 128 px, made by scripts/design/make_icons.py), tinted ----
void BOS_icon(SDL_Surface* dst, const char* name, uint32_t rgb, float alpha, int x, int y, int size);

// ---- status bar (§5.1): title left; music · BT · mute · WiFi · clock · battery right ----
void BOS_setMusicPlaying(bool playing); // the app that knows (nextui asks the music service) tells the kit
void BOS_statusBar(SDL_Surface* dst, const char* title);

// ---- hint bar (§5.2): right = A,B,X,Y order (A drawn rightmost), max 4; left = system buttons, max 2 ----
typedef struct {
	const char* btn;   // "A", "B", "X", "Y" (circle) or "MENU", "SELECT", "L1 R1" … (pill)
	const char* label; // already translated
} BosHint;
void BOS_hintBar(SDL_Surface* dst, const BosHint* right, int nright, const BosHint* left, int nleft);

// ---- large title (§5.5): title 56/700 at x 40, sub 30 text2 under it; returns the block's height ----
int BOS_largeTitle(SDL_Surface* dst, int y, const char* title, const char* sub);

int BOS_tweenMs(void); // the focus/carousel tween's length, from ตั้งค่า › หน้าตา › ภาพเคลื่อนไหว
void BOS_quit(void); // frees fonts and icons

#endif
