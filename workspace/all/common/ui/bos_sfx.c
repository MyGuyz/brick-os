// Brick OS menu sounds (DESIGN.md §5.10). Short tones made in code, one set per ชุดเสียง, played on the system's
// mixing device so they sit on top of background music. Nothing here runs on the UI thread except queueing a voice:
// a worker opens the device on the first sound and closes it again when the menus go quiet.
#include "bos_ui.h"
#include "api.h"
#include "config.h"
#include "defines.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SFX_RATE 48000
#define SFX_VOICES 6
#define SFX_IDLE_MS 20000 // close the device after this long without a sound (opening it takes 140-200 ms on the
						  // Brick Pro, so it stays open while the menus are in use and is opened ahead at start)
#define SFX_LATE_MS 250	 // a sound that waited longer than this for the device is dropped, not played late
#define SFX_USER_DIR SHARED_USERDATA_PATH "/sounds"

typedef struct {
	float f, d, g, to, at; // Hz, seconds, gain 0..1, slide to Hz (0 = none), start offset seconds
} Tone;
typedef struct {
	int wave; // 0 sine, 1 triangle, 2 square
	Tone t[BOS_SFX_COUNT][5];
} Pack;

// The tables are the ones approved in design/v2-MotionSound.html.
static const Pack packs[3] = {
	{0, {
			[BOS_SFX_MOVE] = {{880, .035f, .10f}},
			[BOS_SFX_OK] = {{660, .06f, .16f}, {990, .09f, .16f, 0, .055f}},
			[BOS_SFX_BACK] = {{520, .09f, .14f, 390}},
			[BOS_SFX_BUMP] = {{150, .09f, .22f, 110}},
			[BOS_SFX_ON] = {{700, .07f, .14f, 1050}},
			[BOS_SFX_OFF] = {{1050, .07f, .14f, 700}},
			[BOS_SFX_OPEN] = {{440, .10f, .12f, 660}},
			[BOS_SFX_LAUNCH] = {{523, .09f, .16f}, {659, .09f, .16f, 0, .07f}, {784, .09f, .16f, 0, .14f}, {1047, .22f, .18f, 0, .21f}},
		}},
	{1, {
			[BOS_SFX_MOVE] = {{1800, .012f, .20f}},
			[BOS_SFX_OK] = {{1400, .015f, .25f}, {2100, .02f, .25f, 0, .04f}},
			[BOS_SFX_BACK] = {{1000, .02f, .22f, 700}},
			[BOS_SFX_BUMP] = {{220, .05f, .30f, 160}},
			[BOS_SFX_ON] = {{1600, .02f, .22f, 2200}},
			[BOS_SFX_OFF] = {{2200, .02f, .22f, 1600}},
			[BOS_SFX_OPEN] = {{1200, .03f, .20f, 1700}},
			[BOS_SFX_LAUNCH] = {{1400, .02f, .25f}, {2100, .02f, .25f, 0, .05f}, {2800, .12f, .22f, 0, .10f}},
		}},
	{2, {
			[BOS_SFX_MOVE] = {{740, .03f, .05f}},
			[BOS_SFX_OK] = {{659, .05f, .07f}, {988, .08f, .07f, 0, .05f}},
			[BOS_SFX_BACK] = {{494, .08f, .07f, 330}},
			[BOS_SFX_BUMP] = {{110, .08f, .10f}},
			[BOS_SFX_ON] = {{587, .05f, .07f}, {880, .06f, .07f, 0, .05f}},
			[BOS_SFX_OFF] = {{880, .05f, .07f}, {587, .06f, .07f, 0, .05f}},
			[BOS_SFX_OPEN] = {{440, .05f, .07f}, {660, .06f, .07f, 0, .05f}},
			[BOS_SFX_LAUNCH] = {{392, .07f, .08f}, {523, .07f, .08f, 0, .07f}, {659, .07f, .08f, 0, .14f}, {784, .07f, .08f, 0, .21f}, {1047, .2f, .08f, 0, .28f}},
		}},
};
static const char* names[BOS_SFX_COUNT] = {"", "move", "ok", "back", "bump", "on", "off", "open", "launch"};

typedef struct {
	int16_t* pcm; // mono, SFX_RATE
	int len;
} Clip;
typedef struct {
	const Clip* clip;
	int pos, gain; // gain in 1/256
	uint32_t born;
} Voice;

static Clip clips[BOS_SFX_COUNT];
static int clips_pack = -1;
static Voice voices[SFX_VOICES];
static SDL_mutex* lock;
static SDL_sem* wake;
static SDL_Thread* worker;
static SDL_AudioDeviceID dev;
static volatile int running, playing, need_dev;
static volatile uint32_t last_sound;
static BosSfx pending;
static int pending_gain;
static bool hooked;

extern void (*PAD_pollHook)(void); // api.c: runs after every PAD_poll

static void clipFree(Clip* c) {
	free(c->pcm);
	c->pcm = NULL;
	c->len = 0;
}

// A .wav the user dropped in the sounds folder replaces that sound in every pack.
static bool clipFromFile(Clip* c, const char* name) {
	char path[MAX_PATH];
	snprintf(path, sizeof(path), SFX_USER_DIR "/%s.wav", name);
	SDL_AudioSpec spec;
	Uint8* buf;
	Uint32 len;
	if (!SDL_LoadWAV(path, &spec, &buf, &len))
		return false;
	SDL_AudioCVT cvt;
	bool ok = false;
	if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, 1, SFX_RATE) >= 0 && len < 4 * 1024 * 1024) {
		cvt.len = (int)len;
		cvt.buf = malloc((size_t)len * (cvt.len_mult > 0 ? cvt.len_mult : 1));
		if (cvt.buf) {
			memcpy(cvt.buf, buf, len);
			if (!cvt.needed || SDL_ConvertAudio(&cvt) == 0) {
				int out = cvt.needed ? cvt.len_cvt : (int)len;
				if (out > SFX_RATE * 2 * 3)
					out = SFX_RATE * 2 * 3; // three seconds is plenty for a menu sound
				c->pcm = (int16_t*)cvt.buf;
				c->len = out / 2;
				ok = true;
			} else
				free(cvt.buf);
		}
	}
	SDL_FreeWAV(buf);
	return ok;
}

static void clipFromTones(Clip* c, const Pack* p, const Tone* t) {
	float end = 0;
	for (int i = 0; i < 5 && t[i].f > 0; i++)
		if (t[i].at + t[i].d > end)
			end = t[i].at + t[i].d;
	int n = (int)(end * SFX_RATE) + 1;
	float* mix = calloc((size_t)n, sizeof(float));
	c->pcm = malloc((size_t)n * sizeof(int16_t));
	if (!mix || !c->pcm) {
		free(mix);
		clipFree(c);
		return;
	}
	for (int i = 0; i < 5 && t[i].f > 0; i++) {
		int at = (int)(t[i].at * SFX_RATE), len = (int)(t[i].d * SFX_RATE), attack = SFX_RATE * 4 / 1000;
		double phase = 0;
		for (int k = 0; k < len && at + k < n; k++) {
			float u = (float)k / (float)len;
			float f = t[i].to > 0 ? t[i].f * powf(t[i].to / t[i].f, u) : t[i].f; // the pitch slides evenly by ear
			phase += f / SFX_RATE;
			double ph = phase - floor(phase);
			float w = p->wave == 0 ? sinf((float)(ph * 2 * M_PI)) : p->wave == 1 ? (float)(4 * fabs(ph - 0.5) - 1) : ph < 0.5 ? 1.f : -1.f;
			// 4 ms in, then dying away to nothing at the end: no click at either edge
			float env = k < attack ? (float)k / (float)attack : powf(0.0001f, (float)(k - attack) / (float)(len - attack > 0 ? len - attack : 1));
			mix[at + k] += w * env * t[i].g;
		}
	}
	for (int k = 0; k < n; k++) {
		float v = mix[k] * 2.0f * 32767.f; // the tables are web-audio gains; the Brick's speaker needs twice that
		c->pcm[k] = (int16_t)(v > 32767.f ? 32767 : v < -32767.f ? -32767 : v);
	}
	c->len = n;
	free(mix);
}

static void clipsBuild(int pack) {
	for (int i = 1; i < BOS_SFX_COUNT; i++) {
		clipFree(&clips[i]);
		if (!clipFromFile(&clips[i], names[i]))
			clipFromTones(&clips[i], &packs[pack], packs[pack].t[i]);
	}
	clips_pack = pack;
}

static void SDLCALL sfxCallback(void* unused, Uint8* stream, int len) {
	(void)unused;
	int16_t* out = (int16_t*)stream;
	int frames = len / 4, live = 0;
	memset(stream, 0, (size_t)len);
	SDL_LockMutex(lock);
	uint32_t now = SDL_GetTicks();
	for (int v = 0; v < SFX_VOICES; v++) {
		Voice* vo = &voices[v];
		if (!vo->clip)
			continue;
		if (vo->pos == 0 && now - vo->born > SFX_LATE_MS) { // waited too long for the device
			vo->clip = NULL;
			continue;
		}
		int n = vo->clip->len - vo->pos;
		if (n > frames)
			n = frames;
		const int16_t* src = vo->clip->pcm + vo->pos;
		for (int k = 0; k < n; k++) {
			int s = out[k * 2] + ((src[k] * vo->gain) >> 8);
			s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
			out[k * 2] = out[k * 2 + 1] = (int16_t)s;
		}
		vo->pos += n;
		if (vo->pos >= vo->clip->len)
			vo->clip = NULL;
		else
			live++;
	}
	playing = live;
	SDL_UnlockMutex(lock);
	if (live)
		last_sound = now;
}

static int sfxWorker(void* unused) {
	(void)unused;
	while (running) {
		SDL_SemWaitTimeout(wake, 500);
		if (!running)
			break;
		uint32_t now = SDL_GetTicks();
		if (!dev && need_dev) {
			need_dev = 0;
			if (!SDL_WasInit(SDL_INIT_AUDIO))
				SDL_InitSubSystem(SDL_INIT_AUDIO);
			SDL_AudioSpec want = {0}, got;
			want.freq = SFX_RATE;
			want.format = AUDIO_S16SYS;
			want.channels = 2;
			want.samples = 512;
			want.callback = sfxCallback;
			dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
			if (dev) {
				SDL_PauseAudioDevice(dev, 0);
				if (getenv("BOS_BENCH"))
					LOG_info("BOS_sfx: device open took %u ms, %d Hz, %d frames\n", SDL_GetTicks() - now, got.freq, got.samples);
			} else
				LOG_info("BOS_sfx: no audio device: %s\n", SDL_GetError());
		} else if (dev && !playing && now - last_sound > SFX_IDLE_MS) {
			SDL_CloseAudioDevice(dev);
			dev = 0;
		}
	}
	if (dev)
		SDL_CloseAudioDevice(dev);
	dev = 0;
	return 0;
}

static void sfxStart(BosSfx id, int gain256) {
	int vol = CFG_getMenuSounds(); // 0 ปิด, 1 เบา, 2 ปกติ
	if (!running || id <= BOS_SFX_NONE || id >= BOS_SFX_COUNT || vol <= 0)
		return;
	int pack = CFG_getMenuSoundPack();
	if (pack < 0 || pack > 2)
		pack = 0;
	SDL_LockMutex(lock);
	if (pack != clips_pack) {
		memset(voices, 0, sizeof(voices));
		clipsBuild(pack);
	}
	if (clips[id].pcm) {
		int slot = 0;
		for (int v = 0; v < SFX_VOICES; v++) {
			if (!voices[v].clip) {
				slot = v;
				break;
			}
			if (voices[v].pos > voices[slot].pos)
				slot = v; // all busy: the one furthest along gives way
		}
		voices[slot] = (Voice){&clips[id], 0, gain256 * (vol == 1 ? 90 : 256) / 256, SDL_GetTicks()};
		playing = 1;
	}
	SDL_UnlockMutex(lock);
	last_sound = SDL_GetTicks();
	if (!dev) {
		need_dev = 1;
		SDL_SemPost(wake);
	}
}

void BOS_sfx(BosSfx id) {
	pending = BOS_SFX_NONE; // this replaces the automatic sound for the press being handled
	sfxStart(id, 256);
}

void BOS_sfxFlush(void) {
	if (pending != BOS_SFX_NONE)
		sfxStart(pending, pending_gain);
	pending = BOS_SFX_NONE;
}

// After every input poll: the sound a press makes when the screen handling it says nothing else. A held direction
// repeats more quietly so a long scroll is a soft purr, not a rattle.
static void sfxPollHook(void) {
	BOS_sfxFlush();
	if (PAD_justPressed(BTN_A)) {
		pending = BOS_SFX_OK;
		pending_gain = 256;
	} else if (PAD_justPressed(BTN_B)) {
		pending = BOS_SFX_BACK;
		pending_gain = 256;
	} else {
		static const int dirs[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_L1, BTN_R1, BTN_L2, BTN_R2};
		for (int i = 0; i < 8; i++)
			if (PAD_justRepeated(dirs[i])) {
				pending = BOS_SFX_MOVE;
				pending_gain = PAD_justPressed(dirs[i]) ? 256 : 140;
				break;
			}
		if (pending == BOS_SFX_NONE && (PAD_justPressed(BTN_X) || PAD_justPressed(BTN_Y) || PAD_justPressed(BTN_SELECT))) {
			pending = BOS_SFX_MOVE;
			pending_gain = 256;
		}
	}
}

void BOS_sfxInit(void) {
	if (running)
		return;
	lock = SDL_CreateMutex();
	wake = SDL_CreateSemaphore(0);
	if (!lock || !wake)
		return;
	running = 1;
	need_dev = CFG_getMenuSounds() > 0; // open ahead, so the first press already sounds
	last_sound = SDL_GetTicks();
	SDL_SemPost(wake);
	worker = SDL_CreateThread(sfxWorker, "bos_sfx", NULL);
	if (!worker) {
		running = 0;
		return;
	}
	PAD_pollHook = sfxPollHook;
	hooked = true;
}

// Let the sound being played finish (a launch chime outlives the menu by a moment), then let go of the device.
void BOS_sfxQuit(int wait_ms) {
	if (!running)
		return;
	if (hooked)
		PAD_pollHook = NULL;
	hooked = false;
	BOS_sfxFlush();
	uint32_t until = SDL_GetTicks() + (uint32_t)(wait_ms > 0 ? wait_ms : 0);
	while (playing && SDL_GetTicks() < until)
		SDL_Delay(5);
	running = 0;
	SDL_SemPost(wake);
	SDL_WaitThread(worker, NULL);
	worker = NULL;
	for (int i = 1; i < BOS_SFX_COUNT; i++)
		clipFree(&clips[i]);
	clips_pack = -1;
	memset(voices, 0, sizeof(voices));
}
