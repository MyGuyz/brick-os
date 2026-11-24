// libosdwait.so — LD_PRELOAD shim for the stock TrimUI OSD daemon (trimui_osdd).
//
// The daemon's input thread blocks in SDL_WaitEvent. It never creates an SDL
// window, so SDL 2.30's wait has nothing to block on and falls back to
// "pump, then SDL_Delay(1)": ~920 wakeups/s and ~4.5% of a core, forever,
// while the panel is hidden and nothing is happening (measured 2026-09-19 on
// Brick Pro and Smart Pro S — two thirds of the daemon's idle CPU). The daemon
// imports only SDL_WaitEvent, so a byte patch cannot switch it to a cheaper
// poll; interposing that one symbol can. MinUI.pak/launch.sh starts the daemon
// with LD_PRELOAD pointing here (see .dev/OSD.md, "Idle wait shim").
//
// Semantics preserved: returns 1 with the event filled in, exactly like
// SDL_WaitEvent; pending events come back immediately with no sleep between
// them, so a backlog still drains in one go (the hidden-state discard loop in
// the daemon relies on that — see the input drain patch). Only the *idle*
// cadence changes: 20 ms between polls while the panel is hidden, 1 ms while
// it is shown (SDL's own cadence, so visible input latency is unchanged).
// "Shown" is the daemon's own state marker file, one access() per idle tick.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>

#define STATE_PATH "/tmp/trimui_osd/osdd_show_up"
#define HIDDEN_POLL_US 20000
#define SHOWN_POLL_US 1000

typedef int (*wait_timeout_fn)(void* event, int timeout_ms);
typedef int (*wait_fn)(void* event);

int SDL_WaitEvent(void* event) {
	static wait_timeout_fn poll_fn;
	static wait_fn real_wait;
	static int resolved;
	if (!resolved) {
		poll_fn = (wait_timeout_fn)dlsym(RTLD_NEXT, "SDL_WaitEventTimeout");
		real_wait = (wait_fn)dlsym(RTLD_NEXT, "SDL_WaitEvent");
		resolved = 1;
	}
	if (!poll_fn) // unexpected SDL: behave exactly like the original
		return real_wait ? real_wait(event) : 0;
	for (;;) {
		if (poll_fn(event, 0)) // timeout 0: pump once, return a pending event
			return 1;
		usleep(access(STATE_PATH, F_OK) == 0 ? SHOWN_POLL_US : HIDDEN_POLL_US);
	}
}

// ---- Volume keys while the panel is up (2026-11-24, the user's report) ----
// The Brick's side volume keys come only on the pad device (TRIMUI Player1), and the daemon grabs that device while
// the panel is shown, so keymon, which turns them into volume steps and the HUD, never saw them: the keys did nothing
// in the panel. Every read() the daemon (or its SDL) makes from an input device passes through here; while the panel
// is shown, the volume keys in it are copied to a small uinput device of our own, which keymon reads like any other.
// Hidden, nothing is copied (keymon reads the pad itself then; copying would double each step).
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

static int vol_fd = -1;

__attribute__((constructor)) static void vol_open(void) {
	extern char* program_invocation_short_name;   // the shim is inherited by every widget the daemon starts:
	if (strncmp(program_invocation_short_name, "trimui_osdd", 11) != 0)   // only the daemon reads the pad
		return;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	struct uinput_user_dev dev;
	memset(&dev, 0, sizeof(dev));
	strncpy(dev.name, "BrickOS panel volume", UINPUT_MAX_NAME_SIZE - 1);
	dev.id.bustype = BUS_VIRTUAL;
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEUP) < 0 ||
		ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEDOWN) < 0 || write(fd, &dev, sizeof(dev)) != sizeof(dev) ||
		ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return;
	}
	vol_fd = fd;
}

typedef ssize_t (*read_fn)(int, void*, size_t);

ssize_t read(int fd, void* buf, size_t count) {
	static read_fn real_read;
	if (!real_read)
		real_read = (read_fn)dlsym(RTLD_NEXT, "read");
	ssize_t n = real_read(fd, buf, count);
	if (vol_fd < 0 || n <= 0 || n % sizeof(struct input_event) != 0)
		return n;
	const struct input_event* ev = buf;
	int nev = n / sizeof(struct input_event), any = 0;
	for (int i = 0; i < nev; i++)
		if (ev[i].type == EV_KEY && (ev[i].code == KEY_VOLUMEUP || ev[i].code == KEY_VOLUMEDOWN))
			any = 1;
	struct stat st;
	if (!any || access(STATE_PATH, F_OK) != 0 || fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode) ||
		major(st.st_rdev) != 13) // 13: the input devices
		return n;
	for (int i = 0; i < nev; i++) {
		if (ev[i].type != EV_KEY || (ev[i].code != KEY_VOLUMEUP && ev[i].code != KEY_VOLUMEDOWN))
			continue;
		struct input_event out[2];
		memset(out, 0, sizeof(out));
		out[0].type = EV_KEY;
		out[0].code = ev[i].code;
		out[0].value = ev[i].value;
		out[1].type = EV_SYN;
		out[1].code = SYN_REPORT;
		if (write(vol_fd, out, sizeof(out)) < 0) {
		}
	}
	return n;
}
