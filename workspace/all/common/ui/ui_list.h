#ifndef UI_LIST_H
#define UI_LIST_H

#include <stdbool.h>
#include <stdint.h>
#include "sdl.h"

// Scrolling text state for marquee animation
typedef struct {
	char text[512];		 // Text to display
	int text_width;		 // Full text width in pixels
	int max_width;		 // Maximum display width
	uint32_t start_time; // Animation start time
	bool needs_scroll;	 // True if text is wider than max_width
	int scroll_offset;	 // Current pixel offset for smooth scrolling
	bool use_gpu_scroll; // True = use GPU layer (for lists), False = software (for player)
	int last_x, last_y;	 // Last render position (for animate-only mode)
	// Last font used (for animate-only mode, which draws with it on later frames): must be a long-lived font
	// (font.* from api.h), never a UIFont_get / UI_textRole pointer, which a later UIFont_get may evict and close.
	TTF_Font* last_font;
	SDL_Color last_color;				// Last color used (for animate-only mode)
	SDL_Surface* cached_scroll_surface; // Cached surface for GPU scroll (no bg)
	bool scroll_active;					// True once GPU scroll has actually started (after delay)
	bool rtl;							// True if the text's base direction is RTL (Arabic) — reverses scroll
} ScrollTextState;

void ScrollText_reset(ScrollTextState* state, const char* text,
					  TTF_Font* font, int max_width, bool use_gpu);
// Free the cached scroll surface and return the state to inactive/empty.
// Call when the scrolled row leaves the screen (screen switch, list exit).
void ScrollText_clear(ScrollTextState* state);
bool ScrollText_isScrolling(ScrollTextState* state);
bool ScrollText_needsRender(ScrollTextState* state);
void ScrollText_activateAfterDelay(ScrollTextState* state);
void ScrollText_animateOnly(ScrollTextState* state);
void ScrollText_render(ScrollTextState* state, TTF_Font* font,
					   SDL_Color color, SDL_Surface* screen, int x, int y);

// Unified update: checks for text change, resets if needed, and renders
// use_gpu: true for lists (GPU layer with pill bg), false for player (software, no bg)
void ScrollText_update(ScrollTextState* state, const char* text, TTF_Font* font,
					   int max_width, SDL_Color color, SDL_Surface* screen, int x, int y, bool use_gpu);

// ---- List Layout ----

// LIST-LAYOUT §10.2: a list is a block of whole rows with an arrow strip of equal height (half a row) directly
// above the first row and below the last; the arrows are centred in their strips, and the block is centred
// between the page title's letters (UI_pageTitleBandTop) and the hint bar's icons (UI_buttonHintIconTop). The
// math is the pure UI_listBlock (common/ui_list_layout.h).
typedef struct {
	int list_y;			// Y of the first row
	int list_h;			// Height of the rows (items_per_page * item_h)
	int item_h;			// Height per item
	int items_per_page; // Visible item count
	int max_width;		// Max content width (the pill from UI_listPillX to PADDING short of the right edge)
	int avail_top;		// band the block is centred in (kept so a caller can re-block, see
	int avail_bottom;	//   UI_listLayoutSetRowHeight)
	int strip;			// height of each arrow strip (0: none, an options page that fits)
	int up_y;			// centre y of the up arrow's strip
	int down_y;			// centre y of the down arrow's strip
	bool titled;		// the page has a title (true from UI_calcListLayout*); an untitled options page
						//   (UI_renderSettingsPage) keeps the pill list's top gutter instead of centring
} ListLayout;

// The standard pill list: SCALE1(PILL_SIZE) rows, as many as fit, under a page title and over a hint bar.
// The apps' main lists (a ListView in the large font): their text and row count on this device (ui_text_sizes.h
// TEXT_MENU_LIST, MENU_LIST_ROWS; 0 rows: as many as fit). The font is font.large where the sizes match.
TTF_Font* UI_mainListFont(void);

// The scroll arrows (ASSET_SCROLL_UP / _DOWN at the UI scale), white at UI_SCROLL_ARROW_ALPHA in their pixels' alpha:
// every list's arrows, the main menu's band included.
#define UI_SCROLL_ARROW_ALPHA 115 // 45%
SDL_Surface* UI_scrollArrow(bool up);

ListLayout UI_calcListLayout(SDL_Surface* screen);
// The same in a chosen band: avail_top < 0 is the title's letters, avail_bottom < 0 the hint icons. Rows of
// row_h; rows_wanted > 0 caps the count. For a list under its own header (ratools, Xtras tabs) pass the
// header's bottom as avail_top: the up strip then sits between the header and the first row.
ListLayout UI_calcListLayoutEx(SDL_Surface* screen, int avail_top, int avail_bottom, int row_h, int rows_wanted);
// Re-block an existing layout's band for another row height (rich rows: SCALE1(PILL_SIZE) * 3 / 2).
void UI_listLayoutSetRowHeight(ListLayout* layout, int row_h, int rows_wanted);

// LIST-LAYOUT §1/§10.1: a list row's text starts at UI_listTextX(); its selection pill's edge is at UI_listPillX(),
// 14 dp (SCALE1(BUTTON_PADDING)) left of the text: on the screen's left edge, or UI_listEdgeInset in. Every shared renderer below uses them, and
// the default page title x (UI_pageTitleX) is the same text start.
// The pill's end off the screen's sides (px): 0 on the Bricks, 6 dp on the Smart Pros.
int UI_listEdgeInset(void);
int UI_listTextX(void);
int UI_listPillX(void);
// The pill edge for a row whose text starts at text_x (14 dp left of it, never off screen): the main menu's 24 dp
// List rows use it too.
int UI_listPillXFor(int text_x);

// ---- Pill Rendering (stateless) ----

typedef struct {
	int pill_width;
	int text_x;
	int text_y;
} ListItemPos;

// `truncated` (here and in the UI_render*Pill* helpers below) is the caller's
// buffer of at least UI_LIST_TRUNC_SIZE bytes; the fitted text never exceeds it.
#define UI_LIST_TRUNC_SIZE 256
int UI_calcListPillWidth(TTF_Font* font, const char* text, char* truncated,
						 int max_width, int prefix_width);
void UI_drawListItemBg(SDL_Surface* dst, SDL_Rect* rect, bool selected);
SDL_Color UI_getListTextColor(bool selected);
// The List drawn on the GPU (nextui's game lists): the selection pill (UI_drawListItemBg's) and a row's text
// (UI_renderListItemText's without a marquee, UI_renderListItemTextDimSuffix's) added as sprites over the screen. False
// when one couldn't be (an uncacheable text): the caller draws it the software way.
bool UI_listItemBgSprites(const SDL_Rect* rect, const SDL_Rect* clip);
bool UI_listItemTextSprite(const char* text, TTF_Font* font, int text_x, int text_y, int max_text_width, bool selected);
// The text fits max_text_width as it is (a selected row needs no marquee: it can be a sprite as well).
bool UI_listItemTextFits(const char* text, TTF_Font* font, int max_text_width, bool selected);
bool UI_listItemTextDimSuffixSprite(const char* name, const char* suffix, TTF_Font* font, int text_x, int text_y,
									int max_text_width, bool selected);
// The selected row's marquee title while it holds still (the pre-scroll delay, or a title with nothing to scroll) as a
// sprite: ScrollText_render blits it into the screen, under the sprite pill that would hide it. Call after
// UI_renderListItemText with the same state; shows what that blit shows (the right edge of an overflowing RTL title).
// Returns the sprite's texture, to drop with PLAT_spriteRemove once the GPU scroll takes over between full frames; NULL
// when the state is scrolling (its layer draws the title) or no sprite could be made.
SDL_Texture* UI_listItemMarqueeHoldSprite(const ScrollTextState* state, TTF_Font* font, int text_x, int text_y,
										  bool selected);

// Render a list item's pill background and calculate text position
// Combines: UI_calcListPillWidth + UI_drawListItemBg + text position calculation
// prefix_width: extra width to account for (e.g., checkbox, indicator)
ListItemPos UI_renderListItemPill(SDL_Surface* screen, ListLayout* layout,
								  TTF_Font* font, const char* text,
								  char* truncated, int y, bool selected,
								  int prefix_width);

void UI_renderListItemText(SDL_Surface* screen, ScrollTextState* scroll_state,
						   const char* text, TTF_Font* font,
						   int text_x, int text_y, int max_text_width,
						   bool selected);

// Like UI_renderListItemText, but for a "unique" duplicate-name label whose
// text is `name` immediately followed by `suffix` (e.g. "Zelda" + " (GBA)").
// The name is drawn in the normal list colour and the suffix in
// COLOR_DARK_TEXT so the disambiguating part reads as secondary, matching
// upstream NextUI. Static (non-marquee) path only: callers use
// UI_renderListItemText with a scroll state for the marquee row.
void UI_renderListItemTextDimSuffix(SDL_Surface* screen,
									const char* name, const char* suffix,
									TTF_Font* font, int text_x, int text_y,
									int max_text_width, bool selected);

// ---- Settings Page Component ----

typedef struct {
	const char* label; // Left-side text
	const char* value; // Right-side text (NULL for none)
	int swatch;		   // Color swatch (-1 for none)
	int cycleable;	   // Show "< >" arrows when selected
	const char* desc;  // Description shown when item is selected
	void (*custom_draw)(SDL_Surface* screen, void* ctx,
						int x, int y, int w, int h, int selected);
	void* custom_draw_ctx;
} UISettingsItem;

// Render a compact settings (options) page. Handles layout calculation, scrolling, item
// rendering, scroll indicators, status message, and description text.
// LIST-LAYOUT §10.2 options pages: rows at 0.75 x the pill list pitch (UI_optionsRowHeight); a titled page that
// fits is centred with no arrow strips, one that scrolls has arrow-height + 2 dp strips; an untitled page
// (layout->titled = false) keeps the pill list's first-row y. Reserves 1.5 rows below the list for the selected
// item's description (two lines).
void UI_renderSettingsPage(SDL_Surface* screen, ListLayout* layout,
						   UISettingsItem* items, int count,
						   int selected, int* scroll,
						   const char* status_msg);

// Whether the settings page's description area cuts desc (more than its two lines on this screen): the page then
// offers the full text (Y MORE).
bool UI_settingsDescCut(SDL_Surface* screen, const char* desc);

// As above, but `desc_rows` sets how many rows are reserved for the
// description area (clamped to at least 1). A page with no per-item
// descriptions passes 1 to fit one more item.
void UI_renderSettingsPageEx(SDL_Surface* screen, ListLayout* layout,
							 UISettingsItem* items, int count,
							 int selected, int* scroll,
							 const char* status_msg, int desc_rows);

// ---- Scroll Helpers ----

void UI_adjustListScroll(int selected, int* scroll, int items_per_page);
// Arrows centred in a layout's strips (layout->up_y / down_y): up when scroll > 0, down when rows follow.
void UI_renderScrollIndicatorsAt(SDL_Surface* screen, const ListLayout* layout, int scroll,
								 int items_per_page, int total_count);
// The arrow assets centred horizontally on the screen with their centres at up_y / down_y.
void UI_renderScrollArrows(SDL_Surface* screen, int up_y, int down_y, bool show_up, bool show_down);

// ---- Pill Animation (non-threaded, for main-loop driven apps) ----

typedef struct {
	int current_y;
	int target_y;
	int start_y;
	// Width glides alongside y: a snap to the new row's width while y is still
	// travelling reads as a jump, not a glide (short -> long titles especially).
	int current_w;
	int target_w;
	int start_w;
	uint32_t start_time; // SDL_GetTicks() when the current glide began
	bool active;
} PillAnimState;

void UI_pillAnimSetTarget(PillAnimState* state, int target_y, int target_w, bool animate);
int UI_pillAnimTick(PillAnimState* state);
bool UI_pillAnimIsActive(PillAnimState* state);

// ---- Rich Row (LIST-LAYOUT §10.2 / §10.6) ----

// A thumbnail row: a full-round capsule whose left end holds a circular thumbnail 4 dp in from its left, top and
// bottom edges, with the thumbnail's left edge on the list text start (UI_listTextX, the page title's x), so the
// capsule edge sits 4 dp left of that. The text starts 14 dp after the thumbnail and ends half a row before the
// capsule's right end. Title = the secondary role (0.8 x the list label) capped at row / 2.16, second line 0.8 x
// the title (the caption role); each line's box is 1.2 x its size, the pair centred in the row (UI_richRowText).
// Row height: layout->item_h (use UI_listFitRowHeight for whole rows).
typedef struct {
	int capsule_x, capsule_w;	// the capsule (drawn when selected)
	int image_x, image_y;		// thumbnail top-left
	int image_size;				// thumbnail diameter (row - 2 x 4 dp); load it round (radius image_size / 2)
	int text_x, text_max_width; // both lines
	int title_y, second_y;		// glyph tops (the font height centred in its line box)
	int title_px, second_px;	// font sizes: fetch each with UIFont_getPx(px, false) right where it's drawn (a
								//   UIFont pointer is only valid until the next UIFont_get)
} RichRowPos;

// Thumbnail diameter for rows of row_h (load the images at this size).
int UI_richRowImageSize(int row_h);
// Draw the capsule when selected and return the positions; the caller blits the thumbnail and draws the text
// (UI_renderListItemText with UIFont_getPx(pos.title_px), then the second line in UIFont_getPx(pos.second_px)).
RichRowPos UI_renderRichRow(SDL_Surface* screen, const ListLayout* layout, const char* title,
							const char* second, int y, bool selected);

// ---- Menu Item Pill Rendering ----

// Position information returned by UI_renderMenuItemPill
typedef struct {
	int pill_width; // Width of the rendered pill
	int text_x;		// X position for text (after padding)
	int text_y;		// Y position for text (vertically centered in pill)
	int item_y;		// Y position of this menu item
} MenuItemPos;

// Render a menu item's pill background and calculate text position
// Menu items use PILL_SIZE height. index: menu item index (0-based)
// prefix_width: extra width to account for (e.g., icon)
MenuItemPos UI_renderMenuItemPill(SDL_Surface* screen, ListLayout* layout,
								  const char* text, char* truncated,
								  int index, bool selected, int prefix_width);

// ---- Rounded Rectangle Background ----

// Render a filled rounded rectangle background
// Works at any height (unlike pill asset which requires PILL_SIZE)
void UI_renderRoundedRectBg(SDL_Surface* screen, int x, int y, int w, int h, uint32_t color);

#endif // UI_LIST_H
