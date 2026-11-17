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
	// board v2-VideoTV: the logo's letters, the group
	out->sq_text = channels[i].name, out->sq_raw = true, out->art_wide = true;
	out->sub = channels[i].group;
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
	return BOS_columnsMoving();
}

// Render curated country list for browsing
void render_iptv_curated_countries(SDL_Surface* screen, IndicatorType show_setting,
								   const char* toast_message, uint32_t toast_time) {
	(void)show_setting;
	GFX_clear(screen);

	iptv_curated_countries_view.count = IPTV_curated_get_country_count();
	render_tv_columns(screen, NULL, 0, NULL, NULL, 0);
	UI_renderToast(screen, toast_message, toast_time);
}

// Render curated channels for a country
void render_iptv_curated_channels(SDL_Surface* screen, IndicatorType show_setting,
								  const char* country_code,
								  int selected, int* scroll_offset,
								  const int* sorted_indices, int sorted_count,
								  const char* toast_message, uint32_t toast_time) {
	(void)show_setting;
	render_tv_columns(screen, country_code, selected, scroll_offset, sorted_indices, sorted_count);
	UI_renderToast(screen, toast_message, toast_time);
}
