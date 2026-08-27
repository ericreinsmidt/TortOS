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

/* Start a child and forget it: it outlives this process and leaves no zombie
 * behind. This is how a resident emulator that has died gets started again. */
bool plat_spawn_detached(char *const argv[], const char *const envkv[],
                         const char *workdir);

/* The resident emulator. plat_resident_send() hands over a game and returns
 * at once, so the launcher can animate while it loads; plat_resident_wait()
 * blocks until the game is over - or, on the Diatom transport, until the
 * player opens the in-game menu, which the launcher draws (the emulator hands
 * the display over rather than drawing its own).
 *
 * One transport: Diatom's socket protocol, where peer death is EOF, the stop
 * signal is a message, and the reply is a line saying what actually happened.
 * PLAYOS_DIATOM_SOCKET overrides the path for tests. The fifo transport that
 * drove minarch left with minarch. */
#define RES_DEAD   0   /* emulator missing, dead, or the game never started */
#define RES_EXIT   1   /* the game ran and is over */
#define RES_PAUSED 2   /* Diatom only: menu open, the launcher owns the display */
const char *plat_resident_socket(void);
bool plat_resident_ready(void);
bool plat_resident_send(const char *tag, const char *core, const char *rom,
                        const char *resume, const char *exit_state,
                        const char *preview);
int  plat_resident_wait(void);
/* Diatom only: one protocol line (RESUME, STOP, SAVE\tpath=...), newline added. */
bool plat_resident_line(const char *fmt, ...);
/* Path from the most recent PREVIEW message, or "" - the menu's backdrop. */
const char *plat_resident_last_preview(void);
/* Where Diatom is actually drawing the game, from its DISPLAY message. False
 * until it has said, which is the standalone path and the first moments of a
 * launch. Cached rather than asked for: Diatom reports it from the one place
 * its mode or rect can change, settling included (its ADR-0022), so the last
 * one heard is current. */
bool plat_resident_rect(SDL_Rect *out);

void plat_request_poweroff(void);
void plat_leds_off(void);

/* The two level scales, stated once. PlayOS shares them verbatim with
 * launch.sh and with Diatom, so a level crossing the socket needs no
 * conversion - which only holds while every place that rescales a level agrees
 * on the top of the range, so they live here rather than beside the ladder
 * table that only the settings code can see. */
#define PLAT_VOL_MAX     20   /* 21 positions, 0..20 */
#define PLAT_BRIGHT_MAX  11   /* a 12-rung geometric ladder, 0..11 */

/* Levels come from .userdata/levels.cfg when the player has ever set one, and
 * from playos.cfg otherwise: the saved level is a choice, the config value is
 * only a default. launch.sh applies the same precedence to the boot animation
 * before this process exists, so passing the config defaults in here keeps the
 * rule in one place instead of two that can disagree.
 * cfg_volume_pct is 0..100; cfg_brightness is a rung, 0..PLAT_BRIGHT_MAX.
 * Either may be negative for "not configured". */
void plat_settings_init(int cfg_volume_pct, int cfg_brightness);
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
