/* SPDX-License-Identifier: 0BSD */
#ifndef PLAYOS_PLATFORM_H
#define PLAYOS_PLATFORM_H

#include <SDL.h>
#include <stdbool.h>

/* Everything that knows it is running on a TrimUI Brick lives here: the
 * display, the buttons that arrive on three different devices, the panel
 * backlight, the codec, the battery, and the pipe to the resident emulator.
 * The rest of PlayOS talks to this file and to SDL, and to nothing else. */

#define PLAYOS_SCREEN_W 1024
#define PLAYOS_SCREEN_H 768

/* launch.sh polls for this and powers the device down when it appears. */
#define PLAYOS_POWEROFF_FLAG "/tmp/playos_poweroff"

/* Overridable at runtime so the launcher can be pointed at a test tree. */
extern const char *P_ROOT;     /* /mnt/SDCARD/PlayOS      */
extern const char *P_CARD;     /* /mnt/SDCARD             */
extern const char *P_ROMS;     /* /mnt/SDCARD/Roms        */
extern const char *P_USERDATA; /* /mnt/SDCARD/.userdata/tg5040 */
extern const char *P_SHARED;   /* /mnt/SDCARD/.userdata/shared */
extern const char *P_FONT;     /* the UI typeface          */
void paths_init(void);

typedef enum {
	IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN,
	IN_ACCEPT, IN_BACK, IN_X, IN_Y,
	IN_L1, IN_R1, IN_START, IN_SELECT, IN_MENU,
	IN_VOLUP, IN_VOLDN, IN_BRIGHTUP, IN_BRIGHTDN,
	IN_POWER,
	IN_COUNT,
	IN_NONE = -1,
} in_button;

typedef struct {
	bool   down[IN_COUNT];
	bool   pressed[IN_COUNT];   /* this frame only */
	Uint32 down_since[IN_COUNT];
	Uint32 last_repeat[IN_COUNT];
	bool   quit_requested;
} in_state;

bool plat_video_init(void);
void plat_video_quit(void);
SDL_Renderer *plat_renderer(void);
unsigned plat_now_ms(void);

bool plat_input_init(void);
void plat_input_quit(void);
void plat_input_poll(in_state *st);
void plat_input_flush(void);
bool in_repeat(in_state *st, in_button b);

/* Run a child to completion, watching the power button while it runs.
 * envkv is a NULL-terminated array of "KEY=value" strings. */
int  plat_run(char *const argv[], const char *const envkv[], const char *workdir);
bool plat_run_power_pressed(void);

/* The resident emulator. plat_resident_send() hands over a game and returns
 * at once, so the launcher can animate while it loads; plat_resident_wait()
 * blocks until the game is over. */
bool plat_resident_ready(void);
bool plat_resident_send(const char *req_line);
bool plat_resident_wait(void);

void plat_request_poweroff(void);
void plat_leds_off(void);

void plat_settings_init(void);
int  plat_volume_get(void);
int  plat_brightness_get(void);
void plat_volume_nudge(int delta);
void plat_brightness_nudge(int delta);
void plat_volume_set_pct(int pct);
void plat_brightness_set(int level);
/* kind: 1 = brightness, 2 = volume */
void plat_osd_show(int kind, int val, int max);
void plat_draw_osd(SDL_Renderer *r);

bool plat_battery(int *pct, bool *charging);

#endif
