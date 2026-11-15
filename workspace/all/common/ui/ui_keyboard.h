#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

// Show the modal in-process on-screen keyboard, blocking until the user
// confirms or cancels. Returns a malloc'd string with the input, or NULL
// if cancelled or empty. Caller must free() the result.
// `prompt` is rendered as the label above the input field.
char* UIKeyboard_open(const char* prompt);

// UIKeyboard_openEx flags.
// KB_START_CANCELS: kept for the launcher's Search (#112), which START opens: START there confirms like every
// keyboard (DESIGN.md 7.6), and confirming an empty field closes it, so the same button still toggles it shut.
#define KB_START_CANCELS (1 << 0)
char* UIKeyboard_openEx(const char* prompt, int flags);

// UIKeyboard_openEx with the input line pre-filled with `initial` (NULL or ""
// for none; cut at a character boundary if longer than the input limit), for
// prompts that edit an existing value such as Rename.
char* UIKeyboard_openWith(const char* prompt, const char* initial, int flags);

// What the keyboard shows, for the drawing (Brick OS: BOS_uiKeyboard in bos_shared.c, DESIGN.md 7.6).
#define UIKB_ROWS 5
#define UIKB_COLS 12
// special keys
#define UIKB_SHIFT "⇧"
#define UIKB_DEL "⌫"
#define UIKB_LEFT "◀"
#define UIKB_RIGHT "▶"
#define UIKB_SPACE "Space"
#define UIKB_SYM "#+="
#define UIKB_LANG "ก/A"
#define UIKB_GO "Confirm"
typedef struct {
	const char* prompt;					 // the label over the field
	const char* input;
	int caret;							 // byte offset of the edit position
	const char* keys[UIKB_ROWS][UIKB_COLS]; // NULL ends a row
	int span[UIKB_ROWS][UIKB_COLS];		 // units each key takes
	int units;							 // the rows' width in units (10 English, 12 Thai)
	int row, col;						 // the focused key
	int shift, sym;						 // shift / the symbols page is on
	int search;							 // the confirm key reads "Search" (else "OK")
} UIKeyboardView;

#endif // UI_KEYBOARD_H
