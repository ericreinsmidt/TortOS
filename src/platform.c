/* SPDX-License-Identifier: 0BSD */
#include "platform.h"

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
 * (317/318). SDL maps them to joystick buttons 9 and 10, which minarch calls
 * L3/R3 -- which is why the in-game input map has to drop L3/R3, or every
 * brightness press also reaches the core. */
#define CODE_FN_LEFT  317
#define CODE_FN_RIGHT 318

/* SDL joystick button indices on the Brick's "TRIMUI Player1" device
 * (BTN_SOUTH..BTN_THUMBR enumerate to 0..10; the d-pad is hat 0). */
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

/* PLAYOS_INPUT_DEBUG=1 logs raw evdev codes and SDL button indices, so one
 * press tells you exactly which device a control arrives on. */
static int dbg_input;

const char *P_ROOT = "/mnt/SDCARD/PlayOS";
const char *P_CARD = "/mnt/SDCARD";
const char *P_ROMS = "/mnt/SDCARD/Roms";
const char *P_USERDATA = "/mnt/SDCARD/.userdata/tg5040";
const char *P_SHARED = "/mnt/SDCARD/.userdata/shared";
const char *P_FONT = "/mnt/SDCARD/PlayOS/menu.ttf";

void paths_init(void)
{
	const char *v;
	if ((v = getenv("PLAYOS_ROOT"))) P_ROOT = v;
	if ((v = getenv("PLAYOS_CARD"))) P_CARD = v;
	if ((v = getenv("PLAYOS_ROMS"))) P_ROMS = v;
	if ((v = getenv("PLAYOS_USERDATA"))) P_USERDATA = v;
	if ((v = getenv("PLAYOS_SHARED"))) P_SHARED = v;
	if ((v = getenv("PLAYOS_FONT"))) P_FONT = v;
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
	win = SDL_CreateWindow("PlayOS", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                       PLAYOS_SCREEN_W, PLAYOS_SCREEN_H,
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
	dbg_input = getenv("PLAYOS_INPUT_DEBUG") != NULL || access(marker, F_OK) == 0;
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

void plat_input_poll(in_state *st)
{
	memset(st->pressed, 0, sizeof st->pressed);
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

/* ---- the resident emulator ----------------------------------------------
 * A resident minarch holds the GL context and all three cores between games,
 * which takes a launch from ~1100ms to ~200ms. The launcher talks to it over
 * two fifos rather than forking it per game.
 *
 * Both are opened O_RDWR and held: a fifo opened for reading alone blocks
 * until a writer appears and returns EOF when the last one leaves, which
 * turns a request/reply into a race that deadlocks whichever side opens
 * first. Holding a write descriptor of our own removes both behaviours. */
#define PLAYOS_PID "/tmp/playos_res.pid"
#define PLAYOS_REQ "/tmp/playos_req"
#define PLAYOS_REP "/tmp/playos_rep"

/* The resident process writes its pid here at startup; it is how the power
 * button reaches the running game. Only the device build has a power button
 * to watch, which is why this is behind the same guard as its caller. */
#ifdef __linux__
static pid_t resident_pid(void)
{
	FILE *f = fopen(PLAYOS_PID, "r");
	long v = 0;
	if (!f) return -1;
	if (fscanf(f, "%ld", &v) != 1) v = 0;
	fclose(f);
	return v > 0 ? (pid_t)v : -1;
}
#endif

/* ---------------- the Diatom transport ---------------------------------- */

/* One connection, held across games. Diatom is fine with that - it is the
 * fifo pair that needed reopening per game - and holding it means peer death
 * is a POLLHUP here rather than a pid file that lies after a crash. */
static int    dsock = -1;
static char   dbuf[4096];
static size_t dused;
static char   d_preview[1024];
static int    d_pend_vol = -1, d_pend_vol_n;      /* LEVEL events, held until */
static int    d_pend_bri = -1, d_pend_bri_n;      /* EXIT hands levels back  */

static const char *diatom_socket_path(void) { return getenv("PLAYOS_DIATOM_SOCKET"); }

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
	const char *path = diatom_socket_path();
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

/* ---------------- the fifo transport (minarch) --------------------------- */

/* Is there a resident emulator to talk to?
 *
 * The fifos alone do not answer that. They are only unlinked on a clean exit,
 * so a resident that crashed leaves both nodes behind and a check that just
 * stats them says yes forever -- which is worse than saying no, because the
 * launcher would then never start a replacement and would spend the rest of
 * the session on the slow path. Ask whether the process it wrote down is
 * still alive. */
bool plat_resident_ready(void)
{
	struct stat st;
	pid_t pid;

	if (diatom_socket_path()) return dconnect();

	if (stat(PLAYOS_REQ, &st) != 0 || !S_ISFIFO(st.st_mode)) return false;
	if (stat(PLAYOS_REP, &st) != 0 || !S_ISFIFO(st.st_mode)) return false;
#ifdef __linux__
	pid = resident_pid();
	if (pid <= 0) return false;
	if (kill(pid, 0) != 0) return false;
#else
	(void)pid;
#endif
	return true;
}

static int res_fd_rep = -1;

/* Ask for a game. Returns immediately -- the caller draws its launch
 * animation while the game loads, which is most of what a launch costs. */
bool plat_resident_send(const char *tag, const char *core, const char *rom,
                        const char *resume, const char *exit_state,
                        const char *preview)
{
	int fd_req;
	char c;

	run_power_pressed = false;

	if (diatom_socket_path()) {
		char *l;
		int vol, bri;

		if (!dconnect()) return false;
		while ((l = dline(0))) { }                 /* drop stale events */
		d_preview[0] = '\0';
		d_pend_vol = d_pend_bri = -1;

		if (!dsend("RUN\tcore=%s\trom=%s\ttag=%s"
		           "\tresume=%s\texit_state=%s\tpreview=%s",
		           core, rom, tag,
		           resume ? resume : "", exit_state ? exit_state : "",
		           preview ? preview : ""))
			return false;

		/* The launcher owns levels while it draws (Diatom's ADR-0020), and
		 * it is done drawing the moment the game is up - so the last thing
		 * it does is hand over WHERE THE LEVELS ARE. Without this, a game
		 * starts at whatever the mixer was left at rather than at the
		 * volume the shelf shows. */
		vol = plat_volume_get();
		bri = plat_brightness_get();
		if (vol >= 0) dsend("SETLEVEL\tkind=volume\tindex=%d\tcount=21", vol);
		if (bri >= 0) dsend("SETLEVEL\tkind=brightness\tindex=%d\tcount=11", bri);
		return true;
	}

	if (res_fd_rep >= 0) { close(res_fd_rep); res_fd_rep = -1; }

	fd_req = open(PLAYOS_REQ, O_WRONLY | O_NONBLOCK);
	if (fd_req < 0) return false;              /* nobody listening */
	res_fd_rep = open(PLAYOS_REP, O_RDWR | O_NONBLOCK);
	if (res_fd_rep < 0) { close(fd_req); return false; }

	/* Drop anything left over from a previous game before asking for one. */
	while (read(res_fd_rep, &c, 1) == 1) { }

	{
		char req[2048];
		snprintf(req, sizeof req, "%s\t%s\t%s\n", tag, core, rom);
		if (write(fd_req, req, strlen(req)) < 0) {
			close(fd_req); close(res_fd_rep); res_fd_rep = -1;
			return false;
		}
	}
	close(fd_req);
	return true;
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
 * plane exists to end. They land in libmsettings when EXIT hands them back. */
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

static void d_apply_levels(void)
{
	/* Through the public setters (defined below, past this point in the
	 * file): they own the libmsettings handles, and the rescale to their
	 * percent / 0..10 scales keeps endpoints exact - the same
	 * round-to-nearest rule the protocol pins. */
	if (d_pend_vol >= 0 && d_pend_vol_n > 1)
		plat_volume_set_pct((d_pend_vol * 100 + (d_pend_vol_n - 1) / 2)
		                    / (d_pend_vol_n - 1));
	if (d_pend_bri >= 0 && d_pend_bri_n > 1)
		plat_brightness_set((d_pend_bri * 10 + (d_pend_bri_n - 1) / 2)
		                    / (d_pend_bri_n - 1));
	d_pend_vol = d_pend_bri = -1;
}

static int diatom_wait(void)
{
	int got_running = 0, sent_stop = 0;
	unsigned stop_at = 0, start = plat_now_ms();
	int autostop_s = getenv("PLAYOS_AUTOSTOP_S") ? atoi(getenv("PLAYOS_AUTOSTOP_S")) : 0;

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

		/* STOP is a request; a core wedged inside retro_run cannot honour
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
	int fd_rep = res_fd_rep;
	bool ok = false;
	char c;

	if (diatom_socket_path()) return diatom_wait();

	if (fd_rep < 0) return RES_DEAD;

#ifdef __linux__
	{
		struct input_event ev;
		int sent_stop = 0;
		int no_pid_ms = 0;
		pid_t res = resident_pid();

		while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
			; /* drain stale presses */

		for (;;) {
			if (read(fd_rep, &c, 1) == 1) { ok = true; break; }
			usleep(20 * 1000);

			/* Without a pid there is no way to end the game and no way to
			 * notice the emulator dying, so this loop would sit on a black
			 * screen with a dead power button until the battery went. Keep
			 * looking for it, and give up rather than hang. */
			if (res <= 0) {
				res = resident_pid();
				no_pid_ms += 20;
				if (res <= 0 && no_pid_ms > 2000) {
					fprintf(stderr, "resident: no pid file, giving up on the wait\n");
					break;
				}
			}

			while (fd_power >= 0 &&
			       read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev) {
				if (ev.type == EV_KEY && ev.code == KEY_POWER &&
				    ev.value == 1 && !sent_stop) {
					run_power_pressed = true;
					sent_stop = 1;
					/* End the game, not the process. */
					if (res > 0) kill(res, SIGUSR1);
					else break;   /* nothing to signal -- hand control back */
				}
			}
			if (res > 0 && kill(res, 0) != 0) break;   /* it died */
			if (sent_stop && res <= 0) break;
		}
	}
#else
	(void)c;
#endif
	close(fd_rep);
	res_fd_rep = -1;
	return ok ? RES_EXIT : RES_DEAD;
}

void plat_request_poweroff(void)
{
	FILE *f = fopen(PLAYOS_POWEROFF_FLAG, "w");
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

/* ---- volume and brightness, via libmsettings (dlopen'd at runtime) ---- */

static void (*ms_init)(void);
static int (*ms_get_vol)(void);
static void (*ms_set_vol)(int);
static int (*ms_get_bright)(void);
static void (*ms_set_bright)(int);

void plat_settings_init(void)
{
	void *h = dlopen("libmsettings.so", RTLD_NOW | RTLD_GLOBAL);
	if (!h) {
		fprintf(stderr, "libmsettings: %s\n", dlerror());
		return;
	}
	ms_init = dlsym(h, "InitSettings");
	ms_get_vol = dlsym(h, "GetVolume");
	ms_set_vol = dlsym(h, "SetVolume");
	ms_get_bright = dlsym(h, "GetBrightness");
	ms_set_bright = dlsym(h, "SetBrightness");
	if (ms_init) ms_init();
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* The settings indicator: a thin near-white line across the very top on any
 * volume or brightness change. minarch draws the identical line in game (see
 * PLAYOS_drawSettingLine in the api.c override), so the feedback is one thing
 * everywhere instead of a launcher line here and another firmware's pill
 * there. No glyph and no number -- you know which button you just pressed. */
#define OSD_LINE_H     6
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

int plat_volume_get(void)     { return ms_get_vol ? ms_get_vol() : -1; }
int plat_brightness_get(void) { return ms_get_bright ? ms_get_bright() : -1; }

void plat_draw_osd(SDL_Renderer *r)
{
	if (!osd_kind) return;
	if (SDL_GetTicks() - osd_shown_at > OSD_WINDOW_MS) { osd_kind = 0; return; }

	float pct = (float)osd_val / osd_max;
	if (pct < 0) pct = 0; else if (pct > 1) pct = 1;

	int W = PLAYOS_SCREEN_W;
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 0, 0, 0, 128);                 /* scrim */
	SDL_RenderFillRect(r, &(SDL_Rect){ 0, 0, W, OSD_LINE_H + OSD_PAD * 2 });
	SDL_SetRenderDrawColor(r, 60, 62, 72, 255);              /* faint track */
	SDL_RenderFillRect(r, &(SDL_Rect){ 0, OSD_PAD, W, OSD_LINE_H });
	SDL_SetRenderDrawColor(r, 235, 235, 240, 255);           /* near-white fill */
	SDL_RenderFillRect(r, &(SDL_Rect){ 0, OSD_PAD, (int)(W * pct), OSD_LINE_H });
}

void plat_volume_nudge(int delta)
{
	if (!ms_get_vol || !ms_set_vol) return;
	int v = clampi(ms_get_vol() + delta, 0, 20);
	ms_set_vol(v);
	plat_osd_show(2, v, 20);
}

void plat_brightness_nudge(int delta)
{
	if (!ms_get_bright || !ms_set_bright) return;
	int v = clampi(ms_get_bright() + delta, 0, 10);
	ms_set_bright(v);
	plat_osd_show(1, v, 10);
}

void plat_volume_set_pct(int pct)
{
	if (!ms_set_vol) return;
	ms_set_vol(clampi((pct * 20 + 50) / 100, 0, 20));
}

void plat_brightness_set(int level)
{
	if (!ms_set_bright) return;
	ms_set_bright(clampi(level, 0, 10));
}

/* ---- battery ---- */

bool plat_battery(int *pct, bool *charging)
{
	const char *fake = getenv("PLAYOS_FAKE_BATT");
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
