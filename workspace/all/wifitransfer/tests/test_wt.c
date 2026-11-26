// PC tests for WiFi Transfer's rules, QR and zip. Build: see run.sh. Writes qr.txt and test.zip for the
// outside checks (a QR reader, python's zipfile).
#include "../wt_qr.h"
#include "../wt_rules.h"
#include "../wt_zip.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c)                                                    \
	do {                                                            \
		if (!(c)) {                                                 \
			printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);       \
			fails++;                                                \
		}                                                           \
	} while (0)
#define SEQ(a, b) CHECK(!strcmp((a), (b)))

static bool file_out(void* ctx, const void* b, size_t n) { return fwrite(b, 1, n, (FILE*)ctx) == n; }

int main(void) {
	// file types
	CHECK(wt_game_ok("PS", "Crash.chd"));
	CHECK(wt_game_ok("PS", "Crash.CUE"));
	CHECK(wt_game_ok("PS", "Crash.zip"));
	CHECK(!wt_game_ok("PS", "Pokemon.gba"));
	CHECK(!wt_game_ok("PS", "noext"));
	CHECK(wt_game_ok("GBA", "Pokemon.gba"));
	CHECK(!wt_game_ok("PSP", "God.zip"));
	CHECK(wt_game_ok("PSP", "God.cso"));
	CHECK(!wt_game_ok("PORTS", "x.sh"));
	CHECK(!wt_game_ok("NOPE", "x.bin"));
	CHECK(wt_music_ok("a.FLAC") && !wt_music_ok("a.mp4"));
	CHECK(wt_video_ok("a.mkv") && !wt_video_ok("a.mp3"));
	CHECK(wt_cover_ok("a.webp") && !wt_cover_ok("a.gif"));
	// names
	CHECK(wt_name_safe("Crash (USA).chd"));
	CHECK(wt_name_safe("เกมไทย.gba"));
	CHECK(!wt_name_safe("../x"));
	CHECK(!wt_name_safe("a/b"));
	CHECK(!wt_name_safe(".hidden"));
	CHECK(!wt_name_safe(""));
	CHECK(!wt_name_safe("a\nb"));
	char t[16], b[256];
	CHECK(wt_folder_tag("Sony PlayStation (PS)", t, sizeof(t)));
	SEQ(t, "PS");
	CHECK(wt_folder_tag("Sega CD (GPGX)", t, sizeof(t)));
	SEQ(t, "GPGX");
	CHECK(!wt_folder_tag("Music", t, sizeof(t)));
	CHECK(!wt_folder_tag("x ()", t, sizeof(t)));
	wt_base("Crash (USA).cue", b, sizeof(b));
	SEQ(b, "Crash (USA)");
	// saves: the default formats (SRM uncompressed 3 / 4)
	CHECK(wt_save_target("PS", "anything.srm", "Crash.chd", 3, 4, b, sizeof(b)) == WT_SAVE_BATTERY);
	SEQ(b, "Crash.srm");
	CHECK(wt_save_target("PS", "x.sav", "Crash.chd", 3, 4, b, sizeof(b)) == WT_SAVE_BATTERY);
	SEQ(b, "Crash.srm");
	CHECK(wt_save_target("PS", "x.sav", "Crash.chd", 0, 0, b, sizeof(b)) == WT_SAVE_BATTERY);
	SEQ(b, "Crash.chd.sav");
	CHECK(wt_save_target("PS", "x.srm", "Crash.chd", 2, 4, b, sizeof(b)) == WT_SAVE_BATTERY);
	SEQ(b, "Crash.sav");
	CHECK(wt_save_target("PS", "x.state", "Crash.chd", 3, 4, b, sizeof(b)) == WT_SAVE_STATE);
	SEQ(b, "Crash.state");
	CHECK(wt_save_target("PS", "x.state3", "Crash.chd", 3, 4, b, sizeof(b)) == WT_SAVE_STATE);
	SEQ(b, "Crash.state3");
	CHECK(wt_save_target("PS", "x.state.auto", "Crash.chd", 3, 4, b, sizeof(b)) == WT_SAVE_STATE);
	SEQ(b, "Crash.state.auto");
	CHECK(wt_save_target("PS", "x.st2", "Crash.chd", 3, 1, b, sizeof(b)) == WT_SAVE_STATE);
	SEQ(b, "Crash.state.2");
	CHECK(wt_save_target("PS", "x.state3", "Crash.chd", 3, 0, b, sizeof(b)) == WT_SAVE_STATE);
	SEQ(b, "Crash.chd.st3");
	CHECK(wt_save_target("NDS", "Mario.sav", "Mario Kart DS.nds", 3, 4, b, sizeof(b)) == WT_SAVE_BATTERY);
	SEQ(b, "Mario Kart DS.dsv");
	CHECK(wt_save_target("PS", "x.txt", "Crash.chd", 3, 4, b, sizeof(b)) == WT_SAVE_NONE);
	CHECK(wt_save_target("PSP", "x.srm", "God.iso", 3, 4, b, sizeof(b)) == WT_SAVE_NONE);
	CHECK(wt_save_split("Tekken 3.state3", b, sizeof(b)) == WT_SAVE_STATE);
	SEQ(b, "Tekken 3");
	CHECK(wt_save_split("Tekken 3.state.auto", b, sizeof(b)) == WT_SAVE_STATE);
	SEQ(b, "Tekken 3");
	CHECK(wt_save_split("Tekken 3.state.2", b, sizeof(b)) == WT_SAVE_STATE);
	SEQ(b, "Tekken 3");
	CHECK(wt_save_split("Tekken 3.srm", b, sizeof(b)) == WT_SAVE_BATTERY);
	SEQ(b, "Tekken 3");
	CHECK(wt_save_split("Tekken 3.chd.st1", b, sizeof(b)) == WT_SAVE_STATE);
	SEQ(b, "Tekken 3.chd");
	CHECK(wt_save_split("notes.txt", b, sizeof(b)) == WT_SAVE_NONE);

	// QR: three lengths -> versions 1, 2, 3
	WtQr q;
	CHECK(wt_qr_make("http://1.2.3.4:8080", &q) && q.size == 25);
	CHECK(wt_qr_make("http://192.168.100.200:8080", &q) && q.size == 29);
	CHECK(!wt_qr_make("http://this-is-far-too-long-for-version-three.example/", &q));
	FILE* f = fopen("qr.txt", "w");
	const char* tests[] = {"HELLO", "http://1.2.3.4:8080", "http://192.168.100.200:8080"};
	for (int k = 0; k < 3; k++) {
		wt_qr_make(tests[k], &q);
		fprintf(f, "%s\n%d\n", tests[k], q.size);
		for (int y = 0; y < q.size; y++) {
			for (int x = 0; x < q.size; x++)
				fputc(q.m[y][x] ? '1' : '0', f);
			fputc('\n', f);
		}
	}
	fclose(f);

	// zip: two files from this folder
	WtZipItem it[2] = {{"../wt_rules.h", "a/wt_rules.h", 0, 0}, {"../wt_qr.h", "เกม.h", 0, 1700000000}};
	for (int k = 0; k < 2; k++) {
		FILE* s = fopen(it[k].path, "rb");
		fseek(s, 0, SEEK_END);
		it[k].size = (uint64_t)ftell(s);
		fclose(s);
	}
	uint64_t want = wt_zip_length(it, 2);
	f = fopen("test.zip", "wb");
	CHECK(wt_zip_write(it, 2, file_out, f));
	long got = ftell(f);
	fclose(f);
	CHECK((uint64_t)got == want);
	it[0].size += 5; // promised more than the file has
	f = fopen("bad.zip", "wb");
	CHECK(!wt_zip_write(it, 2, file_out, f));
	fclose(f);

	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails != 0;
}
