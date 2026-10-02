/* SPDX-License-Identifier: MIT */
/* The GKD Pixel 2: a 640x480 panel that is physically 480x640 portrait and
 * mounted turned, buttons read straight from the kernel, the RK817 codec, a
 * sysfs backlight and battery, and no radio. Its system is TortOS-px2, our own
 * Buildroot image: mainline Linux, Panfrost, SDL2 drawing through KMS.
 *
 * TortOS draws exactly as it does on the Brick, at 1024x768 landscape, into an
 * offscreen texture; plat_present scales that to 640x480 and turns it onto the
 * panel. Measured on this GPU 2026-10-01: a turned present holds the panel's
 * full 60 fps.
 *
 * The display is shared with Diatom by passing DRM master (Diatom's ADR-0036):
 * device_display_release before a game presents, device_display_take when the
 * launcher has the screen back. */
#include "audioout.h"
#include "db.h"
#include "device.h"
#include "platform.h"

#include <SDL_syswm.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

const char *P_ROOT = "/mnt/SDCARD/TortOS";
const char *P_CARD = "/mnt/SDCARD";
const char *P_ROMS = "/mnt/SDCARD/Roms";
const char *P_USERDATA = "/mnt/SDCARD/.userdata/pixel2";
const char *P_SHARED = "/mnt/SDCARD/.userdata/shared";
const char *P_WEB = "/mnt/SDCARD/TortOS/res/web";
const char *P_FONT = "/mnt/SDCARD/TortOS/menu.ttf";

/* ---- the screen ------------------------------------------------------------ */

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *screen_tex;       /* TORTOS_SCREEN_W x H, what TortOS draws on */
static int           panel_w, panel_h; /* the panel as it is, portrait */
static int           drm_fd = -1;      /* SDL's, for passing the display to Diatom */

SDL_Renderer *plat_renderer(void) { return ren; }

/* DRM master, by its two ioctls from the kernel's drm.h rather than libdrm,
 * the same way the codec's controls are reached below: two numbers do not
 * need a library. */
#define PX_DRM_IOCTL_SET_MASTER  _IO('d', 0x1e)
#define PX_DRM_IOCTL_DROP_MASTER _IO('d', 0x1f)

/* The boot animation is TortOS-px2's splash, which holds the display while it
 * plays. TortOS starts alongside it and never waits for it: SDL builds
 * everything without the display (SDL_KMSDRM_REQUIRE_DRM_MASTER off), and the
 * first present tells the splash to stop and takes over from it. */
static void signal_splash(int sig)
{
	DIR *proc = opendir("/proc");
	struct dirent *e;

	while (proc && (e = readdir(proc))) {
		char path[64], comm[32] = { 0 };
		int fd;

		if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
		snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
		if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) continue;
		if (read(fd, comm, sizeof comm - 1) > 0 && !strcmp(comm, "splash\n"))
			kill(atoi(e->d_name), sig);
		close(fd);
	}
	if (proc) closedir(proc);
}

/* The splash stops on the frame it is showing and lets go of the display;
 * master being free again is the answer, so this tries until it is. A frame
 * and a flip at most, and bounded, so a stuck splash costs a log line and not
 * the launcher. */
static void take_from_splash(void)
{
	Uint32 t0 = SDL_GetTicks();

	signal_splash(SIGUSR1);
	while (drm_fd >= 0 && ioctl(drm_fd, PX_DRM_IOCTL_SET_MASTER, 0) != 0 &&
	       SDL_GetTicks() - t0 < 300)
		SDL_Delay(1);
	fprintf(stderr, "video: took the display from the splash in %u ms\n",
	        (unsigned)(SDL_GetTicks() - t0));
	/* On the kernel's clock, beside the splash's "panel lit", so the boot
	 * from power to shelf reads off one log (docs/boot-time.md in
	 * TortOS-px2). */
	{
		struct timespec now;
		char line[64];
		int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

		clock_gettime(CLOCK_BOOTTIME, &now);
		snprintf(line, sizeof line, "tortos: first frame at %.3f s\n",
		         now.tv_sec + now.tv_nsec / 1e9);
		if (fd >= 0) {
			if (write(fd, line, strlen(line)) < 0) { /* not fatal */ }
			close(fd);
		}
	}
}

/* Black around the picture, then the picture: 1024x768 down to 640x480 and a
 * quarter turn counter-clockwise, which on this panel ("Left Side Up") is the
 * right way up - checked by eye 2026-10-01 with a marker drawn top-left. */
static void show_screen(void)
{
	SDL_Rect dest = { (panel_w - 640) / 2, (panel_h - 480) / 2, 640, 480 };

	SDL_SetRenderTarget(ren, NULL);
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderClear(ren);
	SDL_RenderCopyEx(ren, screen_tex, NULL, &dest, -90, NULL, SDL_FLIP_NONE);
	SDL_RenderPresent(ren);
	SDL_SetRenderTarget(ren, screen_tex);
}

/* The draw color is put back, because TortOS sets it and expects it to stay.
 *
 * The first present after taking the display from the splash shows TWICE.
 * SDL's first frame on a display it did not own when it started never reaches
 * the panel: filmed on 2026-10-01, the logo went to black for exactly one
 * second, which is the shelf loop's heartbeat redraw - TortOS draws only when
 * something changes, so a lost first frame stayed lost until then. The frame
 * is still in screen_tex, so the second showing costs one copy, and when it
 * returns TortOS's picture is on glass.
 *
 * Then the splash is ended. Its logo is off the screen by then, so nothing
 * shows; and gone, it leaves SDL nothing to restore when TortOS exits, which
 * had put the logo back up at power-off. */
void plat_present(void)
{
	static bool shown;
	Uint8 r, g, b, a;

	SDL_GetRenderDrawColor(ren, &r, &g, &b, &a);
	if (!shown) {
		take_from_splash();
		show_screen();
		show_screen();
		signal_splash(SIGTERM);
		shown = true;
	} else {
		show_screen();
	}
	SDL_SetRenderDrawColor(ren, r, g, b, a);
}

void plat_draw_to_screen(void) { SDL_SetRenderTarget(ren, screen_tex); }

bool plat_video_init(void)
{
	SDL_DisplayMode mode;
	SDL_SysWMinfo info;
	SDL_RendererInfo ri;

	/* Linear, so the scale from 1024x768 to 640x480 is smooth rather than
	 * dropping every third line. */
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
	/* The boot animation holds the display until the first present; see
	 * take_from_splash. */
	SDL_SetHint(SDL_HINT_KMSDRM_REQUIRE_DRM_MASTER, "0");
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "video init: %s\n", SDL_GetError());
		return false;
	}
	if (SDL_GetDesktopDisplayMode(0, &mode) != 0) {
		fprintf(stderr, "display mode: %s\n", SDL_GetError());
		return false;
	}
	panel_w = mode.w;
	panel_h = mode.h;
	win = SDL_CreateWindow("TortOS", 0, 0, panel_w, panel_h,
	                       SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
	if (!win) {
		fprintf(stderr, "window: %s\n", SDL_GetError());
		return false;
	}
	/* KMS draws a pointer unless told not to. */
	SDL_ShowCursor(SDL_DISABLE);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) {
		fprintf(stderr, "renderer: %s\n", SDL_GetError());
		return false;
	}
	screen_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
	                               TORTOS_SCREEN_W, TORTOS_SCREEN_H);
	if (!screen_tex) {
		fprintf(stderr, "screen texture: %s\n", SDL_GetError());
		return false;
	}
	SDL_SetRenderTarget(ren, screen_tex);
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

	SDL_VERSION(&info.version);
	if (SDL_GetWindowWMInfo(win, &info) && info.subsystem == SDL_SYSWM_KMSDRM)
		drm_fd = info.info.kmsdrm.drm_fd;
	else
		fprintf(stderr, "video: no DRM handle; games cannot take the display\n");

	if (SDL_GetRendererInfo(ren, &ri) == 0)
		fprintf(stderr, "renderer: %s, driver: %s, vsync %s, panel %dx%d\n",
		        ri.name, SDL_GetCurrentVideoDriver(),
		        (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? "granted" : "DECLINED (requested)",
		        panel_w, panel_h);
	return true;
}

void plat_video_quit(void)
{
	if (screen_tex) { SDL_DestroyTexture(screen_tex); screen_tex = NULL; }
	if (ren) { SDL_DestroyRenderer(ren); ren = NULL; }
	if (win) { SDL_DestroyWindow(win); win = NULL; }
	drm_fd = -1;
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/* Diatom is about to present: it cannot while this process holds master. */
void device_display_release(void)
{
	if (drm_fd >= 0) ioctl(drm_fd, PX_DRM_IOCTL_DROP_MASTER, 0);
}

/* Diatom has given the display back (it drops master before PAUSED and EXIT).
 * The next plat_present flips onto the CRTC Diatom left lit. */
void device_display_take(void)
{
	if (drm_fd >= 0 && ioctl(drm_fd, PX_DRM_IOCTL_SET_MASTER, 0) != 0)
		fprintf(stderr, "video: could not take the display back\n");
}

/* ---- input ----------------------------------------------------------------- */

#define BITS_PER_LONG   (8 * (int)sizeof(long))
#define NLONGS(max)     (((max) + BITS_PER_LONG) / BITS_PER_LONG)
#define BIT_IS_SET(a,b) (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1UL)

static int fd_pad = -1;     /* "gkd_pixel2_joypad": the face, d-pad, shoulders, FUNCTION */
static int fd_keys = -1;    /* "gpio-keys": volume */
static int fd_power = -1;   /* "rk805 pwrkey" */
static int dbg_input;

/* Kernel code -> TortOS button, by what is printed on the key. Measured
 * 2026-10-01 pressing each in a known order: the A, B, X and Y caps send
 * SOUTH, EAST, NORTH and WEST - named for their labels, not for where they
 * sit, since A is on the right and B at the bottom. L2 and R2 exist and TortOS
 * has no use for them. FUNCTION is MENU, handled apart below. */
static const struct { int code; in_button b; } keymap[] = {
	{ BTN_DPAD_UP, IN_UP },       { BTN_DPAD_DOWN, IN_DOWN },
	{ BTN_DPAD_LEFT, IN_LEFT },   { BTN_DPAD_RIGHT, IN_RIGHT },
	{ BTN_SOUTH, IN_ACCEPT },     { BTN_EAST, IN_BACK },
	{ BTN_NORTH, IN_X },          { BTN_WEST, IN_Y },
	{ BTN_TL, IN_L1 },            { BTN_TR, IN_R1 },
	{ BTN_SELECT, IN_SELECT },    { BTN_START, IN_START },
};
#define CODE_FUNCTION BTN_TRIGGER_HAPPY1

static int open_by_key(int code)
{
	unsigned long bits[NLONGS(KEY_MAX)];
	char path[32];
	int i, fd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		if ((fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) continue;
		memset(bits, 0, sizeof bits);
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0 && BIT_IS_SET(bits, code))
			return fd;
		close(fd);
	}
	return -1;
}

bool plat_input_init(void)
{
	char marker[512];

	snprintf(marker, sizeof marker, "%s/.input_debug", P_ROOT);
	dbg_input = getenv("TORTOS_INPUT_DEBUG") != NULL || access(marker, F_OK) == 0;
	/* Found by what each reports, not by number: there is no udev here, and
	 * enumeration order is not a promise. */
	if (fd_pad < 0)   fd_pad   = open_by_key(BTN_SOUTH);
	if (fd_keys < 0)  fd_keys  = open_by_key(KEY_VOLUMEUP);
	if (fd_power < 0) fd_power = open_by_key(KEY_POWER);
	if (fd_pad < 0)   fprintf(stderr, "input: no gamepad found\n");
	if (fd_keys < 0)  fprintf(stderr, "input: no volume keys found\n");
	if (fd_power < 0) fprintf(stderr, "input: no power key found\n");
	return true;
}

static void drain(int fd)
{
	struct input_event ev;

	while (fd >= 0 && read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) { }
}

/* Throw away everything that queued up while a game owned the screen. */
void plat_input_flush(void)
{
	SDL_Event e;

	SDL_PumpEvents();
	while (SDL_PollEvent(&e)) { }
	drain(fd_pad);
	drain(fd_keys);
	drain(fd_power);
}

/* No wait here, unlike the Brick: the keys are read straight from the kernel,
 * with no daemon or joystick layer coming up in bursts, so whatever the boot
 * produced is already queued and emptying the queues is enough. Measured
 * 2026-10-02: the Brick's 350 ms caught nothing here, power-button boots
 * included. */
void plat_input_settle(in_state *st)
{
	plat_input_flush();
	memset(st, 0, sizeof *st);
}

/* The descriptors stay open: they are display-independent. */
void plat_input_quit(void) { }

/* FUNCTION is MENU, and also the modifier that makes the volume keys
 * brightness: this device has no brightness keys, and MENU plus volume is
 * brightness in games too (Diatom's ADR-0037), so the same chord does the same
 * thing on both sides of a launch.
 *
 * A key that is both cannot report MENU when it goes down, because nothing yet
 * says whether a volume key will follow. So MENU is reported when FUNCTION
 * comes UP, unless a volume key came in between. That costs a tap's length
 * before a menu opens, accepted on 2026-10-01; in games MENU has always worked
 * this way. Holding both volume keys with FUNCTION is holding both brightness
 * keys, which is the Brick's pocket lock, so Muse's lock works unchanged. */
static bool fn_down;            /* FUNCTION is held */
static bool fn_chorded;         /* and a volume key was used under it */
static bool menu_release_next;  /* a MENU tap reported last poll ends this one */
static in_button vol_as[2] = { IN_NONE, IN_NONE };  /* what each volume key went down as */

static void key_event(in_state *st, const struct input_event *ev)
{
	size_t i;

	if (ev->type != EV_KEY || ev->value == 2) return;
	if (dbg_input) fprintf(stderr, "[in] code=%d val=%d\n", ev->code, ev->value);

	if (ev->code == CODE_FUNCTION) {
		if (ev->value) {
			fn_down = true;
			fn_chorded = false;
		} else {
			fn_down = false;
			if (!fn_chorded) {
				plat_set_btn(st, IN_MENU, true);
				menu_release_next = true;
			}
		}
		return;
	}
	if (ev->code == KEY_VOLUMEUP || ev->code == KEY_VOLUMEDOWN) {
		int k = ev->code == KEY_VOLUMEUP ? 0 : 1;

		if (ev->value) {
			if (fn_down) {
				vol_as[k] = k == 0 ? IN_BRIGHTUP : IN_BRIGHTDN;
				fn_chorded = true;
			} else {
				vol_as[k] = k == 0 ? IN_VOLUP : IN_VOLDN;
			}
			plat_set_btn(st, vol_as[k], true);
		} else if (vol_as[k] != IN_NONE) {
			/* Released as whatever it went down as, so letting go of
			 * FUNCTION first does not leave a brightness key stuck down. */
			plat_set_btn(st, vol_as[k], false);
			vol_as[k] = IN_NONE;
		}
		return;
	}
	if (ev->code == KEY_POWER) {
		plat_set_btn(st, IN_POWER, ev->value == 1);
		return;
	}
	for (i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
		if (keymap[i].code == ev->code) {
			plat_set_btn(st, keymap[i].b, ev->value == 1);
			return;
		}
}

static void poll_fd(int fd, in_state *st)
{
	struct input_event ev;

	while (fd >= 0 && read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev)
		key_event(st, &ev);
}

void plat_input_poll(in_state *st)
{
	SDL_Event e;

	memset(st->pressed, 0, sizeof st->pressed);
	if (plat_terminating()) st->quit_requested = true;
	if (menu_release_next) {
		plat_set_btn(st, IN_MENU, false);
		menu_release_next = false;
	}
	/* Whoever pumps input owns the volume; see the Brick's file. */
	plat_audio_jack_poll();
	while (SDL_PollEvent(&e))
		if (e.type == SDL_QUIT) st->quit_requested = true;
	poll_fd(fd_pad, st);
	poll_fd(fd_keys, st);
	poll_fd(fd_power, st);
}

bool device_power_pressed(void)
{
	struct input_event ev;
	bool pressed = false;

	while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
		if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1)
			pressed = true;
	return pressed;
}

/* No LEDs on this device. */
void plat_leds_off(void) { }

/* ---- volume and brightness ------------------------------------------------- */

/* The codec's controls by ioctl on /dev/snd/controlC0, the kernel's struct
 * vendored as the Brick's file does it (see there for why). */
struct px_ctl_elem_id {
	unsigned int  numid;
	int           iface;
	unsigned int  device;
	unsigned int  subdevice;
	unsigned char name[44];
	unsigned int  index;
};
struct px_aes_iec958 {
	unsigned char status[24], subcode[147], pad, dig_subframe[4];
};
struct px_ctl_elem_value {
	struct px_ctl_elem_id id;
	unsigned int indirect: 1;
	union {
		union { long value[128]; long *value_ptr; } integer;
		union { long long value[64]; long long *value_ptr; } integer64;
		union { unsigned int item[128]; unsigned int *item_ptr; } enumerated;
		union { unsigned char data[512]; unsigned char *data_ptr; } bytes;
		struct px_aes_iec958 iec958;
	} value;
	unsigned char reserved[128];
};
#define PX_CTL_ELEM_WRITE _IOWR('U', 0x13, struct px_ctl_elem_value)

/* The RK817's "Master Playback Volume": 0-255 on both channels, NOT inverted -
 * 255 is 0 dB and 0 is -95 dB, read with amixer 2026-10-01. It has no switch,
 * so 0 is the cut. Speaker or headphones is "Playback Mux", which the system's
 * jackswitch daemon drives.
 *
 * The window the twenty positions spread over is NOT YET MEASURED: 255 at the
 * top and 45 dB below it at the bottom, the range the Brick's speaker spans,
 * as a starting point to be set by ear the way the Brick's was. Headphones
 * share it until they are measured too.
 *
 * Kept as ATTENUATION from 255, so the shared aout_level_to_raw - written for
 * the Brick's inverted register, loud end low - lays the positions out with
 * the same rounding. Diatom's Pixel port uses the same numbers and the same
 * arithmetic: a level that crosses the socket has to mean the same thing on
 * both sides. Change one, change the other. */
#define GAIN_CTL      "Master Playback Volume"
#define GAIN_RAW_MAX  255
#define SPK_ATT_TOP   0      /* attenuation at position 20 */
#define SPK_ATT_BOTTOM 121   /* and at position 0, which the cut replaces */
#define HP_ATT_TOP    0
#define HP_ATT_BOTTOM 121
#define VOL_MAX       PLAT_VOL_MAX

/* sysfs, 0-255, 0 dark. The Brick's ladder, which is the launcher's rungs and
 * Diatom's: brightness reads in ratios. 1 was still lit on this panel and 0
 * dark, checked by eye 2026-10-01. */
#define BACKLIGHT       "/sys/class/backlight/backlight/brightness"
#define BACKLIGHT_POWER "/sys/class/backlight/backlight/bl_power"
static const unsigned char bright_ladder[] = {
	2, 4, 8, 16, 32, 48, 72, 96, 128, 160, 192, 255
};
#define BRIGHT_MAX ((int)(sizeof bright_ladder / sizeof bright_ladder[0]) - 1)
_Static_assert(BRIGHT_MAX == PLAT_BRIGHT_MAX, "bright_ladder vs PLAT_BRIGHT_MAX");

static int mixer_fd = -1;
static int cur_vol = -1, cur_bright = -1;
static int jack_fd = -1, jack_was = -1;

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void write_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0) return;
	if (write(fd, text, strlen(text)) < 0) { /* best effort */ }
	close(fd);
}

static int read_int(const char *path)
{
	char buf[16];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0) return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = '\0';
	return atoi(buf);
}

static void jack_open(void)
{
	unsigned long bits[NLONGS(SW_MAX)];
	char path[32];
	int i, fd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		if ((fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) continue;
		memset(bits, 0, sizeof bits);
		if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof bits), bits) >= 0 &&
		    BIT_IS_SET(bits, SW_HEADPHONE_INSERT)) {
			jack_fd = fd;
			return;
		}
		close(fd);
	}
	fprintf(stderr, "settings: no headphone jack input node\n");
}

static int jack_present(void)
{
	unsigned long bits[NLONGS(SW_MAX)];

	if (jack_fd < 0) return 0;
	memset(bits, 0, sizeof bits);
	if (ioctl(jack_fd, EVIOCGSW(sizeof bits), bits) < 0) return 0;
	return BIT_IS_SET(bits, SW_HEADPHONE_INSERT) ? 1 : 0;
}

static void apply_volume(int v)
{
	struct px_ctl_elem_value c;
	int hp = jack_present();
	long raw = v == 0 ? 0
	         : GAIN_RAW_MAX - aout_level_to_raw(v, VOL_MAX,
	                                            hp ? HP_ATT_TOP : SPK_ATT_TOP,
	                                            hp ? HP_ATT_BOTTOM : SPK_ATT_BOTTOM);

	jack_was = hp;
	cur_vol = v;
	if (mixer_fd < 0) return;
	memset(&c, 0, sizeof c);
	c.id.iface = 2;                                 /* SNDRV_CTL_ELEM_IFACE_MIXER */
	snprintf((char *)c.id.name, sizeof c.id.name, "%s", GAIN_CTL);
	/* Both channels: one written alone reads as a balance problem. */
	c.value.integer.value[0] = raw;
	c.value.integer.value[1] = raw;
	if (ioctl(mixer_fd, PX_CTL_ELEM_WRITE, &c) < 0)
		fprintf(stderr, "settings: mixer rejected '%s' = %ld\n", GAIN_CTL, raw);
}

static void apply_brightness(int b)
{
	char text[8];

	cur_bright = b;
	snprintf(text, sizeof text, "%d\n", bright_ladder[b]);
	write_file(BACKLIGHT, text);
}

static void levels_save(void)
{
	db_set_int(db_dev(), "volume", cur_vol);
	db_set_int(db_dev(), "brightness", cur_bright);
	db_write_boot_env();
}

void plat_settings_init(void)
{
	int v, b, raw;

	mixer_fd = open("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
	if (mixer_fd < 0) fprintf(stderr, "settings: no /dev/snd/controlC0\n");
	jack_open();

	/* The player's last choice wins, as on the Brick; failing that, whatever
	 * the panel is already at, to the nearest rung. */
	v = db_get_int(db_dev(), "volume", -1);
	b = db_get_int(db_dev(), "brightness", -1);
	if (b < 0 && (raw = read_int(BACKLIGHT)) >= 0) {
		int i;

		b = 0;
		for (i = 1; i <= BRIGHT_MAX; i++)
			if (abs(bright_ladder[i] - raw) < abs(bright_ladder[b] - raw)) b = i;
	}
	cur_vol    = v >= 0 ? clampi(v, 0, VOL_MAX)    : -1;
	cur_bright = b >= 0 ? clampi(b, 0, BRIGHT_MAX) : BRIGHT_MAX / 2;
	if (cur_vol >= 0) apply_volume(cur_vol);
	apply_brightness(cur_bright);
}

/* Off and back on at the same level: bl_power, the backlight's own switch, so
 * the brightness is not touched. Muse's pocket lock. */
void plat_backlight(bool on)
{
	write_file(BACKLIGHT_POWER, on ? "0\n" : "4\n");
}

/* The windows will differ by jack once both are measured, so the level is
 * re-applied when a plug goes in or out, as on the Brick. */
void plat_audio_jack_poll(void)
{
	if (!aout_should_reapply(jack_was, jack_present() != 0, cur_vol >= 0)) return;
	apply_volume(cur_vol);
}

bool plat_headphones_present(void) { return jack_present() != 0; }

/* No mute switch on this device. */
bool plat_mute_poll(bool own_volume) { (void)own_volume; return false; }
bool plat_muted(void) { return false; }

void device_levels_forget(void) { jack_was = -1; }

int plat_volume_get(void)     { return cur_vol; }
int plat_brightness_get(void) { return cur_bright; }

void plat_volume_nudge(int delta)
{
	if (mixer_fd < 0) return;
	apply_volume(clampi((cur_vol < 0 ? 0 : cur_vol) + delta, 0, VOL_MAX));
	levels_save();
	plat_osd_show(2, cur_vol, VOL_MAX);
}

void plat_brightness_nudge(int delta)
{
	apply_brightness(clampi(cur_bright + delta, 0, BRIGHT_MAX));
	levels_save();
	plat_osd_show(1, cur_bright, BRIGHT_MAX);
}

void plat_volume_set_pct(int pct)
{
	if (mixer_fd < 0) return;
	apply_volume(clampi((pct * VOL_MAX + 50) / 100, 0, VOL_MAX));
	levels_save();
}

void plat_brightness_set(int level)
{
	apply_brightness(clampi(level, 0, BRIGHT_MAX));
	levels_save();
}

/* No radio on the board. A USB dongle could change that one day; then these
 * would ask whether an adapter is there. */
bool plat_has_wifi(void)      { return false; }
bool plat_has_bluetooth(void) { return false; }

/* ---- battery ---- */

bool plat_battery(int *pct, bool *charging)
{
	const char *fake = getenv("TORTOS_FAKE_BATT");
	char st[32] = { 0 };
	FILE *s;
	int v;

	if (fake && *fake) {
		if (pct) *pct = atoi(fake);
		if (charging) *charging = false;
		return true;
	}
	if ((v = read_int("/sys/class/power_supply/battery/capacity")) < 0) return false;
	if (pct) *pct = v;
	if (charging) {
		*charging = false;
		if ((s = fopen("/sys/class/power_supply/battery/status", "r"))) {
			if (fgets(st, sizeof st, s) &&
			    (strncmp(st, "Charging", 8) == 0 || strncmp(st, "Full", 4) == 0))
				*charging = true;
			fclose(s);
		}
	}
	return true;
}
