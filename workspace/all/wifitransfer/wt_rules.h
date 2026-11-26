// WiFi Transfer: what may go where (DESIGN §6.13). Pure string work, no file system: tested on the PC.
#ifndef WT_RULES_H
#define WT_RULES_H

#include <stdbool.h>
#include <stddef.h>

// The file types a console's player opens, from the cores' own valid extensions. NULL: not a console the
// page offers (Ports and unknown tags). The list is lowercase, separated by spaces.
const char* wt_console_exts(const char* tag);
// True when minarch runs this console, so a .zip / .7z is opened for it too.
bool wt_console_archives(const char* tag);
// True when the console keeps its saves in Saves/<tag> the minarch way (or DraStic's .dsv for NDS).
bool wt_console_saves(const char* tag);
// True when the game file type is allowed for that console (zip / 7z included where they work).
bool wt_game_ok(const char* tag, const char* name);

bool wt_music_ok(const char* name);
bool wt_video_ok(const char* name);
bool wt_cover_ok(const char* name); // jpg png webp (the page sends a PNG; this checks the name it shows)
bool wt_bios_ok(const char* name);
bool wt_shot_ok(const char* name);

// A save or a state, by its extension. The suffix the device uses comes back in out (with the dot):
// battery saves -> the save format's own ".srm" / ".sav" (dsv for NDS), states -> the state format's own.
enum { WT_SAVE_NONE = 0, WT_SAVE_BATTERY = 1, WT_SAVE_STATE = 2 };
// save_format / state_format are CFG_getSaveFormat() / CFG_getStateFormat(). rom_file is the game's own file
// name (the SAV format keeps its extension). out gets the whole file name to write.
int wt_save_target(const char* tag, const char* upload_name, const char* rom_file, int save_format, int state_format,
				   char* out, size_t out_size);
// The kind of an existing file in Saves/<tag> or the states folder, and the game base it belongs to
// ("Crash.state3" -> "Crash"). 0 when it is not a save or a state.
int wt_save_split(const char* file, char* base, size_t base_size);

// The file name part is safe to create: not empty, no '/', '\\', no "..", no leading '.', no control bytes,
// at most 255 bytes.
bool wt_name_safe(const char* name);
// "Sony PlayStation (PS)" -> "PS". False when the folder name has no "(TAG)" at the end.
bool wt_folder_tag(const char* folder, char* tag, size_t tag_size);
// "Crash (USA).cue" -> "Crash (USA)" (the last extension off).
void wt_base(const char* name, char* out, size_t out_size);
// The lowercase extension without the dot ("" for none) into out.
void wt_ext(const char* name, char* out, size_t out_size);

#endif
