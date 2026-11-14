#include <stdio.h>
#include <string.h>

#include "ui_list.h"
#include "api.h"
#include "ui_buttonhintbar.h"
#include "ui_menubar.h"
#include "ui_iptv.h"
#include "ui_fonts.h"
#include "ui_listview.h"
#include "bos_ui.h"
#include "i18n.h"
#include "ui_draw.h"
#include "ui_toast.h"
#include "iptv.h"
#include "iptv_curated.h"

// Full-mode ListViews (the widget owns selection, scroll, glide and marquee;
// module_iptv drives input through the accessors below).
static ListView iptv_user_channels_view;
static ListView iptv_curated_countries_view;

ListView* IPTVUserChannels_view(void) {
	return &iptv_user_channels_view;
}
ListView* IPTVCuratedCountries_view(void) {
	return &iptv_curated_countries_view;
}

static void iptv_user_get_row(void* ctx, int i, bool selected,
							  ListViewRow* out) {
	const IPTVChannel* channels = ctx;
	out->label = channels[i].name;
	if (UI_bos) { // board v2-VideoTV: the logo's letters, the group
		out->sq_text = channels[i].name, out->sq_raw = true, out->art_wide = true;
		out->sub = channels[i].group;
	}
	(void)selected;
}

static void iptv_countries_get_row(void* ctx, int i, bool selected,
								   ListViewRow* out) {
	// Channel counts are unknown until a country is fetched (lazy per-country
	// loading), so country rows show just the name.
	const CuratedTVCountry* countries = ctx;
	out->label = countries[i].name;
	(void)selected;
}

// Render user's channel list (main screen). The widget absorbs the empty
// state when count == 0.
void render_iptv_user_channels(SDL_Surface* screen, IndicatorType show_setting,
							   const char* toast_message, uint32_t toast_time) {
	(void)show_setting;
	GFX_clear(screen);

	UI_renderMenuBar(screen, "Media Player | Online TV");

	int channel_count = IPTV_getUserChannelCount();
	const IPTVChannel* channels = IPTV_getUserChannels();

	ListView* v = &iptv_user_channels_view;
	v->title = NULL; // menu bar drawn above (caller-owned chrome)
	v->font = font.medium;
	v->count = channel_count;
	v->get_row = iptv_user_get_row;
	v->ctx = (void*)channels;
	v->list_id = (const void*)channels;
	v->empty_title = "No channels saved";
	v->empty_subtitle = "Press A to browse channels";
	// Empty state carries its own centered buttons; the bottom hint bar only
	// appears with content. Browse/remove otherwise live in the MENU menu.
	v->empty_btn_pairs = (char*[]){"B", "BACK", "A", "MANAGE", NULL};
	v->hint_pairs = (channel_count > 0)
						? (char*[]){"B", "BACK", "A", "PLAY", NULL}
						: NULL;
	if (UI_bos) {
		static char sub[64];
		snprintf(sub, sizeof(sub), I18N_text("%d channels · needs WiFi"), channel_count);
		v->bos_title = I18N_text("Online TV"), v->bos_sub = sub;
		v->bos_empty_icon = "tv";
		static BosHint hr[2], hl[1];
		hr[0] = (BosHint){"A", I18N_text("Watch")};
		hr[1] = (BosHint){"B", I18N_text("Back")};
		hl[0] = (BosHint){"MENU", I18N_text("Options")};
		if (channel_count > 0)
			v->bos_hints = hr, v->bos_nhints = 2, v->bos_left = hl, v->bos_nleft = 1;
		else
			v->bos_hints = NULL, v->bos_nhints = 0, v->bos_left = NULL, v->bos_nleft = 0;
	}
	UI_listViewRender(v, screen);

	// Playback failure / status feedback set by module_iptv.
	UI_renderToast(screen, toast_message, toast_time);
}

// Brick OS (board v2-VideoTVAdd): countries left, the country's channels right
typedef struct {
	const CuratedTVChannel* ch;
	const int* order;
} TvAddCtx;
static void bos_tv_country_row(void* ctx, int i, bool hot, ListViewRow* r) {
	(void)ctx, (void)hot;
	r->label = IPTV_curated_get_countries()[i].name;
}
static void bos_tv_channel_row(void* ctx, int i, bool hot, ListViewRow* r) {
	TvAddCtx* a = ctx;
	const CuratedTVChannel* c = &a->ch[a->order[i]];
	(void)hot;
	r->label = c->name;
	r->sq_text = c->name, r->sq_raw = true, r->art_wide = true;
	r->sub = c->category;
	bool added = IPTV_userChannelExists(c->url);
	r->right_icon = added ? "checkc" : "plus";
	r->right_icon_colour = added ? BOS_C_OK : BOS_C_TEXT2;
}
static void render_tv_columns(SDL_Surface* screen, const char* code, int sel, int* scroll, const int* order, int n) {
	BosColumnsView v = {0};
	v.status = I18N_text("Add channels");
	v.nleft = IPTV_curated_get_country_count();
	v.left_sel = iptv_curated_countries_view.selected, v.left_scroll = &iptv_curated_countries_view.scroll;
	v.get_left = bos_tv_country_row;
	static BosHint hr[2];
	static char sub[96];
	TvAddCtx a = {0};
	if (code) {
		int count = 0;
		a.ch = IPTV_curated_get_channels(code, &count), a.order = order;
		const CuratedTVCountry* cs = IPTV_curated_get_countries();
		for (int i = 0; i < v.nleft; i++)
			if (strcmp(cs[i].code, code) == 0)
				v.title = cs[i].name;
		snprintf(sub, sizeof(sub), I18N_text("%d channels · from iptv-org · some may not play"), n);
		v.sub = sub;
		v.nright = n, v.right_sel = sel, v.right_scroll = scroll, v.right_focused = true;
		v.get_right = bos_tv_channel_row;
		bool added = n > 0 && sel < n && IPTV_userChannelExists(a.ch[order[sel]].url);
		hr[0] = (BosHint){"A", I18N_text(added ? "Remove" : "Add")};
	} else {
		v.title = I18N_text("Add channels");
		v.note = I18N_text("Pick a country to see its channels. They come from iptv-org; some may be offline or only in their country.");
		hr[0] = (BosHint){"A", I18N_text("Open")};
	}
	hr[1] = (BosHint){"B", I18N_text("Back")};
	v.hints = hr, v.nhints = 2;
	v.ctx = &a;
	iptv_curated_countries_view.input_pending = false;
	BOS_columns(screen, &v);
}

bool IPTVAdd_bosMoving(void) {
	return UI_bos && BOS_columnsMoving();
}

// Render curated country list for browsing
void render_iptv_curated_countries(SDL_Surface* screen, IndicatorType show_setting,
								   const char* toast_message, uint32_t toast_time) {
	(void)show_setting;
	GFX_clear(screen);

	if (UI_bos) {
		iptv_curated_countries_view.count = IPTV_curated_get_country_count();
		render_tv_columns(screen, NULL, 0, NULL, NULL, 0);
		UI_renderToast(screen, toast_message, toast_time);
		return;
	}
	UI_renderMenuBar(screen, "Media Player | Browse Channels");

	int country_count = IPTV_curated_get_country_count();
	const CuratedTVCountry* countries = IPTV_curated_get_countries();

	ListView* v = &iptv_curated_countries_view;
	v->title = NULL; // menu bar drawn above (caller-owned chrome)
	v->font = font.medium;
	v->count = country_count;
	v->get_row = iptv_countries_get_row;
	v->ctx = (void*)countries;
	v->list_id = (const void*)countries;
	// Catalog is fetched from iptv-org; an empty list means the fetch failed.
	v->empty_title = "Couldn't load channel list";
	v->empty_subtitle = "Press MENU to refresh, or check Wi-Fi";
	v->hint_pairs = (char*[]){"B", "BACK", "A", "SELECT", NULL};
	UI_listViewRender(v, screen);

	// Disclaimer / error feedback set by module_iptv.
	UI_renderToast(screen, toast_message, toast_time);
}

// Render curated channels for a country
void render_iptv_curated_channels(SDL_Surface* screen, IndicatorType show_setting,
								  const char* country_code,
								  int selected, int* scroll_offset,
								  const int* sorted_indices, int sorted_count,
								  const char* toast_message, uint32_t toast_time) {
	if (UI_bos) {
		(void)show_setting;
		render_tv_columns(screen, country_code, selected, scroll_offset, sorted_indices, sorted_count);
		UI_renderToast(screen, toast_message, toast_time);
		return;
	}
	GFX_clear(screen);

	int hw = screen->w;
	char truncated[256];

	// Get country name for title
	const char* country_name = "Channels";
	const CuratedTVCountry* countries = IPTV_curated_get_countries();
	int country_count = IPTV_curated_get_country_count();
	for (int i = 0; i < country_count; i++) {
		if (strcmp(countries[i].code, country_code) == 0) {
			country_name = countries[i].name;
			break;
		}
	}

	UI_renderMenuBarPage(screen, "Media Player", country_name);

	int channel_count = 0;
	const CuratedTVChannel* channels = IPTV_curated_get_channels(country_code, &channel_count);

	ListLayout layout = UI_calcListLayout(screen);
	UI_adjustListScroll(selected, scroll_offset, layout.items_per_page);

	// Determine if the currently selected channel is already added
	bool selected_exists = false;
	if (sorted_count > 0 && selected < sorted_count) {
		int sel_actual = sorted_indices[selected];
		if (sel_actual < channel_count) {
			selected_exists = IPTV_userChannelExists(channels[sel_actual].url);
		}
	}

	for (int i = 0; i < layout.items_per_page && *scroll_offset + i < sorted_count; i++) {
		int idx = *scroll_offset + i;
		int actual_idx = sorted_indices[idx];
		const CuratedTVChannel* channel = &channels[actual_idx];
		bool is_selected = (idx == selected);
		bool added = IPTV_userChannelExists(channel->url);

		int y = layout.list_y + i * layout.item_h;

		// Calculate prefix width for added indicator
		int prefix_width = 0;
		if (added) {
			int pw, ph;
			GFX_measureText(font.small, "[+]", &pw, &ph);
			prefix_width = pw + SCALE1(6);
		}

		// Render pill background and get text position
		int name_max_width = layout.max_width - prefix_width - SCALE1(60);
		int text_width = GFX_truncateText(font.medium, channel->name, truncated, sizeof(truncated), name_max_width, SCALE1(BUTTON_PADDING * 2));
		int pill_width = MIN(layout.max_width, prefix_width + text_width + SCALE1(BUTTON_PADDING));

		SDL_Rect pill_rect = {UI_listPillX(), y, pill_width, layout.item_h};
		Fonts_drawListItemBg(screen, &pill_rect, is_selected);

		int text_x = UI_listTextX();
		int text_y = y + (layout.item_h - TTF_FontHeight(font.medium)) / 2;

		// Added indicator prefix
		if (added) {
			SDL_Color prefix_color = Fonts_getListTextColor(is_selected);
			SDL_Surface* prefix_text = GFX_renderText(font.small, "[+]", prefix_color);
			if (prefix_text) {
				SDL_BlitSurface(prefix_text, NULL, screen, &(SDL_Rect){text_x, y + (layout.item_h - prefix_text->h) / 2});
				SDL_FreeSurface(prefix_text);
			}
		}

		// Channel name
		UI_renderListItemText(screen, NULL, channel->name, font.medium,
							  text_x + prefix_width, text_y, name_max_width, is_selected);

		// Category on right
		if (channel->category[0]) {
			SDL_Color cat_color = is_selected ? COLOR_GRAY : COLOR_DARK_TEXT;
			SDL_Surface* cat_text = GFX_renderText(font.tiny, channel->category, cat_color);
			if (cat_text) {
				SDL_BlitSurface(cat_text, NULL, screen, &(SDL_Rect){hw - cat_text->w - SCALE1(PADDING * 2), y + (layout.item_h - cat_text->h) / 2});
				SDL_FreeSurface(cat_text);
			}
		}
	}

	UI_renderScrollIndicatorsAt(screen, &layout, *scroll_offset, layout.items_per_page, sorted_count);

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);

	// Button hints - dynamic based on whether selected channel is already added
	if (selected_exists) {
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", "A", "REMOVE", NULL});
	} else {
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", "A", "ADD", NULL});
	}
}
