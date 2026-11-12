// scraper_libretro.c - box art from libretro-thumbnails when the build has no ScreenScraper developer key
// (DESIGN §6.7d). ScreenScraper refuses every request without one; thumbnails.libretro.com needs no account. It is
// keyed by the No-Intro / Redump name, so the ROM's file name is tried as it is, then with the usual region tags.
#include "scraper_libretro.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "api.h" // LOG_info
#include "ca_bundle.h"
#include "utils.h"

#define LR_BASE "https://thumbnails.libretro.com"

typedef struct {
	int id; // ScreenScraper system id (scraper_systems.c)
	const char* dir;
} LrSystem;

static const LrSystem lr_systems[] = {
	{3, "Nintendo - Nintendo Entertainment System"},
	{106, "Nintendo - Family Computer Disk System"},
	{4, "Nintendo - Super Nintendo Entertainment System"},
	{14, "Nintendo - Nintendo 64"},
	{9, "Nintendo - Game Boy"},
	{10, "Nintendo - Game Boy Color"},
	{12, "Nintendo - Game Boy Advance"},
	{15, "Nintendo - Nintendo DS"},
	{11, "Nintendo - Virtual Boy"},
	{211, "Nintendo - Pokemon Mini"},
	{2, "Sega - Master System - Mark III"},
	{21, "Sega - Game Gear"},
	{1, "Sega - Mega Drive - Genesis"},
	{19, "Sega - 32X"},
	{20, "Sega - Mega-CD - Sega CD"},
	{22, "Sega - Saturn"},
	{23, "Sega - Dreamcast"},
	{109, "Sega - SG-1000"},
	{57, "Sony - PlayStation"},
	{61, "Sony - PlayStation Portable"},
	{31, "NEC - PC Engine - TurboGrafx 16"},
	{114, "NEC - PC Engine CD - TurboGrafx-CD"},
	{105, "NEC - PC Engine SuperGrafx"},
	{72, "NEC - PC-FX"},
	{26, "Atari - 2600"},
	{40, "Atari - 5200"},
	{41, "Atari - 7800"},
	{28, "Atari - Lynx"},
	{27, "Atari - Jaguar"},
	{25, "SNK - Neo Geo Pocket"},
	{82, "SNK - Neo Geo Pocket Color"},
	{70, "SNK - Neo Geo CD"},
	{45, "Bandai - WonderSwan"},
	{46, "Bandai - WonderSwan Color"},
	{48, "Coleco - ColecoVision"},
	{115, "Mattel - Intellivision"},
	{113, "Microsoft - MSX"},
	{116, "Microsoft - MSX2"},
	{66, "Commodore - 64"},
	{64, "Commodore - Amiga"},
	{65, "Amstrad - CPC"},
	{76, "Sinclair - ZX Spectrum"},
	{104, "Magnavox - Odyssey2"},
	{29, "The 3DO Company - 3DO"},
	{102, "GCE - Vectrex"},
	{80, "Fairchild - Channel F"},
	{207, "Watara - Supervision"},
	{90, "Welback - Mega Duck"},
	{234, "PICO-8"},
	{222, "TIC-80"},
};

static const char* lrDir(int system_id) {
	for (size_t i = 0; i < sizeof(lr_systems) / sizeof(lr_systems[0]); i++)
		if (lr_systems[i].id == system_id)
			return lr_systems[i].dir;
	return NULL;
}

// Percent-encode for a URL path segment: letters, digits and -._~ stay, everything else (space, '(', ',' ...) is %XX.
static void pathEncode(const char* s, char* out, size_t size) {
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (const unsigned char* p = (const unsigned char*)s; *p && o + 4 < size; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p))
			out[o++] = *p;
		else {
			out[o++] = '%';
			out[o++] = hex[*p >> 4];
			out[o++] = hex[*p & 15];
		}
	}
	out[o] = 0;
}

// libretro's file names replace &*/:`<>?\| with '_'
static void lrName(const char* s, char* out, size_t size) {
	size_t o = 0;
	for (; *s && o + 1 < size; s++)
		out[o++] = strchr("&*/:`<>?\\|", *s) ? '_' : *s;
	out[o] = 0;
}

static void lrUrl(const char* dir, const char* kind, const char* name, char* out, size_t size) {
	char d[256], n[1024];
	pathEncode(dir, d, sizeof(d));
	pathEncode(name, n, sizeof(n));
	snprintf(out, size, LR_BASE "/%s/%s/%s.png", d, kind, n);
}

// HEAD request: true when the picture is there (HTTP 2xx)
static bool lrExists(const char* url) {
	char* q = shellQuoteAlloc(url);
	if (!q)
		return false;
	const char* ca = nx_ca_bundle_path();
	char* qca = ca ? shellQuoteAlloc(ca) : NULL;
	char cmd[4096];
	int n = snprintf(cmd, sizeof(cmd), "curl -s --fail -I -o /dev/null %s%s%s --connect-timeout 10 --max-time 20 %s",
					 qca ? "--cacert " : "-k", qca ? qca : "", " ", q);
	free(q);
	free(qca);
	if (n < 0 || (size_t)n >= sizeof(cmd))
		return false;
	return system(cmd) == 0;
}

// ---- matching against the console's box art list (DESIGN §6.7d) ----
// File names on a card are often not No-Intro names: "House of the Dead 2 The (USA)", "Patapon USA",
// "Konami Wai Wai World (Japan)_TH", "Dragon Ball Z Shin Budokai 2". Both sides are reduced to a key: the title
// before the first tag, letters and digits only, lower case, without the words "the" and "and" and a trailing
// region word. The console's list is one page (thumbnails.libretro.com/<console>/Named_Boxarts/), kept in /tmp
// so the rest of the queue reuses it.

#define LR_KEY_MAX 256

static const char* lr_trailing_regions[] = {"usa", "us", "europe", "eu", "eur", "japan", "jp", "jpn", "world", "th",
											  "thai", NULL};

// "The House of the Dead 2, The (USA) [TH]" -> "houseofdead2"
static void lrKey(const char* name, char* key, size_t size) {
	char title[512];
	snprintf(title, sizeof(title), "%s", name);
	// the title ends at the first tag
	for (char* p = title; *p; p++)
		if (*p == '(' || *p == '[') {
			*p = 0;
			break;
		}
	// words of letters and digits; "_" (libretro's '&') and every other mark split words
	char words[32][64];
	int n = 0, len = 0;
	for (const unsigned char* p = (const unsigned char*)title;; p++) {
		if (*p && isalnum(*p)) {
			if (n < 32 && len < 63)
				words[n][len++] = (char)tolower(*p);
			continue;
		}
		if (len && n < 32) {
			words[n][len] = 0;
			n++;
		}
		len = 0;
		if (!*p)
			break;
	}
	// "Patapon USA", "Mario_TH": a region word at the end is a tag
	while (n > 1) {
		bool region = false;
		for (int r = 0; lr_trailing_regions[r]; r++)
			if (!strcmp(words[n - 1], lr_trailing_regions[r]))
				region = true;
		if (!region)
			break;
		n--;
	}
	size_t o = 0;
	for (int i = 0; i < n; i++) {
		if (!strcmp(words[i], "the") || !strcmp(words[i], "and") || !strcmp(words[i], "a"))
			continue;
		for (const char* w = words[i]; *w && o + 1 < size; w++)
			key[o++] = *w;
	}
	key[o] = 0;
}

// Lower is better: the user's region tag when the ROM has one, else USA, World, Europe, Japan; versions that are not
// the plain release (Rev, Beta, Demo, Proto, Kiosk ...) after those.
static int lrRank(const char* name, const char* rom_tags) {
	int rank = 50;
	static const char* pref[] = {"(USA", "(World", "(Europe", "(Japan", NULL};
	for (int i = 0; pref[i]; i++)
		if (strstr(name, pref[i])) {
			rank = 10 + i * 5;
			break;
		}
	if (rom_tags[0]) {
		// the same first tag as the ROM, e.g. "(USA, Australia)"
		const char* t = strchr(name, '(');
		if (t && !strncmp(t, rom_tags, strcspn(rom_tags, ")") + 1))
			rank = 0;
	}
	static const char* worse[] = {"(Rev", "(Beta", "(Demo", "(Proto", "(Kiosk", "(Sample", "(Virtual Console",
								  "(Taikenban", "(PSN", "(Unl", "(Pirate", NULL};
	for (int i = 0; worse[i]; i++)
		if (strstr(name, worse[i]))
			rank += 20;
	return rank;
}

static int hexval(int c) {
	return isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
}

// The console's box art list, downloaded once into /tmp. True when it is there.
static bool lrList(int system_id, const char* dir, char* path, size_t size) {
	snprintf(path, size, "/tmp/scraper_lr_%d.html", system_id);
	struct stat st;
	if (stat(path, &st) == 0 && st.st_size > 1024)
		return true;
	char d[256], url[512], part[600];
	pathEncode(dir, d, sizeof(d));
	snprintf(url, sizeof(url), LR_BASE "/%s/Named_Boxarts/", d);
	snprintf(part, sizeof(part), "%s.part", path);
	if (!ScraperAPI_downloadFile(url, part))
		return false;
	return rename(part, path) == 0;
}

// The best list entry for rom_filename (without ".png"), or false when none has its key.
static bool lrMatch(const char* list_path, const char* base, char* out, size_t size) {
	char want[LR_KEY_MAX];
	lrKey(base, want, sizeof(want));
	if (!want[0])
		return false;
	const char* tag = strchr(base, '(');
	const char* rom_tags = tag ? tag : "";

	FILE* f = fopen(list_path, "r");
	if (!f)
		return false;
	char line[4096], name[1024], key[LR_KEY_MAX];
	int best = 1 << 30;
	out[0] = 0;
	while (fgets(line, sizeof(line), f)) {
		for (char* h = strstr(line, "href=\""); h; h = strstr(h, "href=\"")) {
			h += 6;
			char* end = strchr(h, '"');
			if (!end)
				break;
			// percent-decode the file name; only .png entries are pictures
			size_t o = 0;
			for (char* p = h; p < end && o + 1 < sizeof(name); p++) {
				if (*p == '%' && p + 2 < end && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
					name[o++] = (char)(hexval((unsigned char)p[1]) * 16 + hexval((unsigned char)p[2]));
					p += 2;
				} else
					name[o++] = *p;
			}
			name[o] = 0;
			h = end + 1;
			size_t nl = strlen(name);
			if (nl < 5 || strcasecmp(name + nl - 4, ".png") || strchr(name, '/'))
				continue;
			name[nl - 4] = 0;
			lrKey(name, key, sizeof(key));
			if (strcmp(key, want))
				continue;
			int rank = lrRank(name, rom_tags);
			if (rank < best) {
				best = rank;
				snprintf(out, size, "%s", name);
			}
		}
	}
	fclose(f);
	return out[0] != 0;
}

static void lrFill(const char* dir, const char* name, ScraperGameInfo* info) {
	lrUrl(dir, "Named_Boxarts", name, info->boxart_url, sizeof(info->boxart_url));
	lrUrl(dir, "Named_Snaps", name, info->screenshot_url, sizeof(info->screenshot_url));
	lrUrl(dir, "Named_Logos", name, info->wheel_url, sizeof(info->wheel_url));
	snprintf(info->game_name, sizeof(info->game_name), "%s", name);
	info->found = true;
}

bool ScraperLibretro_search(const char* rom_filename, int system_id, ScraperGameInfo* info) {
	memset(info, 0, sizeof(*info));
	const char* dir = lrDir(system_id);
	if (!dir || !rom_filename)
		return false;

	// the file name without its extension
	char base[512];
	snprintf(base, sizeof(base), "%s", rom_filename);
	char* dot = strrchr(base, '.');
	if (dot && dot != base)
		*dot = 0;

	char list[256], match[1024];
	if (lrList(system_id, dir, list, sizeof(list))) {
		if (!lrMatch(list, base, match, sizeof(match)))
			return false;
		lrFill(dir, match, info);
		LOG_info("Scraper (libretro list): %s -> %s\n", rom_filename, match);
		return true;
	}

	static const char* regions[] = {"", " (USA)", " (USA, Europe)", " (Europe)", " (World)", " (Japan)", NULL};
	bool tagged = strchr(base, '(') != NULL; // already a No-Intro name: only as it is
	char name[600], clean[600], url[2048];
	for (int r = 0; regions[r]; r++) {
		if (r > 0 && tagged)
			break;
		snprintf(name, sizeof(name), "%s%s", base, regions[r]);
		lrName(name, clean, sizeof(clean));
		lrUrl(dir, "Named_Boxarts", clean, url, sizeof(url));
		if (!lrExists(url)) {
			// some sets only have the snap: still a match when the snap is there
			char snap[2048];
			lrUrl(dir, "Named_Snaps", clean, snap, sizeof(snap));
			if (!lrExists(snap))
				continue;
			snprintf(info->screenshot_url, sizeof(info->screenshot_url), "%s", snap);
		} else {
			snprintf(info->boxart_url, sizeof(info->boxart_url), "%s", url);
			lrUrl(dir, "Named_Snaps", clean, info->screenshot_url, sizeof(info->screenshot_url));
		}
		// the snap and the logo may be missing: scrapeOne's downloads just fail for them
		lrUrl(dir, "Named_Logos", clean, info->wheel_url, sizeof(info->wheel_url));
		snprintf(info->game_name, sizeof(info->game_name), "%s", name);
		info->found = true;
		LOG_info("Scraper (libretro): %s -> %s\n", rom_filename, name);
		return true;
	}
	return false;
}
