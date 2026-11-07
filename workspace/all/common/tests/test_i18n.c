#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../i18n.h"

int main(void) {
	assert(!strcmp(I18N_text("Settings"), "Settings")); // nothing loaded

	const char* path = "/tmp/nx_test_i18n.txt";
	FILE* f = fopen(path, "w");
	fputs("# comment\tignored\n", f);
	fputs("Settings\tตั้งค่า\n", f);
	fputs("Two\\nlines\tสอง\\nบรรทัด\n", f); // escaped newlines in the file
	fputs("no tab here\n", f);
	fputs("Empty\t\n", f);
	fputs("Settings\tการตั้งค่า\r\n", f); // later line wins, CRLF ok
	fclose(f);

	assert(I18N_loadFile(path) == 3);
	assert(!strcmp(I18N_text("Settings"), "การตั้งค่า"));
	assert(!strcmp(I18N_text("Two\nlines"), "สอง\nบรรทัด"));
	assert(!strcmp(I18N_text("Empty"), "Empty"));
	assert(!strcmp(I18N_text("Unknown"), "Unknown"));
	assert(I18N_text(NULL) == NULL);
	assert(!strcmp(I18N_text("# comment"), "# comment"));

	assert(I18N_loadFile("/nonexistent/x.txt") == 0);
	assert(!strcmp(I18N_text("Settings"), "Settings"));
	printf("test_i18n: all tests passed\n");
	return 0;
}
