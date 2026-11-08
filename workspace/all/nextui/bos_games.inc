// เกม (DESIGN.md 6.3): consoles on the left, the highlighted console's games on the right. Included at the end of
// home_bos.c (it shares its fonts, text and drawing helpers).

#define FAV_PATH COLLECTIONS_PATH "/Favorites.txt"
#define GAME_ROWS 6

typedef struct {
	char path[MAX_PATH]; // console folder, or FAV_PATH
	char name[128];
	int count;
} BgConsole;
static BgConsole* bg_cons;
static int bg_ncons, bg_sel, bg_top;
static Directory* bg_games; // the selected console's list (Directory_new)
static int bg_games_for = -1, bg_gsel, bg_gtop;
static bool bg_right; // focus in the games column
static bool bg_built;

bool BosGames_active(void) {
	return stack && stack->count == 1 && MenuTabs_current() == MENU_TAB_CONSOLES && !GameSwitcher_isList();
}

static void bgBuild(void) {
	k = FIXED_WIDTH / 1024.0f;
	free(bg_cons);
	bg_cons = NULL;
	bg_ncons = 0;
	Directory* root = Directory_new((char*)ROMS_PATH, 0);
	int n = root && root->entries ? root->entries->count : 0;
	bg_cons = calloc((size_t)n + 1, sizeof(BgConsole));
	if (!bg_cons) {
		if (root)
			Directory_free(root);
		return;
	}
	int favs = countLines(FAV_PATH);
	if (favs > 0) {
		snprintf(bg_cons[0].path, MAX_PATH, "%s", FAV_PATH);
		snprintf(bg_cons[0].name, sizeof(bg_cons[0].name), "%s", I18N_text("Favorites"));
		bg_cons[0].count = favs;
		bg_ncons = 1;
	}
	for (int i = 0; i < n; i++) {
		Entry* e = root->entries->items[i];
		BgConsole* c = &bg_cons[bg_ncons];
		snprintf(c->path, MAX_PATH, "%s", e->path);
		snprintf(c->name, sizeof(c->name), "%s", e->name);
		c->count = -1;
		bg_ncons++;
	}
	// game counts from the launcher's own index (path \t name \t count)
	FILE* f = fopen(USERDATA_PATH "/emulist_cache.txt", "r");
	if (f) {
		char line[MAX_PATH * 2];
		while (fgets(line, sizeof(line), f)) {
			if (line[0] == '#')
				continue;
			char* t1 = strchr(line, '\t');
			if (!t1)
				continue;
			*t1 = '\0';
			char* t2 = strchr(t1 + 1, '\t');
			if (!t2)
				continue;
			for (int i = 0; i < bg_ncons; i++)
				if (!strcmp(bg_cons[i].path, line))
					bg_cons[i].count = atoi(t2 + 1);
		}
		fclose(f);
	}
	if (root)
		Directory_free(root);
	if (bg_sel >= bg_ncons)
		bg_sel = bg_ncons > 0 ? bg_ncons - 1 : 0;
	bg_built = true;
}

static void bgLoadGames(void) {
	if (bg_games_for == bg_sel && bg_games)
		return;
	if (bg_games)
		Directory_free(bg_games);
	bg_games = bg_ncons > 0 ? Directory_new(bg_cons[bg_sel].path, 0) : NULL;
	// other OSes' art folders and game lists left in a console's folder (Imgs_*, miyoogamelist.xml) aren't games
	if (bg_games && bg_games->entries) {
		Array* a = bg_games->entries;
		int w = 0;
		for (int i = 0; i < a->count; i++) {
			Entry* e = a->items[i];
			const char* base = strrchr(e->path, '/');
			base = base ? base + 1 : e->path;
			bool junk = (e->type == ENTRY_DIR && !strncasecmp(base, "Imgs", 4)) || !strncasecmp(base, "miyoogamelist", 13);
			if (junk)
				Entry_free(e);
			else
				a->items[w++] = e;
		}
		a->count = w;
	}
	bg_games_for = bg_sel;
	bg_gsel = bg_gtop = 0;
}

void BosGames_reset(void) {
	bg_built = false;
	bg_games_for = -1;
}

// Favorites: a collection (Collections/Favorites.txt, SD-relative ROM paths), so the Collections tab shows it too
static void toggleFavorite(const char* rom_path) {
	const char* rel = rom_path + strlen(SDCARD_PATH);
	static char lines[512][MAX_PATH];
	int n = 0;
	bool had = false;
	FILE* f = fopen(FAV_PATH, "r");
	if (f) {
		char line[MAX_PATH];
		while (n < 512 && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (!line[0])
				continue;
			if (!strcmp(line, rel)) {
				had = true;
				continue;
			}
			snprintf(lines[n++], MAX_PATH, "%s", line);
		}
		fclose(f);
	}
	if (!had && n < 512)
		snprintf(lines[n++], MAX_PATH, "%s", rel);
	mkdir(COLLECTIONS_PATH, 0755);
	if (n == 0) {
		unlink(FAV_PATH);
		return;
	}
	f = fopen(FAV_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < n; i++)
		fprintf(f, "%s\n", lines[i]);
	fclose(f);
}

static void bgHints(SDL_Surface* s) {
	int top = s->h - P(64), cy = top + P(32);
	SDL_FillRect(s, &(SDL_Rect){0, top, s->w, P(1) > 0 ? P(1) : 1}, mapc(s, C_SURFACE1, 1));
	const char* hints[][2] = {{"A", "Play"}, {"B", "Back"}, {"X", "Favorites"}, {"Y", "Search"}};
	int x = P(40);
	for (int i = 0; i < 4; i++) {
		rrect(s, x, cy - P(16), P(32), P(32), P(16), C_SURFACE2, 1);
		text(s, hints[i][0], P(16), 700, C_TEXT, 1, x + (P(32) - textW(hints[i][0], P(16), 700)) / 2, cy - lineH(P(16)) / 2);
		x += P(32) + P(10);
		const char* l = I18N_text(hints[i][1]);
		text(s, l, P(19), 400, C_TEXT2, 1, x, cy - lineH(P(19)) / 2);
		x += textW(l, P(19), 400) + P(32);
	}
}

static void sizeText(const char* path, char* out, size_t size) {
	struct stat st;
	out[0] = '\0';
	if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
		snprintf(out, size, "%.1f MB", st.st_size / (1024.0 * 1024.0));
}

void BosGames_render(SDL_Surface* s) {
	if (!bg_built)
		bgBuild();
	bgLoadGames();
	pending = false;
	SDL_FillRect(s, NULL, mapc(s, C_BG, 1));
	// status bar (5.1: the time, the battery %)
	int bar = P(56), cy = bar / 2;
	char clock[16];
	time_t now = time(NULL);
	strftime(clock, sizeof(clock), "%H:%M", localtime(&now));
	text(s, clock, P(21), 600, C_TEXT, 1, P(32), cy - lineH(P(21)) / 2);
	int charging = 0, pct = 0;
	PLAT_getBatteryStatusFine(&charging, &pct);
	char p[16];
	snprintf(p, sizeof(p), "%d%%", pct);
	text(s, p, P(19), 600, C_TEXT, 1, s->w - P(32) - textW(p, P(19), 600), cy - lineH(P(19)) / 2);

	// left: the title, then the console group (rows 60, padding 7, radius 22; the selected row in the accent)
	int lx = P(32), lw = P(340), top = bar + P(8);
	text(s, I18N_text("Games"), P(35), 700, C_TEXT, 1, lx + P(10), top);
	int gy = top + lineH(P(35)) + P(4) + P(12);
	int hint_top = s->h - P(64) - P(16);
	int rows_fit = (hint_top - gy - 2 * P(7)) / P(60);
	if (rows_fit > bg_ncons)
		rows_fit = bg_ncons;
	if (bg_sel < bg_top)
		bg_top = bg_sel;
	if (rows_fit > 0 && bg_sel >= bg_top + rows_fit)
		bg_top = bg_sel - rows_fit + 1;
	if (rows_fit > 0) {
		rrect(s, lx, gy, lw, rows_fit * P(60) + 2 * P(7), P(22), C_SURFACE1, 1);
		for (int r = 0; r < rows_fit; r++) {
			int i = bg_top + r, ry = gy + P(7) + r * P(60);
			bool sel = i == bg_sel;
			if (sel) // the accent while the list has focus; #2C2C2E once the games have it
				rrect(s, lx + P(7), ry, lw - 2 * P(7), P(60), P(15), bg_right ? C_SURFACE2 : C_ACCENT, 1);
			char cnt[16] = "";
			if (bg_cons[i].count >= 0)
				snprintf(cnt, sizeof(cnt), "%d", bg_cons[i].count);
			int cw = textW(cnt, P(19), 400);
			char name[160];
			fit(bg_cons[i].name, P(21), 500, lw - 2 * P(7) - 2 * P(16) - cw - P(10), name, sizeof(name));
			text(s, name, P(21), 500, C_TEXT, 1, lx + P(7) + P(16), ry + (P(60) - lineH(P(21))) / 2);
			text(s, cnt, P(19), 400, sel && !bg_right ? 0xFFFFFF : C_TEXT2, 1, lx + lw - P(7) - P(16) - cw,
				 ry + (P(60) - lineH(P(19))) / 2);
		}
	}

	// right: the console's name and "เรียง A–Z", then the game group (rows 80, 6 visible)
	int rx = lx + lw + P(24), rw = s->w - P(32) - rx;
	if (bg_ncons > 0) {
		const char* sort = I18N_text("Sorted A-Z");
		int sw = textW(sort, P(19), 400);
		char head[160];
		fit(bg_cons[bg_sel].name, P(28), 700, rw - 2 * P(10) - sw - P(16), head, sizeof(head));
		text(s, head, P(28), 700, C_TEXT, 1, rx + P(10), top + P(6));
		TTF_Font* hf = fontFor(P(28), 700, false);
		TTF_Font* sf = fontFor(P(19), 400, false);
		int base = top + P(6) + (hf ? TTF_FontAscent(hf) : P(28));
		text(s, sort, P(19), 400, C_TEXT2, 1, rx + rw - P(10) - sw, base - (sf ? TTF_FontAscent(sf) : P(19)));
	}
	int list_y = top + P(6) + lineH(P(28)) + P(12);
	int ng = bg_games && bg_games->entries ? bg_games->entries->count : 0;
	if (bg_gsel < bg_gtop)
		bg_gtop = bg_gsel;
	if (bg_gsel >= bg_gtop + GAME_ROWS)
		bg_gtop = bg_gsel - GAME_ROWS + 1;
	int shown = ng - bg_gtop < GAME_ROWS ? ng - bg_gtop : GAME_ROWS;
	if (shown > 0) {
		rrect(s, rx, list_y, rw, shown * P(80), P(22), C_SURFACE1, 1);
		for (int r = 0; r < shown; r++) {
			int i = bg_gtop + r, ry = list_y + r * P(80);
			Entry* e = bg_games->entries->items[i];
			bool sel = bg_right && i == bg_gsel;
			if (sel) { // #2C2C2E, keeping the group's rounded ends
				int rtop = r == 0 ? P(22) : 0, rbot = r == shown - 1 ? P(22) : 0;
				rrect(s, rx, ry, rw, P(80), rtop > rbot ? rtop : rbot, C_SURFACE2, 1);
				if (rtop && !rbot)
					SDL_FillRect(s, &(SDL_Rect){rx, ry + P(40), rw, P(40)}, mapc(s, C_SURFACE2, 1));
				if (rbot && !rtop)
					SDL_FillRect(s, &(SDL_Rect){rx, ry, rw, P(40)}, mapc(s, C_SURFACE2, 1));
			}
			if (r < shown - 1)
				SDL_FillRect(s, &(SDL_Rect){rx, ry + P(80) - 1, rw, 1}, mapc(s, C_SURFACE2, 1));
			int ax = rx + P(20), ay = ry + (P(80) - P(45)) / 2;
			SDL_Surface* pic = NULL;
			if (e->type == ENTRY_ROM && HomeArt_pin(e->path, P(60), P(45), P(10), &pic) == HOMEART_LOADING)
				pending = true;
			if (pic)
				SDL_BlitSurface(pic, NULL, s, &(SDL_Rect){ax, ay});
			else
				rrect(s, ax, ay, P(60), P(45), P(10), C_SURFACE2, 1);
			int tx = ax + P(60) + P(16), pill_w = 0;
			ResumeState rs;
			readyResumeInto(e, &rs);
			if (rs.can_resume) { // "เล่นต่อ": 16/600, black on #F2F2F7, radius 10, padding 4 11
				const char* l = I18N_text("Continue");
				int pw = textW(l, P(16), 600) + 2 * P(11), ph = lineH(P(16)) + 2 * P(4);
				int px = rx + rw - P(20) - pw, py = ry + (P(80) - ph) / 2;
				rrect(s, px, py, pw, ph, P(10), C_TEXT, 1);
				text(s, l, P(16), 600, 0x000000, 1, px + P(11), py + P(4));
				pill_w = pw + P(16);
			}
			int maxw = rx + rw - P(20) - pill_w - tx;
			char name[256], meta[96] = "";
			fit(e->name, P(21), 600, maxw, name, sizeof(name));
			GameInfo gi;
			if (e->type == ENTRY_ROM && GameInfo_prefetch(e->path, &gi) && gi.has_time && gi.last_played > 0) {
				char when[64];
				GameInfo_whenText(now, gi.last_played, when, sizeof(when));
				snprintf(meta, sizeof(meta), I18N_text("Last played %s"), when);
			} else
				sizeText(e->path, meta, sizeof(meta));
			int nh = lineH(P(21)), mh = lineH(P(16)), ty = ry + (P(80) - nh - P(1) - mh) / 2;
			text(s, name, P(21), 600, C_TEXT, 1, tx, ty);
			text(s, meta, P(16), 400, C_TEXT2, 1, tx, ty + nh + P(1));
		}
	}
	bgHints(s);
}

bool BosGames_animating(void) {
	return pending;
}

BosGamesInput BosGames_handleInput(bool* dirty) {
	if (!bg_built)
		bgBuild();
	bgLoadGames();
	int ng = bg_games && bg_games->entries ? bg_games->entries->count : 0;
	if (PAD_justPressed(BTN_Y))
		return BOSGAMES_SEARCH;
	if (PAD_justPressed(BTN_L1) || PAD_justPressed(BTN_R1))
		return BOSGAMES_HANDLED; // no tabs (the mockup)
	if (PAD_justPressed(BTN_B)) {
		if (!bg_right)
			return BOSGAMES_HOME;
		bg_right = false;
		*dirty = true;
		return BOSGAMES_HANDLED;
	}
	if (!bg_right) {
		if (PAD_justRepeated(BTN_UP) && bg_sel > 0)
			bg_sel--, *dirty = true;
		else if (PAD_justRepeated(BTN_DOWN) && bg_sel < bg_ncons - 1)
			bg_sel++, *dirty = true;
		else if ((PAD_justPressed(BTN_RIGHT) || PAD_justPressed(BTN_A)) && ng > 0)
			bg_right = true, *dirty = true;
		else
			return BOSGAMES_UNHANDLED;
		return BOSGAMES_HANDLED;
	}
	if (PAD_justRepeated(BTN_UP) && bg_gsel > 0)
		bg_gsel--, *dirty = true;
	else if (PAD_justRepeated(BTN_DOWN) && bg_gsel < ng - 1)
		bg_gsel++, *dirty = true;
	else if (PAD_justPressed(BTN_LEFT))
		bg_right = false, *dirty = true;
	else if (PAD_justPressed(BTN_X) && ng > 0) {
		Entry* e = bg_games->entries->items[bg_gsel];
		if (e->type == ENTRY_ROM) {
			bool in_favs = !strcmp(bg_cons[bg_sel].path, FAV_PATH);
			char keep[MAX_PATH];
			snprintf(keep, sizeof(keep), "%s", bg_cons[bg_sel].path);
			int gsel = bg_gsel;
			toggleFavorite(e->path);
			bgBuild(); // the Favorites row comes and goes, its count changes
			bg_sel = 0;
			for (int i = 0; i < bg_ncons; i++)
				if (!strcmp(bg_cons[i].path, keep))
					bg_sel = i;
			bg_games_for = -1;
			bgLoadGames();
			int n2 = bg_games && bg_games->entries ? bg_games->entries->count : 0;
			bg_gsel = in_favs ? (gsel < n2 ? gsel : n2 - 1) : gsel;
			if (bg_gsel < 0) {
				bg_gsel = 0;
				bg_right = false;
			}
			*dirty = true;
		}
	} else if (PAD_justPressed(BTN_A) && ng > 0) {
		Entry* e = bg_games->entries->items[bg_gsel];
		if (e->type == ENTRY_ROM) {
			readyResume(e);
			resume.should_resume = resume.can_resume;
		}
		Entry_open(e);
		*dirty = true;
	} else
		return BOSGAMES_UNHANDLED;
	return BOSGAMES_HANDLED;
}
