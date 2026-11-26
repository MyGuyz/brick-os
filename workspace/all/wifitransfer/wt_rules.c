#include "wt_rules.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// From each pak's core (valid_extensions in the .so, or the standalone emulator's own list). One console per
// tag: the Sega folders that share GPGX get genesis_plus_gx's list.
static const struct {
	const char* tag;
	const char* exts;
	bool minarch;
} CONSOLES[] = {
	{"PS", "bin cue img mdf pbp toc cbn m3u chd iso exe", true},
	{"PSX", "exe psexe psf minipsf m3u cue bin img iso chd pbp ecm mds", true},
	{"GBA", "gba", true},
	{"MGBA", "gba", true},
	{"SGB", "gb gbc sgb", true},
	{"GB", "gb gbc dmg", true},
	{"GBC", "gb gbc dmg", true},
	{"FC", "nes fds unf unif", true},
	{"FDS", "fds", true},
	{"SFC", "smc sfc swc fig bs st", true},
	{"SUPA", "smc sfc swc fig", true},
	{"MD", "bin gen smd md 68k sgd", true},
	{"SMS", "sms bin", true},
	{"GG", "gg", true},
	{"SG1000", "sg sc bin", true},
	{"SEGACD", "cue iso chd m3u bin", true},
	{"32X", "32x bin", true},
	{"GPGX", "m3u mdx md smd gen bin cue iso chd bms sms gg sg 68k sgd", true},
	{"PCE", "pce cue ccd chd toc m3u", true},
	{"N64", "n64 v64 z64 bin u1 ndd", true},
	{"DC", "chd cdi elf cue gdi lst bin dat m3u", true},
	{"DCX", "chd cdi elf cue gdi lst bin dat m3u", true},
	{"FBN", "zip 7z cue ccd", true},
	{"NGP", "ngp ngc", true},
	{"NGPC", "ngp ngc ngpc npc", true},
	{"WSC", "ws wsc pc2", true},
	{"VB", "vb vboy bin", true},
	{"MSX", "rom ri mx1 mx2 dsk col sg sc sf cas m3u", true},
	{"CPC", "dsk sna tap cdt voc cpr m3u", true},
	{"COLECO", "col cv bin rom", true},
	{"A2600", "a26 bin", true},
	{"A5200", "a52 bin", true},
	{"A7800", "a78 bin cdf", true},
	{"LYNX", "lnx o", true},
	{"PKM", "min", true},
	{"PRBOOM", "wad iwad pwad lmp", true},
	{"C64", "d64 d71 d80 d81 d82 g64 g41 x64 t64 tap prg p00 crt bin d6z d7z d8z g6z g4z x6z cmd m3u vfl vsf nib nbz", true},
	{"C128", "d64 d71 d80 d81 d82 g64 g41 x64 t64 tap prg p00 crt bin d6z d7z d8z g6z g4z x6z cmd m3u vfl vsf nib nbz", true},
	{"PET", "d64 d71 d80 d81 d82 g64 g41 x64 t64 tap prg p00 crt bin d6z d7z d8z g6z g4z x6z cmd m3u vfl vsf nib nbz", true},
	{"PLUS4", "d64 d71 d80 d81 d82 g64 g41 x64 t64 tap prg p00 crt bin d6z d7z d8z g6z g4z x6z cmd m3u vfl vsf nib nbz", true},
	{"VIC", "d64 d71 d80 d81 d82 g64 g41 x64 t64 tap prg p00 crt bin d6z d7z d8z g6z g4z x6z cmd m3u vfl vsf nib nbz", true},
	{"PUAE", "adf adz dms fdi ipf hdf hdz lha slave info cue ccd nrg mds iso chd uae m3u rp9", true},
	{"DOS", "zip dosz exe com bat iso cue ins img ima vhd jrc tc m3u conf", true},
	// standalone emulators: only what they open themselves
	{"PSP", "iso cso chd pbp elf prx", false},
	{"NDS", "nds", false},
	{"P8", "p8 png", false},
};
#define NCONSOLES (int)(sizeof(CONSOLES) / sizeof(CONSOLES[0]))

static int find(const char* tag) {
	if (!tag)
		return -1;
	for (int i = 0; i < NCONSOLES; i++)
		if (!strcmp(CONSOLES[i].tag, tag))
			return i;
	return -1;
}

const char* wt_console_exts(const char* tag) {
	int i = find(tag);
	return i < 0 ? NULL : CONSOLES[i].exts;
}

bool wt_console_archives(const char* tag) {
	int i = find(tag);
	return i >= 0 && CONSOLES[i].minarch;
}

bool wt_console_saves(const char* tag) {
	int i = find(tag);
	return i >= 0 && (CONSOLES[i].minarch || !strcmp(tag, "NDS"));
}

void wt_ext(const char* name, char* out, size_t out_size) {
	if (!out_size)
		return;
	out[0] = '\0';
	const char* dot = name ? strrchr(name, '.') : NULL;
	if (!dot || dot == name || strchr(dot, '/'))
		return;
	size_t i = 0;
	for (const char* p = dot + 1; *p && i + 1 < out_size; p++)
		out[i++] = (char)tolower((unsigned char)*p);
	out[i] = '\0';
}

void wt_base(const char* name, char* out, size_t out_size) {
	if (!out_size)
		return;
	snprintf(out, out_size, "%s", name ? name : "");
	char* dot = strrchr(out, '.');
	if (dot && dot != out)
		*dot = '\0';
}

// ext is one of the space-separated words in list
static bool in_list(const char* list, const char* ext) {
	if (!list || !ext[0])
		return false;
	size_t n = strlen(ext);
	for (const char* p = list; *p;) {
		const char* e = strchr(p, ' ');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len == n && !strncmp(p, ext, n))
			return true;
		if (!e)
			break;
		p = e + 1;
	}
	return false;
}

static bool name_in(const char* name, const char* list) {
	char ext[16];
	wt_ext(name, ext, sizeof(ext));
	return in_list(list, ext);
}

bool wt_game_ok(const char* tag, const char* name) {
	int i = find(tag);
	if (i < 0)
		return false;
	if (name_in(name, CONSOLES[i].exts))
		return true;
	return CONSOLES[i].minarch && name_in(name, "zip 7z");
}

// musicplayer/music_format.c and mediaplayer/vp_defines.h
bool wt_music_ok(const char* name) { return name_in(name, "mp3 wav ogg opus flac m4a aac mod xm s3m it"); }
bool wt_video_ok(const char* name) { return name_in(name, "mp4 mkv avi webm mov ts flv m4v wmv mpg"); }
bool wt_cover_ok(const char* name) { return name_in(name, "jpg jpeg png webp"); }
bool wt_bios_ok(const char* name) { return name_in(name, "bin rom zip img dat sys bios"); }
bool wt_shot_ok(const char* name) { return name_in(name, "png jpg jpeg webp bmp"); }

bool wt_name_safe(const char* name) {
	if (!name || !name[0] || name[0] == '.' || strlen(name) > 255)
		return false;
	if (strstr(name, ".."))
		return false;
	for (const unsigned char* p = (const unsigned char*)name; *p; p++)
		if (*p < 0x20 || *p == '/' || *p == '\\' || *p == 0x7f)
			return false;
	return true;
}

bool wt_folder_tag(const char* folder, char* tag, size_t tag_size) {
	if (!folder || !tag_size)
		return false;
	size_t n = strlen(folder);
	if (n < 3 || folder[n - 1] != ')')
		return false;
	const char* open = strrchr(folder, '(');
	if (!open)
		return false;
	size_t len = (size_t)(folder + n - 1 - (open + 1));
	if (!len || len + 1 > tag_size)
		return false;
	snprintf(tag, tag_size, "%.*s", (int)len, open + 1);
	return true;
}

// minarch's SavePaths_stripExtension: drops an extension of 1 to 4 letters
static void strip_ext(char* name) {
	char* t = strrchr(name, '.');
	if (t && strlen(t) > 2 && strlen(t) <= 5)
		*t = '\0';
}

// The state slot an upload name means: 0-8, 9 for the auto slot; -1 when it is not a state.
static int state_slot(const char* name) {
	const char* p;
	size_t n = strlen(name);
	if (n > 11 && !strcasecmp(name + n - 11, ".state.auto"))
		return 9;
	if ((p = strrchr(name, '.'))) {
		if (!strcasecmp(p, ".state"))
			return 0;
		if (!strncasecmp(p, ".state", 6) && isdigit((unsigned char)p[6]) && !p[7])
			return p[6] - '0';
		if (!strncasecmp(p, ".st", 3) && isdigit((unsigned char)p[3]) && !p[4])
			return p[3] - '0';
		if (isdigit((unsigned char)p[1]) && !p[2] && p - name > 6 && !strncasecmp(p - 6, ".state", 6))
			return p[1] - '0'; // .state.3
	}
	return -1;
}

int wt_save_split(const char* file, char* base, size_t base_size) {
	if (!file || !base_size)
		return WT_SAVE_NONE;
	snprintf(base, base_size, "%s", file);
	size_t n = strlen(base);
	if (n > 11 && !strcasecmp(base + n - 11, ".state.auto")) {
		base[n - 11] = '\0';
		return WT_SAVE_STATE;
	}
	char* p = strrchr(base, '.');
	if (!p || p == base)
		return WT_SAVE_NONE;
	if (!strcasecmp(p, ".srm") || !strcasecmp(p, ".sav") || !strcasecmp(p, ".dsv") || !strcasecmp(p, ".rtc")) {
		*p = '\0';
		return WT_SAVE_BATTERY;
	}
	if (state_slot(file) >= 0) {
		*p = '\0';
		// "Game.state.3": the ".state" before the digit goes too
		size_t m = strlen(base);
		if (m > 6 && !strcasecmp(base + m - 6, ".state"))
			base[m - 6] = '\0';
		return WT_SAVE_STATE;
	}
	return WT_SAVE_NONE;
}

int wt_save_target(const char* tag, const char* upload_name, const char* rom_file, int save_format, int state_format,
				   char* out, size_t out_size) {
	if (!out_size)
		return WT_SAVE_NONE;
	out[0] = '\0';
	if (!wt_console_saves(tag) || !upload_name || !rom_file || !rom_file[0])
		return WT_SAVE_NONE;
	char ext[16], work[512];
	wt_ext(upload_name, ext, sizeof(ext));
	snprintf(work, sizeof(work), "%s", rom_file);
	bool nds = !strcmp(tag, "NDS");
	if (nds) { // DraStic: Game.dsv, no states it can take from here
		if (strcmp(ext, "dsv") && strcmp(ext, "sav"))
			return WT_SAVE_NONE;
		strip_ext(work);
		snprintf(out, out_size, "%s.dsv", work);
		return WT_SAVE_BATTERY;
	}
	if (!strcmp(ext, "srm") || !strcmp(ext, "sav")) {
		if (save_format == 0) // SAV: Game.gba.sav
			snprintf(out, out_size, "%s.sav", work);
		else {
			strip_ext(work);
			snprintf(out, out_size, "%s%s", work, save_format == 2 ? ".sav" : ".srm");
		}
		return WT_SAVE_BATTERY;
	}
	int slot = state_slot(upload_name);
	if (slot < 0)
		return WT_SAVE_NONE;
	if (state_format == 0) { // SAV: Game.gba.st0
		snprintf(out, out_size, "%s.st%d", work, slot);
		return WT_SAVE_STATE;
	}
	strip_ext(work);
	if (slot == 9)
		snprintf(out, out_size, "%s.state.auto", work);
	else if (state_format == 1 || state_format == 2)
		snprintf(out, out_size, "%s.state.%d", work, slot);
	else if (slot == 0)
		snprintf(out, out_size, "%s.state", work);
	else
		snprintf(out, out_size, "%s.state%d", work, slot);
	return WT_SAVE_STATE;
}
