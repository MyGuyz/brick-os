// How long the overlay daemon (trimui_osdd) takes to take in and draw a HUD toast: it deletes the message file once
// the toast is added and the layer redrawn, so the time from writing the file to its going is that delay.
// Usage: hudlat [steps] [gap_ms] [first_wait_ms]
//   first_wait_ms: pause before the first step (over 1.3 s: the pill is down, so it measures its appearing)
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define MSG "/tmp/trimui_osd/osd_toast_msg"

static double ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void toast(int v) {
	FILE* f = fopen(MSG ".tmp", "w");
	if (!f)
		return;
	fprintf(f,
			"{\"type\":\"default\",\"id\":\"com.trimui.osd.msg.volumeglobal\",\"duration\":1200,\"size\":0,"
			"\"x\":292,\"y\":76,\"w\":440,\"h\":76,\"message\":\" \",\"font\":\"\","
			"\"bg\":\"/usr/trimui/osd/hud/vol_%02d.png\",\"icon\":\"\",\"fontsize\":24,\"fontcolor\":\"FFFFFFFF\"}\n",
			v);
	fclose(f);
	rename(MSG ".tmp", MSG);
}

int main(int argc, char** argv) {
	int steps = argc > 1 ? atoi(argv[1]) : 10, gap = argc > 2 ? atoi(argv[2]) : 100;
	int first = argc > 3 ? atoi(argv[3]) : 2000;
	usleep(first * 1000);
	double sum = 0, worst = 0;
	int n = 0;
	for (int k = 0; k < steps; k++) {
		double t0 = ms(), t1 = 0;
		toast(4 + k % 16);
		while (ms() - t0 < 1000) {
			if (access(MSG, F_OK) != 0) {
				t1 = ms();
				break;
			}
			usleep(500);
		}
		if (t1) {
			printf("%s%.0f", k ? " " : "", t1 - t0);
			sum += t1 - t0, n++;
			if (t1 - t0 > worst)
				worst = t1 - t0;
		} else
			printf(" >1000");
		while (ms() - t0 < gap)
			usleep(500);
	}
	printf("\navg %.1f ms, worst %.1f ms (%d steps, %d ms apart)\n", n ? sum / n : 0, worst, steps, gap);
	return 0;
}
