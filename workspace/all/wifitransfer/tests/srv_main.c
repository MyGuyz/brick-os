// The server alone on the PC: srv <fake card> <web dir>. PIN 1234.
#include "../wt_server.h"

#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
	if (argc < 3)
		return 2;
	WtConfig c = {argv[1], argv[2], 3, 4};
	wt_server_set_pin("1234");
	if (!wt_server_start(&c)) {
		perror("start");
		return 1;
	}
	printf("ready on %d\n", WT_PORT);
	fflush(stdout);
	for (;;)
		sleep(60);
}
