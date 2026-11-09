// เกม v2 (DESIGN.md 6.2–6.4), drawn from bos_ui: step 1 the console wheel, step 2 the console's games as a cover
// carousel or a list (SELECT, remembered per console). Included at the end of home_bos.c (shares tween/cover helpers).

#define FAV_PATH COLLECTIONS_PATH "/Favorites.txt"
#define LISTVIEW_PATH USERDATA_PATH "/bos_games_list.txt" // consoles shown as a list (one folder path per line)
#define LIST_ROWS 5

typedef struct {
	char path[MAX_PATH]; // console folder, or FAV_PATH
	char name[128];		 // the folder name without its "(TAG)"
	char tag[32];
	int count;
	bool fav;
	SDL_Surface* pic;	 // the picture as loaded (NULL: none, see tried)
	bool tried;
	SDL_Surface* sized[3]; // pic fitted to the centre / neighbour / outer box, made once
} BgConsole;
static BgConsole* bg_cons;
static int bg_ncons, bg_sel;
static float bg_pos, bg_from; // wheel position shown, tween start
static uint32_t bg_start;
static Directory* bg_games; // the selected console's list (Directory_new)
static int bg_games_for = -1, bg_gsel, bg_gtop;
static float bg_gpos, bg_gfrom;
static uint32_t bg_gstart;
static bool bg_step2; // showing the console's games
static bool bg_listmode;
static bool bg_built;
static uint32_t bg_menu_down;

// the wheel's slots (§6.2): box w, h, how much lower than the centre, alpha
static const struct {
	int w, h, drop;
	float a;
} WSLOT[3] = {{560, 380, 0, 1.0f}, {300, 220, 100, 0.5f}, {220, 150, 160, 0.22f}};

bool BosGames_active(void) {
	return stack && stack->count == 1 && MenuTabs_current() == MENU_TAB_CONSOLES && !GameSwitcher_isList();
}

static void freePics(BgConsole* c) {
	for (int k3 = 0; k3 < 3; k3++)
		if (c->sized[k3]) {
			SDL_FreeSurface(c->sized[k3]);
			c->sized[k3] = NULL;
		}
	if (c->pic)
		SDL_FreeSurface(c->pic);
	c->pic = NULL;
	c->tried = false;
}

static void freeConsoles(void) {
	for (int i = 0; i < bg_ncons; i++)
		freePics(&bg_cons[i]);
	free(bg_cons);
	bg_cons = NULL;
	bg_ncons = 0;
}

static void bgBuild(void) {
	freeConsoles();
	Directory* root = Directory_new((char*)ROMS_PATH, 0);
	int n = root && root->entries ? root->entries->count : 0;
	bg_cons = calloc((size_t)n + 1, sizeof(BgConsole));
	if (!bg_cons) {
		if (root)
			Directory_free(root);
		return;
	}
	BgConsole* f = &bg_cons[bg_ncons++]; // ★ รายการโปรด is always the first slot (0 games: "ยังไม่มีเกม")
	snprintf(f->path, MAX_PATH, "%s", FAV_PATH);
	snprintf(f->name, sizeof(f->name), "%s", I18N_text("Favorites"));
	f->count = countLines(FAV_PATH);
	f->fav = true;
	for (int i = 0; i < n; i++) {
		Entry* e = root->entries->items[i];
		BgConsole* c = &bg_cons[bg_ncons++];
		snprintf(c->path, MAX_PATH, "%s", e->path);
		snprintf(c->name, sizeof(c->name), "%s", e->name);
		tagOf(e->path, c->tag, sizeof(c->tag));
		if (!c->tag[0]) { // the folder itself is "Name (TAG)": tagOf reads the parent's last ( ), use our own name
			const char* o = strrchr(e->path, '(');
			const char* cl = o ? strchr(o, ')') : NULL;
			if (o && cl && cl - o > 1)
				snprintf(c->tag, sizeof(c->tag), "%.*s", (int)(cl - o - 1), o + 1);
		}
		c->count = -1;
	}
	// game counts from the launcher's own index (path \t name \t count)
	FILE* fp = fopen(USERDATA_PATH "/emulist_cache.txt", "r");
	if (fp) {
		char line[MAX_PATH * 2];
		while (fgets(line, sizeof(line), fp)) {
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
		fclose(fp);
	}
	if (root)
		Directory_free(root);
	if (bg_sel >= bg_ncons)
		bg_sel = bg_ncons > 0 ? bg_ncons - 1 : 0;
	bg_pos = bg_from = (float)bg_sel;
	bg_built = true;
}

static bool listModeFor(const char* path) {
	return lineInFile(LISTVIEW_PATH, path);
}

static void setListMode(const char* path, bool on) {
	char lines[128][MAX_PATH];
	int n = 0;
	FILE* f = fopen(LISTVIEW_PATH, "r");
	if (f) {
		char line[MAX_PATH];
		while (n < 128 && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (line[0] && strcmp(line, path))
				snprintf(lines[n++], MAX_PATH, "%s", line);
		}
		fclose(f);
	}
	if (on && n < 128)
		snprintf(lines[n++], MAX_PATH, "%s", path);
	f = fopen(LISTVIEW_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < n; i++)
		fprintf(f, "%s\n", lines[i]);
	fclose(f);
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
	bg_gpos = bg_gfrom = 0;
	bg_listmode = bg_ncons > 0 && listModeFor(bg_cons[bg_sel].path);
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

// ---- the console picture (§6.2): <console>/.media/console.png, else the OS's picture (brightened), else none ----
static SDL_Surface* consolePic(BgConsole* c) {
	if (c->tried)
		return c->pic;
	c->tried = true;
	if (c->fav)
		return NULL;
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s/.media/console.png", c->path);
	SDL_Surface* raw = exists(path) ? IMG_Load(path) : NULL;
	if (!raw) {
		const char* base = strrchr(c->path, '/');
		const char* id = Pad_idForFolder(base ? base + 1 : c->path);
		if (id) {
			snprintf(path, sizeof(path), "%s/bos/consoles/%s.png", RES_PATH, id);
			raw = IMG_Load(path);
		}
	}
	if (raw) {
		c->pic = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
		SDL_FreeSurface(raw);
	}
	return c->pic;
}

static SDL_Surface* consoleSized(BgConsole* c, int slot) { // fitted (contain) into the slot's box, made once
	if (c->sized[slot])
		return c->sized[slot];
	SDL_Surface* p = consolePic(c);
	if (!p)
		return NULL;
	int bw = BOS_px(WSLOT[slot].w), bh = BOS_px(WSLOT[slot].h);
	float k3 = fminf(bw / (float)p->w, bh / (float)p->h);
	int w = (int)(p->w * k3), h = (int)(p->h * k3);
	SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!d)
		return NULL;
	SDL_SetSurfaceBlendMode(p, SDL_BLENDMODE_NONE);
	SDL_BlitScaled(p, NULL, d, NULL);
	SDL_SetSurfaceBlendMode(d, SDL_BLENDMODE_BLEND);
	c->sized[slot] = d;
	return d;
}

static void wheelItem(SDL_Surface* s, BgConsole* c, int slot, int cx, int top) {
	int bw = BOS_px(WSLOT[slot].w), bh = BOS_px(WSLOT[slot].h);
	float a = WSLOT[slot].a;
	int bx = cx - bw / 2;
	if (c->fav) { // ★ card: surface, radius 32, 62 % x 90 % of the box, the star half its height in warn
		int w = bw * 62 / 100, h = bh * 90 / 100, x = cx - w / 2, y = top + (bh - h) / 2;
		BOS_rrect(s, x, y, w, h, BOS_px(32), over(BOS_C_SURFACE, a));
		int ic = h / 2;
		BOS_icon(s, "star", BOS_C_WARN, a, cx - ic / 2, y + (h - ic) / 2, ic);
		return;
	}
	SDL_Surface* p = consoleSized(c, slot);
	if (p) {
		Uint8 m = (Uint8)(255 * a); // over the black page, alpha a = the colour scaled by a
		SDL_SetSurfaceColorMod(p, m, m, m);
		SDL_BlitSurface(p, NULL, s, &(SDL_Rect){cx - p->w / 2, top + (bh - p->h) / 2});
		return;
	}
	// no picture: the console's tag as a wordmark on a surface card (radius 28)
	int h = bh * 60 / 100, y = top + (bh - h) / 2;
	BOS_rrect(s, bx + bw / 10, y, bw * 8 / 10, h, BOS_px(28), over(BOS_C_SURFACE, a));
	const char* t = c->tag[0] ? c->tag : c->name;
	int px = h * 30 / 100;
	char fitted[64];
	BOS_fit(t, px, 700, bw * 7 / 10, fitted, sizeof(fitted));
	BOS_textMid(s, fitted, px, 700, BOS_C_TEXT, a, cx - BOS_textW(fitted, px, 700) / 2, y + h / 2);
}

static float wrapD(float d, int n) { // the shortest way round the wheel
	while (d > n / 2.0f)
		d -= n;
	while (d < -n / 2.0f)
		d += n;
	return d;
}

static void wheelRender(SDL_Surface* s) {
	BOS_statusBar(s, I18N_text("Games"));
	int top = BOS_px(BOS_BAR_H) + BOS_px(10);
	int n = bg_ncons;
	// slot centres out from the middle: 328 / 498 px (the mockup's neighbour left 34, outer left -96)
	float c1 = BOS_px(328), c2 = BOS_px(498);
	for (int pass = 2; pass >= 0; pass--)
		for (int i = 0; i < n; i++) {
			float d = n > 4 ? wrapD(i - bg_pos, n) : i - bg_pos;
			float ad = fabsf(d);
			int slot = (int)(ad + 0.5f);
			if (slot != pass || ad > 2.6f)
				continue;
			float off = ad <= 1 ? ad * c1 : ad <= 2 ? c1 + (ad - 1) * (c2 - c1) : c2;
			float drop = ad <= 1 ? ad * WSLOT[1].drop : ad <= 2 ? WSLOT[1].drop + (ad - 1) * (WSLOT[2].drop - WSLOT[1].drop) : WSLOT[2].drop;
			int cx = s->w / 2 + (int)(d < 0 ? -off : off);
			wheelItem(s, &bg_cons[i], slot, cx, top + BOS_px(drop));
		}
	if (n == 0)
		return;
	// the centred console's name under its picture: 56/700, then "<N> เกม" 30 text2
	BgConsole* c = &bg_cons[bg_sel];
	int ny = top + BOS_px(WSLOT[0].h) + BOS_px(18);
	char name[128];
	BOS_fit(c->name, BOS_FS_TITLE, 700, s->w - 2 * BOS_px(BOS_PAGE_PAD), name, sizeof(name));
	BOS_text(s, name, BOS_FS_TITLE, 700, BOS_C_TEXT, 1, (s->w - BOS_textW(name, BOS_FS_TITLE, 700)) / 2, ny);
	char cnt[64];
	if (c->count > 0)
		snprintf(cnt, sizeof(cnt), I18N_text("%d games"), c->count);
	else if (c->count == 0)
		snprintf(cnt, sizeof(cnt), "%s", I18N_text("No games yet"));
	else
		cnt[0] = '\0';
	ny += (int)(BOS_FS_TITLE * 1.15f) + BOS_px(2);
	BOS_text(s, cnt, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, (s->w - BOS_textW(cnt, BOS_FS_SMALL, 400)) / 2, ny);
	// page dots, centred 12 above the hint bar (only while they fit)
	int dw = BOS_px(10), cur = BOS_px(28), gap = BOS_px(8), total = (n - 1) * (dw + gap) + cur;
	if (total < s->w - 2 * BOS_px(BOS_PAGE_PAD)) {
		int x = (s->w - total) / 2, y = s->h - BOS_px(BOS_BAR_H) - BOS_px(12) - dw;
		for (int i = 0; i < n; i++) {
			int w = i == bg_sel ? cur : dw;
			BOS_rrect(s, x, y, w, dw, dw / 2, i == bg_sel ? BOS_C_TEXT : BOS_C_SURFACE2);
			x += w + gap;
		}
	}
	BosHint r[] = {{"A", I18N_text("Open")}, {"B", I18N_text("Back")}, {"Y", I18N_text("Search")}};
	BOS_hintBar(s, r, 3, NULL, 0);
}

// ---- step 2: the console's games ----
static int gamesCount(void) {
	return bg_games && bg_games->entries ? bg_games->entries->count : 0;
}

static void gamePic(SDL_Surface* s, Entry* e, int x, int y, int w, int h, int r, float a, bool ring) {
	if (ring) {
		int o = BOS_px(BOS_RING), q = 2 * o;
		BOS_rrect(s, x - q, y - q, w + 2 * q, h + 2 * q, r + q, BOS_theme()->focus);
		BOS_rrect(s, x - o, y - o, w + 2 * o, h + 2 * o, r + o, BOS_theme()->bg);
	}
	SDL_Surface* pic = NULL;
	if (e->type == ENTRY_ROM) {
		HomeArtState st = HomeArt_pin(e->path, w, h, r, &pic);
		if (st == HOMEART_LOADING)
			pending = true;
	}
	if (pic) {
		SDL_SetSurfaceAlphaMod(pic, (Uint8)(255 * a));
		SDL_BlitSurface(pic, NULL, s, &(SDL_Rect){x, y});
		SDL_SetSurfaceAlphaMod(pic, 255);
	} else {
		BOS_rrect(s, x, y, w, h, r, over(BOS_C_SURFACE, a));
		int ic = h / 3;
		BOS_icon(s, e->type == ENTRY_DIR ? "folder" : "game", BOS_C_TEXTOFF, a, x + (w - ic) / 2, y + (h - ic) / 2, ic);
	}
}

static void lastPlayed(Entry* e, char* out, size_t size) {
	out[0] = '\0';
	GameInfo gi;
	if (e->type == ENTRY_ROM && GameInfo_prefetch(e->path, &gi) && gi.has_time && gi.last_played > 0) {
		char when[64];
		GameInfo_whenText(time(NULL), gi.last_played, when, sizeof(when));
		snprintf(out, size, I18N_text("Last played %s"), when);
	}
}

static bool canResume(Entry* e) {
	if (e->type != ENTRY_ROM)
		return false;
	ResumeState rs;
	readyResumeInto(e, &rs);
	return rs.can_resume;
}

static void coversRender(SDL_Surface* s, int top) { // §6.3: the full-size carousel and its caption
	int ng = gamesCount();
	int cw = BOS_px(500), ch = BOS_px(375), sw = BOS_px(300), sh = BOS_px(225);
	int ow = sw * 3 / 4, oh = sh * 3 / 4, gap = BOS_px(28);
	float c1 = cw / 2.0f + gap + sw / 2.0f, c2 = cw / 2.0f + gap + sw + gap + ow / 2.0f;
	for (int pass = 2; pass >= 0; pass--)
		for (int i = 0; i < ng; i++) {
			float d = i - bg_gpos, ad = fabsf(d);
			int slot = (int)(ad + 0.5f);
			if (slot != pass || ad > 2.6f)
				continue;
			float off = ad <= 1 ? ad * c1 : ad <= 2 ? c1 + (ad - 1) * (c2 - c1) : c2;
			int w = slot == 0 ? cw : slot == 1 ? sw : ow, h = slot == 0 ? ch : slot == 1 ? sh : oh;
			float a = slot == 0 ? 1.0f : slot == 1 ? 0.5f : 0.25f;
			int r = BOS_px(slot == 0 ? 24 : slot == 1 ? 20 : 18);
			int cx = s->w / 2 + (int)(d < 0 ? -off : off);
			gamePic(s, bg_games->entries->items[i], cx - w / 2, top + (ch - h) / 2, w, h, r, a, slot == 0 && bg_gstart == 0);
		}
	if (ng == 0)
		return;
	Entry* e = bg_games->entries->items[bg_gsel];
	int y = top + ch + BOS_px(14);
	char name[256];
	BOS_fit(e->name, BOS_FS_BODY, 600, s->w - 2 * BOS_px(BOS_PAGE_PAD), name, sizeof(name));
	BOS_text(s, name, BOS_FS_BODY, 600, BOS_C_TEXT, 1, (s->w - BOS_textW(name, BOS_FS_BODY, 600)) / 2, y);
	y += BOS_lineH(BOS_FS_BODY) + BOS_px(4);
	char meta[128];
	lastPlayed(e, meta, sizeof(meta));
	bool res = canResume(e);
	const char* rl = I18N_text("Continue");
	int mw = BOS_textW(meta, BOS_FS_SMALL, 400), pw = res ? (meta[0] ? BOS_px(16) : 0) + pillW(rl, true) : 0;
	int x = (s->w - mw - pw) / 2, cy = y + BOS_px(22);
	BOS_textMid(s, meta, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, x, cy);
	if (res)
		pill(s, x + mw + (meta[0] ? BOS_px(16) : 0), cy, rl, BOS_C_OK);
}

static void listRender(SDL_Surface* s, int top) { // §6.4: rows of 96 on the left, the focused game's cover on the right
	int ng = gamesCount();
	if (bg_gsel < bg_gtop)
		bg_gtop = bg_gsel;
	if (bg_gsel >= bg_gtop + LIST_ROWS)
		bg_gtop = bg_gsel - LIST_ROWS + 1;
	int lx = BOS_px(24), lw = BOS_px(560), rh = BOS_px(BOS_ROW_H), pad = BOS_px(24);
	for (int r = 0; r < LIST_ROWS && bg_gtop + r < ng; r++) {
		int i = bg_gtop + r, y = top + r * rh;
		Entry* e = bg_games->entries->items[i];
		bool f = i == bg_gsel;
		int x = lx, w = lw, h = rh;
		if (f) { // focus: white fill, x1.04
			int gw = (int)(lw * BOS_ROW_SCALE) - lw, gh = (int)(rh * BOS_ROW_SCALE) - rh;
			x -= gw / 2, w += gw, y -= gh / 2, h += gh;
			BOS_rrect(s, x, y, w, h, BOS_px(BOS_RADIUS_ROW), BOS_theme()->focus);
		}
		float a = f ? 1.0f : BOS_DIM;
		uint32_t fg = f ? BOS_theme()->onfocus : BOS_C_TEXT;
		int pill_w = 0;
		if (canResume(e)) {
			const char* rl = I18N_text("Continue");
			pill_w = pillW(rl, true) + BOS_px(12);
			if (!f) // the pill is drawn opaque: only on rows it doesn't need to be dimmed with
				pill(s, x + w - pad - pillW(rl, true), y + h / 2, rl, BOS_C_OK);
			else
				pill(s, x + w - pad - pillW(rl, true), y + h / 2, rl, BOS_C_OK);
		}
		char name[256];
		BOS_fit(e->name, BOS_FS_BODY, f ? 600 : 500, w - 2 * pad - pill_w, name, sizeof(name));
		BOS_textMid(s, name, BOS_FS_BODY, f ? 600 : 500, fg, a, x + pad, y + h / 2);
	}
	if (ng == 0)
		return;
	Entry* e = bg_games->entries->items[bg_gsel];
	int rx = lx + lw + BOS_px(32), cw = BOS_px(384), ch = BOS_px(288);
	gamePic(s, e, rx, top, cw, ch, BOS_px(24), 1, false);
	char meta[128];
	lastPlayed(e, meta, sizeof(meta));
	int y = top + ch + BOS_px(12);
	if (meta[0]) {
		BOS_text(s, meta, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, rx, y);
		y += BOS_lineH(BOS_FS_SMALL) + BOS_px(12);
	}
	if (canResume(e))
		pill(s, rx, y + BOS_px(22), I18N_text("Continue"), BOS_C_OK);
}

static void gamesRender(SDL_Surface* s) {
	BOS_statusBar(s, I18N_text("Games"));
	BgConsole* c = &bg_cons[bg_sel];
	char sub[96];
	snprintf(sub, sizeof(sub), I18N_text("%d games · Sorted A-Z"), gamesCount());
	int top = BOS_px(BOS_BAR_H);
	top += BOS_largeTitle(s, top, c->name, sub);
	if (gamesCount() == 0) {
		const char* t = I18N_text("No games yet");
		BOS_text(s, t, BOS_FS_BODY, 400, BOS_C_TEXT2, 1, (s->w - BOS_textW(t, BOS_FS_BODY, 400)) / 2, top + BOS_px(120));
	} else if (bg_listmode)
		listRender(s, top);
	else
		coversRender(s, top);
	BosHint r[] = {{"A", I18N_text("Play")}, {"B", I18N_text("Back")}, {"X", I18N_text("Favorite")}, {"Y", I18N_text("Search")}};
	BosHint l[] = {{"MENU", I18N_text("Options")}};
	BOS_hintBar(s, r, 4, l, gamesCount() > 0 ? 1 : 0);
}

static void tweenStep(float* pos, float* from, uint32_t* start, int target) {
	if (!*start)
		return;
	float t = (SDL_GetTicks() - *start) / (float)TWEEN_MS;
	if (t >= 1) {
		t = 1;
		*start = 0;
	}
	*pos = *from + (target - *from) * easeOut(t);
}

void BosGames_render(SDL_Surface* s) {
	if (!bg_built)
		bgBuild();
	pending = false;
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	if (bg_ncons == 0) {
		BOS_statusBar(s, I18N_text("Games"));
		BosHint r[] = {{"B", I18N_text("Back")}};
		BOS_hintBar(s, r, 1, NULL, 0);
		return;
	}
	if (bg_step2) {
		bgLoadGames();
		tweenStep(&bg_gpos, &bg_gfrom, &bg_gstart, bg_gsel);
		gamesRender(s);
	} else {
		if (bg_start) { // the wheel tween, the short way round
			float t = (SDL_GetTicks() - bg_start) / (float)TWEEN_MS;
			if (t >= 1) {
				t = 1;
				bg_start = 0;
			}
			float d = bg_ncons > 4 ? wrapD(bg_sel - bg_from, bg_ncons) : bg_sel - bg_from;
			bg_pos = bg_from + d * easeOut(t);
			if (!bg_start)
				bg_pos = (float)bg_sel;
		}
		wheelRender(s);
	}
}

bool BosGames_animating(void) {
	return pending || bg_start != 0 || bg_gstart != 0 || HomeArt_checkAsyncLoaded() || GameInfo_checkAsyncLoaded();
}

Entry* BosGames_focusedGame(void) {
	return bg_step2 && gamesCount() > 0 ? bg_games->entries->items[bg_gsel] : NULL;
}

static void wheelMove(int dir) {
	if (bg_ncons < 2)
		return;
	bg_from = bg_pos;
	bg_sel = (bg_sel + dir + bg_ncons) % bg_ncons;
	bg_start = SDL_GetTicks() ? SDL_GetTicks() : 1;
	// keep only the pictures near the selection (each console holds up to three fitted copies)
	for (int i = 0; i < bg_ncons; i++)
		if (fabsf(wrapD(i - bg_sel, bg_ncons)) > 3.5f && bg_cons[i].tried)
			freePics(&bg_cons[i]);
}

static void gameMove(int to) {
	int ng = gamesCount();
	if (to < 0 || to >= ng || to == bg_gsel)
		return;
	bg_gfrom = bg_gpos;
	bg_gsel = to;
	if (!bg_listmode)
		bg_gstart = SDL_GetTicks() ? SDL_GetTicks() : 1;
	else
		bg_gpos = (float)to;
}

BosGamesInput BosGames_handleInput(unsigned long now, bool* dirty, bool tap_menu, bool tap_select) {
	if (!bg_built)
		bgBuild();
	*dirty = true;
	if (PAD_justPressed(BTN_MENU))
		bg_menu_down = (uint32_t)now;
	if (bg_menu_down && PAD_isPressed(BTN_MENU) && (uint32_t)now - bg_menu_down >= 500) {
		bg_menu_down = 0;
		return BOSGAMES_SWITCHER;
	}
	if (!PAD_isPressed(BTN_MENU))
		bg_menu_down = 0;
	if (PAD_justPressed(BTN_Y))
		return BOSGAMES_SEARCH;
	if (PAD_justPressed(BTN_L1) || PAD_justPressed(BTN_R1))
		return BOSGAMES_HANDLED; // no tabs
	if (!bg_step2) {
		if (tap_menu)
			return BOSGAMES_HANDLED;
		if (PAD_justPressed(BTN_B))
			return BOSGAMES_HOME;
		if (PAD_justRepeated(BTN_LEFT))
			wheelMove(-1);
		else if (PAD_justRepeated(BTN_RIGHT))
			wheelMove(1);
		else if (PAD_justPressed(BTN_A) && bg_ncons > 0) {
			bg_games_for = -1;
			bgLoadGames();
			bg_step2 = true;
		} else
			return BOSGAMES_UNHANDLED;
		return BOSGAMES_HANDLED;
	}
	bgLoadGames();
	int ng = gamesCount();
	if (tap_menu)
		return ng > 0 ? BOSGAMES_OPTIONS : BOSGAMES_HANDLED;
	if (PAD_justPressed(BTN_B)) {
		bg_step2 = false;
		return BOSGAMES_HANDLED;
	}
	if (tap_select) {
		bg_listmode = !bg_listmode;
		setListMode(bg_cons[bg_sel].path, bg_listmode);
		bg_gpos = (float)bg_gsel;
		bg_gstart = 0;
		return BOSGAMES_HANDLED;
	}
	int prev = bg_listmode ? BTN_UP : BTN_LEFT, next = bg_listmode ? BTN_DOWN : BTN_RIGHT;
	if (PAD_justRepeated(prev))
		gameMove(bg_gsel - 1);
	else if (PAD_justRepeated(next))
		gameMove(bg_gsel + 1);
	else if (bg_listmode && PAD_justRepeated(BTN_LEFT))
		gameMove(bg_gsel - LIST_ROWS > 0 ? bg_gsel - LIST_ROWS : 0);
	else if (bg_listmode && PAD_justRepeated(BTN_RIGHT))
		gameMove(bg_gsel + LIST_ROWS < ng ? bg_gsel + LIST_ROWS : ng - 1);
	else if (PAD_justPressed(BTN_X) && ng > 0) {
		Entry* e = bg_games->entries->items[bg_gsel];
		if (e->type == ENTRY_ROM) {
			bool in_favs = bg_cons[bg_sel].fav;
			char keep[MAX_PATH];
			snprintf(keep, sizeof(keep), "%s", bg_cons[bg_sel].path);
			int gsel = bg_gsel;
			toggleFavorite(e->path);
			bgBuild(); // the Favorites count changes
			bg_sel = 0;
			for (int i = 0; i < bg_ncons; i++)
				if (!strcmp(bg_cons[i].path, keep))
					bg_sel = i;
			bg_pos = bg_from = (float)bg_sel;
			bg_games_for = -1;
			bgLoadGames();
			int n2 = gamesCount();
			bg_gsel = in_favs ? (gsel < n2 ? gsel : n2 - 1) : gsel;
			if (bg_gsel < 0)
				bg_gsel = 0;
			bg_gpos = bg_gfrom = (float)bg_gsel;
		}
	} else if (PAD_justPressed(BTN_A) && ng > 0) {
		Entry* e = bg_games->entries->items[bg_gsel];
		if (e->type == ENTRY_ROM) {
			readyResume(e);
			resume.should_resume = resume.can_resume;
		}
		Entry_open(e);
	} else
		return BOSGAMES_UNHANDLED;
	return BOSGAMES_HANDLED;
}
