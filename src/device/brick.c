/* SPDX-License-Identifier: MIT */
/* The TrimUI Brick (TG3040): the display, the buttons that arrive on three
 * different devices, the LEDs, the codec, the panel backlight, the battery,
 * and where things live on its card. Everything here is what makes a Brick a
 * Brick; the rest of TortOS reaches it through platform.h and device.h. */
#include "atomic.h"
#include "audioout.h"
#include "db.h"
#include "device.h"
#include "platform.h"

#include <fcntl.h>
#ifdef __linux__
#include <linux/input.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The front function keys report on the gamepad node as BTN_THUMBL/BTN_THUMBR
 * (317/318). SDL maps them to joystick buttons 9 and 10, which emulators call
 * L3/R3 -- which is why the in-game input map has to drop L3/R3, or every
 * brightness press also reaches the core. */
#define CODE_FN_LEFT  317
#define CODE_FN_RIGHT 318

/* SDL joystick button indices on the Brick's "TRIMUI Player1" device.
 *
 * SDL numbers these in ascending evdev-code order and the device declares
 * 304 305 307 308 310 311 314 315 316 317 318, so index 2 is BTN_NORTH and
 * index 3 is BTN_WEST. The buttons printed on this shell are Y and X
 * respectively - crossed from the evdev names, as A and B already are.
 *
 * These were swapped here on 2026-08-29 and swapped straight back, because
 * the change was reasoned from the evdev names rather than tested, and Eric
 * pressing the buttons settled it in one try. The table below is correct;
 * this note exists so the next person to notice that JOY_Y sits at the index
 * called BTN_NORTH does not "fix" it again. */
enum {
	JOY_B = 0, JOY_A = 1, JOY_Y = 2, JOY_X = 3,
	JOY_L1 = 4, JOY_R1 = 5, JOY_SELECT = 6, JOY_START = 7,
	JOY_MENU = 8, JOY_L3 = 9, JOY_R3 = 10,
	JOY_VOLDN = 13, JOY_VOLUP = 14,
};

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Joystick *joy;
static int fd_power = -1; /* axp2202-pek: KEY_POWER */
static int fd_keys = -1;  /* sunxi-keyboard: volume keys */
static int fd_joy = -1;   /* TRIMUI Player1: raw, for the front F1/F2 keys */

/* TORTOS_INPUT_DEBUG=1 logs raw evdev codes and SDL button indices, so one
 * press tells you exactly which device a control arrives on. */
static int dbg_input;

const char *P_ROOT = "/mnt/SDCARD/TortOS";
const char *P_CARD = "/mnt/SDCARD";
const char *P_ROMS = "/mnt/SDCARD/Roms";
const char *P_USERDATA = "/mnt/SDCARD/.userdata/tg3040";
const char *P_SHARED = "/mnt/SDCARD/.userdata/shared";
/* Over The Hare's page. On the card rather than in the binary so it can be
 * restyled with a text editor and a reload, which is the whole argument for
 * a file-transfer feature existing at all. */
const char *P_WEB = "/mnt/SDCARD/TortOS/res/web";

const char *P_FONT = "/mnt/SDCARD/TortOS/menu.ttf";

SDL_Renderer *plat_renderer(void) { return ren; }

/* The Brick's panel is the screen, so drawing to the screen is drawing to the
 * window. */
void plat_present(void) { SDL_RenderPresent(ren); }
void plat_draw_to_screen(void) { SDL_SetRenderTarget(ren, NULL); }

bool plat_video_init(void)
{
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
#ifdef __linux__
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "video init: %s\n", SDL_GetError());
		return false;
	}
	SDL_ShowCursor(SDL_DISABLE);
	win = SDL_CreateWindow("TortOS", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                       TORTOS_SCREEN_W, TORTOS_SCREEN_H,
	                       SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
	if (!win) {
		fprintf(stderr, "window: %s\n", SDL_GetError());
		return false;
	}
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) {
		fprintf(stderr, "renderer: %s\n", SDL_GetError());
		return false;
	}
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	/* Whether vsync was GRANTED, not whether it was asked for. PRESENTVSYNC
	 * is a request the driver may decline in silence, and the shelf loop has
	 * no delay in it - so a declined request is not a slower animation, it is
	 * an uncapped loop presenting mid-scanout. This line existed and read
	 * info.name only, which is how the request came to be talked about as if
	 * it were the result. */
	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(ren, &info) == 0)
		fprintf(stderr, "renderer: %s, driver: %s, vsync %s\n",
		        info.name, SDL_GetCurrentVideoDriver(),
		        (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "granted"
		                                                 : "DECLINED (requested)");
	/* The cube turns two offscreen faces, so this is load-bearing rather than
	 * informational: without it the vertical shelves have nothing to draw. */
	if (SDL_GetRendererInfo(ren, &info) == 0)
		fprintf(stderr, "renderer: render-to-texture %s\n",
		        (info.flags & SDL_RENDERER_TARGETTEXTURE) ? "available"
		                                                  : "MISSING");
	return true;
}

void plat_video_quit(void)
{
	if (ren) { SDL_DestroyRenderer(ren); ren = NULL; }
	if (win) { SDL_DestroyWindow(win); win = NULL; }
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void open_joystick(void)
{
	for (int i = 0; i < SDL_NumJoysticks(); i++) {
		SDL_Joystick *j = SDL_JoystickOpen(i);
		if (!j) continue;
		fprintf(stderr, "joystick %d: %s (%d buttons, %d hats, %d axes)\n",
		        i, SDL_JoystickName(j), SDL_JoystickNumButtons(j),
		        SDL_JoystickNumHats(j), SDL_JoystickNumAxes(j));
		if (!joy) joy = j; /* the first is TRIMUI Player1 */
	}
}

bool plat_input_init(void)
{
	char marker[512];
	snprintf(marker, sizeof marker, "%s/.input_debug", P_ROOT);
	dbg_input = getenv("TORTOS_INPUT_DEBUG") != NULL || access(marker, F_OK) == 0;
	if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
		fprintf(stderr, "joystick init: %s\n", SDL_GetError());
		return false;
	}
	SDL_JoystickEventState(SDL_ENABLE);
	open_joystick();
#ifdef __linux__
	if (fd_power < 0) fd_power = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
	if (fd_keys < 0) fd_keys = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	if (fd_joy < 0) fd_joy = open("/dev/input/event3", O_RDONLY | O_NONBLOCK);
#endif
	return true;
}

/* Throw away everything that queued up while a game owned the screen. The
 * joystick stayed open through the game, so SDL has been collecting presses
 * meant for it, and the raw descriptors have too. */
void plat_input_flush(void)
{
	SDL_Event e;
	SDL_PumpEvents();
	while (SDL_PollEvent(&e)) { }
#ifdef __linux__
	{
		struct input_event ev;
		int fds[3], i;
		fds[0] = fd_power; fds[1] = fd_keys; fds[2] = fd_joy;
		for (i = 0; i < 3; i++)
			while (fds[i] >= 0 &&
			       read(fds[i], &ev, sizeof ev) == (ssize_t)sizeof ev) { }
	}
#endif
}

/* The input noise a Brick's boot produces - replayed wake presses, the bursts
 * its input devices emit as they come up - is waited out, 350 ms of polling
 * and discarding, before anything is honored. Moved here unchanged from the
 * launcher's startup when other devices turned out not to make any. */
void plat_input_settle(in_state *st)
{
	Uint32 grace = SDL_GetTicks() + 350;

	while (SDL_GetTicks() < grace) { plat_input_poll(st); SDL_Delay(8); }
	memset(st, 0, sizeof *st);
}

void plat_input_quit(void)
{
	if (joy) { SDL_JoystickClose(joy); joy = NULL; }
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
	/* keep the raw descriptors; they are display-independent */
}

static in_button map_joy_button(int jb)
{
	switch (jb) {
	case JOY_A: return IN_ACCEPT;
	case JOY_B: return IN_BACK;
	case JOY_X: return IN_X;
	case JOY_Y: return IN_Y;
	case JOY_L1: return IN_L1;
	case JOY_R1: return IN_R1;
	case JOY_START: return IN_START;
	case JOY_SELECT: return IN_SELECT;
	case JOY_MENU: return IN_MENU;
	case JOY_VOLUP: return IN_VOLUP;
	case JOY_VOLDN: return IN_VOLDN;
	default: return IN_NONE;
	}
}

static void poll_raw_fd(int fd, in_state *st)
{
#ifdef __linux__
	struct input_event ev;
	while (fd >= 0 && read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type != EV_KEY) continue;
		if (dbg_input)
			fprintf(stderr, "[in] raw fd=%d code=%d val=%d\n", fd, ev.code, ev.value);
		in_button b = IN_NONE;
		if (ev.code == KEY_POWER) b = IN_POWER;
		else if (ev.code == KEY_VOLUMEUP) b = IN_VOLUP;
		else if (ev.code == KEY_VOLUMEDOWN) b = IN_VOLDN;
		else if (ev.code == CODE_FN_RIGHT) b = IN_BRIGHTUP;
		else if (ev.code == CODE_FN_LEFT) b = IN_BRIGHTDN;
		if (b != IN_NONE && ev.value != 2)
			plat_set_btn(st, b, ev.value == 1);
	}
#else
	(void)fd; (void)st;
#endif
}

/* keyboard fallback so the native dev build is drivable */
static in_button map_key(SDL_Keycode k)
{
	switch (k) {
	case SDLK_LEFT: return IN_LEFT;
	case SDLK_RIGHT: return IN_RIGHT;
	case SDLK_UP: return IN_UP;
	case SDLK_DOWN: return IN_DOWN;
	case SDLK_RETURN: return IN_ACCEPT;
	case SDLK_ESCAPE: return IN_BACK;
	case SDLK_BACKSPACE: return IN_BACK;
	case SDLK_MINUS: return IN_VOLDN;
	case SDLK_EQUALS: return IN_VOLUP;
	default: return IN_NONE;
	}
}

void plat_input_poll(in_state *st)
{
	memset(st->pressed, 0, sizeof st->pressed);
	if (plat_terminating()) st->quit_requested = true;
	/* The jack, checked here because this is the one thing every screen does
	 * once a frame - and, more to the point, the one thing the launcher stops
	 * doing while a game runs, since it is blocked in plat_resident_wait. That
	 * is exactly the ownership boundary the level needs: whoever is pumping
	 * input owns the volume, so the launcher must not re-apply a level over
	 * Diatom's while Diatom holds it. Diatom polls the same switch on its own
	 * side for the same reason. */
	plat_audio_jack_poll();
	/* And the mute switch, for the same reason and on the same frame. */
	plat_mute_poll(true);
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
			st->quit_requested = true;
			break;
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
			if (dbg_input && e.type == SDL_JOYBUTTONDOWN)
				fprintf(stderr, "[in] joy button=%d\n", e.jbutton.button);
			plat_set_btn(st, map_joy_button(e.jbutton.button),
			        e.type == SDL_JOYBUTTONDOWN);
			break;
		case SDL_JOYHATMOTION: {
			Uint8 v = e.jhat.value;
			plat_set_btn(st, IN_LEFT,  (v & SDL_HAT_LEFT)  != 0);
			plat_set_btn(st, IN_RIGHT, (v & SDL_HAT_RIGHT) != 0);
			plat_set_btn(st, IN_UP,    (v & SDL_HAT_UP)    != 0);
			plat_set_btn(st, IN_DOWN,  (v & SDL_HAT_DOWN)  != 0);
			break;
		}
		case SDL_JOYDEVICEADDED:
			if (!joy) open_joystick();
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			if (e.key.repeat) break;
			plat_set_btn(st, map_key(e.key.keysym.sym), e.type == SDL_KEYDOWN);
			if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q)
				st->quit_requested = true;
			break;
		}
	}
	poll_raw_fd(fd_power, st);
	poll_raw_fd(fd_keys, st);
	poll_raw_fd(fd_joy, st);
}

/* The launcher and Diatom take turns on one framebuffer here, so there is
 * nothing to hand over. */
void device_display_release(void) { }
void device_display_take(void) { }

/* The power key's queue, drained: true if a press was in it. The launcher
 * watches power this way while a child or a game owns the screen, which is
 * the one control that always has to work. */
bool device_power_pressed(void)
{
	bool pressed = false;
#ifdef __linux__
	struct input_event ev;

	while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
		if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1)
			pressed = true;
#endif
	return pressed;
}

#ifdef __linux__
static void write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return;
	if (write(fd, val, strlen(val)) < 0) { /* best effort */ }
	close(fd);
}
#endif

void plat_leds_off(void)
{
#ifdef __linux__
	/* Stop the animation engine, scale every group to zero, then zero all 23
	 * raw channels directly -- so it holds whatever state the stock input
	 * daemon left the engine in. */
	write_str("/sys/class/led_anim/effect_enable", "0");
	static const char *groups[] = { "l", "r", "lr", "m", "f1", "f2" };
	char path[96];
	for (size_t i = 0; i < sizeof groups / sizeof *groups; i++) {
		snprintf(path, sizeof path, "/sys/class/led_anim/effect_rgb_hex_%s", groups[i]);
		write_str(path, "000000 ");
	}
	write_str("/sys/class/led_anim/max_scale", "0");
	write_str("/sys/class/led_anim/max_scale_lr", "0");
	write_str("/sys/class/led_anim/max_scale_f1f2", "0");
	for (int n = 0; n < 23; n++) {
		for (const char *c = "rgb"; *c; c++) {
			snprintf(path, sizeof path,
			         "/sys/class/leds/sunxi_led%d%c/brightness", n, *c);
			write_str(path, "0");
		}
	}
#endif
}

/* ---- volume and brightness, straight at the hardware ----------------------
 *
 * TortOS's own code, against the same two device interfaces the emulator uses.
 * It replaced a third-party settings library, which bought three things beyond
 * independence:
 *
 *   - The scales MATCH. Diatom drives volume as 21 positions and brightness
 *     as a 12-rung ladder; libmsettings used 0-20 and 0-10. Every level
 *     crossing the socket had to be rescaled, and a rescale is where an
 *     off-by-one hides. Both sides now speak the same ladder and the
 *     conversion is the identity.
 *   - Settings persist in TortOS's own file rather than the stock firmware's
 *     /mnt/UDISK/system.json, which is TrimUI's state and not ours to own.
 *   - One fewer binary on the card.
 *
 * The device facts here were measured, not guessed, and two of them are traps:
 *
 *   `digital volume` (0-63) is the speaker level and is INVERTED - 0 is
 *   loudest, 63 is quietest - while the driver's own metadata advertises the
 *   opposite. It also declares mute=0, so its minimum is maximum attenuation
 *   (about -74 dB) rather than silence; the speaker switch carries the last
 *   step.
 *
 *   `Headphone Volume` is NOT a speaker level. Raising it routes audio to the
 *   jack and mutes the speakers. It stays at zero.
 *
 * The backlight has no /sys/class/backlight on this device; it is the
 * Allwinner disp2 engine, addressed with plain command numbers (not _IOWR) and
 * an unsigned long[4] argument block - the same call tools/setbright.c makes
 * before the launcher exists. */

#ifdef __linux__
#include <sys/ioctl.h>

/* From the kernel UAPI (sound/asound.h), vendored rather than depended on.
 * Only the integer case is needed; the union is declared at full size because
 * its size is what _IOWR bakes into the request number, and a wrong layout
 * produces a wrong request number rather than a failed call. */
struct pl_ctl_elem_id {
	unsigned int  numid;
	int           iface;
	unsigned int  device;
	unsigned int  subdevice;
	unsigned char name[44];
	unsigned int  index;
};
struct pl_aes_iec958 {
	unsigned char status[24], subcode[147], pad, dig_subframe[4];
};
struct pl_ctl_elem_value {
	struct pl_ctl_elem_id id;
	unsigned int indirect: 1;
	union {
		union { long value[128]; long *value_ptr; } integer;
		union { long long value[64]; long long *value_ptr; } integer64;
		union { unsigned int item[128]; unsigned int *item_ptr; } enumerated;
		union { unsigned char data[512]; unsigned char *data_ptr; } bytes;
		struct pl_aes_iec958 iec958;
	} value;
	unsigned char reserved[128];
};
#define PL_CTL_ELEM_READ   _IOWR('U', 0x12, struct pl_ctl_elem_value)
#define PL_CTL_ELEM_WRITE  _IOWR('U', 0x13, struct pl_ctl_elem_value)

#define GAIN_CTL     "digital volume"
#define GAIN_RAW_MAX 63          /* 0 is loudest, 63 quietest */

/* Only the top of that register is worth spending on the volume keys. The
 * control is 1.16 dB per step, so using all 63 puts 73 dB across 21 positions -
 * 3.65 dB a press, which made 60% of the scale -29 dB and the bottom two thirds
 * inaudible. That is not a curve shape problem: dB is already the perceptually
 * even axis, the same reason the brightness ladder below is geometric. It is a
 * RANGE problem, and 73 dB is simply more than a handheld speaker has.
 *
 * Measured on the device 2026-08-28 with a 440 Hz tone at -1.4 dBFS captured on
 * its own microphone. Coarse sweep, room baseline ~40 rms:
 *
 *   raw 0   10034 rms   201x room      raw 31    114 rms   2.3x room
 *   raw 16   1485 rms    30x room      raw 47     34 rms   inaudible
 *
 * The floor was first set at 34 by reading that table, and Eric reported the
 * bottom of the scale as dead. A finer sweep says why - room baseline 33:
 *
 *   raw 18  1091 rms  32.7x      raw 30    85 rms   2.5x
 *   raw 22   404 rms  12.1x      raw 34    55 rms   1.6x
 *   raw 26   173 rms   5.2x
 *
 * 34 is 1.6x room noise, so the last few positions were all sitting in the
 * noise and were indistinguishable from each other. 26 is 5.2x: quiet, and
 * unmistakably present. That is the floor, ~1.5 dB a press across the 21
 * positions.
 *
 * Those rms numbers were captured with HP_CTL already at 0 - the sweep script
 * sets it before measuring - so they describe the chain as it behaves now, and
 * the 18 dB that mixer_defaults() restored was never inside them. A note here
 * briefly claimed the opposite. That was inferred rather than read off the
 * script which produced the table, and the script was on disk the whole time.
 * The 18 dB gap was between the SWEEP and gameplay, not inside the sweep, which
 * is exactly why the table looked sane while the device sounded quiet.
 *
 * What is weak here is the instrument, not the conditions. Those readings came
 * from a microphone across the room, where raw 26 is 5.2x the room and raw 34
 * is indistinguishable from it. A handheld at arm's length is a different
 * question: what reads as silence over there is plainly audible in your hands.
 * That is why the bottom of the scale was still reported as too loud once the
 * chain was fixed, and why the floor below is now set by ear rather than by
 * that table.
 *
 * That session had ONE WORKING SPEAKER, which nobody knew at the time. The
 * quiet channel turned out on 2026-09-02 to be a loose connection on the PCB;
 * it was resoldered and both now play evenly. So 39 was originally judged
 * against roughly half this device's output.
 *
 * It stood anyway. Re-heard on the repaired hardware the same day - the quiet
 * end, the balance and the general sound - and called right, so the constant
 * now has two independent confirmations rather than one lucky derivation. Do
 * not weaken it back to a guess on the strength of the history above.
 *
 * 39, chosen on the device on 2026-08-31 with a game playing, stepping the
 * register down until Eric called it: raw 37 is barely audible and is where he
 * wanted position 1. 39 is the constant that puts position 1 on 37 in this
 * ladder AND in Diatom's, which round differently; 26 put it on 25. Position 20
 * still lands on raw 0, so nothing about maximum changes.
 *
 * It costs resolution. Twenty positions across 39 is about 2.3 dB a press
 * rather than 1.5, bought with a range of 45 dB rather than 30. That is the
 * trade worth making: 30 dB down is not quiet in a room that is quiet, which
 * a microphone across the room could not tell us and an ear could. */
#define GAIN_RAW_USABLE 39

/* Headphones need a different window, not the same one moved.
 *
 * Set by ear on 2026-09-02 with a game playing and a plug in the jack, the same
 * method that produced 39. The ceiling first: above raw 8 it is uncomfortable,
 * so 8 is where position 20 belongs. Then the floor, stepping down - 37, 45,
 * 49, 53 and 57 were each still too loud to be a minimum, and 61 was called
 * right.
 *
 * So the jack spends 53 register steps where the speaker spends 39, and an
 * offset cannot express that. An offset from the measured ceiling would have
 * put position 1 on 47, which was rejected on the way past. That is physically
 * unsurprising: headphones are far more efficient than this speaker, so the
 * same twenty presses have to cover more ground.
 *
 * The cost is resolution. 53 steps across 20 positions is about 3.1 dB a press
 * against the speaker's 2.3. Giving the jack its own number of positions would
 * fix that and is not worth it - 21 positions are shared verbatim with Diatom
 * and launch.sh so a level crossing the socket needs no conversion, and that
 * property is worth more than 0.8 dB of granularity.
 *
 * Both pairs are tied to one game's mix, exactly as 39 was. A quieter game sits
 * under this floor. Consistent with what was already here, not worse. */
#define SPK_RAW_TOP     0
#define SPK_RAW_BOTTOM  GAIN_RAW_USABLE
#define HP_RAW_TOP      8
#define HP_RAW_BOTTOM   61

#define SPEAKER_CTL  "HpSpeaker Switch"   /* the speaker's only true mute */
#define HP_CTL       "Headphone Volume"   /* 0-7, 6 dB a step, INVERTED */
#define HP_CTL_QUIET 7                    /* its quiet end, for the cut */
#define SWAP_CTL     "DAC Swap"           /* 1 crosses left and right */
#define VOL_MAX      PLAT_VOL_MAX         /* 21 positions, 0..20 - Diatom's scale */

#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103

/* Perceived brightness is proportional, not linear, so the rungs are
 * geometric. The first is the panel's measured floor: 0 and 1 are black on
 * this display and the driver clamps neither. Identical to Diatom's ladder,
 * which is what makes the level handoff exact. */
static const unsigned char bright_ladder[] = {
	2, 4, 8, 16, 32, 48, 72, 96, 128, 160, 192, 255
};
#define BRIGHT_MAX ((int)(sizeof bright_ladder / sizeof bright_ladder[0]) - 1)
/* The table is the definition; the header only publishes its extent. If they
 * ever disagree this stops the build rather than shipping a rescale that is
 * quietly off by a rung, which is the bug that put the maximum in a header. */
_Static_assert(BRIGHT_MAX == PLAT_BRIGHT_MAX, "bright_ladder vs PLAT_BRIGHT_MAX");

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static int mixer_fd = -1, disp_fd = -1;
static int cur_vol = -1, cur_bright = -1;

static int ctl_io(const char *name, long *val, int write)
{
	struct pl_ctl_elem_value v;

	if (mixer_fd < 0) return -1;
	memset(&v, 0, sizeof v);
	v.id.iface = 2;                                 /* SNDRV_CTL_ELEM_IFACE_MIXER */
	snprintf((char *)v.id.name, sizeof v.id.name, "%s", name);
	if (write) {
		v.value.integer.value[0] = *val;
		return ioctl(mixer_fd, PL_CTL_ELEM_WRITE, &v);
	}
	if (ioctl(mixer_fd, PL_CTL_ELEM_READ, &v) < 0) return -1;
	*val = v.value.integer.value[0];
	return 0;
}

/* Every nudge of the rocker lands here, including while a game is running, so
 * what this costs is a frame-budget question rather than a tidiness one.
 * Measured on the card 2026-09-05: the atomic_open/atomic_commit this replaces
 * was 3.03 ms median with a 9.27 ms tail, against a 16.7 ms frame. The two
 * writes below are 0.48 ms each with a 0.59 ms tail. tools/storeprobe.c. */
static void levels_save(void)
{
	db_set_int(db_dev(), "volume", cur_vol);
	db_set_int(db_dev(), "brightness", cur_bright);
	/* launch.sh sets the panel from this before the boot animation and cannot
	 * read a database, so the export has to follow a brightness change.
	 *
	 * It costs nothing when nothing in it moved - db_write_boot_env compares
	 * against what it last wrote and returns. That matters because VOLUME is
	 * not in boot.env at all, and calling this unconditionally put a file
	 * replacement back on the volume rocker: 3.03 ms median, 9.27 ms at the
	 * tail, while a game is running. Which is the exact cost moving settings
	 * into the database was meant to remove. */
	db_write_boot_env();
}

/* Write a control and complain if it does not land. Discarding this return is
 * what cost the project 18 dB for its whole life; see mixer_defaults(). */
static void ctl_set(const char *name, long val)
{
	if (ctl_io(name, &val, 1) < 0)
		fprintf(stderr, "settings: mixer rejected '%s' = %ld\n", name, val);
}

/* Codec-wide state that the volume keys do not own, set once at init.
 *
 * HP_CTL is 0-7 at 6 dB a step and is INVERTED, exactly like GAIN_CTL above:
 * 0 is loudest, 7 is near silence. The driver's TLV claims the opposite
 * (dBscale-min=-42 dB, step +6 dB) and is wrong in the same way it is wrong
 * about GAIN_CTL, so the metadata cannot be trusted on this codec at all.
 *
 * Despite its name it is not the jack. SPEAKER_CTL drives the speaker off the
 * headphone stage, so this control gates everything the device plays. Diatom's
 * port carried a comment insisting that raising it rerouted output to the jack
 * and muted the speakers - what actually happens is that raising it attenuates
 * the speaker, which sounds identical and is not the same thing. Settled by
 * ear on 2026-08-31: at 7 the speaker is barely audible, at 0 it is loud.
 *
 * This write already existed, in apply_volume, spelled "Headphone" - a control
 * this codec does not have. PL_CTL_ELEM_WRITE matches names exactly, the
 * return was thrown away, and so every sound the device ever made came out
 * 18 dB down while the source read as though it were setting this to zero.
 *
 * SWAP_CTL at 1 crosses the channels. The stock hook clears it
 * (runtrimui-original.sh: `tinymix set 1 0`) and so does NextUI; we never did,
 * so left and right have been backwards the entire time. It is an enumerated
 * control rather than an integer, but the value union overlaps and we only
 * ever write item 0, so the integer path reaches it.
 *
 * HP_CTL stays at 0 for both outputs. The jack was calibrated on 2026-09-02 and
 * the answer was not this control: 9.3 dB was wanted at the ceiling and this
 * steps in sixes, and it attenuates the speaker as well, so it cannot be moved
 * for the jack alone. The headphone case is handled by its own window on
 * GAIN_CTL instead - see HP_RAW_TOP. The one exception is the cut, in
 * apply_volume. */
static void mixer_defaults(void)
{
	if (mixer_fd < 0) return;
	ctl_set(HP_CTL, 0);
	ctl_set(SWAP_CTL, 0);
}

/* Is there a plug in the headphone jack?
 *
 * SW_HEADPHONE_INSERT on the codec's own input node. This device exposes the
 * state nowhere else: there is no ALSA jack kcontrol among its seventeen
 * controls and nothing under /sys for it, so the only way to ask is EVIOCGSW,
 * which is why this opens an input device rather than reading a file.
 *
 * Found by capability rather than by number. It is /dev/input/event2 today, but
 * that is an enumeration order and not a promise, and the cost of being wrong
 * is a volume ladder silently calibrated for the wrong output. */
#define BITS_PER_LONG   (8 * (int)sizeof(long))
#define SW_NLONGS       ((SW_MAX + BITS_PER_LONG) / BITS_PER_LONG)
#define BIT_IS_SET(a,b) (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1UL)

static int jack_fd = -1;

static void jack_open(void)
{
	unsigned long bits[SW_NLONGS];
	char path[32];
	int i, fd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0) continue;
		memset(bits, 0, sizeof bits);
		if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof bits), bits) >= 0 &&
		    BIT_IS_SET(bits, SW_HEADPHONE_INSERT)) {
			jack_fd = fd;
			return;
		}
		close(fd);
	}
	fprintf(stderr, "settings: no headphone jack input node; "
	                "volume will use the speaker ladder\n");
}

static int jack_present(void)
{
	unsigned long bits[SW_NLONGS];

	if (jack_fd < 0) return 0;
	memset(bits, 0, sizeof bits);
	if (ioctl(jack_fd, EVIOCGSW(sizeof bits), bits) < 0) return 0;
	return BIT_IS_SET(bits, SW_HEADPHONE_INSERT) ? 1 : 0;
}

static int jack_was = -1;         /* last state acted on; -1 = never asked */

/* THE MUTE SWITCH, and why it is read here rather than as an input event.
 *
 * Measured 2026-09-16 by watching every input device and every exported GPIO
 * while the switch was flipped. It reports two ways: as EV_SW code 1 on
 * /dev/input/event3, and as this pin. The pin is the one worth reading,
 * because an event only arrives on a CHANGE - it says nothing about which way
 * the switch is pointing at boot, after a resume, or after the launcher
 * restarts, which is exactly when a physical control and the software can
 * disagree. A file that always holds the truth cannot drift.
 *
 * DOWN, which reads 1, is muted. Eric's call: "off" is what the position says,
 * and on a mute switch that labels the sound.
 *
 * Opened once and pread, not opened per poll: this runs from plat_input_poll,
 * which every screen calls once a frame. */
#define MUTE_GPIO "/sys/class/gpio/gpio243/value"
static int mute_fd = -1;
/* Tri-state: -1 means "not known", which is not the same as "not muted".
 *
 * The switch can be flipped DURING a game, when Diatom owns the codec and this
 * side is blocked in plat_resident_wait seeing nothing. Coming back with a
 * remembered position, the poll below compares the hardware against it, finds
 * them equal and returns without re-applying - so the launcher would play at
 * full volume with the switch down. Exactly the fault jack_forget exists to
 * prevent, and found by reading its comment. */
static int muted = -1;

static bool mute_switch_down(void)
{
	char c = 0;

	if (mute_fd < 0) mute_fd = open(MUTE_GPIO, O_RDONLY | O_CLOEXEC);
	if (mute_fd < 0) return false;
	if (pread(mute_fd, &c, 1, 0) != 1) return false;
	return c == '1';
}

static void apply_volume(int v)
{
	int hp = jack_present();
	bool cut = v == 0 || muted == 1;
	long raw = cut ? GAIN_RAW_MAX
	         : aout_level_to_raw(v, VOL_MAX,
	                             hp ? HP_RAW_TOP : SPK_RAW_TOP,
	                             hp ? HP_RAW_BOTTOM : SPK_RAW_BOTTOM);
	long quiet = cut ? HP_CTL_QUIET : 0;
	long on = !cut;

	jack_was = hp;
	cur_vol = v;
	/* Zero has to cut the path, not merely attenuate it - and so does the
	 * switch, which is why it lands here rather than beside the callers: every
	 * route that re-applies a level (a nudge, a jack coming out) passes
	 * through this line and honors the switch for free.
	 *
	 * SPEAKER_CTL cuts the speaker only, so with headphones in the switch and
	 * level 0 both left them playing until 2026-09-30. They are cut by level
	 * instead: GAIN_CTL and HP_CTL both at their quiet ends, about -116 dB and
	 * silent by ear that day. Not "Headphone Switch", tried the same day: with
	 * it and SPEAKER_CTL both off the codec stops taking samples (hw_ptr stood
	 * still), and whatever waits on its stream waits until one comes back on -
	 * a game froze switching to a headset until the mute came off. */
	ctl_io(GAIN_CTL, &raw, 1);
	ctl_io(HP_CTL, &quiet, 1);
	ctl_io(SPEAKER_CTL, &on, 1);
}

/* Re-apply if the plug went in or came out since the last time.
 *
 * Without this a level only moves to the right ladder at the next volume press,
 * so plugging in mid-game leaves the old register in place - which is precisely
 * the moment the difference is 9 dB and being worn on your head. Cheap enough
 * to call from a periodic path: one ioctl on an already-open fd, and it writes
 * nothing unless the state actually changed. */
bool plat_headphones_present(void) { return jack_present() != 0; }

/* The Brick's port can play through a USB-C DAC once it is a host, but nothing
 * on the board tells it a device is plugged in, and switching it to host by
 * hand risks supplying power into a charger: not worth a board. 2026-10-05. */
bool plat_usb_audio_present(void) { return false; }

/* Files go over Wi-Fi here, by Over The Hare. */
bool plat_cable_link(char *addr, size_t n)
{
	(void)addr; (void)n;
	return false;
}

/* The switch, checked wherever the jack is and for the same reason: this is
 * the one thing every screen does once a frame.
 *
 * It mutes the LAUNCHER's audio only. While a game runs the launcher is
 * blocked in plat_resident_wait and Diatom owns the codec - it must, or the
 * two would fight over one control - so the switch reaches a game by being
 * told to it, not by both of them reading the same pin. Deciding what a
 * physical control MEANS is the launcher's job; being quiet when asked is the
 * emulator's. See BACKLOG 28. */
bool plat_mute_poll(bool own_volume)
{
	int now = mute_switch_down() ? 1 : 0;

	if (now == muted) return false;
	muted = now;
	/* Out of a game this side is the only writer. Diatom used to be told here
	 * too, and it answered by applying its own level - which out of a game is
	 * -1, unknown - so every unmute switched the speaker back off a moment
	 * after this side turned it on, and with headphones in wrote a gain past
	 * the register's end that wrapped around to full volume. Found
	 * 2026-09-30. It needs no telling here: every RUN sends the switch's
	 * state before the game starts. */
	if (own_volume) {
		if (cur_vol >= 0) apply_volume(cur_vol);
		return true;
	}
	/* DURING A GAME, ONLY THE CUT - never this side's level.
	 *
	 * apply_volume would write GAIN_CTL from cur_vol, which is this side's
	 * idea of the volume and is stale the moment Diatom takes over: the
	 * player can change it in-game and only Diatom knows where it ended up.
	 * Re-applying it here is the exact fault the comment above plat_input_poll
	 * warns about, arriving by a new road.
	 *
	 * Turning the speaker back ON is safe even if Diatom sits at level 0,
	 * because it attenuates as well as switching - its own comment puts the
	 * control's minimum near -74 dB - so the worst case is a path opened onto
	 * something already inaudible, and Diatom's own next write settles it.
	 *
	 * This side cuts because it is immediate: 100 ms, the resident loop's
	 * period. The SETMUTE is what makes the cut STICK, since Diatom writes
	 * the same controls whenever it applies a level (ADR-0031). Neither alone
	 * is enough - one is fast and one is durable. */
	plat_resident_tell("SETMUTE\ton=%d", muted == 1 ? 1 : 0);
	{
		long on = (muted != 1);

		ctl_io(SPEAKER_CTL, &on, 1);
		/* And the headphones, by level as in apply_volume - the cut only.
		 * Coming back, the level is Diatom's, and the SETMUTE above has it
		 * put its own back. */
		if (!on) {
			long raw = GAIN_RAW_MAX, quiet = HP_CTL_QUIET;

			ctl_io(GAIN_CTL, &raw, 1);
			ctl_io(HP_CTL, &quiet, 1);
		}
	}
	return true;
}

bool plat_muted(void) { return muted == 1; }

/* Whatever this side remembers about the switch was formed while it was
 * driving; a game just was. Called where jack_forget is, and for its reason. */
static void mute_forget(void) { muted = -1; }

void plat_audio_jack_poll(void)
{
	if (!aout_should_reapply(jack_was, jack_present() != 0, cur_vol >= 0))
		return;
	apply_volume(cur_vol);
}

/* Forget which way the jack was, so the next poll re-applies whatever the
 * hardware says now instead of trusting a memory formed before a handover. */
static void jack_forget(void) { jack_was = -1; }

static void apply_brightness(int b)
{
	unsigned long a[4] = { 0, 0, 0, 0 };

	cur_bright = b;
	if (disp_fd < 0) return;
	a[1] = bright_ladder[b];
	ioctl(disp_fd, DISP_LCD_SET_BRIGHTNESS, a);
}

void plat_settings_init(void)
{
	int v = -1, b = -1;

	mixer_fd = open("/dev/snd/controlC0", O_RDWR);
	disp_fd  = open("/dev/disp", O_RDWR);
	if (mixer_fd < 0) fprintf(stderr, "settings: no /dev/snd/controlC0\n");
	if (disp_fd  < 0) fprintf(stderr, "settings: no /dev/disp\n");
	mixer_defaults();
	jack_open();

	/* The player's last choice, and it wins. The two-tier lookup this
	 * replaces - a saved level, then a shipped default - is one key each,
	 * because seeding writes the default once and a nudge overwrites it. Same
	 * outcome, and it can no longer get the precedence wrong: the config used
	 * to be reapplied over the saved level at every boot, which put it ahead
	 * of the player. */
	v = db_get_int(db_dev(), "volume", -1);
	b = db_get_int(db_dev(), "brightness", -1);

	/* Both are stored in the units this code uses - a rung each - so there is
	 * no conversion here and no way for one key to mean two things. */

	/* Last, whatever the panel is already at, so the launcher's first OSD
	 * tells the truth even with nothing configured anywhere - launch.sh set a
	 * brightness before this process existed. Nearest rung, because the panel
	 * reports raw values and only the ladder has rungs. */
	if (b < 0 && disp_fd >= 0) {
		int raw = ioctl(disp_fd, DISP_LCD_GET_BRIGHTNESS, (unsigned long[4]){ 0, 0, 0, 0 });
		int i;
		if (raw >= 0) {
			b = 0;
			for (i = 1; i <= BRIGHT_MAX; i++)
				if (abs(bright_ladder[i] - raw) < abs(bright_ladder[b] - raw)) b = i;
		}
	}
	cur_vol    = v >= 0 ? clampi(v, 0, VOL_MAX)    : -1;
	cur_bright = b >= 0 ? clampi(b, 0, BRIGHT_MAX) : BRIGHT_MAX / 2;
	if (cur_vol >= 0) apply_volume(cur_vol);
	apply_brightness(cur_bright);
}

/* The disp2 engine's LCD commands, beside SET/GET_BRIGHTNESS in the vendor's
 * sunxi_display2.h: 0x104 lights the backlight, 0x105 puts it out. Screen 0
 * in the first word, as for brightness. Watched on the device both ways. */
#define DISP_LCD_BACKLIGHT_ENABLE  0x104
#define DISP_LCD_BACKLIGHT_DISABLE 0x105

void plat_backlight(bool on)
{
	unsigned long a[4] = { 0, 0, 0, 0 };

	if (disp_fd < 0) return;
	ioctl(disp_fd, on ? DISP_LCD_BACKLIGHT_ENABLE : DISP_LCD_BACKLIGHT_DISABLE, a);
}

#else   /* not __linux__ */

/* The host build exists to look at the shelf while changing how it looks
 * (mk/native.mk). There is no codec and no display engine here, so the
 * settings are state and nothing more - enough that the OSD draws and the
 * levels the launcher reports are consistent. */
#define VOL_MAX    PLAT_VOL_MAX
#define BRIGHT_MAX PLAT_BRIGHT_MAX
static int cur_vol = 8, cur_bright = 7;
static int mixer_fd = 0, disp_fd = 0;    /* "present", so the nudges run */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static void levels_save(void) { }
static void apply_volume(int v) { cur_vol = v; }
static void apply_brightness(int b) { cur_bright = b; }
/* No jack on the host, so nothing can be plugged into it. */
void plat_audio_jack_poll(void) { }
/* And no switch to flip. */
bool plat_mute_poll(bool own_volume) { (void)own_volume; return false; }
bool plat_muted(void) { return false; }
bool plat_headphones_present(void) { return false; }
bool plat_usb_audio_present(void) { return false; }
bool plat_cable_link(char *addr, size_t n)
{
	(void)addr; (void)n;
	return false;
}
static void jack_forget(void) { }
static void mute_forget(void) { }
void plat_backlight(bool on) { (void)on; }

/* No settings database on the host, so the config defaults are all there is.
 * Taken anyway rather than ignored: a shelf rendered by --shot should show the
 * OSD at the levels the card is configured for. */
void plat_settings_init(void)
{
	/* The host has no codec and no display engine, so the levels above are
	 * all there is and nothing needs applying to hardware. */
}

#endif  /* __linux__ */

/* Called by the transport the moment a game hands input back: whatever this
 * side remembers about the jack and the switch was formed while something
 * else was driving. */
void device_levels_forget(void)
{
	jack_forget();
	mute_forget();
}

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
	if (disp_fd < 0) return;
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
	if (disp_fd < 0) return;
	apply_brightness(clampi(level, 0, BRIGHT_MAX));
	levels_save();
}

/* Both radios are on the Brick's board: the xradio chip carries Wi-Fi and
 * Bluetooth. */
bool plat_has_wifi(void)      { return true; }
bool plat_has_bluetooth(void) { return true; }

/* ---- battery ---- */

bool plat_battery(int *pct, bool *charging)
{
	const char *fake = getenv("TORTOS_FAKE_BATT");
	if (fake && *fake) {
		if (pct) *pct = atoi(fake);
		if (charging) *charging = false;
		return true;
	}
#ifdef __linux__
	FILE *f = fopen("/sys/class/power_supply/axp2202-battery/capacity", "r");
	if (!f) return false;
	int v = -1;
	if (fscanf(f, "%d", &v) != 1) v = -1;
	fclose(f);
	if (v < 0) return false;
	if (pct) *pct = v;
	if (charging) {
		*charging = false;
		FILE *s = fopen("/sys/class/power_supply/axp2202-battery/status", "r");
		if (s) {
			char st[32] = { 0 };
			if (fgets(st, sizeof st, s) &&
			    (strncmp(st, "Charging", 8) == 0 || strncmp(st, "Full", 4) == 0))
				*charging = true;
			fclose(s);
		}
	}
	return true;
#else
	(void)pct;
	(void)charging;
	return false;
#endif
}
