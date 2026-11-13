// How fast the quick panel (trimui_osdd) answers a key: presses right / left alternately on the virtual pad (the
// /tmp/keys/*.bin events of make_keys.py, written to /dev/input/event3), and times each press to the next g2d
// interrupt (the daemon redraws the panel with one g2d blit). Open the panel first (touch /tmp/show_osdd).
// Usage: panellat [presses] [gap_ms]
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static long g2dCount(void) {
	static char buf[16384];
	int fd = open("/proc/interrupts", O_RDONLY);
	if (fd < 0)
		return -1;
	int n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	char* p = strstr(buf, "g2d");
	if (!p)
		return -1;
	while (p > buf && p[-1] != '\n')
		p--;
	strtol(p, &p, 10); // irq number
	p++;
	long total = 0;
	for (int c = 0; c < 4; c++)
		total += strtol(p, &p, 10);
	return total;
}

static int key(const char* name, const char* edge) {
	char path[96], buf[256];
	snprintf(path, sizeof(path), "/tmp/keys/%s_%s.bin", name, edge);
	int f = open(path, O_RDONLY);
	if (f < 0)
		return -1;
	int n = read(f, buf, sizeof(buf));
	close(f);
	int d = open("/dev/input/event3", O_WRONLY);
	if (d < 0)
		return -1;
	write(d, buf, n);
	close(d);
	return 0;
}

int main(int argc, char** argv) {
	int presses = argc > 1 ? atoi(argv[1]) : 20, gap = argc > 2 ? atoi(argv[2]) : 250;
	if (access("/tmp/trimui_osd/osdd_show_up", F_OK) != 0) {
		fprintf(stderr, "the quick panel is not open\n");
		return 1;
	}
	usleep(500000);
	double sum = 0, worst = 0;
	int n = 0;
	for (int k = 0; k < presses; k++) {
		long c0 = g2dCount();
		double t0 = ms(), t1 = 0;
		if (key(k & 1 ? "left" : "right", "down") != 0) {
			fprintf(stderr, "no /tmp/keys\n");
			return 1;
		}
		while (ms() - t0 < 2000) {
			if (g2dCount() != c0) {
				t1 = ms();
				break;
			}
			usleep(500);
		}
		usleep(60000);
		key(k & 1 ? "left" : "right", "up");
		if (t1) {
			printf("%s%.0f", k ? " " : "", t1 - t0);
			sum += t1 - t0, n++;
			if (t1 - t0 > worst)
				worst = t1 - t0;
		} else
			printf(" >2000");
		fflush(stdout);
		while (ms() - t0 < gap)
			usleep(1000);
	}
	printf("\navg %.1f ms, worst %.1f ms (%d presses, %d ms apart)\n", n ? sum / n : 0, worst, presses, gap);
	return 0;
}
