/* SPDX-License-Identifier: 0BSD */
#include "platform.h"

#include "ui.h"   /* the settings line shares the rail's weight and palette */

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#ifdef __linux__
#include <linux/input.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
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

/* Hold-to-repeat. Short delay and a quick rate: this is a shelf you sweep
 * along, and waiting on a key repeat is the most obvious kind of slow. */
#define REPEAT_DELAY_MS 300
#define REPEAT_RATE_MS  90

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
const char *P_USERDATA = "/mnt/SDCARD/.userdata/tg5040";
const char *P_SHARED = "/mnt/SDCARD/.userdata/shared";

/* ---- core options, read once from coreopts.cfg --------------------------- */
/* Lines before any [SECTION] apply to every game; a [TAG] section applies only
 * to that system, keyed on the same tag systems.cfg uses for saves and states.
 *
 * The per-system half exists because mgba_gb_model cannot be set globally.
 * Autodetect is right for Game Boy Color and GBA and wrong only for Game Boy,
 * where it reads the SGB flag and boots a Super Game Boy - a SNES accessory -
 * so a DMG cartridge comes out colorized and framed in a border, on a shelf
 * that says Game Boy. Pinning the model globally would force GBC titles into
 * DMG mode too, and a 0xC0 cartridge would refuse to boot. */
#define COREOPT_MAX 48
static struct { char tag[8]; char kv[192]; } coreopts[COREOPT_MAX];
static int  ncoreopts = -1;   /* -1 = not read yet */

static void coreopts_load(void)
{
	char path[512], line[256], section[8] = "";
	FILE *f;

	ncoreopts = 0;
	snprintf(path, sizeof path, "%s/coreopts.cfg", P_ROOT);
	if (!(f = fopen(path, "r"))) return;
	while (ncoreopts < COREOPT_MAX && fgets(line, sizeof line, f)) {
		char *p = line, *end;
		while (*p == ' ' || *p == '\t') p++;
		end = p + strcspn(p, "\r\n");
		*end = '\0';
		while (end > p && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
		if (*p == '#' || !*p) continue;
		if (*p == '[') {
			char *close = strchr(p, ']');
			if (!close) continue;              /* malformed, ignore quietly */
			*close = '\0';
			snprintf(section, sizeof section, "%s", p + 1);
			continue;
		}
		if (!strchr(p, '=')) continue;         /* not key=value */
		/* A line too long for the slot is REFUSED, not stored short. Half a
		 * key=value pair is still a valid-looking key=value pair, and it would
		 * be sent to a core as though someone meant it. */
		if (strlen(p) >= sizeof coreopts[0].kv) {
			fprintf(stderr, "coreopts.cfg: line too long, ignoring: %.40s...\n", p);
			continue;
		}
		snprintf(coreopts[ncoreopts].tag, sizeof coreopts[0].tag, "%s", section);
		snprintf(coreopts[ncoreopts].kv,  sizeof coreopts[0].kv,  "%s", p);
		ncoreopts++;
	}
	fclose(f);
}

/* Global entries first, then this tag's, so a system can override a global. */
static int coreopt_nth(const char *tag, int want, const char **out)
{
	int pass, i, seen = 0;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < ncoreopts; i++) {
			bool global = coreopts[i].tag[0] == '\0';
			if (pass == 0 ? !global : global) continue;
			if (pass == 1 && (!tag || strcmp(coreopts[i].tag, tag))) continue;
			if (seen++ == want) { if (out) *out = coreopts[i].kv; return 1; }
		}
	return 0;
}

int plat_coreopt_count(const char *tag)
{
	int n = 0;
	if (ncoreopts < 0) coreopts_load();
	while (coreopt_nth(tag, n, NULL)) n++;
	return n;
}

const char *plat_coreopt(const char *tag, int i)
{
	const char *kv = "";
	if (ncoreopts < 0) coreopts_load();
	coreopt_nth(tag, i, &kv);
	return kv;
}
const char *P_FONT = "/mnt/SDCARD/TortOS/menu.ttf";

void paths_init(void)
{
	const char *v;
	if ((v = getenv("TORTOS_ROOT"))) P_ROOT = v;
	if ((v = getenv("TORTOS_CARD"))) P_CARD = v;
	if ((v = getenv("TORTOS_ROMS"))) P_ROMS = v;
	if ((v = getenv("TORTOS_USERDATA"))) P_USERDATA = v;
	if ((v = getenv("TORTOS_SHARED"))) P_SHARED = v;
	if ((v = getenv("TORTOS_FONT"))) P_FONT = v;
}

SDL_Renderer *plat_renderer(void) { return ren; }
unsigned plat_now_ms(void) { return SDL_GetTicks(); }

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
	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(ren, &info) == 0)
		fprintf(stderr, "renderer: %s, driver: %s\n", info.name, SDL_GetCurrentVideoDriver());
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

void plat_input_quit(void)
{
	if (joy) { SDL_JoystickClose(joy); joy = NULL; }
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
	/* keep the raw descriptors; they are display-independent */
}

static void set_btn(in_state *st, in_button b, bool down)
{
	if (b == IN_NONE) return;
	if (down && !st->down[b]) {
		st->pressed[b] = true;
		st->down_since[b] = SDL_GetTicks();
		st->last_repeat[b] = 0;
	}
	st->down[b] = down;
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
			set_btn(st, b, ev.value == 1);
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

/* Set from main's SIGTERM handler. Surfaced here rather than checked at each
 * call site because `quit_requested` is already polled by every loop in the
 * program, so one assignment reaches all of them - including modal loops that
 * were written later and would otherwise have to remember. The keyboard was
 * exactly that case: it checked quit_requested and not main's want_quit, so
 * SIGTERM was ignored while it was open and `make adb-restart` silently did
 * nothing until the process was killed with -9. */
static volatile sig_atomic_t g_terminating;

void plat_terminate(void) { g_terminating = 1; }

void plat_input_poll(in_state *st)
{
	memset(st->pressed, 0, sizeof st->pressed);
	if (g_terminating) st->quit_requested = true;
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
			set_btn(st, map_joy_button(e.jbutton.button),
			        e.type == SDL_JOYBUTTONDOWN);
			break;
		case SDL_JOYHATMOTION: {
			Uint8 v = e.jhat.value;
			set_btn(st, IN_LEFT,  (v & SDL_HAT_LEFT)  != 0);
			set_btn(st, IN_RIGHT, (v & SDL_HAT_RIGHT) != 0);
			set_btn(st, IN_UP,    (v & SDL_HAT_UP)    != 0);
			set_btn(st, IN_DOWN,  (v & SDL_HAT_DOWN)  != 0);
			break;
		}
		case SDL_JOYDEVICEADDED:
			if (!joy) open_joystick();
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			if (e.key.repeat) break;
			set_btn(st, map_key(e.key.keysym.sym), e.type == SDL_KEYDOWN);
			if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q)
				st->quit_requested = true;
			break;
		}
	}
	poll_raw_fd(fd_power, st);
	poll_raw_fd(fd_keys, st);
	poll_raw_fd(fd_joy, st);
}

bool in_repeat(in_state *st, in_button b)
{
	if (st->pressed[b]) return true;
	if (!st->down[b]) return false;
	Uint32 now = SDL_GetTicks();
	if (now - st->down_since[b] < REPEAT_DELAY_MS) return false;
	if (now - st->last_repeat[b] < REPEAT_RATE_MS) return false;
	st->last_repeat[b] = now;
	return true;
}

/* Whether the last game ended because the player hit power rather than because
 * they quit. The launcher owns the shutdown sequence, so it has to be able to
 * tell the two apart when control comes back. */
static bool run_power_pressed;

bool plat_run_power_pressed(void) { return run_power_pressed; }

int plat_run(char *const argv[], const char *const envkv[], const char *workdir)
{
	pid_t pid;
	run_power_pressed = false;
	pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		if (workdir) {
			if (chdir(workdir) != 0) { /* still try to run */ }
		}
		for (int i = 0; envkv && envkv[i]; i++)
			putenv((char *)envkv[i]);
		execv(argv[0], argv);
		fprintf(stderr, "execv %s: failed\n", argv[0]);
		_exit(127);
	}
	int status = 0;
#ifdef __linux__
	/* Escape hatch: a core that dead-ends (a bad ROM, a missing BIOS) leaves
	 * the child showing an error screen that eats no input. Watch the power
	 * button while waiting and end the child on a press. */
	struct input_event ev;
	while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
		; /* drain stale events */
	int ms_since_term = -1;
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) break;
		if (r < 0) return -1;
		usleep(100 * 1000);
		while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev) {
			if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1 &&
			    ms_since_term < 0) {
				run_power_pressed = true;
				kill(pid, SIGTERM);
				ms_since_term = 0;
			}
		}
		if (ms_since_term >= 0) {
			ms_since_term += 100;
			if (ms_since_term > 3000) {
				kill(pid, SIGKILL);
				ms_since_term = -1;
			}
		}
	}
#else
	if (waitpid(pid, &status, 0) < 0) return -1;
#endif
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Start a child that outlives this process. Double-forked, so the middle
 * child is reaped here and the grandchild is inherited by init rather than
 * becoming a zombie nobody waits for. */
bool plat_spawn_detached(char *const argv[], const char *const envkv[],
                         const char *workdir)
{
	pid_t pid = fork();
	if (pid < 0) return false;
	if (pid == 0) {
		if (fork() == 0) {
			setsid();
			if (workdir) { if (chdir(workdir) != 0) { /* still try */ } }
			for (int i = 0; envkv && envkv[i]; i++)
				putenv((char *)envkv[i]);
			execv(argv[0], argv);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
	return true;
}


/* ---------------- the Diatom transport ---------------------------------- */

/* One connection, held across games. Diatom is fine with that - it is the
 * fifo pair that needed reopening per game - and holding it means peer death
 * is a POLLHUP here rather than a pid file that lies after a crash. */
static int    dsock = -1;
static char   dbuf[4096];
static size_t dused;
static char   d_preview[1024];
static SDL_Rect d_rect;
static bool     d_rect_known;
static int    d_pend_vol = -1, d_pend_vol_n;      /* LEVEL events, held until */
static int    d_pend_bri = -1, d_pend_bri_n;      /* EXIT hands levels back  */

const char *plat_resident_socket(void)
{
	const char *v = getenv("TORTOS_DIATOM_SOCKET");
	return v ? v : "/tmp/diatom.sock";
}

static void dclose(void)
{
	if (dsock >= 0) close(dsock);
	dsock = -1;
	dused = 0;
}

static bool dsend(const char *fmt, ...)
{
	char line[2048];
	va_list ap;
	int n;

	if (dsock < 0) return false;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	if (n < 0) return false;
	if (n >= (int)sizeof line - 1) n = (int)sizeof line - 2;
	if (n == 0 || line[n - 1] != '\n') line[n++] = '\n';
	if (write(dsock, line, (size_t)n) != n) { dclose(); return false; }
	return true;
}

/* One line, or NULL after timeout_ms with nothing complete. -1 blocks. */
static char *dline(int timeout_ms)
{
	static char out[2048];

	for (;;) {
		char *nl = memchr(dbuf, '\n', dused);
		struct pollfd p = { dsock, POLLIN, 0 };
		ssize_t n;

		if (nl) {
			size_t len = (size_t)(nl - dbuf);
			if (len >= sizeof out) len = sizeof out - 1;
			memcpy(out, dbuf, len);
			out[len] = '\0';
			memmove(dbuf, nl + 1, dused - (size_t)(nl - dbuf) - 1);
			dused -= (size_t)(nl - dbuf) + 1;
			return out;
		}
		if (dsock < 0) return NULL;
		if (poll(&p, 1, timeout_ms) <= 0) return NULL;
		n = read(dsock, dbuf + dused, sizeof dbuf - dused);
		if (n <= 0) { dclose(); return NULL; }
		dused += (size_t)n;
	}
}

/* launch.sh writes the pid when it starts Diatom. Verified against
 * /proc/<pid>/cmdline before any signal is sent: a stale file after a crash
 * and respawn would otherwise aim SIGTERM at whoever inherited the number. */
static pid_t diatom_pid(void)
{
	char path[64], cmd[256];
	FILE *f = fopen("/tmp/diatom.pid", "r");
	long v = 0;
	int fd, n;

	if (!f) return -1;
	if (fscanf(f, "%ld", &v) != 1) v = 0;
	fclose(f);
	if (v <= 0) return -1;

	snprintf(path, sizeof path, "/proc/%ld/cmdline", v);
	fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	n = (int)read(fd, cmd, sizeof cmd - 1);
	close(fd);
	if (n <= 0) return -1;
	cmd[n] = '\0';
	return strstr(cmd, "diatom") ? (pid_t)v : -1;
}

/* Connect if not connected, and cope with what READY says. state=running
 * means a previous launcher died mid-game and this one just started: the
 * game on screen is real, but this launcher believes it owns the display, so
 * end the session and start clean rather than draw over live output. */
static bool dconnect(void)
{
	struct sockaddr_un a;
	const char *path = plat_resident_socket();
	char *l;

	if (dsock >= 0) return true;
	if (!path) return false;

	dsock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (dsock < 0) return false;
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
	if (connect(dsock, (struct sockaddr *)&a, sizeof a) != 0) { dclose(); return false; }

	while ((l = dline(400))) {
		if (strncmp(l, "READY", 5) == 0) {
			if (strstr(l, "state=running")) {
				unsigned t0 = plat_now_ms();
				fprintf(stderr, "diatom: had a game running; stopping it\n");
				dsend("STOP");
				while ((l = dline(500)) || plat_now_ms() - t0 < 4000)
					if (l && strncmp(l, "EXIT", 4) == 0) break;
			}
			return true;
		}
	}
	/* No READY inside the window: not our protocol on the other end. */
	dclose();
	return false;
}

bool plat_resident_ready(void)
{
	return dconnect();
}



/* Ask for a game. Returns immediately -- the caller draws its launch
 * animation while the game loads, which is most of what a launch costs. */
static void (*d_on_unlock)(int id);

void plat_resident_on_unlock(void (*fn)(int id)) { d_on_unlock = fn; }

static void (*d_on_tick)(void);

void plat_resident_on_tick(void (*fn)(void)) { d_on_tick = fn; }

bool plat_resident_send(const char *tag, const char *core, const char *rom,
                        const char *resume, const char *exit_state,
                        const char *preview,
                        int console, const char *cheevos)
{
	run_power_pressed = false;

	{
		char *l;
		int vol, bri;

		if (!dconnect()) return false;
		while ((l = dline(0))) { }                 /* drop stale events */
		d_preview[0] = '\0';
		d_rect_known = false;
		d_pend_vol = d_pend_bri = -1;

		/* Before RUN, not after. mgba_sgb_borders and mgba_use_bios are both
		 * marked (Restart) by the core, meaning they are read during
		 * retro_load_game; a SETOPT that arrives after the game is up applies
		 * to the NEXT launch and looks like it did nothing. Diatom holds a
		 * value for a key the active core has not declared and applies it when
		 * one does, so sending the whole list every time is correct. */
		{
			int i;
			for (i = 0; i < plat_coreopt_count(tag); i++) {
				const char *kv = plat_coreopt(tag, i), *eq = strchr(kv, '=');
				if (!eq) continue;
				dsend("SETOPT\tkey=%.*s\tvalue=%s",
				      (int)(eq - kv), kv, eq + 1);
			}
		}

		/* console and cheevos on RUN rather than after it, so a set is
		 * watched from the first frame - an achievement can fire in the
		 * opening seconds and Diatom cannot evaluate what it has not been
		 * given yet. Both are ignored by an older Diatom, which is what
		 * ADR-0009 promises about unknown keys. */
		if (!dsend("RUN\tcore=%s\trom=%s\ttag=%s"
		           "\tresume=%s\texit_state=%s\tpreview=%s"
		           "\tconsole=%d\tcheevos=%s",
		           core, rom, tag,
		           resume ? resume : "", exit_state ? exit_state : "",
		           preview ? preview : "",
		           console, cheevos ? cheevos : ""))
			return false;

		/* The launcher owns levels while it draws (Diatom's ADR-0020), and
		 * it is done drawing the moment the game is up - so the last thing
		 * it does is hand over WHERE THE LEVELS ARE. Without this, a game
		 * starts at whatever the mixer was left at rather than at the
		 * volume the shelf shows.
		 *
		 * `count` is POSITIONS, not the top index - Diatom answers with
		 * BRIGHT_LEVELS + 1 and rescales by it. Brightness said 11 for a
		 * twelve-rung ladder, so every level handed to a game arrived one rung
		 * too bright, and the inbound rescale then took a rung off on the way
		 * back. The two canceled often enough to look like nothing was wrong.
		 * Both are written off the shared maxima now. */
		vol = plat_volume_get();
		bri = plat_brightness_get();
		if (vol >= 0) dsend("SETLEVEL\tkind=volume\tindex=%d\tcount=%d",
		                    vol, PLAT_VOL_MAX + 1);
		if (bri >= 0) dsend("SETLEVEL\tkind=brightness\tindex=%d\tcount=%d",
		                    bri, PLAT_BRIGHT_MAX + 1);
		return true;
	}

}

bool plat_resident_line(const char *fmt, ...)
{
	char line[1600];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	return dsend("%s", line);
}

const char *plat_resident_last_preview(void) { return d_preview; }

/* Block until the game is over, watching the power button throughout -- the
 * same job plat_run does for the one-shot binary, except the signal ends the
 * GAME rather than the process, so residency survives to serve the next one. */
/* One LEVEL line: "LEVEL\tkind=volume\tindex=8\tcount=21". The values are
 * held rather than applied - Diatom owns the hardware while the game runs
 * (its ADR-0020), and applying over the top is exactly the fight the state
 * plane exists to end. They are applied when EXIT hands ownership back. */
static void d_note_level(const char *l)
{
	const char *k = strstr(l, "kind=");
	const char *i = strstr(l, "index=");
	const char *c = strstr(l, "count=");
	int idx, cnt;

	if (!k || !i || !c) return;
	idx = atoi(i + 6);
	cnt = atoi(c + 6);
	if (cnt < 2) return;
	if (strncmp(k + 5, "volume", 6) == 0)          { d_pend_vol = idx; d_pend_vol_n = cnt; }
	else if (strncmp(k + 5, "brightness", 10) == 0) { d_pend_bri = idx; d_pend_bri_n = cnt; }
}

/* "CHEEVO\tid=24698\tstate=unlocked". Diatom sends this the moment a
 * condition fires, which is mid-game, when the emulator owns the display and
 * this process cannot draw a thing. So it is recorded and shown at the next
 * moment the launcher owns the screen: the in-game menu, or the shelf. */
static void d_note_cheevo(const char *l)
{
	const char *i = strstr(l, "id=");
	const char *st = strstr(l, "state=");

	if (!i || !st) return;
	if (strncmp(st + 6, "unlocked", 8) != 0) return;
	if (d_on_unlock) d_on_unlock(atoi(i + 3));
}

/* "DISPLAY\tmode=native\tfilter=nearest\trect=256x224+384+272" */
static void d_note_display(const char *l)
{
	const char *r = strstr(l, "rect=");
	int w, h, x, y;

	if (!r || sscanf(r + 5, "%dx%d+%d+%d", &w, &h, &x, &y) != 4) return;
	if (w <= 0 || h <= 0) return;
	d_rect.w = w; d_rect.h = h; d_rect.x = x; d_rect.y = y;
	d_rect_known = true;
}

bool plat_resident_rect(SDL_Rect *out)
{
	if (!d_rect_known) return false;
	*out = d_rect;
	return true;
}

/* Only safe while Diatom is paused, which is the only place it is called from:
 * a game that is running can send EXIT, and this would eat it. Paused, the only
 * traffic is what our own SETDISPLAY provoked. */
bool plat_resident_sync_rect(int timeout_ms)
{
	unsigned t0 = SDL_GetTicks();
	char *l;

	if (dsock < 0) return false;
	while ((int)(SDL_GetTicks() - t0) < timeout_ms) {
		l = dline(timeout_ms);
		if (!l) break;
		if (strncmp(l, "DISPLAY\t", 8) == 0) { d_note_display(l); return true; }
		if (strncmp(l, "LEVEL\t", 6) == 0) d_note_level(l);
	}
	return false;
}

static void d_apply_levels(void)
{
	/* Through the public setters (defined below, past this point in the
	 * file): they own the device handles. Both sides use the same ladders, so
	 * `count` matches and the rescale is the identity - but it is written as a
	 * rescale anyway, because a launcher should not break if the emulator ever
	 * changes its scale.
	 *
	 * The rescale has to target THIS side's top of range. It read 10 while the
	 * brightness ladder had grown to twelve rungs, which is not the identity:
	 * a level of 7 came back as 6, and every brightness set inside a game lost
	 * a rung on the way out. That is why the maxima are in platform.h now. */
	if (d_pend_vol >= 0 && d_pend_vol_n > 1)
		plat_volume_set_pct((d_pend_vol * 100 + (d_pend_vol_n - 1) / 2)
		                    / (d_pend_vol_n - 1));
	if (d_pend_bri >= 0 && d_pend_bri_n > 1)
		plat_brightness_set((d_pend_bri * PLAT_BRIGHT_MAX + (d_pend_bri_n - 1) / 2)
		                    / (d_pend_bri_n - 1));
	d_pend_vol = d_pend_bri = -1;
}

static int diatom_wait(void)
{
	int got_running = 0, sent_stop = 0;
	unsigned stop_at = 0, start = plat_now_ms();
	int autostop_s = getenv("TORTOS_AUTOSTOP_S") ? atoi(getenv("TORTOS_AUTOSTOP_S")) : 0;

#ifdef __linux__
	{
		struct input_event ev;
		while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
			; /* drain stale presses */
	}
#endif

	for (;;) {
		char *l;

		if (dsock < 0) return RES_DEAD;
		while ((l = dline(100))) {
			if      (strncmp(l, "RUNNING", 7) == 0) got_running = 1;
			else if (strncmp(l, "PAUSED", 6) == 0)  return RES_PAUSED;
			else if (strncmp(l, "PREVIEW\tpath=", 13) == 0)
				snprintf(d_preview, sizeof d_preview, "%s", l + 13);
			else if (strncmp(l, "LEVEL\t", 6) == 0) d_note_level(l);
			else if (strncmp(l, "DISPLAY\t", 8) == 0) d_note_display(l);
			else if (strncmp(l, "CHEEVO\t", 7) == 0) d_note_cheevo(l);
			else if (strncmp(l, "EXIT", 4) == 0) { d_apply_levels(); return RES_EXIT; }
			else if (strncmp(l, "ERROR", 5) == 0) {
				fprintf(stderr, "diatom: %s\n", l);
				/* Before RUNNING it means the game never started and the
				 * display is still ours. After, it is a failed menu op the
				 * menu already showed; the game is still going. */
				if (!got_running) return RES_DEAD;
			}
		}
		if (dsock < 0) return RES_DEAD;            /* EOF mid-game */

		/* The launcher's own slice of the game session. Bounded work only -
		 * see plat_resident_on_tick. */
		if (d_on_tick) d_on_tick();

#ifdef __linux__
		{
			struct input_event ev;
			while (fd_power >= 0 &&
			       read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev) {
				if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1 &&
				    !sent_stop) {
					run_power_pressed = true;
					sent_stop = 1;
					stop_at = plat_now_ms();
					dsend("STOP");
				}
			}
		}
#endif
		if (autostop_s > 0 && !sent_stop &&
		    plat_now_ms() - start > (unsigned)autostop_s * 1000u) {
			sent_stop = 1;
			stop_at = plat_now_ms();
			dsend("STOP");
		}

		/* STOP is a request; a core wedged inside retro_run cannot honor
		 * it. Escalate on the clock: SIGTERM still flushes saves, SIGKILL
		 * is the end of the line and reports the emulator dead. */
		if (sent_stop && stop_at) {
			unsigned waited = plat_now_ms() - stop_at;
			pid_t pid = diatom_pid();
			if (waited > 5000) {
				if (pid > 0) kill(pid, SIGKILL);
				dclose();
				return RES_DEAD;
			}
			if (waited > 2500 && pid > 0) kill(pid, SIGTERM);
		}
	}
}

int plat_resident_wait(void)
{
	return diatom_wait();
}

void plat_request_poweroff(void)
{
	FILE *f = fopen(TORTOS_POWEROFF_FLAG, "w");
	if (f) fclose(f);
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
 * positions. Detail and method in VOLUME-CURVE.md.
 *
 * This is the SPEAKER floor. The headphone amp is a separate control
 * ("Headphone Volume", 0-7 at 6 dB) which apply_volume pins to zero, so it is
 * not calibrated and headphones will be near-silent until it is. */
#define GAIN_RAW_USABLE 26
#define SPEAKER_CTL  "HpSpeaker Switch"   /* the only true mute on this codec */
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

static char levels_file[512];

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

static void levels_save(void)
{
	FILE *f;

	if (!levels_file[0]) return;
	f = fopen(levels_file, "w");
	if (!f) return;
	fprintf(f, "volume=%d\nbrightness=%d\n", cur_vol, cur_bright);
	fclose(f);
}

static void apply_volume(int v)
{
	long raw = ((long)(VOL_MAX - v) * GAIN_RAW_USABLE + VOL_MAX / 2) / VOL_MAX;
	long on;

	cur_vol = v;
	ctl_io("Headphone", &(long){ 0 }, 1);        /* never the jack */
	ctl_io(GAIN_CTL, &raw, 1);
	/* Zero has to cut the path, not merely attenuate it. */
	on = (v > 0);
	ctl_io(SPEAKER_CTL, &on, 1);
}

static void apply_brightness(int b)
{
	unsigned long a[4] = { 0, 0, 0, 0 };

	cur_bright = b;
	if (disp_fd < 0) return;
	a[1] = bright_ladder[b];
	ioctl(disp_fd, DISP_LCD_SET_BRIGHTNESS, a);
}

void plat_settings_init(int cfg_volume_pct, int cfg_brightness)
{
	FILE *f;
	int v = -1, b = -1;

	mixer_fd = open("/dev/snd/controlC0", O_RDWR);
	disp_fd  = open("/dev/disp", O_RDWR);
	if (mixer_fd < 0) fprintf(stderr, "settings: no /dev/snd/controlC0\n");
	if (disp_fd  < 0) fprintf(stderr, "settings: no /dev/disp\n");

	/* The player's last choice, and it wins: a level the player set with the
	 * rocker survives a restart, which is the whole reason every nudge writes
	 * this file. All or nothing on purpose - a half-written file describes no
	 * level anyone chose, so it falls through to the defaults below rather
	 * than pairing one saved level with one config default. */
	snprintf(levels_file, sizeof levels_file, "%s/levels.cfg", P_USERDATA);
	f = fopen(levels_file, "r");
	if (f) {
		if (fscanf(f, "volume=%d brightness=%d", &v, &b) != 2) v = b = -1;
		fclose(f);
	}

	/* Then tortos.cfg, which is a default for a device that has never had a
	 * level set on it - not an instruction to be obeyed at every boot. It used
	 * to be applied over the top of the above by main(), which put the config
	 * ahead of the player and disagreed with launch.sh into the bargain. */
	if (v < 0 && cfg_volume_pct >= 0)
		v = (cfg_volume_pct * VOL_MAX + 50) / 100;
	if (b < 0 && cfg_brightness >= 0)
		b = cfg_brightness;

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

/* No levels.cfg to read on the host, so the config defaults are all there is.
 * Taken anyway rather than ignored: a shelf rendered by --shot should show the
 * OSD at the levels the card is configured for. */
void plat_settings_init(int cfg_volume_pct, int cfg_brightness)
{
	if (cfg_volume_pct >= 0)
		cur_vol = clampi((cfg_volume_pct * VOL_MAX + 50) / 100, 0, VOL_MAX);
	if (cfg_brightness >= 0)
		cur_bright = clampi(cfg_brightness, 0, BRIGHT_MAX);
}

#endif  /* __linux__ */


/* The settings indicator: a thin line across the very top on any volume or
 * brightness change, tinted by which of the two it is -- warm for brightness,
 * cyan for volume. Diatom draws its own thin bar in game for the same reason,
 * so the feedback is one thing everywhere instead of a launcher line here and
 * another firmware's pill there; the tint is the part Diatom has to be taught
 * to match. Still no glyph and no number: the color and the button you just
 * pressed agree, and neither needs a label. */
#define OSD_LINE_H     UI_BAR_H
#define OSD_PAD        3    /* half-black scrim above and below */
#define OSD_WINDOW_MS  900  /* visible this long after the last change */

static int    osd_kind = 0; /* 0 none, 1 brightness, 2 volume */
static int    osd_val = 0, osd_max = 1;
static Uint32 osd_shown_at = 0;

void plat_osd_show(int kind, int val, int max)
{
	osd_kind = kind;
	osd_max = max > 0 ? max : 1;
	osd_val = val < 0 ? 0 : (val > osd_max ? osd_max : val);
	osd_shown_at = SDL_GetTicks();
}

int plat_volume_get(void)     { return cur_vol; }
int plat_brightness_get(void) { return cur_bright; }

void plat_draw_osd(SDL_Renderer *r)
{
	if (!osd_kind) return;
	if (SDL_GetTicks() - osd_shown_at > OSD_WINDOW_MS) { osd_kind = 0; return; }

	float pct = (float)osd_val / osd_max;
	if (pct < 0) pct = 0; else if (pct > 1) pct = 1;

	SDL_Color fill = osd_kind == 1 ? UI_OSD_BRIGHT : UI_OSD_VOLUME;
	int W = TORTOS_SCREEN_W;
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 0, 0, 0, 128);                 /* scrim */
	SDL_RenderFillRect(r, &(SDL_Rect){ 0, 0, W, OSD_LINE_H + OSD_PAD * 2 });
	SDL_SetRenderDrawColor(r, 60, 62, 72, 255);              /* faint track */
	SDL_RenderFillRect(r, &(SDL_Rect){ 0, OSD_PAD, W, OSD_LINE_H });
	SDL_SetRenderDrawColor(r, fill.r, fill.g, fill.b, 255);
	SDL_RenderFillRect(r, &(SDL_Rect){ 0, OSD_PAD, (int)(W * pct), OSD_LINE_H });
}

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
