#ifndef __I18N_H__
#define __I18N_H__

// Brick OS: UI translation at the text-render boundary. GFX_renderText,
// GFX_measureText and GFX_renderTextWrapped pass every string through
// I18N_text, so an English UI string that has an entry in
// RES_PATH/lang/<lang>.txt is drawn translated with no change at its call
// site. The language is the first word of SHARED_USERDATA_PATH/lang ("th" when
// the file is missing; "en" turns translation off).
//
// lang/<lang>.txt: one entry per line, the English text, a TAB, the
// translation; "\n" in either half is a newline; lines starting with # are
// comments.

// The translation of s for the current language, or s itself.
const char* I18N_text(const char* s);

// Forget the loaded table and language (after the language setting changes).
void I18N_reload(void);

// Load a table straight from a file, for tests and tools (lang ignored).
int I18N_loadFile(const char* path);

#endif // __I18N_H__
