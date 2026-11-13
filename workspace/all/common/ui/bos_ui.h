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
// BOS_BENCH=1: prints the ms since the last lap as "name=ms " on stderr (name NULL only restarts the clock).
// Costs one test when the switch is off. With GFX_flip's "[bosbench]" line this says where a frame's time goes.
void BOS_benchLap(const char* name);

// ---- theme (ธีมสี; defaults from bos_tokens.h) ----
typedef struct {
	uint32_t focus, onfocus;	 // สีตอนเลือก and the text drawn on it
	uint32_t primary, onprimary; // สีหลัก (switches, main buttons) and the text on it
	uint32_t hint_cap, onhint;	 // สีปุ่มแถบล่าง (the hint bar's key caps) and the letter on them
	uint32_t bg;				 // พื้นหลัง
	uint32_t surface, surface2;	 // groups and controls on them (one step lighter on a dark grey page)
	int icon_mode;				 // สีไอคอน: 0 per app / category, 1 white, 2 primary
} BosTheme;
const BosTheme* BOS_theme(void);			  // read from the shared config on every call
uint32_t BOS_iconColor(uint32_t own);		  // an app / category icon colour after สีไอคอน
double BOS_contrast(uint32_t a, uint32_t b);  // WCAG contrast ratio, 1..21
uint32_t BOS_textOn(uint32_t bg, int mode);	  // 0 auto (the more readable of onfocus / white), 1 white, 2 black,
											  // THEME_TEXT_RGB|rgb any colour
void BOS_preset(int i, uint32_t* focus, uint32_t* primary); // ธีมสำเร็จรูป 0-5 (0 = ค่าเริ่มต้น)

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
// A focused text too long for max_w: drawn clipped to it and scrolling (hold 0.5 s, 120 px/s, hold 1 s, back), centred on
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

// ---- menu sounds (§5.10, bos_sfx.c) ----
typedef enum {
	BOS_SFX_NONE,
	BOS_SFX_MOVE,	// focus moved
	BOS_SFX_OK,		// A
	BOS_SFX_BACK,	// B
	BOS_SFX_BUMP,	// a held direction reached the end
	BOS_SFX_ON,		// a switch went on
	BOS_SFX_OFF,	// a switch went off
	BOS_SFX_OPEN,	// a sheet came up
	BOS_SFX_LAUNCH, // a game or app starts
	BOS_SFX_COUNT
} BosSfx;
void BOS_sfxInit(void);		   // after PAD_init. From then every press makes its usual sound by itself
void BOS_sfx(BosSfx id);	   // the sound for the press being handled, instead of the usual one (NONE: silence)
void BOS_sfxFlush(void);	   // plays the usual sound now (before a long render) instead of at the next poll
void BOS_sfxQuit(int wait_ms); // lets the playing sound finish, up to wait_ms

// ---- motion (§5.7, bos_motion.c) ----
float BOS_ease(float t);		// 0..1 -> 0..1, quick off the mark, soft landing
int BOS_motionMs(int full_ms);	// the time a move takes under ภาพเคลื่อนไหว: full, 120 at most (น้อย), 0 (ปิด)
typedef enum {
	BOS_TR_PUSH,  // a page deeper: in from the right
	BOS_TR_POP,	  // back: out to the right
	BOS_TR_FADE,  // one page into another
	BOS_TR_ARRIVE // an app's first frame, from black
} BosTrans;
// Before the new page is drawn, while the screen still holds the old one. area NULL = the whole screen.
void BOS_transBegin(SDL_Surface* s, BosTrans kind, const SDL_Rect* area);
bool BOS_transFrame(SDL_Surface* s); // after every draw of the page: puts the move's frame on s. True while moving
bool BOS_transFrameOnly(SDL_Surface* s); // the same, for a frame whose page was NOT drawn again (s is as it was)
bool BOS_transMoving(void);
bool BOS_transActive(void);
void BOS_transCancel(void);
bool BOS_transHold(SDL_Surface* s, int max_ms); // the new page is not ready: the old one stays on s. False: go
void BOS_launchOut(SDL_Surface* s, const SDL_Rect* focus); // a game or app starts: 260 ms, blocking
void BOS_bump(int dir); // a held direction has nowhere to go (-1 up/left, 1 down/right): the knock and the nudge
int BOS_bumpPx(void);	// the nudge now, to add to the highlight's place
bool BOS_bumpActive(void);
typedef struct {
	bool set;
	float from, to, cur;
	uint32_t t0;
} BosGlide;
// A value following its target (a highlight's place, a scroll, a switch's knob); snap lands at once.
float BOS_glideTo(BosGlide* g, float target, int full_ms, bool snap);
bool BOS_glideMoving(const BosGlide* g);
bool BOS_motionBusy(void); // something is still moving: keep drawing frames
// A frame of which only the rows [y, y+h) differ from the last one drawn (a carousel sliding, a highlight gliding).
// key: a number made of everything that shows OUTSIDE those rows; ok: the page allows it this frame. True: the
// surface is clipped to the band (draw only what is in it); false: draw the whole page. Always followed by
// BOS_partialEnd once the page is drawn. BOS_clip: a page's own clip, kept inside the band.
bool BOS_partialBegin(SDL_Surface* s, uint32_t key, bool ok, int y, int h);
bool BOS_partialBeginR(SDL_Surface* s, uint32_t key, bool ok, const SDL_Rect* r); // the same, any rectangle
void BOS_partialEnd(SDL_Surface* s);
void BOS_partialCancel(void); // something else is drawn over the page this frame: the whole screen goes up
bool BOS_partialOn(void);
void BOS_clip(SDL_Surface* s, const SDL_Rect* r); // NULL: back to the band (or the whole surface)
uint32_t BOS_keyMix(uint32_t key, uint32_t v);
// A picture (ARGB8888, with its own alpha) onto the page at opacity a, inside the page's clip
void BOS_blit(SDL_Surface* pic, SDL_Surface* s, int x, int y, float a);

#endif
