// WiFi Transfer's web server (DESIGN §6.13): the phone page and its small API. It runs only while the app's
// screen is open.
#ifndef WT_SERVER_H
#define WT_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#define WT_PORT 8080

typedef struct {
	char name[160];
	char folder[96];
	bool up; // into the device
	uint64_t done, total;
	int state; // 0 moving, 1 done, 2 failed
} WtRecent;

typedef struct {
	const char* sd;		 // the card, "/mnt/SDCARD" (a test folder on the PC)
	const char* web_dir; // where index.html is
	int save_format, state_format;
} WtConfig;

bool wt_server_start(const WtConfig* cfg);
void wt_server_stop(void);
void wt_server_set_pin(const char* pin); // a new PIN signs every browser out
int wt_server_recent(WtRecent* out, int max); // newest first
int wt_server_moving(void);					  // files moving now
bool wt_server_changed(void);				  // something new for the screen since the last call

#endif
