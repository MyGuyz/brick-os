#include "ui_confirmdialog.h"
#include "ui_draw.h"
#include "ui_pindialog.h"
#include "confirm_wrap.h"
#include "api.h"
#include "defines.h"

#include <string.h>
#include <stdio.h>

#define CONFIRM_MAX_SUB_LINES 4

void UI_renderConfirmDialog(SDL_Surface* dst, const char* title,
							const char* subtitle) {
	UI_renderConfirmDialogHints(dst, title, subtitle, NULL);
}

void BOS_uiConfirm(SDL_Surface* s, const char* title, const char* subtitle, char** pairs) __attribute__((weak));
void UI_renderConfirmDialogHints(SDL_Surface* dst, const char* title,
								 const char* subtitle, char** hints) {
	GFX_clearLayers(LAYER_SCROLLTEXT);
	BOS_uiConfirm(dst, title, subtitle, hints && !hints[0] ? hints : hints);
}

int UI_modalLoop(const UI_ModalOpts* o) {
	int result = -1;
	bool dirty = true;
	uint32_t start = SDL_GetTicks();
	if (o->clear_layers) {
		GFX_clearLayers(LAYER_ALL);
	} else {
		// a modal that keeps the layers under it still drops the GPU sprites, which composite above the screen and so
		// over the modal (GFX_clearLayers(LAYER_ALL) drops them too)
		PLAT_spritesClear();
	}
	while (1) {
		if (o->quit_flag && *o->quit_flag)
			break;
		if (o->timeout_ms && SDL_GetTicks() - start >= o->timeout_ms)
			break;
		GFX_startFrame();
		PAD_poll();
		int r = o->handle(o->ctx);
		if (r >= 0) {
			result = r;
			break;
		}
		if (r == UI_MODAL_DIRTY)
			dirty = true;
		// keep auto-sleep and the power button alive while the modal blocks
		PWR_update(&dirty, NULL, NULL, NULL);
		if (dirty) {
			o->render(o->screen, o->ctx);
			GFX_flip(o->screen);
			dirty = false;
		} else {
			GFX_delay();
		}
	}
	if (o->clear_layers)
		GFX_clearLayers(LAYER_ALL);
	if (o->reset_pad) {
		PAD_poll();
		PAD_reset();
	}
	return result;
}

typedef struct {
	const char* title;
	const char* subtitle;
	char** hints;
} UI_ConfirmModalCtx;

static void confirmModal_render(SDL_Surface* screen, void* vctx) {
	UI_ConfirmModalCtx* ctx = vctx;
	UI_renderConfirmDialogHints(screen, ctx->title, ctx->subtitle, ctx->hints);
}

static int confirmModal_handle(void* vctx) {
	(void)vctx;
	if (PAD_justPressed(BTN_A))
		return 1;
	if (PAD_justPressed(BTN_B))
		return 0;
	return UI_MODAL_CONTINUE;
}

bool UI_confirmModal(SDL_Surface* screen, const char* title, const char* subtitle,
					 const volatile bool* quit_flag, bool clear_layers, bool reset_pad) {
	return UI_confirmModalHints(screen, title, subtitle, NULL, quit_flag, clear_layers, reset_pad);
}

bool UI_confirmModalHints(SDL_Surface* screen, const char* title, const char* subtitle,
						  char** hints, const volatile bool* quit_flag, bool clear_layers,
						  bool reset_pad) {
	UI_ConfirmModalCtx ctx = {title, subtitle, hints};
	UI_ModalOpts opts = {
		.screen = screen,
		.render = confirmModal_render,
		.handle = confirmModal_handle,
		.ctx = &ctx,
		.quit_flag = quit_flag,
		.timeout_ms = 0,
		.clear_layers = clear_layers,
		.reset_pad = reset_pad,
	};
	return UI_modalLoop(&opts) == 1;
}

typedef struct {
	const char* title;
	const char* subtitle;
	char* buttons[7];
} UI_ChoiceModalCtx;

static void choiceModal_render(SDL_Surface* screen, void* vctx) {
	UI_ChoiceModalCtx* ctx = vctx;
	UI_renderConfirmDialogHints(screen, ctx->title, ctx->subtitle, ctx->buttons);
}

static int choiceModal_handle(void* vctx) {
	(void)vctx;
	if (PAD_justPressed(BTN_A))
		return UI_CHOICE_YES;
	if (PAD_justPressed(BTN_X))
		return UI_CHOICE_NO;
	if (PAD_justPressed(BTN_B))
		return UI_CHOICE_CANCEL;
	return UI_MODAL_CONTINUE;
}

int UI_choiceModal(SDL_Surface* screen, const char* title, const char* subtitle,
				   const volatile bool* quit_flag, bool clear_layers, bool reset_pad) {
	UI_ChoiceModalCtx ctx = {
		.title = title,
		.subtitle = subtitle,
		.buttons = {"B", "CANCEL", "X", "NO", "A", "YES", NULL},
	};
	UI_ModalOpts opts = {
		.screen = screen,
		.render = choiceModal_render,
		.handle = choiceModal_handle,
		.ctx = &ctx,
		.quit_flag = quit_flag,
		.timeout_ms = 0,
		.clear_layers = clear_layers,
		.reset_pad = reset_pad,
	};
	int result = UI_modalLoop(&opts);
	return result >= 0 ? result : UI_CHOICE_CANCEL;
}

typedef struct {
	char* pin_out;
} UI_PinModalCtx;

static void pinModal_render(SDL_Surface* screen, void* vctx) {
	(void)vctx;
	PinDialog_render(screen);
}

static int pinModal_handle(void* vctx) {
	UI_PinModalCtx* ctx = vctx;
	PinDialogResult r = PinDialog_handleInput();
	if (r.action == PINDIALOG_CONFIRMED) {
		snprintf(ctx->pin_out, PINDIALOG_PIN_LEN + 1, "%s", r.pin);
		return 1;
	}
	if (r.action == PINDIALOG_CANCEL)
		return 0;
	// any held/pressed button may have changed a digit
	return PAD_anyPressed() ? UI_MODAL_DIRTY : UI_MODAL_CONTINUE;
}

bool UI_pinModal(SDL_Surface* screen, const char* title, const char* error, char* pin_out,
				 const volatile bool* quit_flag, bool clear_layers, bool reset_pad) {
	PinDialog_init(title);
	PinDialog_setError(error);
	UI_PinModalCtx ctx = {pin_out};
	UI_ModalOpts opts = {
		.screen = screen,
		.render = pinModal_render,
		.handle = pinModal_handle,
		.ctx = &ctx,
		.quit_flag = quit_flag,
		.timeout_ms = 0,
		.clear_layers = clear_layers,
		.reset_pad = reset_pad,
	};
	bool confirmed = UI_modalLoop(&opts) == 1;
	PinDialog_quit();
	return confirmed;
}
