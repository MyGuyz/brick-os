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
	SDL_Surface* big; // pic fitted to the centre box, made once: every size is drawn from it (carPic)
} BgConsole;
static BgConsole* bg_cons;
static int bg_ncons, bg_sel;
static float bg_pos, bg_vel; // wheel position shown and its speed (springStep)
static uint32_t bg_start, bg_last; // moving (non-zero) / the last step's time
static Directory* bg_games; // the selected console's list (Directory_new)
static int bg_games_for = -1, bg_gsel, bg_gtop;
static float bg_gpos, bg_gvel;
static uint32_t bg_gstart, bg_glast;
static bool bg_step2; // showing the console's games
static bool bg_listmode;
static bool bg_built;
static uint32_t bg_menu_down;
// The game to stand on whenever its console's list is loaded (BosGames_restore). Kept until the first move: the
// page is reset and its list loaded again (Home_reset) after the restore.
static char bg_want_rom[MAX_PATH];

// the wheel (§6.2, board v2-GamesMotion): the centred console fills the middle, the others keep their size; box
// w, h, how much lower than the centre, alpha, one and two steps out. In between, everything follows the distance.
static const struct {
	int w, h, drop;
	float a;
} WSLOT[3] = {{640, 440, 0, 1.0f}, {300, 270, 40, 0.85f}, {220, 198, 60, 0.6f}};
#define WSTEP1 420 // the neighbour's centre from the middle
#define WSTEP2 180 // and the next one's from the neighbour's

int BosGames_page(void) {
	return bg_step2 ? 11 : 10;
}

bool BosGames_active(void) {
	return stack && stack->count == 1 && MenuTabs_current() == MENU_TAB_CONSOLES && !GameSwitcher_isList();
}

static void freePics(BgConsole* c) {
	GFX_freeSurfaceAndTexture(c->big);
	c->big = NULL;
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
	bg_pos = (float)bg_sel;
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
	bg_gpos = 0;
	bg_gstart = 0;
	bg_listmode = bg_ncons > 0 && listModeFor(bg_cons[bg_sel].path);
	if (bg_want_rom[0] && bg_games && bg_games->entries) {
		for (int i = 0; i < bg_games->entries->count; i++) {
			Entry* e = bg_games->entries->items[i];
			size_t l = strlen(e->path); // the game itself, or the folder it is in
			if (!strncmp(e->path, bg_want_rom, l) && (bg_want_rom[l] == '\0' || bg_want_rom[l] == '/')) {
				bg_gsel = i;
				bg_gpos = (float)i;
				break;
			}
		}
	}
}

// Back from a game: เกม opens on that console's games, standing on the game. The launcher's own restore walks its
// directory stack into the console, which is the old list page; nextui.c pops that and calls this.
void BosGames_restore(const char* console_path, const char* rom_path) {
	if (!bg_built)
		bgBuild();
	for (int i = 0; i < bg_ncons; i++) {
		if (strcmp(bg_cons[i].path, console_path))
			continue;
		bg_sel = i;
		bg_pos = (float)i;
		bg_start = 0;
		snprintf(bg_want_rom, sizeof(bg_want_rom), "%s", rom_path ? rom_path : "");
		bg_games_for = -1;
		bgLoadGames();
		bg_step2 = true;
		return;
	}
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

#define WBOX_W(ad) byDist(ad, WSLOT[0].w, WSLOT[1].w, WSLOT[2].w)
#define WBOX_H(ad) byDist(ad, WSLOT[0].h, WSLOT[1].h, WSLOT[2].h)
#define WALPHA(ad) byDist(ad, WSLOT[0].a, WSLOT[1].a, WSLOT[2].a)

static SDL_Surface* consoleBig(BgConsole* c) { // the picture fitted (contain) into the centre box, made once
	if (c->big)
		return c->big;
	SDL_Surface* p = consolePic(c);
	if (!p)
		return NULL;
	int bw = BOS_px(WSLOT[0].w), bh = BOS_px(WSLOT[0].h);
	float k3 = fminf(bw / (float)p->w, bh / (float)p->h);
	int w = (int)(p->w * k3), h = (int)(p->h * k3);
	SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!d)
		return NULL;
	SDL_SetSurfaceBlendMode(p, SDL_BLENDMODE_NONE);
	SDL_BlitScaled(p, NULL, d, NULL);
	SDL_SetSurfaceBlendMode(d, SDL_BLENDMODE_BLEND);
	c->big = d;
	return d;
}

static void wheelItem(SDL_Surface* s, BgConsole* c, float ad, int cx, int top) {
	int bw = BOS_px((int)WBOX_W(ad)), bh = BOS_px((int)WBOX_H(ad));
	float a = WALPHA(ad);
	int bx = cx - bw / 2;
	if (c->fav) { // ★ card: surface, radius 32, 62 % x 90 % of the box, the star half its height in warn
		int h = bh * 90 / 100, w = h * 69 / 100, x = cx - w / 2, y = top + (bh - h) / 2;
		BOS_rrect(s, x, y, w, h, h * 32 / 311, over(BOS_theme()->surface, a));
		int ic = h / 2;
		BOS_icon(s, "star", BOS_C_WARN, a, cx - ic / 2, y + (h - ic) / 2, ic);
		return;
	}
	SDL_Surface* p = consoleBig(c);
	if (p) { // the centre copy scaled to this box, faded (by the GPU)
		float k3 = fminf(bw / (float)p->w, bh / (float)p->h);
		int w = (int)(p->w * k3 + 0.5f), h = (int)(p->h * k3 + 0.5f);
		carPic(s, p, (SDL_Rect){cx - w / 2, top + (bh - h) / 2, w, h}, a);
		return;
	}
	// no picture: the console's tag as a wordmark on a surface card (radius 28)
	int h = bh * 60 / 100, y = top + (bh - h) / 2;
	BOS_rrect(s, bx + bw / 10, y, bw * 8 / 10, h, BOS_px(28), over(BOS_theme()->surface, a));
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

static int bg_band_y[3], bg_band_h[3]; // the rows that move: of the wheel, of the covers, of the list
static int bg_title_h;

static void wheelRender(SDL_Surface* s) {
	barTitle(s, I18N_text("Games"));
	int top = BOS_px(BOS_BAR_H) + BOS_px(18);
	bg_band_y[0] = 0;
	bg_band_h[0] = top + BOS_px(WSLOT[0].h); // no console hangs lower than the centre one
	int n = bg_ncons;
	// farthest first, so the nearer ones (and last the centred one) are drawn over them
	int order[64], no = 0;
	for (int i = 0; i < n && no < 64; i++) {
		float d = n > 4 ? wrapD(i - bg_pos, n) : i - bg_pos;
		if (fabsf(d) <= 2.6f)
			order[no++] = i;
	}
	for (int a1 = 0; a1 < no; a1++)
		for (int b1 = a1 + 1; b1 < no; b1++) {
			float da = fabsf(n > 4 ? wrapD(order[a1] - bg_pos, n) : order[a1] - bg_pos);
			float db = fabsf(n > 4 ? wrapD(order[b1] - bg_pos, n) : order[b1] - bg_pos);
			if (db > da) {
				int t = order[a1];
				order[a1] = order[b1];
				order[b1] = t;
			}
		}
	for (int k = 0; k < no; k++) {
		int i = order[k];
		float d = n > 4 ? wrapD(i - bg_pos, n) : i - bg_pos, ad = fabsf(d);
		float off = ad <= 1 ? ad * WSTEP1 : WSTEP1 + (ad - 1) * WSTEP2;
		int cx = s->w / 2 + BOS_px((int)(d < 0 ? -off : off));
		int bh = BOS_px((int)WBOX_H(ad));
		// every box centred on the centre one's middle, the side ones lower by their drop
		int y = top + (BOS_px(WSLOT[0].h) - bh) / 2 + BOS_px((int)byDist(ad, WSLOT[0].drop, WSLOT[1].drop, WSLOT[2].drop));
		wheelItem(s, &bg_cons[i], ad, cx, y);
	}
	BOS_benchLap("wheel");
	if (n == 0 || BOS_partialOn())
		return;
	// the centred console's name under its picture: 56/700, then "<N> เกม" 30 text2
	BgConsole* c = &bg_cons[bg_sel];
	int ny = top + BOS_px(WSLOT[0].h) + BOS_px(20);
	char name[128];
	centredName(s, c->name, BOS_FS_TITLE, 700, BOS_C_TEXT, 1, ny);
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
			BOS_rrect(s, x, y, w, dw, dw / 2, i == bg_sel ? BOS_C_TEXT : BOS_theme()->surface2);
			x += w + gap;
		}
	}
	BOS_benchLap("name+dots");
	if (pick_kind) {
		BosHint p[] = {{"A", I18N_text(pick_kind == PICK_CONSOLE ? "Choose" : "Open")}, {"B", I18N_text("Cancel")}};
		BOS_hintBar(s, p, 2, NULL, 0);
		return;
	}
	BosHint r[] = {{"A", I18N_text("Open")}, {"B", I18N_text("Back")}, {"Y", I18N_text("Search")}};
	BOS_hintBar(s, r, 3, NULL, 0);
	BOS_benchLap("hint");
}

// ---- step 2: the console's games ----
static int gamesCount(void) {
	return bg_games && bg_games->entries ? bg_games->entries->count : 0;
}

static void gamePic(SDL_Surface* s, Entry* e, int x, int y, int w, int h, int r, float a, bool ring) {
	SDL_Surface* pic = NULL;
	if (e->type == ENTRY_ROM) {
		HomeArtState st = HomeArt_pin(e->path, w, h, r, &pic);
		if (st == HOMEART_LOADING)
			pending = true;
	}
	if (pic) {
		BOS_blit(pic, s, x, y, a);
	} else {
		BOS_rrect(s, x, y, w, h, r, over(BOS_theme()->surface, a));
		int ic = h / 3;
		BOS_icon(s, e->type == ENTRY_DIR ? "folder" : "game", BOS_C_TEXTOFF, a, x + (w - ic) / 2, y + (h - ic) / 2, ic);
	}
	if (ring) // last, over the picture (DESIGN 1: draw order)
		BOS_focusRing(s, x, y, w, h, r);
}

// A cover of the carousel in box b at colour a: the picture pinned once at the centre size, scaled by carPic
static void coverPic(SDL_Surface* s, Entry* e, SDL_Rect b, int r, float a, int cw, int ch) {
	SDL_Surface* pic = NULL;
	if (e->type == ENTRY_ROM) {
		HomeArtState st = HomeArt_pin(e->path, cw, ch, BOS_px(24), &pic);
		if (st == HOMEART_LOADING)
			pending = true;
	}
	if (pic) {
		carPic(s, pic, b, a);
	} else {
		BOS_rrect(s, b.x, b.y, b.w, b.h, r, over(BOS_theme()->surface, a));
		int ic = b.h / 3;
		BOS_icon(s, e->type == ENTRY_DIR ? "folder" : "game", BOS_C_TEXTOFF, a, b.x + (b.w - ic) / 2, b.y + (b.h - ic) / 2, ic);
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

// A direction held long enough to race through the games: the letter going by, large (§5.7)
static bool bg_letter_shown;
static void heldLetter(SDL_Surface* s, Entry* e, int cx, int cy, bool sprite) {
	bg_letter_shown = false;
	if (PAD_heldRepeats(BTN_LEFT | BTN_RIGHT | BTN_UP | BTN_DOWN) <= 5 || !e->name[0])
		return;
	char ch[8] = {0};
	unsigned char c0 = (unsigned char)e->name[0];
	int n = c0 < 0x80 ? 1 : c0 < 0xE0 ? 2 : c0 < 0xF0 ? 3 : 4;
	for (int i = 0; i < n && e->name[i]; i++)
		ch[i] = e->name[i];
	if (n == 1 && ch[0] >= 'a' && ch[0] <= 'z')
		ch[0] -= 32;
	int box = BOS_px(168), px = BOS_px(104);
	bg_letter_shown = true;
	if (sprite && carSprites()) { // over the covers (GPU sprites): a sprite of its own, made once a letter
		static SDL_Surface* ls;
		static char lch[8];
		if (!ls || strcmp(lch, ch)) {
			GFX_freeSurfaceAndTexture(ls);
			ls = carShape(box, box, BOS_px(36), 0, BOS_theme()->focus);
			if (ls)
				BOS_textMid(ls, ch, px, 700, BOS_theme()->onfocus, 1, (box - BOS_textW(ch, px, 700)) / 2, box / 2);
			snprintf(lch, sizeof(lch), "%s", ch);
		}
		SDL_Texture* t = ls ? PLAT_textureForSurface(ls) : NULL;
		if (t) {
			PLAT_spriteAdd(t, NULL, &(SDL_Rect){cx - box / 2, cy - box / 2, box, box}, 255, NULL);
			return;
		}
	}
	BOS_rrect(s, cx - box / 2, cy - box / 2, box, box, BOS_px(36), BOS_theme()->focus);
	BOS_textMid(s, ch, px, 700, BOS_theme()->onfocus, 1, cx - BOS_textW(ch, px, 700) / 2, cy);
}

// §6.3 (board v2-GamesMotion): the cover carousel CAR_COVERS (home_bos.c)
static void coversRender(SDL_Surface* s, int top) {
	int ng = gamesCount();
	int cw = BOS_px(CAR_COVERS.cw), ch = BOS_px(CAR_COVERS.ch);
	int order[8], no = carOrder(bg_gpos, 0, ng - 1, order, 8);
	for (int k = 0; k < no; k++) { // the farthest first: the centred one over them all
		int r;
		float a, d = order[k] - bg_gpos;
		SDL_Rect b = carBox(&CAR_COVERS, s->w / 2 - BOS_bumpPx(), top, d, &r, &a);
		coverPic(s, bg_games->entries->items[order[k]], b, r, a, cw, ch);
		if (fabsf(d) < 0.001f && bg_gstart == 0)
			carRing(s, b, r); // its ring once it has settled, last (DESIGN 1)
	}
	if (ng == 0)
		return;
	Entry* e = bg_games->entries->items[bg_gsel];
	launch_rect = (SDL_Rect){(s->w - cw) / 2, top, cw, ch};
	bg_band_y[1] = top - 2 * BOS_px(BOS_RING); // the ring, the covers, the caption: down to the hint bar
	bg_band_h[1] = s->h - BOS_px(BOS_BAR_H) - bg_band_y[1];
	heldLetter(s, e, s->w / 2, top + ch / 2, true);
	int y = top + ch + BOS_px(14);
	char name[256];
	centredName(s, e->name, BOS_FS_BODY, 600, BOS_C_TEXT, 1, y);
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

static void listRowText(SDL_Surface* s, Entry* e, int x, int y, int w, int h, bool on, bool marquee) {
	int pad = BOS_px(24), pill_w = 0;
	float a = on ? 1.0f : BOS_DIM;
	uint32_t fg = on ? BOS_theme()->onfocus : BOS_C_TEXT;
	if (canResume(e)) {
		const char* rl = I18N_text("Continue");
		pill_w = pillW(rl, true) + BOS_px(12);
		pill(s, x + w - pad - pillW(rl, true), y + h / 2, rl, BOS_C_OK);
	}
	if (marquee) // the focused row's long name scrolls
		BOS_textMarquee(s, e->name, BOS_FS_BODY, 600, fg, a, x + pad, y + h / 2, w - 2 * pad - pill_w);
	else {
		char name[256];
		BOS_fit(e->name, BOS_FS_BODY, on ? 600 : 500, w - 2 * pad - pill_w, name, sizeof(name));
		BOS_textMid(s, name, BOS_FS_BODY, on ? 600 : 500, fg, a, x + pad, y + h / 2);
	}
}

static BosGlide bg_gl_top, bg_gl_sel; // the list's scroll and its highlight follow the selection (§5.7: 180 / 120 ms)
static bool bg_list_live, bg_list_was; // the list was this page's last frame
static Directory* bg_list_of;

static void listRender(SDL_Surface* s, int top) { // §6.4: rows of 96 on the left, the focused game's cover on the right
	int ng = gamesCount();
	if (bg_gsel < bg_gtop)
		bg_gtop = bg_gsel;
	if (bg_gsel >= bg_gtop + LIST_ROWS)
		bg_gtop = bg_gsel - LIST_ROWS + 1;
	int lx = BOS_px(24), lw = BOS_px(560), rh = BOS_px(BOS_ROW_H);
	int gw = (int)(lw * BOS_ROW_SCALE) - lw, gh = (int)(rh * BOS_ROW_SCALE) - rh; // the focus fill is x1.04
	bool snap = !bg_list_was || bg_list_of != bg_games; // the list was not on screen just now: it starts in place
	bg_list_live = true;
	bg_list_of = bg_games;
	float ftop = BOS_glideTo(&bg_gl_top, (float)bg_gtop, 180, snap);
	float fsel = BOS_glideTo(&bg_gl_sel, (float)bg_gsel, 120, snap);
	int bump = BOS_bumpPx();
	bool still = !BOS_glideMoving(&bg_gl_top) && !BOS_glideMoving(&bg_gl_sel) && !bump;
	SDL_Rect area = {0, top - gh, lx + lw + gw, LIST_ROWS * rh + 2 * gh};
	bg_band_y[2] = area.y;
	bg_band_h[2] = area.h;
	int first = (int)ftop;
	BOS_clip(s, &area);
	for (int i = first < 0 ? 0 : first; i <= first + LIST_ROWS && i < ng; i++) {
		if (still && i == bg_gsel)
			continue; // drawn on the fill below
		listRowText(s, bg_games->entries->items[i], lx, top + (int)((i - ftop) * rh), lw, rh, false, false);
	}
	if (ng > 0) {
		int px = lx - gw / 2, py = top + (int)((fsel - ftop) * rh) - gh / 2 + bump, pw = lw + gw, ph = rh + gh;
		BOS_rrect(s, px, py, pw, ph, BOS_px(BOS_RADIUS_ROW), BOS_theme()->focus);
		if (still)
			listRowText(s, bg_games->entries->items[bg_gsel], px, py, pw, ph, true, true);
		else { // on its way: whatever rows it covers, in the focus colours, cut to its shape
			SDL_Rect pr = {px, py, pw, ph}, cl;
			if (SDL_IntersectRect(&pr, &area, &cl)) {
				BOS_clip(s, &cl);
				for (int i = first < 0 ? 0 : first; i <= first + LIST_ROWS && i < ng; i++) {
					int y = top + (int)((i - ftop) * rh);
					if (y + rh > py && y < py + ph)
						listRowText(s, bg_games->entries->items[i], px, y - gh / 2, pw, ph, true, false);
				}
			}
		}
	}
	BOS_clip(s, NULL);
	BOS_benchLap("rows");
	if (ng == 0)
		return;
	Entry* e = bg_games->entries->items[bg_gsel];
	int rx = lx + lw + BOS_px(32), cw = BOS_px(384), ch = BOS_px(288);
	gamePic(s, e, rx, top, cw, ch, BOS_px(24), 1, false);
	BOS_benchLap("pic");
	launch_rect = (SDL_Rect){rx, top, cw, ch};
	char meta[128];
	lastPlayed(e, meta, sizeof(meta));
	int y = top + ch + BOS_px(12);
	if (meta[0])
		BOS_text(s, meta, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, rx, y);
	y += BOS_lineH(BOS_FS_SMALL) + BOS_px(12); // the line's space stays, so the pill never jumps
	if (canResume(e))
		pill(s, rx, y + BOS_px(22), I18N_text("Continue"), BOS_C_OK);
	heldLetter(s, e, lx + lw / 2, top + LIST_ROWS * rh / 2, false);
}

static void gamesRender(SDL_Surface* s) {
	bool part = BOS_partialOn(); // only the rows of the covers (or the list): the bars and the title stay as they are
	int top = BOS_px(BOS_BAR_H);
	if (!part) {
		barTitle(s, I18N_text("Games"));
		BgConsole* c = &bg_cons[bg_sel];
		char sub[96];
		snprintf(sub, sizeof(sub), I18N_text("%d games · Sorted A-Z"), gamesCount());
		bg_title_h = BOS_largeTitle(s, top, c->name, sub);
	}
	top += bg_title_h;
	BOS_benchLap("title");
	if (gamesCount() == 0) {
		const char* t = I18N_text("No games yet");
		BOS_text(s, t, BOS_FS_BODY, 400, BOS_C_TEXT2, 1, (s->w - BOS_textW(t, BOS_FS_BODY, 400)) / 2, top + BOS_px(120));
	} else if (bg_listmode)
		listRender(s, top);
	else
		coversRender(s, top);
	BOS_benchLap("body");
	if (part)
		return;
	if (pick_kind) { // nothing to pick in an empty console: only B
		BosHint p[] = {{"A", I18N_text("Choose")}, {"B", I18N_text("Back")}};
		BOS_hintBar(s, gamesCount() > 0 ? p : p + 1, gamesCount() > 0 ? 2 : 1, NULL, 0);
		return;
	}
	BosHint r[] = {{"A", I18N_text("Play")}, {"B", I18N_text("Back")}, {"X", I18N_text("Favorite")}, {"Y", I18N_text("Search")}};
	BosHint l[] = {{"MENU", I18N_text("Options")}};
	BOS_hintBar(s, r, 4, l, gamesCount() > 0 ? 1 : 0);
	BOS_benchLap("hint");
}

void BosGames_render(SDL_Surface* s) {
	if (!bg_built)
		bgBuild();
	pending = false;
	car_tex_made = 0;
	launch_rect = (SDL_Rect){0, 0, 0, 0};
	bg_letter_shown = false;
	bg_list_was = bg_list_live;
	bg_list_live = false;
	BOS_benchLap(NULL);
	// While only the wheel, the covers or the list move, only their rows are drawn. The rows of the wheel start at
	// the top of the screen (its bar is always drawn); those of the games leave the bars out, so they are only the
	// frames of a move (a clock that changes on a still page gets a whole frame).
	bool can = bg_ncons > 0;
	static bool was_loading; // the last frame was waiting for a cover
	bool moving = bg_gstart != 0 || BOS_glideMoving(&bg_gl_top) || BOS_glideMoving(&bg_gl_sel) || BOS_bumpActive() ||
				  BOS_marqueeActive() || was_loading;
	if (can && bg_step2) {
		bgLoadGames();
		can = gamesCount() > 0 && moving;
	}
	int kind = !bg_step2 ? 0 : bg_listmode ? 2 : 1;
	// (the focused game shows only inside the games' rows; the minute is in the bar they leave out)
	uint32_t key = BOS_keyMix(BOS_keyMix(BOS_keyMix(20 + kind, bg_sel), bg_step2 ? (uint32_t)(time(NULL) / 60) : 0), pick_kind);
	key = BOS_keyMix(BOS_keyMix(key, bg_step2 ? gamesCount() : bg_ncons), bg_ncons > 0 ? bg_cons[bg_sel].count : 0);
	BOS_partialBegin(s, key, can, bg_band_y[kind], bg_band_h[kind]);
	SDL_FillRect(s, NULL, BOS_map(s, BOS_theme()->bg));
	BOS_benchLap("fill");
	if (bg_ncons == 0) {
		barTitle(s, I18N_text("Games"));
		BosHint r[] = {{"B", I18N_text("Back")}};
		BOS_hintBar(s, r, 1, NULL, 0);
		return;
	}
	if (bg_step2) {
		bgLoadGames();
		springStep(&bg_gpos, &bg_gvel, &bg_glast, &bg_gstart, (float)bg_gsel);
		gamesRender(s);
	} else {
		if (bg_start) { // the wheel's spring, the short way round
			bool wrap = bg_ncons > 4;
			springStep(&bg_pos, &bg_vel, &bg_last, &bg_start, wrap ? bg_pos + wrapD(bg_sel - bg_pos, bg_ncons) : bg_sel);
			if (wrap)
				bg_pos = fmodf(fmodf(bg_pos, bg_ncons) + bg_ncons, bg_ncons);
			if (!bg_start)
				bg_pos = (float)bg_sel;
		}
		wheelRender(s);
	}
	was_loading = pending;
	BOS_partialEnd(s);
}

bool BosGames_animating(void) {
	return pending || bg_letter_shown || bg_start != 0 || bg_gstart != 0 || BOS_marqueeActive() || HomeArt_checkAsyncLoaded() || GameInfo_checkAsyncLoaded();
}

Entry* BosGames_focusedGame(void) {
	return bg_step2 && gamesCount() > 0 ? bg_games->entries->items[bg_gsel] : NULL;
}

static void wheelMove(int dir) {
	if (bg_ncons < 2)
		return;
	bg_sel = (bg_sel + dir + bg_ncons) % bg_ncons;
	bg_want_rom[0] = '\0';
	if (!bg_start) // from rest: no old speed, no old time
		bg_vel = 0, bg_last = 0;
	bg_start = 1;
	// keep only the pictures near the selection (each console holds its fitted copies, one per size step)
	for (int i = 0; i < bg_ncons; i++)
		if (fabsf(wrapD(i - bg_sel, bg_ncons)) > 5.5f && bg_cons[i].tried)
			freePics(&bg_cons[i]);
}

static void gameMove(int to) {
	int ng = gamesCount();
	if (to < 0 || to >= ng || to == bg_gsel)
		return;
	bg_gsel = to;
	bg_want_rom[0] = '\0';
	if (!bg_listmode) {
		if (!bg_gstart)
			bg_gvel = 0, bg_glast = 0;
		bg_gstart = 1;
	} else
		bg_gpos = (float)to;
}

// one step along the games: at an end a fresh press goes round, a held one knocks (§5.7)
static void gameStep(int dir, int btn) {
	int ng = gamesCount(), to = bg_gsel + dir;
	if (to < 0 || to >= ng) {
		if (ng < 2 || !PAD_justPressed(btn)) {
			BOS_bump(dir);
			return;
		}
		to = to < 0 ? ng - 1 : 0;
		gameMove(to);
		if (!bg_listmode) // the covers come in from one place outside, not past every game on the way
			bg_gpos = to == 0 ? -1.0f : (float)ng, bg_gvel = 0;
		return;
	}
	gameMove(to);
}

// pick mode (DESIGN §6.9b): the wheel and the games as they are, A picks, B backs out; nothing else
static BosGamesInput pickGamesInput(bool tap_select) {
	if (!bg_step2) {
		if (PAD_justPressed(BTN_B))
			pickDone(NULL, NULL);
		else if (PAD_justRepeated(BTN_LEFT))
			wheelMove(-1);
		else if (PAD_justRepeated(BTN_RIGHT))
			wheelMove(1);
		else if (PAD_justPressed(BTN_A) && bg_ncons > 0) {
			BgConsole* c = &bg_cons[bg_sel];
			if (pick_kind == PICK_CONSOLE) {
				char v[MAX_PATH + 16];
				snprintf(v, sizeof(v), "console:%s", c->path);
				pickDone(v, c->name);
			} else {
				bg_games_for = -1;
				bgLoadGames();
				bg_step2 = true;
			}
		}
		return BOSGAMES_HANDLED;
	}
	bgLoadGames();
	int ng = gamesCount();
	int prev = bg_listmode ? BTN_UP : BTN_LEFT, next = bg_listmode ? BTN_DOWN : BTN_RIGHT;
	if (PAD_justPressed(BTN_B))
		bg_step2 = false;
	else if (tap_select) {
		bg_listmode = !bg_listmode;
		setListMode(bg_cons[bg_sel].path, bg_listmode);
		bg_gpos = (float)bg_gsel;
		bg_gstart = 0;
	} else if (PAD_justRepeated(prev))
		gameStep(-1, prev);
	else if (PAD_justRepeated(next))
		gameStep(1, next);
	else if (bg_listmode && PAD_justRepeated(BTN_LEFT))
		gameMove(bg_gsel - LIST_ROWS > 0 ? bg_gsel - LIST_ROWS : 0);
	else if (bg_listmode && PAD_justRepeated(BTN_RIGHT))
		gameMove(bg_gsel + LIST_ROWS < ng ? bg_gsel + LIST_ROWS : ng - 1);
	else if (PAD_justPressed(BTN_A) && ng > 0) {
		Entry* e = bg_games->entries->items[bg_gsel];
		char v[MAX_PATH + 16];
		snprintf(v, sizeof(v), "game:%s", e->path);
		pickDone(v, e->name);
	}
	return BOSGAMES_HANDLED;
}

BosGamesInput BosGames_handleInput(unsigned long now, bool* dirty, bool tap_menu, bool tap_select) {
	static bool bg_was_busy = true;
	bool bg_busy = !bg_built || PAD_anyPressed() || PAD_anyJustReleased() || tap_menu || tap_select;
	if (!bg_built)
		bgBuild();
	if (bg_busy || bg_was_busy)
		*dirty = true;
	bg_was_busy = bg_busy;
	if (pick_kind == PICK_CONSOLE || pick_kind == PICK_GAME)
		return pickGamesInput(tap_select);
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
		gameStep(-1, prev);
	else if (PAD_justRepeated(next))
		gameStep(1, next);
	else if (bg_listmode && PAD_justRepeated(BTN_LEFT))
		gameMove(bg_gsel - LIST_ROWS > 0 ? bg_gsel - LIST_ROWS : 0);
	else if (bg_listmode && PAD_justRepeated(BTN_RIGHT))
		gameMove(bg_gsel + LIST_ROWS < ng ? bg_gsel + LIST_ROWS : ng - 1);
	else if ((PAD_justRepeated(BTN_L2) || PAD_justRepeated(BTN_R2)) && ng > 0) { // a screen, no wrap (DESIGN 7.7)
		int dir = PAD_justRepeated(BTN_L2) ? -1 : 1, to = bg_gsel + dir * (bg_listmode ? LIST_ROWS : 5);
		to = to < 0 ? 0 : to >= ng ? ng - 1 : to;
		if (to != bg_gsel)
			gameMove(to);
		else
			BOS_bump(dir);
	} else if (PAD_justPressed(BTN_X) && ng > 0) {
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
			bg_pos = (float)bg_sel;
			bg_games_for = -1;
			bgLoadGames();
			int n2 = gamesCount();
			bg_gsel = in_favs ? (gsel < n2 ? gsel : n2 - 1) : gsel;
			if (bg_gsel < 0)
				bg_gsel = 0;
			bg_gpos = (float)bg_gsel;
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
