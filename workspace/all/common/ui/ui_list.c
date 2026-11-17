#include <stdio.h>
#include <string.h>
#include "ui_list.h"
#include "api.h"
#include "defines.h"
#include "config.h"
#include "ui_draw.h"
#include "text_shape.h"
#include "ui_menubar.h"
#include "ui_buttonhintbar.h"
#include "../ui_list_layout.h"
#include "../ui_text_sizes.h"
#include "ui_font.h"
#include "ui_accent.h"

// Scroll gap for software scrolling
#define SCROLL_GAP 30

// Delay before scrolling starts (ms) - show static text first
#define SCROLL_START_DELAY 1000

// ============================================
// Scroll Text (marquee animation)
// ============================================

void ScrollText_reset(ScrollTextState* state, const char* text, TTF_Font* font, int max_width, bool use_gpu) {
	GFX_clearLayers(LAYER_SCROLLTEXT);
	// The GPU marquee keeps its offset in platform statics that outlive this
	// state struct. Reset them here, not just in ScrollText_render's
	// activation branch: idle-driven hosts activate via
	// ScrollText_activateAfterDelay and never take that branch, so a stale
	// offset from the previous item would make the new marquee start mid-text.
	GFX_resetScrollText();

	if (state->cached_scroll_surface) {
		SDL_FreeSurface(state->cached_scroll_surface);
		state->cached_scroll_surface = NULL;
	}

	strncpy(state->text, text, sizeof(state->text) - 1);
	state->text[sizeof(state->text) - 1] = '\0';
	state->rtl = TextShape_baseIsRTL(state->text);
	int text_h = 0;
	GFX_measureText(font, state->text, &state->text_width, &text_h);
	state->max_width = max_width;
	state->start_time = SDL_GetTicks();
	// RTL starts right-aligned (window at the text's right edge); LTR at 0.
	state->scroll_offset = (state->rtl && state->text_width > max_width)
							   ? state->text_width - max_width
							   : 0;
	state->use_gpu_scroll = use_gpu;
	state->scroll_active = false;
	state->needs_scroll = false;

	if ((state->text_width > max_width) && use_gpu) {
		int padding = SCALE1(SCROLL_GAP);
		int total_width = state->text_width * 2 + padding;
		int height = TTF_FontHeight(font);

		state->cached_scroll_surface = SDL_CreateRGBSurfaceWithFormat(0,
																	  total_width, height, 32, SDL_PIXELFORMAT_ARGB8888);

		if (state->cached_scroll_surface) {
			SDL_FillRect(state->cached_scroll_surface, NULL, 0);

			SDL_Color white = {255, 255, 255, 255};
			SDL_Surface* text_surf = GFX_renderText(font, state->text, white);
			if (text_surf) {
				SDL_SetSurfaceBlendMode(text_surf, SDL_BLENDMODE_NONE);
				SDL_BlitSurface(text_surf, NULL, state->cached_scroll_surface, &(SDL_Rect){0, 0, 0, 0});
				SDL_BlitSurface(text_surf, NULL, state->cached_scroll_surface, &(SDL_Rect){state->text_width + padding, 0, 0, 0});
				SDL_FreeSurface(text_surf);
			}
		}
	}
}

void ScrollText_clear(ScrollTextState* state) {
	if (state->cached_scroll_surface) {
		SDL_FreeSurface(state->cached_scroll_surface);
		state->cached_scroll_surface = NULL;
	}
	state->text[0] = '\0';
	state->needs_scroll = false;
	state->scroll_active = false;
}

bool ScrollText_isScrolling(ScrollTextState* state) {
	return state->needs_scroll;
}

bool ScrollText_needsRender(ScrollTextState* state) {
	return state->text[0] && state->text_width > state->max_width && !state->needs_scroll;
}

void ScrollText_activateAfterDelay(ScrollTextState* state) {
	if (!state->needs_scroll && state->text_width > state->max_width &&
		SDL_GetTicks() - state->start_time >= SCROLL_START_DELAY) {
		state->needs_scroll = true;
	}
}

void ScrollText_animateOnly(ScrollTextState* state) {
	if (!state->text[0] || !state->needs_scroll || !state->use_gpu_scroll)
		return;
	if (!state->last_font)
		return;

	GFX_clearLayers(LAYER_SCROLLTEXT);
	GFX_scrollTextTexture(
		state->last_font,
		state->text,
		state->last_x, state->last_y,
		state->max_width,
		TTF_FontHeight(state->last_font),
		state->last_color,
		1.0f,
		state->rtl,
		NULL);
}

void ScrollText_render(ScrollTextState* state, TTF_Font* font, SDL_Color color,
					   SDL_Surface* screen, int x, int y) {
	if (!state->text[0])
		return;

	state->last_x = x;
	state->last_y = y;
	state->last_font = font;
	state->last_color = color;

	if (!state->needs_scroll && state->text_width > state->max_width &&
		SDL_GetTicks() - state->start_time >= SCROLL_START_DELAY) {
		if (state->use_gpu_scroll && !state->scroll_active) {
			GFX_resetScrollText();
			state->scroll_active = true;
		} else {
			state->needs_scroll = true;
		}
	}

	if (!state->needs_scroll) {
		GFX_clearLayers(LAYER_SCROLLTEXT);
		SDL_Surface* surf = GFX_renderText(font, state->text, color);
		if (surf) {
			// RTL overflow (pre-scroll hold): show the right edge (the start of
			// the Arabic) rather than the left edge.
			int src_x = (state->rtl && surf->w > state->max_width) ? surf->w - state->max_width : 0;
			SDL_Rect src = {src_x, 0, surf->w > state->max_width ? state->max_width : surf->w, surf->h};
			SDL_BlitSurface(surf, &src, screen, &(SDL_Rect){x, y, 0, 0});
			SDL_FreeSurface(surf);
		}
		return;
	}

	if (state->use_gpu_scroll) {
		GFX_clearLayers(LAYER_SCROLLTEXT);
		GFX_scrollTextTexture(
			font,
			state->text,
			x, y,
			state->max_width,
			TTF_FontHeight(font),
			color,
			1.0f,
			state->rtl,
			NULL);
	} else {
		GFX_clearLayers(LAYER_SCROLLTEXT);

		SDL_Surface* single_surf = GFX_renderText(font, state->text, color);
		if (!single_surf)
			return;

		SDL_Surface* full_surf = SDL_CreateRGBSurfaceWithFormat(0,
																state->text_width * 2 + SCROLL_GAP, single_surf->h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (!full_surf) {
			SDL_FreeSurface(single_surf);
			return;
		}

		SDL_FillRect(full_surf, NULL, 0);
		SDL_SetSurfaceBlendMode(single_surf, SDL_BLENDMODE_NONE);
		SDL_BlitSurface(single_surf, NULL, full_surf, &(SDL_Rect){0, 0, 0, 0});
		SDL_BlitSurface(single_surf, NULL, full_surf, &(SDL_Rect){state->text_width + SCROLL_GAP, 0, 0, 0});
		SDL_FreeSurface(single_surf);

		if (state->rtl) {
			state->scroll_offset -= 2; // window moves left -> text scrolls right
			if (state->scroll_offset < 0)
				state->scroll_offset += state->text_width + SCROLL_GAP;
		} else {
			state->scroll_offset += 2;
			if (state->scroll_offset >= state->text_width + SCROLL_GAP)
				state->scroll_offset = 0;
		}

		SDL_SetSurfaceBlendMode(full_surf, SDL_BLENDMODE_BLEND);
		SDL_Rect src = {state->scroll_offset, 0, state->max_width, full_surf->h};
		SDL_Rect dst = {x, y, 0, 0};
		SDL_BlitSurface(full_surf, &src, screen, &dst);
		SDL_FreeSurface(full_surf);
	}
}

void ScrollText_update(ScrollTextState* state, const char* text, TTF_Font* font,
					   int max_width, SDL_Color color, SDL_Surface* screen, int x, int y, bool use_gpu) {
	// Reset on width change too, not just text change: the async thumbnail
	// arriving narrows the row mid-selection, and a stale max_width leaves
	// the scroll band wider than the pill it sits on (and the needs-scroll
	// decision measured against the wrong width).
	if (strcmp(state->text, text) != 0 || state->max_width != max_width) {
		ScrollText_reset(state, text, font, max_width, use_gpu);
	}
	ScrollText_render(state, font, color, screen, x, y);
}

// ============================================
// List Layout
// ============================================

ListLayout UI_calcListLayoutEx(SDL_Surface* screen, int avail_top, int avail_bottom, int row_h, int rows_wanted) {
	ListLayout layout;
	layout.avail_top = avail_top >= 0 ? avail_top : UI_pageTitleBandTop();
	layout.avail_bottom = avail_bottom >= 0 ? avail_bottom : UI_buttonHintIconTop(screen->h);
	// the pill runs from UI_listPillX() to PADDING short of the right edge (symmetric with the flush-left pill)
	layout.max_width = screen->w - SCALE1(PADDING) - UI_listEdgeInset() - UI_listPillX();
	layout.item_h = row_h;
	layout.titled = true;
	UI_listLayoutSetRowHeight(&layout, row_h, rows_wanted);
	return layout;
}

ListLayout UI_calcListLayout(SDL_Surface* screen) {
	return UI_calcListLayoutEx(screen, -1, -1, SCALE1(PILL_SIZE), 0);
}

void UI_listLayoutSetRowHeight(ListLayout* layout, int row_h, int rows_wanted) {
	UIListBlock b = UI_listBlock(layout->avail_top, layout->avail_bottom, row_h, rows_wanted);
	layout->item_h = row_h;
	layout->items_per_page = b.rows;
	layout->list_y = b.top;
	layout->list_h = b.rows * row_h;
	layout->strip = b.strip;
	layout->up_y = b.up_y;
	layout->down_y = b.down_y;
}

// The pill's end off the screen's sides: none on the Bricks (the pill's edge on the screen's), 6 dp on the Smart Pros,
// whose panel edge clips a pill's end flush with it.
int UI_listEdgeInset(void) {
	return UIScale_deviceIndex(UI_DEVICE_NAME) == UI_DEVICE_SMARTPRO ? NX_DP(6) : 0;
}

int UI_listTextX(void) {
	return UI_listEdgeInset() + SCALE1(BUTTON_PADDING); // the pill's padding in from its edge
}

int UI_listPillXFor(int text_x) {
	// The pill pads its text by SCALE1(BUTTON_PADDING) (14 dp), so its edge sits that far left.
	int x = text_x - SCALE1(BUTTON_PADDING);
	return x > 0 ? x : 0;
}

int UI_listPillX(void) {
	return UI_listPillXFor(UI_listTextX());
}

// ============================================
// Pill Rendering (stateless)
// ============================================

int UI_calcListPillWidth(TTF_Font* font, const char* text, char* truncated, int max_width, int prefix_width) {
	int available_width = max_width - prefix_width;
	int padding = SCALE1(BUTTON_PADDING * 2);

	int raw_text_w = 0;
	if (GFX_fitTextCached(font, text, truncated, UI_LIST_TRUNC_SIZE, available_width, padding, &raw_text_w))
		return max_width;
	return MIN(max_width, prefix_width + raw_text_w + padding);
}

// The selection pill's fill: the accent, screen-mapped like the THEME_COLORn it replaces (the GFX_*Color blits decode
// it with the screen's format).
static uint32_t accentPill(void) {
	return UI_accentMapped(GFX_getScreen()->format);
}

void UI_drawListItemBg(SDL_Surface* dst, SDL_Rect* rect, bool selected) {
	if (selected) {
		GFX_blitPillColor(ASSET_WHITE_PILL, dst, rect, accentPill(), RGB_WHITE);
	}
}

bool UI_listItemBgSprites(const SDL_Rect* rect, const SDL_Rect* clip) {
	return GFX_pillSprites(ASSET_WHITE_PILL, rect, accentPill(), clip);
}

// A cached text surface as a sprite at (x, y), cut at max_w px and clipped to clip (NULL: none). False when the text
// isn't cacheable (the caller draws it the software way).
static bool textSprite(TTF_Font* font, const char* text, SDL_Color color, int x, int y, int max_w, const SDL_Rect* clip,
					   int* drawn_w) {
	SDL_Surface* surf = GFX_getCachedText(font, text, color);
	SDL_Texture* tex = surf ? PLAT_textureForSurface(surf) : NULL;
	if (!tex)
		return false;
	int w = surf->w > max_w ? max_w : surf->w;
	if (w > 0)
		PLAT_spriteAdd(tex, &(SDL_Rect){0, 0, w, surf->h}, &(SDL_Rect){x, y, w, surf->h}, 255, clip);
	if (drawn_w)
		*drawn_w = surf->w;
	return true;
}

bool UI_listItemTextSprite(const char* text, TTF_Font* font, int text_x, int text_y, int max_text_width,
						   bool selected) {
	if (!text || !text[0])
		return true;
	SDL_Rect clip = {text_x, text_y, max_text_width, TTF_FontHeight(font)};
	return textSprite(font, text, UI_getListTextColor(selected), text_x, text_y, max_text_width, &clip, NULL);
}

bool UI_listItemTextFits(const char* text, TTF_Font* font, int max_text_width, bool selected) {
	if (!text || !text[0])
		return true;
	SDL_Surface* surf = GFX_getCachedText(font, text, UI_getListTextColor(selected));
	return surf && surf->w <= max_text_width;
}

bool UI_listItemTextDimSuffixSprite(const char* name, const char* suffix, TTF_Font* font, int text_x, int text_y,
									int max_text_width, bool selected) {
	SDL_Rect clip = {text_x, text_y, max_text_width, TTF_FontHeight(font)};
	int name_w = 0;
	if (name && name[0] &&
		!textSprite(font, name, UI_getListTextColor(selected), text_x, text_y, max_text_width, &clip, &name_w))
		return false;
	if (suffix && suffix[0] && name_w < max_text_width)
		return textSprite(font, suffix, COLOR_DARK_TEXT, text_x + name_w, text_y, max_text_width - name_w, &clip,
						  NULL);
	return true;
}

SDL_Texture* UI_listItemMarqueeHoldSprite(const ScrollTextState* state, TTF_Font* font, int text_x, int text_y,
										  bool selected) {
	if (!state->text[0] || state->needs_scroll)
		return NULL;
	SDL_Color color = UI_getListTextColor(selected);
	SDL_Surface* surf = GFX_getCachedText(font, state->text, color);
	if (!surf) {
		// a title too long for the text cache: one owned render, kept until the title or its colour changes
		static SDL_Surface* own = NULL;
		static char own_text[sizeof(state->text)];
		static SDL_Color own_color;
		static TTF_Font* own_font = NULL;
		if (!own || own_font != font || strcmp(own_text, state->text) != 0 || own_color.r != color.r ||
			own_color.g != color.g || own_color.b != color.b || own_color.a != color.a) {
			if (own) {
				PLAT_freeSurfaceTexture(own);
				SDL_FreeSurface(own);
			}
			own = GFX_renderText(font, state->text, color);
			strcpy(own_text, state->text);
			own_color = color;
			own_font = font;
		}
		surf = own;
	}
	SDL_Texture* tex = surf ? PLAT_textureForSurface(surf) : NULL;
	if (!tex)
		return NULL;
	int max_w = state->max_width;
	int w = surf->w > max_w ? max_w : surf->w;
	// as ScrollText_render's hold: an overflowing RTL title shows its right edge (the start of the Arabic)
	int src_x = (state->rtl && surf->w > max_w) ? surf->w - max_w : 0;
	if (w > 0)
		PLAT_spriteAdd(tex, &(SDL_Rect){src_x, 0, w, surf->h}, &(SDL_Rect){text_x, text_y, w, surf->h}, 255,
					   &(SDL_Rect){text_x, text_y, max_w, TTF_FontHeight(font)});
	return tex;
}

SDL_Color UI_getListTextColor(bool selected) {
	return selected ? UI_onAccent() : uintToColour(THEME_COLOR4_255);
}

ListItemPos UI_renderListItemPill(SDL_Surface* screen, ListLayout* layout,
								  TTF_Font* font, const char* text,
								  char* truncated, int y, bool selected,
								  int prefix_width) {
	ListItemPos pos;

	pos.pill_width = UI_calcListPillWidth(font, text, truncated, layout->max_width, prefix_width);

	SDL_Rect pill_rect = {UI_listPillX(), y, pos.pill_width, layout->item_h};
	UI_drawListItemBg(screen, &pill_rect, selected);

	pos.text_x = UI_listPillX() + SCALE1(BUTTON_PADDING);
	pos.text_y = y + (layout->item_h - TTF_FontHeight(font)) / 2;

	return pos;
}

// Intersect the current clip with a list-item text run at (text_x, text_y) of
// size (max_text_width x font height) and set it, saving the previous clip in
// *old_clip. Returns false (clip left untouched) when the intersection is
// empty, so the caller can skip drawing. Pair with list_text_end_clip.
static bool list_text_begin_clip(SDL_Surface* screen, int text_x, int text_y, int max_text_width, TTF_Font* font, SDL_Rect* old_clip) {
	SDL_GetClipRect(screen, old_clip);
	SDL_Rect clip = {text_x, text_y, max_text_width, TTF_FontHeight(font)};
	if (old_clip->w > 0 && old_clip->h > 0) {
		int left = clip.x > old_clip->x ? clip.x : old_clip->x;
		int top = clip.y > old_clip->y ? clip.y : old_clip->y;
		int right = (clip.x + clip.w) < (old_clip->x + old_clip->w) ? (clip.x + clip.w) : (old_clip->x + old_clip->w);
		int bottom = (clip.y + clip.h) < (old_clip->y + old_clip->h) ? (clip.y + clip.h) : (old_clip->y + old_clip->h);
		if (right > left && bottom > top) {
			clip = (SDL_Rect){left, top, right - left, bottom - top};
		} else {
			return false;
		}
	}
	SDL_SetClipRect(screen, &clip);
	return true;
}

// Restore the clip saved by list_text_begin_clip.
static void list_text_end_clip(SDL_Surface* screen, const SDL_Rect* old_clip) {
	if (old_clip->w > 0 && old_clip->h > 0)
		SDL_SetClipRect(screen, old_clip);
	else
		SDL_SetClipRect(screen, NULL);
}

void UI_renderListItemText(SDL_Surface* screen, ScrollTextState* scroll_state,
						   const char* text, TTF_Font* font,
						   int text_x, int text_y, int max_text_width,
						   bool selected) {
	SDL_Color text_color = UI_getListTextColor(selected);

	SDL_Rect old_clip;
	if (!list_text_begin_clip(screen, text_x, text_y, max_text_width, font, &old_clip))
		return;

	if (selected && scroll_state) {
		// Logical text + primary font: the marquee's own renders go through the
		// Arabic-aware GFX_renderText/measureText (single shaping point), and
		// ScrollText picks the RTL scroll direction from the text itself.
		ScrollText_update(scroll_state, text, font, max_text_width,
						  text_color, screen, text_x, text_y, true);
	} else {
		// Cached surface (owned by the cache — do NOT free). Falls back to a
		// one-off render+free only for oversized strings the cache rejects.
		SDL_Surface* text_surf = GFX_getCachedText(font, text, text_color);
		bool owned = false;
		if (!text_surf) {
			text_surf = GFX_renderText(font, text, text_color);
			owned = true;
		}
		if (text_surf) {
			SDL_Rect src = {0, 0, text_surf->w > max_text_width ? max_text_width : text_surf->w, text_surf->h};
			SDL_BlitSurface(text_surf, &src, screen, &(SDL_Rect){text_x, text_y, 0, 0});
			if (owned)
				SDL_FreeSurface(text_surf);
		}
	}

	list_text_end_clip(screen, &old_clip);
}

void UI_renderListItemTextDimSuffix(SDL_Surface* screen,
									const char* name, const char* suffix,
									TTF_Font* font, int text_x, int text_y,
									int max_text_width, bool selected) {
	SDL_Rect old_clip;
	if (!list_text_begin_clip(screen, text_x, text_y, max_text_width, font, &old_clip))
		return;

	// Name in the normal list colour. Cached surface is owned by the cache
	// (do NOT free); the fallback render is owned and freed after blitting.
	int name_w = 0;
	SDL_Color name_color = UI_getListTextColor(selected);
	SDL_Surface* name_surf = GFX_getCachedText(font, name, name_color);
	bool name_owned = false;
	if (!name_surf) {
		name_surf = GFX_renderText(font, name, name_color);
		name_owned = true;
	}
	if (name_surf) {
		SDL_Rect src = {0, 0, name_surf->w > max_text_width ? max_text_width : name_surf->w, name_surf->h};
		SDL_BlitSurface(name_surf, &src, screen, &(SDL_Rect){text_x, text_y, 0, 0});
		name_w = name_surf->w;
		if (name_owned)
			SDL_FreeSurface(name_surf);
	}

	// Suffix (the disambiguator) dimmed in COLOR_DARK_TEXT, right after the
	// name — only while some width remains. src.w is clamped to what's left.
	if (suffix && suffix[0] && name_w < max_text_width) {
		SDL_Surface* suffix_surf = GFX_getCachedText(font, suffix, COLOR_DARK_TEXT);
		bool suffix_owned = false;
		if (!suffix_surf) {
			suffix_surf = GFX_renderText(font, suffix, COLOR_DARK_TEXT);
			suffix_owned = true;
		}
		if (suffix_surf) {
			int avail = max_text_width - name_w;
			SDL_Rect src = {0, 0, suffix_surf->w > avail ? avail : suffix_surf->w, suffix_surf->h};
			SDL_BlitSurface(suffix_surf, &src, screen, &(SDL_Rect){text_x + name_w, text_y, 0, 0});
			if (suffix_owned)
				SDL_FreeSurface(suffix_surf);
		}
	}

	list_text_end_clip(screen, &old_clip);
}

// ============================================
// Badged Pill Rendering
// ============================================

// ============================================
// Settings Page Component
// ============================================

void BOS_uiSettingsPage(SDL_Surface* s, UISettingsItem* items, int count, int selected, int* scroll,
						const char* status_msg);
void UI_renderSettingsPageEx(SDL_Surface* screen, ListLayout* layout,
							 UISettingsItem* items, int count,
							 int selected, int* scroll,
							 const char* status_msg, int desc_rows) {
	if (count == 0)
		return;
	// Brick OS: Settings' rows (ui_draw.h)
	(void)layout, (void)desc_rows;
	BOS_uiSettingsPage(screen, items, count, selected, scroll, status_msg);
}

bool UI_settingsDescCut(SDL_Surface* screen, const char* desc) {
	if (!screen || !desc || !desc[0])
		return false;
	int max_w = screen->w - SCALE1(PADDING * 2); // as the area draws it
	char wrapped[256];
	snprintf(wrapped, sizeof(wrapped), "%s", desc);
	if (strlen(desc) >= sizeof(wrapped))
		return true; // the area's own copy is cut there too
	if (!strchr(wrapped, '\n'))
		GFX_wrapText(font.tiny, wrapped, max_w, 0); // unlimited: every line it would take
	int lines = 0;
	for (char* line = wrapped; line; lines++) {
		char* nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		int w = 0;
		GFX_measureText(font.tiny, line, &w, NULL);
		if (w > max_w || lines >= 2)
			return true; // a line the area truncates, or one past its two
		line = nl ? nl + 1 : NULL;
	}
	return false;
}

// Default: reserve two rows for the (up to two-line) description area.
void UI_renderSettingsPage(SDL_Surface* screen, ListLayout* layout,
						   UISettingsItem* items, int count,
						   int selected, int* scroll,
						   const char* status_msg) {
	UI_renderSettingsPageEx(screen, layout, items, count, selected, scroll,
							status_msg, 2);
}

// ============================================
// Settings Row Rendering
// ============================================

#define SETTINGS_ROW_PADDING 8

// ============================================
// GPU Scroll Without Background
// ============================================

// ============================================
// Scroll Helpers
// ============================================

void UI_adjustListScroll(int selected, int* scroll, int items_per_page) {
	if (selected < *scroll) {
		*scroll = selected;
	}
	if (selected >= *scroll + items_per_page) {
		*scroll = selected - items_per_page + 1;
	}
}

TTF_Font* UI_mainListFont(void) {
	int px = (int)TextPx_for(TEXT_MENU_LIST, UIScale_deviceIndex(UI_DEVICE_NAME));
	TTF_Font* f = px > 0 && px != SCALE1(FONT_LARGE) ? UIFont_getPx(px, false) : NULL;
	return f ? f : font.large;
}

// The arrow art is dark grey (0x26, drawn for light grounds): its coverage kept, in white, at UI_SCROLL_ARROW_ALPHA.
// Baked into the pixels (not an alpha mod), so a blend onto a cleared layer and the layer's own blend can't apply it
// twice. Built once (the scale is fixed for the process).
SDL_Surface* UI_scrollArrow(bool up) {
	static SDL_Surface* arrows[2] = {NULL, NULL};
	SDL_Surface** a = &arrows[up ? 0 : 1];
	if (*a)
		return *a;
	int asset = up ? ASSET_SCROLL_UP : ASSET_SCROLL_DOWN;
	SDL_Rect r;
	GFX_assetRect(asset, &r);
	if (r.w <= 0 || r.h <= 0)
		return NULL;
	SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, r.w, r.h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s)
		return NULL;
	SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 255, 255, 255, 0));
	GFX_blitAsset(asset, NULL, s, &(SDL_Rect){0, 0});
	if (SDL_MUSTLOCK(s))
		SDL_LockSurface(s);
	for (int y = 0; y < s->h; y++) {
		Uint32* px = (Uint32*)((Uint8*)s->pixels + y * s->pitch);
		for (int x = 0; x < s->w; x++)
			px[x] = (((px[x] >> 24) * UI_SCROLL_ARROW_ALPHA + 127) / 255) << 24 | 0x00FFFFFF;
	}
	if (SDL_MUSTLOCK(s))
		SDL_UnlockSurface(s);
	SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_BLEND);
	*a = s;
	return s;
}

void UI_renderScrollArrows(SDL_Surface* screen, int up_y, int down_y, bool show_up, bool show_down) {
	// the arrow assets are 24 x 6 logical
	int ox = (screen->w - SCALE1(24)) / 2;
	int half = SCALE1(6) / 2;
	SDL_Surface* up = show_up ? UI_scrollArrow(true) : NULL;
	SDL_Surface* down = show_down ? UI_scrollArrow(false) : NULL;
	if (up)
		SDL_BlitSurface(up, NULL, screen, &(SDL_Rect){ox, up_y - half});
	if (down)
		SDL_BlitSurface(down, NULL, screen, &(SDL_Rect){ox, down_y - half});
}

void UI_renderScrollIndicatorsAt(SDL_Surface* screen, const ListLayout* layout, int scroll,
								 int items_per_page, int total_count) {
	if (total_count <= items_per_page)
		return;
	UI_renderScrollArrows(screen, layout->up_y, layout->down_y, scroll > 0,
						  scroll + items_per_page < total_count);
}

// ============================================
// Pill Animation (non-threaded, for main-loop driven apps)
// ============================================

// Duration of the selection pill's glide to a new row, in milliseconds. The
// glide is time-based (elapsed vs SDL_GetTicks) so it lasts the same wall-clock
// time no matter how fast the caller redraws — lower = snappier. Kept at 150ms:
// the number of interpolation frames is duration / frame time, and a nextui
// list frame measures ~30ms on device (vsync-locked, ~2 refresh periods of
// CPU+GPU work — see 2026-08-10 frame profiling). 80ms yielded ~3 frames and
// read as a snap; 150ms gives ~5 evenly-paced frames, which with the
// smoothstep easing below reads as a glide while staying responsive.
#define PILL_ANIM_MS 150


void UI_pillAnimSetTarget(PillAnimState* state, int target_y, int target_w, bool animate) {
	if (target_y == state->current_y && target_w == state->current_w && !state->active)
		return;

	// "Show menu animations" (Settings > Appearance) turns the glide off for
	// every list at this single choke point: the game list, ListView screens
	// and the search results all retarget through here.
	if (!animate || !CFG_getMenuAnimations()) {
		state->current_y = target_y;
		state->target_y = target_y;
		state->current_w = target_w;
		state->target_w = target_w;
		state->active = false;
		return;
	}

	state->start_y = state->current_y;
	state->target_y = target_y;
	state->start_w = state->current_w;
	state->target_w = target_w;
	state->start_time = SDL_GetTicks();
	state->active = true;
}

int UI_pillAnimTick(PillAnimState* state) {
	if (!state->active)
		return state->current_y;

	uint32_t elapsed = SDL_GetTicks() - state->start_time;
	if (elapsed >= PILL_ANIM_MS) {
		state->current_y = state->target_y;
		state->current_w = state->target_w;
		state->active = false;
		return state->current_y;
	}

	float t = (float)elapsed / PILL_ANIM_MS;
	// smoothstep (ease-in-out): peak velocity mid-glide so the travel itself is
	// visible. The previous ease-out quad front-loaded ~75% of the distance into
	// the first frames, which read as a snap rather than a glide.
	t = t * t * (3.0f - 2.0f * t);
	state->current_y = state->start_y + (int)((state->target_y - state->start_y) * t);
	state->current_w = state->start_w + (int)((state->target_w - state->start_w) * t);
	return state->current_y;
}

bool UI_pillAnimIsActive(PillAnimState* state) {
	return state->active;
}

// ============================================
// List Selection Glide (shared wiring around the pill animation)
// ============================================

// ============================================
// Rich Pill Rendering
// ============================================

// ============================================
// Rich Row (LIST-LAYOUT §10.2 / §10.6)
// ============================================

int UI_richRowImageSize(int row_h) {
	int s = row_h - 2 * NX_DP(4);
	return s > 0 ? s : 0;
}

RichRowPos UI_renderRichRow(SDL_Surface* screen, const ListLayout* layout, const char* title,
							const char* second, int y, bool selected) {
	RichRowPos pos;
	int row_h = layout->item_h;
	int pad = NX_DP(4);
	UIRichRowText t = UI_richRowText(row_h, SCALE1(FONT_LARGE));

	pos.image_x = UI_listTextX();
	pos.capsule_x = pos.image_x - pad;
	if (pos.capsule_x < 0)
		pos.capsule_x = 0;
	pos.image_y = y + pad;
	pos.image_size = UI_richRowImageSize(row_h);
	pos.text_x = pos.image_x + pos.image_size + NX_DP(NX_RICH_LIST_GAP_DP);

	pos.title_px = t.title_px;
	pos.second_px = t.second_px;
	// each font is fetched, measured and dropped before the next UIFont_getPx (a pointer is only valid until then)
	int content_w = 0;
	TTF_Font* f = UIFont_getPx(t.title_px, false);
	pos.title_y = y + t.title_top + (t.title_box - (f ? TTF_FontHeight(f) : t.title_box)) / 2;
	if (f && title && title[0])
		GFX_measureText(f, title, &content_w, NULL);
	f = UIFont_getPx(t.second_px, false);
	pos.second_y = y + t.second_top + (t.second_box - (f ? TTF_FontHeight(f) : t.second_box)) / 2;
	if (f && second && second[0]) {
		int w = 0;
		GFX_measureText(f, second, &w, NULL);
		if (w > content_w)
			content_w = w;
	}

	// the capsule hugs the longer line, its text ending half a row before the right end
	int max_w = screen->w - SCALE1(PADDING) - pos.capsule_x;
	pos.capsule_w = (pos.text_x - pos.capsule_x) + content_w + row_h / 2;
	if (pos.capsule_w > max_w)
		pos.capsule_w = max_w;
	pos.text_max_width = pos.capsule_x + pos.capsule_w - row_h / 2 - pos.text_x;
	if (pos.text_max_width < 0)
		pos.text_max_width = 0;

	if (selected)
		UI_fillRoundedRect(screen, pos.capsule_x, y, pos.capsule_w, row_h, row_h / 2, accentPill());
	return pos;
}

// ============================================
// Menu Item Pill Rendering
// ============================================

MenuItemPos UI_renderMenuItemPill(SDL_Surface* screen, ListLayout* layout,
								  const char* text, char* truncated,
								  int index, bool selected, int prefix_width) {
	MenuItemPos pos;

	int item_h = SCALE1(PILL_SIZE);
	pos.item_y = layout->list_y + index * item_h;

	pos.pill_width = UI_calcListPillWidth(font.large, text, truncated, layout->max_width - prefix_width, prefix_width);

	SDL_Rect pill_rect = {UI_listPillX(), pos.item_y, pos.pill_width, SCALE1(PILL_SIZE)};
	UI_drawListItemBg(screen, &pill_rect, selected);

	pos.text_x = UI_listPillX() + SCALE1(BUTTON_PADDING);
	pos.text_y = pos.item_y + (SCALE1(PILL_SIZE) - TTF_FontHeight(font.large)) / 2;

	return pos;
}

// ============================================
// Rounded Rectangle Background
// ============================================

void UI_renderRoundedRectBg(SDL_Surface* screen, int x, int y, int w, int h, uint32_t color) {
	UI_fillRoundedRect(screen, x, y, w, h, SCALE1(7), color);
}
