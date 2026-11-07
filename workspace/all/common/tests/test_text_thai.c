#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../text_thai.h"

static void expect(const char* in, const char* want) {
	char out[256];
	int n = TextThai_toGlyphs(in, out, sizeof(out));
	if (strcmp(out, want) != 0 || n != (int)strlen(want)) {
		fprintf(stderr, "FAIL %s\n  got:", in);
		for (const unsigned char* s = (const unsigned char*)out; *s; s++)
			fprintf(stderr, " %02X", *s);
		fprintf(stderr, "\n");
		assert(0);
	}
}

int main(void) {
	assert(TextThai_has("Hello ไทย"));
	assert(!TextThai_has("Hello"));
	assert(!TextThai_has(""));

	expect("Hello", "Hello");
	expect("ก่", "ก่");                                // low tone, nothing to move
	expect("ที่", "ที\xEF\x9C\x8A");                      // tone above ี -> F70A
	expect("น้ำ", "น\xEF\x9C\xA1ำ");                     // tone above ำ's circle -> F721
	expect("ป่า", "ป\xEF\x9C\x85า");                     // tone off ป's stem -> F705
	expect("ปิ่น", "ป\xEF\x9C\x81\xEF\x9C\x93น");          // ิ -> F701, tone -> F713
	expect("ป้ำ", "ป\xEF\x9C\xA6\xEF\x9C\x91า");          // tone F726, ำ -> F711 + า
	expect("ญุ", "\xEF\x9C\x8Fุ");                       // ญ without descender
	expect("ฐู", "\xEF\x9C\x80ู");                       // ฐ without descender
	expect("ฎุ", "\xEF\x9C\x9B\xEF\x9C\x98");             // short ฎ F71B, lowered ุ F718
	expect("ทั้ง", "ทั\xEF\x9C\x8Bง");                    // tone above ั
	expect("A่", "A่");                                // stray mark stays

	char tiny[4];
	assert(TextThai_toGlyphs("ทดสอบ", tiny, sizeof(tiny)) == 0);
	printf("test_text_thai: all tests passed\n");
	return 0;
}
