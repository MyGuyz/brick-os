/*
 * settings_updater.c - System updater for NxRedux Settings
 *
 * Checks GitHub for the latest release, compares against the installed
 * version, and allows the user to download + install updates.
 *
 * Single "Updater" item in the About page:
 *   - "Checking for update.."       while checking in background
 *   - "Install Update"  (desc=tag)  when update available, A to install
 *   - "You already have latest version"  when up to date
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "utils.h"
#include <sys/wait.h>

#include "settings_updater.h"
#include "settings_menu.h"
#include "defines.h"
#include "api.h"
#include "wifi.h"
#include "ui_buttonhintbar.h"
#include "ui_downloadprogress.h"
#include "ui_menubar.h"
#include "wget_fetch.h"
#include "updater_url.h"
#include "cjson/cJSON.h"
#include "i18n.h"
#include "bos_ui.h"
#include <ctype.h>
#include <signal.h>
#include <strings.h>
#include <sys/statvfs.h>

// ============================================
// Configuration
// ============================================

#define UPDATER_REPO_OWNER "MyGuyz"
#define UPDATER_REPO_NAME "brick-os"
// Web endpoint that 302-redirects to /releases/tag/<TAG>; unlike
// api.github.com it is not subject to the 60 req/hour per-IP rate limit.
#define UPDATER_WEB_LATEST_URL \
	"https://github.com/" UPDATER_REPO_OWNER "/" UPDATER_REPO_NAME "/releases/latest"
#define VERSION_FILE_PATH "/mnt/SDCARD/.system/version.txt"
#define DOWNLOAD_PATH "/mnt/SDCARD/.tmp_update.zip"
#define EXTRACT_DEST "/mnt/SDCARD/"
#define UPDATER_7Z "/mnt/SDCARD/.system/shared/bin/7zzs.aarch64"
// Free space needed before downloading: the release zip and the MinUI.zip
// unpacked from it sit on the card together (~550 MB), then the next boot
// unpacks MinUI.zip (~540 MB) while it is still there.
#define UPDATE_MIN_FREE_MB 1024

// ============================================
// Device name mapping
// ============================================

static const char* get_device_name(void) {
	if (strcmp(PLATFORM, "tg5050") == 0)
		return "smartpros";
	char* device = getenv("DEVICE");
	if (device && strcmp(device, "brick") == 0)
		return "brick";
	if (device && strcmp(device, "brickpro") == 0)
		return "brickpro";
	return "smartpro";
}

// Release-asset filename suffix after "BrickOS-<tag>-": one zip per device.
static const char* get_asset_suffix(void) {
	static char suffix[80];
	snprintf(suffix, sizeof(suffix), "%s.zip", get_device_name());
	return suffix;
}

// ============================================
// Release info
// ============================================

typedef struct {
	char tag_name[128];
	char download_url[512];
	char release_notes[8192]; // the release body as plain text ("" if not fetched)
	char expected_sha256[80]; // hex SHA-256 from the asset "digest" ("" if none)
} ReleaseInfo;

// ============================================
// State
// ============================================

typedef enum {
	UPDATE_IDLE,
	UPDATE_CHECKING,
	UPDATE_UP_TO_DATE,
	UPDATE_AVAILABLE,
	UPDATE_ERROR,
} UpdateCheckState;

static UpdateCheckState auto_state = UPDATE_IDLE;
static ReleaseInfo cached_release = {0};
static char item_label[160] = "Updater";
static char item_desc[256] = "";
static char current_tag_cache[128] = "";

// Background check thread
static pthread_t auto_tid;
static volatile int auto_done = 0;
static volatile int auto_success = 0;
static char auto_tag[128] = "";
static char auto_error[256] = "";

// ============================================
// Version reading
// ============================================

static void read_current_version(char* version, size_t ver_size,
								 char* sha, size_t sha_size,
								 char* tag, size_t tag_size) {
	version[0] = '\0';
	sha[0] = '\0';
	tag[0] = '\0';

	FILE* f = fopen(VERSION_FILE_PATH, "r");
	if (!f) {
		snprintf(version, ver_size, "Unknown");
		return;
	}

	if (!fgets(version, ver_size, f)) {
		snprintf(version, ver_size, "Unknown");
		fclose(f);
		return;
	}
	version[strcspn(version, "\r\n")] = '\0';

	if (!fgets(sha, sha_size, f))
		sha[0] = '\0';
	sha[strcspn(sha, "\r\n")] = '\0';

	if (!fgets(tag, tag_size, f))
		tag[0] = '\0';
	tag[strcspn(tag, "\r\n")] = '\0';

	fclose(f);
}

// ============================================
// Command execution (no shell, avoids injection)
// ============================================

static int run_command(char* const argv[]) {
	pid_t pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		freopen("/dev/null", "w", stderr);
		execvp(argv[0], argv);
		_exit(127);
	}

	int status;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return -1;
}

// ============================================
// Background auto-check thread
// ============================================

// Buffer size for GitHub API response (releases/latest JSON)
#define API_RESPONSE_SIZE (64 * 1024)

// Buffer size for the wget -S header dump of the releases/latest redirect
#define HEADERS_BUF_SIZE 8192
// A moved or renamed repo answers releases/latest with a 301 to its new
// releases/latest first; follow that many hops to reach the tag redirect.
#define UPDATER_MAX_HOPS 3

static void* auto_check_thread(void* arg) {
	(void)arg;

	char headers[HEADERS_BUF_SIZE];
	char url[512];
	char location[512];
	int found = 0;

	snprintf(url, sizeof(url), "%s", UPDATER_WEB_LATEST_URL);
	for (int hop = 0; hop < UPDATER_MAX_HOPS && !found; hop++) {
		if (wget_fetch_headers_noredirect(url, headers, sizeof(headers)) < 0 ||
			updater_parse_location_header(headers, location, sizeof(location)) != 0)
			break;
		if (updater_parse_tag_from_location(location, auto_tag, sizeof(auto_tag)) == 0)
			found = 1;
		else if (strncmp(location, "https://github.com/", 19) == 0)
			snprintf(url, sizeof(url), "%s", location);
		else
			break;
	}

	if (!found) {
		snprintf(auto_error, sizeof(auto_error), "Failed to check for updates");
		auto_success = 0;
		__sync_synchronize();
		auto_done = 1;
		return NULL;
	}

	auto_success = 1;
	__sync_synchronize();
	auto_done = 1;
	return NULL;
}

// Compare the fetched tag and update state. Called from main thread.
static void process_auto_check_result(void) {
	pthread_join(auto_tid, NULL);

	if (!auto_success) {
		auto_state = UPDATE_ERROR;
		snprintf(item_label, sizeof(item_label), "Updater");
		snprintf(item_desc, sizeof(item_desc), "%s", auto_error);
		return;
	}

	// Compare tag names
	int is_same = 0;
	if (current_tag_cache[0] && auto_tag[0]) {
		if (strcmp(current_tag_cache, auto_tag) == 0)
			is_same = 1;
	}

	if (is_same) {
		auto_state = UPDATE_UP_TO_DATE;
		snprintf(item_label, sizeof(item_label), "You already have latest version");
		item_desc[0] = '\0';
	} else {
		// Only the tag is known from the redirect; download URL, release
		// notes and sha256 are filled in at install time (fetch_release_details).
		memset(&cached_release, 0, sizeof(cached_release));
		snprintf(cached_release.tag_name, sizeof(cached_release.tag_name),
				 "%s", auto_tag);
		auto_state = UPDATE_AVAILABLE;
		// อัปเดต (DESIGN 6.7a): "อัปเดตเป็น 0.7.2" with a ใหม่ pill
		const char* v = cached_release.tag_name;
		if (!strncmp(v, "bos-", 4))
			v += 4;
		if (v[0] == 'v')
			v++;
		snprintf(item_label, sizeof(item_label), "%s %s", I18N_text("Update to"), v);
		item_desc[0] = '\0';
	}
}

// ============================================
// Extract context (for 7z thread)
// ============================================

typedef struct {
	volatile int done;
	volatile int success;
	char error[256];
} ExtractContext;

static void* extract_thread(void* arg) {
	ExtractContext* ctx = (ExtractContext*)arg;

	char out_arg[256];
	snprintf(out_arg, sizeof(out_arg), "-o%s", EXTRACT_DEST);
	// The release zip also carries the card skeleton (Roms/Bios folders,
	// Shaders presets, Overlays, sample art) that players edit or replace,
	// so that part only adds what is missing (-aos). The system parts -
	// MinUI.zip, the pakz, the stock-launcher shim and the shader sources -
	// are always replaced (-aoa) in a second pass.
	char* user_argv[] = {UPDATER_7Z, "x", DOWNLOAD_PATH, out_arg, "-aos",
						 "-x!MinUI.zip", "-x!*.pakz", "-x!trimui", "-x!README.txt",
						 "-x!Shaders/glsl", NULL};
	char* system_argv[] = {UPDATER_7Z, "x", DOWNLOAD_PATH, out_arg, "-aoa",
						   "MinUI.zip", "*.pakz", "trimui", "README.txt", "Shaders/glsl", NULL};

	int ret = run_command(user_argv);
	if (ret == 0)
		ret = run_command(system_argv);
	if (ret != 0) {
		// Don't leave the zip in the card root: a retry downloads it again,
		// and Windows Explorer prompts for a "multi-volume set" on a bad zip.
		unlink(DOWNLOAD_PATH);
		// nor a possibly truncated MinUI.zip for the next boot to install
		unlink(EXTRACT_DEST "MinUI.zip");
		snprintf(ctx->error, sizeof(ctx->error), "Extraction failed");
		ctx->success = 0;
		__sync_synchronize();
		ctx->done = 1;
		return NULL;
	}

	unlink(DOWNLOAD_PATH);

	ctx->success = 1;
	__sync_synchronize();
	ctx->done = 1;
	return NULL;
}

// ============================================
// Download state (shared with download thread)
// ============================================

static WgetDownloadJob dl_ctx;

// ============================================
// Full-screen update page helpers
// ============================================

static void render_update_page(SDL_Surface* screen, const char* title,
							   const char* status, int progress_pct,
							   const char* speed_str, const char* eta_str,
							   int show_cancel) {
	SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
	BOS_statusBar(screen, I18N_text("Update"));
	int px = BOS_px(BOS_PAGE_PAD), w = screen->w - 2 * px; // under the large title
	int y = BOS_px(BOS_BAR_H) + BOS_px(16);
	y += BOS_largeTitle(screen, y, I18N_text(title), NULL);
	y += BOS_px(24);
	if (status && status[0])
		y += BOS_textWrap(screen, I18N_text(status), BOS_FS_BODY, 600, BOS_C_TEXT, 1, px, y, w, 3) + BOS_px(28);
	if (progress_pct >= 0) { // the bar and its percent, as the HUD's level bar
		int pct = progress_pct > 100 ? 100 : progress_pct, bh = BOS_px(14);
		char num[16];
		snprintf(num, sizeof(num), "%d%%", pct);
		int nw = BOS_textW("100%", BOS_FS_SMALL, 600), bw = w - nw - BOS_px(24);
		int cy = y + BOS_lineH(BOS_FS_SMALL) / 2;
		BOS_rrect(screen, px, cy - bh / 2, bw, bh, bh / 2, BOS_theme()->surface2);
		int fw = bw * pct / 100;
		if (fw > 0)
			BOS_rrect(screen, px, cy - bh / 2, fw < bh ? bh : fw, bh, bh / 2, BOS_theme()->primary);
		BOS_textMid(screen, num, BOS_FS_SMALL, 600, BOS_C_TEXT, 1, px + w - BOS_textW(num, BOS_FS_SMALL, 600), cy);
		y += BOS_lineH(BOS_FS_SMALL) + BOS_px(16);
	}
	char detail[128] = "", left[64] = ""; // speed and time left
	if (eta_str && eta_str[0])
		snprintf(left, sizeof(left), I18N_text("%s left"), eta_str);
	if (speed_str && speed_str[0] && left[0])
		snprintf(detail, sizeof(detail), "%s · %s", speed_str, left);
	else if (speed_str && speed_str[0])
		snprintf(detail, sizeof(detail), "%s", speed_str);
	else
		snprintf(detail, sizeof(detail), "%s", left);
	if (detail[0])
		BOS_text(screen, detail, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, px, y);
	if (show_cancel) {
		BosHint r[] = {{"B", I18N_text("Cancel")}};
		BOS_hintBar(screen, r, 1, NULL, 0);
	} else
		BOS_hintBar(screen, NULL, 0, NULL, 0);
	GFX_flip(screen);
}

static void show_message_page(SDL_Surface* screen, const char* title, const char* message) {
	unsigned long start = SDL_GetTicks();
	while (SDL_GetTicks() - start < 2000) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B))
			break;
		render_update_page(screen, title, message, -1, NULL, NULL, 0);
	}
}

// ============================================
// On-demand release details (notes + asset URL + digest)
// ============================================

// Fetched from api.github.com only when the user actually installs — the
// frequent automatic check never touches the rate-limited API. When this
// fails (e.g. the 60 req/hour per-IP limit), the updater falls back to the
// tag-derived asset URL with no release notes and no sha256 digest.

enum { DETAILS_RUNNING = 0,
	   DETAILS_DONE = 1,
	   DETAILS_ABANDONED = 2 };

typedef struct {
	volatile int state; // DETAILS_* handoff: whoever loses the CAS cleans up
	char* data;			// malloc'd JSON on success, else NULL
} DetailsFetchContext;

static void* details_fetch_thread(void* arg) {
	DetailsFetchContext* ctx = (DetailsFetchContext*)arg;

	char url[256];
	snprintf(url, sizeof(url),
			 "https://api.github.com/repos/%s/%s/releases/latest",
			 UPDATER_REPO_OWNER, UPDATER_REPO_NAME);

	uint8_t* buf = malloc(API_RESPONSE_SIZE);
	if (buf) {
		int ret = wget_fetch(url, buf, API_RESPONSE_SIZE);
		if (ret > 0) {
			buf[ret] = '\0';
			ctx->data = (char*)buf;
		} else {
			free(buf);
		}
	}

	if (!__sync_bool_compare_and_swap(&ctx->state, DETAILS_RUNNING, DETAILS_DONE)) {
		// User cancelled while we were fetching; nobody will read this.
		free(ctx->data);
		free(ctx);
	}
	return NULL;
}

// Fill `release` (already carrying the tag from the redirect check) with the
// download URL, notes and digest. Returns 1 to proceed, 0 if the user
// cancelled, -1 if no download URL could be determined.
static int fetch_release_details(SDL_Surface* screen, ReleaseInfo* release) {
	DetailsFetchContext* ctx = calloc(1, sizeof(*ctx));
	char* data = NULL;

	if (ctx) {
		pthread_t tid;
		if (pthread_create(&tid, NULL, details_fetch_thread, ctx) != 0) {
			free(ctx);
			ctx = NULL; // fall through to the tag-derived URL
		} else {
			pthread_detach(tid);
		}
	}

	while (ctx) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_justPressed(BTN_B)) {
			if (!__sync_bool_compare_and_swap(&ctx->state, DETAILS_RUNNING,
											  DETAILS_ABANDONED)) {
				// Thread finished between frames; the context is ours.
				__sync_synchronize();
				free(ctx->data);
				free(ctx);
			}
			return 0;
		}

		if (ctx->state == DETAILS_DONE) {
			__sync_synchronize();
			data = ctx->data;
			free(ctx);
			ctx = NULL;
			break;
		}

		render_update_page(screen, "Update Available",
						   "Fetching release info...", -1, NULL, NULL, 1);
	}

	if (data) {
		// Full release info from the API: exact asset URL, notes, sha256.
		ReleaseInfo api = {0};
		if (json_extract_string(data, "tag_name", api.tag_name,
								sizeof(api.tag_name)) &&
			updater_find_zip_asset_url(data, get_asset_suffix(), api.download_url,
									   sizeof(api.download_url),
									   api.expected_sha256, sizeof(api.expected_sha256))) {
			// The whole body, unescaped by cJSON (json_extract_string stops at an escaped quote)
			cJSON* root = cJSON_Parse(data);
			const char* body = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "body")) : NULL;
			if (body) {
				snprintf(api.release_notes, sizeof(api.release_notes), "%s", body);
				size_t n = strlen(api.release_notes);
				if (n == sizeof(api.release_notes) - 1) { // cut: drop a character split in two
					while (n > 0 && ((unsigned char)api.release_notes[n - 1] & 0xC0) == 0x80)
						n--;
					if (n > 0 && (unsigned char)api.release_notes[n - 1] >= 0xC0)
						n--;
					api.release_notes[n] = '\0';
				}
			}
			cJSON_Delete(root);
			*release = api;
			free(data);
			return 1;
		}
		free(data);
	}

	// API unavailable (rate limited) or unparsable: derive the asset URL
	// from the tag. Assets are named NXRedux-<tag>-<suffix> from the
	// first release that ships this updater.
	release->release_notes[0] = '\0';
	release->expected_sha256[0] = '\0';
	if (updater_build_fallback_url(UPDATER_REPO_OWNER, UPDATER_REPO_NAME,
								   release->tag_name, get_device_name(),
								   release->download_url,
								   sizeof(release->download_url)) != 0)
		return -1;
	return 1;
}

// The release notes as the update page shows them (DESIGN 6.7e): one block per line of the body
#define NOTE_MAX 80
typedef struct {
	int kind; // NOTE_* (updater_url.h)
	int h;	  // height on the page, its gap below included
	char text[400];
} NoteBlock;
static NoteBlock note_blocks[NOTE_MAX];

static int note_lh(void) { return BOS_lineH(BOS_FS_SMALL); }
static int note_indent(void) { return BOS_px(32); }

// Lay the notes out for width w; returns the block count, *total the height of all of them
static int notes_layout(const char* notes, int w, int* total) {
	int n = 0;
	*total = 0;
	for (const char* p = notes; *p && n < NOTE_MAX;) {
		const char* e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		NoteBlock* b = &note_blocks[n];
		b->kind = updater_note_line(p, len, b->text, sizeof(b->text));
		p += len + (e ? 1 : 0);
		if (b->kind == NOTE_BLANK) {
			if (n == 0 || note_blocks[n - 1].kind == NOTE_BLANK)
				continue; // no gap at the top, one gap for many blank lines
			b->h = BOS_px(16);
		} else {
			int tw = b->kind == NOTE_BULLET ? w - note_indent() : w;
			int weight = b->kind == NOTE_HEADING ? 700 : 400;
			b->h = BOS_textLines(b->text, BOS_FS_SMALL, weight, tw, 99) * note_lh() + BOS_px(6);
		}
		*total += b->h;
		n++;
	}
	while (n > 0 && note_blocks[n - 1].kind == NOTE_BLANK) // no gap at the end
		*total -= note_blocks[--n].h;
	return n;
}

// Show update info (version + every line of the release notes) with A to confirm, B to cancel; ▲▼ scroll the notes,
// L1/R1 a page.
static int show_update_info(SDL_Surface* screen, ReleaseInfo* release) {
	bool dirty = true;
	int px = BOS_px(BOS_PAGE_PAD), w = screen->w - 2 * px;
	const char* notes = release->release_notes[0]
							? release->release_notes
							: I18N_text("Release notes are unavailable right now, but you can still update to the "
										"latest version. Press A to install.");
	int total = 0, scroll = 0;
	int nblocks = notes_layout(notes, w - BOS_px(16), &total); // room for the scroll mark
	int view_top = 0, view_h = 0;
	while (1) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_justPressed(BTN_B))
			return 0;
		if (PAD_justPressed(BTN_A))
			return 1;
		if (view_h > 0 && total > view_h) {
			int to = scroll, page = view_h - note_lh();
			if (PAD_justRepeated(BTN_DOWN))
				to += note_lh() * 2;
			if (PAD_justRepeated(BTN_UP))
				to -= note_lh() * 2;
			if (PAD_justRepeated(BTN_R1))
				to += page;
			if (PAD_justRepeated(BTN_L1))
				to -= page;
			if (to > total - view_h)
				to = total - view_h;
			if (to < 0)
				to = 0;
			if (to != scroll) {
				scroll = to;
				dirty = true;
			}
		}
		if (!dirty) { // a still page: nothing to draw until a key
			GFX_sync();
			continue;
		}
		dirty = false;

		SDL_FillRect(screen, NULL, BOS_map(screen, BOS_theme()->bg));
		BOS_statusBar(screen, I18N_text("Update"));
		int y = BOS_px(BOS_BAR_H) + BOS_px(16); // under the large title
		char sub[160];
		if (current_tag_cache[0])
			snprintf(sub, sizeof(sub), "%s → %s", current_tag_cache, release->tag_name);
		else
			snprintf(sub, sizeof(sub), "%s", release->tag_name);
		y += BOS_largeTitle(screen, y, I18N_text("Update Available"), sub);
		y += BOS_px(24);
		view_top = y;
		view_h = screen->h - BOS_px(BOS_BAR_H) - BOS_px(8) - view_top;
		if (scroll > total - view_h)
			scroll = total - view_h > 0 ? total - view_h : 0;
		SDL_Rect view = {0, view_top, screen->w, view_h};
		BOS_clip(screen, &view);
		int by = view_top - scroll;
		for (int i = 0; i < nblocks && by < view_top + view_h; by += note_blocks[i++].h) {
			NoteBlock* b = &note_blocks[i];
			if (b->kind == NOTE_BLANK || by + b->h <= view_top)
				continue;
			if (b->kind == NOTE_HEADING)
				BOS_textWrap(screen, b->text, BOS_FS_SMALL, 700, BOS_C_TEXT, 1, px, by, w - BOS_px(16), 99);
			else if (b->kind == NOTE_BULLET) {
				BOS_text(screen, "•", BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, px + BOS_px(6), by);
				BOS_textWrap(screen, b->text, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, px + note_indent(), by,
							 w - BOS_px(16) - note_indent(), 99);
			} else
				BOS_textWrap(screen, b->text, BOS_FS_SMALL, 400, BOS_C_TEXT2, 1, px, by, w - BOS_px(16), 99);
		}
		BOS_clip(screen, NULL);
		if (total > view_h) { // a scroll mark (DESIGN 5: 6 px textoff at the right)
			int mh = view_h * view_h / total, my = view_top + (view_h - mh) * scroll / (total - view_h);
			BOS_rrect(screen, screen->w - BOS_px(14), my, BOS_px(6), mh, BOS_px(3), BOS_C_TEXTOFF);
		}
		BosHint r[] = {{"A", I18N_text("Install")}, {"B", I18N_text("Cancel")}};
		BOS_hintBar(screen, r, 2, NULL, 0);
		GFX_flip(screen);
	}
}

// Compute the SHA-256 of a file as a lowercase hex string, using the device's
// sha256sum (busybox). `path` is a compile-time constant (DOWNLOAD_PATH), so
// there is no shell-injection surface. Returns 0 on success, -1 otherwise.
static int sha256_file_hex(const char* path, char out_hex[65]) {
	char cmd[300];
	snprintf(cmd, sizeof(cmd), "sha256sum '%s' 2>/dev/null", path);
	FILE* p = popen(cmd, "r");
	if (!p)
		return -1;
	char line[160] = {0};
	char* got = fgets(line, sizeof(line), p);
	pclose(p);
	if (!got)
		return -1;
	// Output is "<64 hex chars>  <path>"; take and lowercase the first 64.
	for (int i = 0; i < 64; i++) {
		char c = line[i];
		if (!isxdigit((unsigned char)c))
			return -1;
		out_hex[i] = (char)tolower((unsigned char)c);
	}
	out_hex[64] = '\0';
	return 0;
}

// Confirm the downloaded package matches the SHA-256 advertised in the release
// JSON (GitHub's asset "digest"). This guards against a corrupted/truncated
// download being extracted over the system partition. It is an integrity check,
// not authentication — a network attacker who can rewrite the download can also
// rewrite the digest (see the audit notes for why signing would be needed for
// that). Returns 1 = matched, 0 = no digest advertised (skip), -1 = MISMATCH.
static int verify_update_hash(const char* zip_path, const char* expected_sha256) {
	if (!expected_sha256 || expected_sha256[0] == '\0') {
		LOG_warn("[Updater] release has no sha256 digest; skipping hash check\n");
		return 0;
	}
	char actual[65];
	if (sha256_file_hex(zip_path, actual) != 0) {
		LOG_warn("[Updater] could not compute sha256 (sha256sum missing?); skipping\n");
		return 0; // don't block updates if the tool is unavailable
	}
	if (strcasecmp(actual, expected_sha256) != 0) {
		LOG_error("[Updater] sha256 mismatch: expected %s got %s\n", expected_sha256, actual);
		return -1;
	}
	LOG_info("[Updater] update sha256 verified\n");
	return 1;
}

// Run download + extract + reboot with full-screen progress pages.
static void do_install(SDL_Surface* screen, ReleaseInfo* release) {
	pthread_t tid;

	// Running out of space mid-install leaves a half-unpacked update, so
	// refuse up front (a stale partial download is not counted against it)
	unlink(DOWNLOAD_PATH);
	struct statvfs vfs;
	if (statvfs(EXTRACT_DEST, &vfs) == 0) {
		unsigned long long free_mb = (unsigned long long)vfs.f_bavail * vfs.f_frsize / (1024 * 1024);
		if (free_mb < UPDATE_MIN_FREE_MB) {
			char msg[128];
			snprintf(msg, sizeof(msg), I18N_text("Free up %llu MB on the SD card"),
					 UPDATE_MIN_FREE_MB - free_mb);
			show_message_page(screen, "Not Enough Space", msg);
			return;
		}
	}

	// Prevent screen off and disable WiFi power save during update
	PWR_disableAutosleep();
	PWR_disableSleep();
	system("iw dev wlan0 set power_save off 2>/dev/null");

	// --- Download phase ---
	unlink(DOWNLOAD_PATH);

	// Start download in background thread via wget_download_file
	if (!wget_job_start(&dl_ctx, release->download_url, DOWNLOAD_PATH, 0)) {
		PWR_enableSleep();
		PWR_enableAutosleep();
		system("iw dev wlan0 set power_save on 2>/dev/null");
		show_message_page(screen, "Update Error", "Failed to start download");
		return;
	}

	int cancelled = 0;
	int dl_success = 0;

	while (!dl_ctx.done) {
		GFX_startFrame();
		PAD_poll();

		// Cancel
		if (PAD_justPressed(BTN_B)) {
			dl_ctx.cancel = true;
			wget_job_join(&dl_ctx);
			unlink(DOWNLOAD_PATH);
			cancelled = 1;
			break;
		}

		// Read progress from download thread
		int progress_pct = dl_ctx.progress;
		int speed_bps = dl_ctx.speed;
		int eta_sec = dl_ctx.eta;

		char dl_status[256];
		if (progress_pct > 0)
			snprintf(dl_status, sizeof(dl_status), "%s (%d%%)", release->tag_name, progress_pct);
		else
			snprintf(dl_status, sizeof(dl_status), "%s - Connecting...", release->tag_name);

		char speed_str[32] = "";
		char eta_str[32] = "";
		wget_format_speed(speed_bps, speed_str, sizeof(speed_str));
		wget_format_eta(eta_sec, eta_str, sizeof(eta_str));

		render_update_page(screen, "Downloading Update",
						   dl_status, progress_pct, speed_str, eta_str, 1);
	}

	if (!cancelled) {
		wget_job_join(&dl_ctx);
		dl_success = (dl_ctx.result > 0);
	}

	// Re-enable WiFi power save after download (keep sleep disabled through extract)
	system("iw dev wlan0 set power_save on 2>/dev/null");

	if (cancelled) {
		PWR_enableSleep();
		PWR_enableAutosleep();
		return;
	}

	if (!dl_success) {
		// wget_download_file keeps partial files for resume, but the updater
		// never resumes (it unlinks before each download). A truncated zip
		// left in the card root makes Windows Explorer prompt for "the last
		// disk of the multi-volume set" whenever the card is plugged in.
		unlink(DOWNLOAD_PATH);
		PWR_enableSleep();
		PWR_enableAutosleep();
		show_message_page(screen, "Update Error", "Download failed");
		return;
	}

	// --- Verify integrity before extracting over the system partition ---
	{
		render_update_page(screen, "Verifying Update", "Checking download...", -1, NULL, NULL, 0);
		int vr = verify_update_hash(DOWNLOAD_PATH, release->expected_sha256);
		if (vr < 0) { // -1 = mismatch (corrupted download); 0 = nothing to check
			unlink(DOWNLOAD_PATH);
			PWR_enableSleep();
			PWR_enableAutosleep();
			show_message_page(screen, "Update Error", "Download corrupted");
			return;
		}
	}

	// --- Extract phase ---
	ExtractContext ex = {0};

	if (pthread_create(&tid, NULL, extract_thread, &ex) != 0) {
		PWR_enableSleep();
		PWR_enableAutosleep();
		show_message_page(screen, "Update Error", "Failed to start installation");
		return;
	}

	// Render installing page until done
	while (!ex.done) {
		GFX_startFrame();
		PAD_poll();
		render_update_page(screen, "Installing Update",
						   "Please do not power off...", -1, NULL, NULL, 0);
	}
	__sync_synchronize();
	pthread_join(tid, NULL);

	if (!ex.success) {
		PWR_enableSleep();
		PWR_enableAutosleep();
		show_message_page(screen, "Update Error", ex.error);
		return;
	}

	// --- Reboot ---
	PWR_enableSleep();
	PWR_enableAutosleep();
	render_update_page(screen, "Update Complete",
					   "Rebooting...", -1, NULL, NULL, 0);
	sleep(2);
	system("reboot");
}

// ============================================
// Find the updater item in the page
// ============================================

static SettingItem* find_updater_item(SettingsPage* page) {
	for (int i = 0; i < page->item_count; i++) {
		if (page->items[i].on_press == updater_check_for_updates)
			return &page->items[i];
	}
	return NULL;
}

// ============================================
// Public API
// ============================================

void updater_about_on_show(SettingsPage* page) {
	if (page && page->item_count > 1)
		page->selected = 1; // the updater row, not "This version"

	char version[128];
	char sha[64];
	read_current_version(version, sizeof(version),
						 sha, sizeof(sha),
						 current_tag_cache, sizeof(current_tag_cache));

	// Only auto-check once per session (retry allowed on error)
	if (auto_state == UPDATE_CHECKING || auto_state == UPDATE_UP_TO_DATE ||
		auto_state == UPDATE_AVAILABLE)
		return;

	// Check WiFi before attempting update check
	if (!Wifi_isConnected()) {
		auto_state = UPDATE_ERROR;
		snprintf(item_label, sizeof(item_label), "Updater");
		snprintf(item_desc, sizeof(item_desc), "No internet connection");
		SettingItem* item = find_updater_item(page);
		if (item) {
			item->name = item_label;
			item->desc = item_desc;
		}
		return;
	}

	auto_done = 0;
	auto_success = 0;
	auto_tag[0] = '\0';
	auto_error[0] = '\0';
	auto_state = UPDATE_CHECKING;
	snprintf(item_label, sizeof(item_label), "Checking for update..");
	item_desc[0] = '\0';

	SettingItem* item = find_updater_item(page);
	if (item) {
		item->name = item_label;
		item->desc = item_desc;
		item->type = ITEM_STATIC;
	}

	if (pthread_create(&auto_tid, NULL, auto_check_thread, NULL) != 0) {
		auto_state = UPDATE_ERROR;
		snprintf(item_label, sizeof(item_label), "Updater");
		snprintf(item_desc, sizeof(item_desc), "Failed to start update check");
		if (item) {
			item->name = item_label;
			item->desc = item_desc;
			item->type = ITEM_BUTTON;
		}
	}
}

void updater_about_on_tick(SettingsPage* page) {
	if (auto_state != UPDATE_CHECKING || !auto_done)
		return;

	__sync_synchronize();
	process_auto_check_result();

	SettingItem* item = find_updater_item(page);
	if (item) {
		item->name = item_label;
		item->desc = item_desc;
		// Only make clickable if there's an update or error (retry)
		item->type = (auto_state == UPDATE_AVAILABLE || auto_state == UPDATE_ERROR)
						 ? ITEM_BUTTON
						 : ITEM_STATIC;
		item->pill = auto_state == UPDATE_AVAILABLE ? "New" : NULL;
		item->chevron = auto_state == UPDATE_AVAILABLE;
		item->a_label = auto_state == UPDATE_AVAILABLE ? "Update" : auto_state == UPDATE_ERROR ? "Try again" : "";
		page->needs_layout = 1;
	}
}

void updater_check_for_updates(void) {
	// Item is ITEM_STATIC during checking, so this shouldn't be called,
	// but guard anyway
	if (auto_state == UPDATE_CHECKING)
		return;

	// Do nothing if already up to date
	if (auto_state == UPDATE_UP_TO_DATE)
		return;

	// If update available, show info and install
	if (auto_state == UPDATE_AVAILABLE) {
		SettingsPage* page = settings_menu_current();
		SDL_Surface* settings_screen(void);
		SDL_Surface* screen = page && page->screen ? page->screen : settings_screen();
		if (!screen)
			return;

		int dr = fetch_release_details(screen, &cached_release);
		if (dr < 0) {
			show_message_page(screen, "Update Error", "Could not determine download URL");
			return;
		}
		if (dr == 0)
			return;

		if (!show_update_info(screen, &cached_release))
			return;

		do_install(screen, &cached_release);
		return;
	}

	// Error or idle: retry the check (non-blocking, will update via on_tick)
	auto_state = UPDATE_IDLE;
	SettingsPage* page = settings_menu_current();
	if (page)
		updater_about_on_show(page);

	SettingItem* item = find_updater_item(page);
	if (item) {
		item->name = item_label;
		item->desc = item_desc;
	}
}
