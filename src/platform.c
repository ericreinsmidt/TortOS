/* SPDX-License-Identifier: MIT */
/* The portable half of the platform layer: what every device shares. The
 * settings the library database holds for games, the input helpers, running
 * and spawning children, the power-off request and the volume and brightness
 * line. The device itself is in src/device/<name>.c, chosen at build time,
 * and the Diatom transport is in resident.c. */
#include "db.h"
#include "device.h"
#include "platform.h"

#include "ui.h"   /* the settings line shares the rail's weight and palette */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/rtc.h>
#endif

/* Hold-to-repeat. Short delay and a quick rate: this is a shelf you sweep
 * along, and waiting on a key repeat is the most obvious kind of slow. */
#define REPEAT_DELAY_MS 300
#define REPEAT_RATE_MS  90

/* ---- core options, read once from the library database ------------------- */
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

static bool coreopt_row(const char *key, const char *value, void *ctx)
{
	const char *rest = key + strlen("coreopt.");
	const char *dot = strchr(rest, '.');
	char kv[192];

	(void)ctx;
	if (!dot || ncoreopts >= COREOPT_MAX) return ncoreopts < COREOPT_MAX;

	/* Refused, not stored short. Half a key=value pair is still a
	 * valid-looking one, and it would be sent to a core as though someone
	 * meant it. */
	if ((size_t)(dot - rest) >= sizeof coreopts[0].tag ||
	    snprintf(kv, sizeof kv, "%s=%s", dot + 1, value) >= (int)sizeof kv) {
		fprintf(stderr, "coreopts: entry too long, ignoring: %.40s\n", key);
		return true;
	}
	snprintf(coreopts[ncoreopts].tag, sizeof coreopts[0].tag, "%.*s",
	         (int)(dot - rest), rest);
	snprintf(coreopts[ncoreopts].kv, sizeof coreopts[0].kv, "%s", kv);
	ncoreopts++;
	return true;
}

/* Keys are "coreopt.<tag>.<option>", with an empty tag for a global - so
 * "coreopt..mgba_sgb_borders" is global and "coreopt.GB.mgba_gb_model" is not.
 * The empty segment looks odd and is the point: one prefix scan brings back
 * both kinds, and coreopt_nth below still decides precedence. */
static void coreopts_load(void)
{
	ncoreopts = 0;
	db_each_prefix(db_lib(), "coreopt.", coreopt_row, NULL);
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

/* ---- turbo, read once from the library database -------------------------- */
/* One canonical map per system tag, handed to Diatom after RUN. Its ADR-0028
 * makes a pulse a property of a BINDING, so `x:a~3,y:b~3` is the whole feature:
 * X becomes a turbo A and Y a turbo B, three frames pressed and three released.
 *
 * Per system because it is only safe where those two buttons are SPARE. Seven of
 * the eleven consoles here have two face buttons; a Genesis 6-button pad and a
 * SNES pad use X and Y for real, and turbo would take them away.
 *
 * A file rather than a table in the binary because the rate is exactly the sort
 * of thing a player wants to change, and because a remap screen would one day
 * write this same field over the same protocol. */
#define TURBO_MAX 16
static struct { char tag[8]; char map[96]; } turbos[TURBO_MAX];
static int nturbos = -1;                     /* -1 = not read yet */

static bool turbo_row(const char *key, const char *value, void *ctx)
{
	const char *tag = key + strlen("turbo.");

	(void)ctx;
	if (nturbos >= TURBO_MAX) return false;
	/* Refused, not stored short. Half a map is a map nobody wrote, and Diatom
	 * rejects a bad one whole - after the game is already up, where the
	 * refusal is invisible. */
	if (strlen(tag) >= sizeof turbos[0].tag ||
	    strlen(value) >= sizeof turbos[0].map) {
		fprintf(stderr, "turbo: entry too long, ignoring: %.32s\n", tag);
		return true;
	}
	snprintf(turbos[nturbos].tag, sizeof turbos[0].tag, "%s", tag);
	snprintf(turbos[nturbos].map, sizeof turbos[0].map, "%s", value);
	nturbos++;
	return true;
}

static void turbos_load(void)
{
	nturbos = 0;
	db_each_prefix(db_lib(), "turbo.", turbo_row, NULL);
}

/* The map for this system, or NULL for one that wants none. */
const char *plat_turbo_map(const char *tag)
{
	int i;

	if (nturbos < 0) turbos_load();
	if (!tag) return NULL;
	for (i = 0; i < nturbos; i++)
		if (!strcmp(turbos[i].tag, tag)) return turbos[i].map;
	return NULL;
}

void paths_init(void)
{
	const char *v;
	if ((v = getenv("TORTOS_ROOT"))) P_ROOT = v;
	if ((v = getenv("TORTOS_CARD"))) P_CARD = v;
	if ((v = getenv("TORTOS_ROMS"))) P_ROMS = v;
	if ((v = getenv("TORTOS_USERDATA"))) P_USERDATA = v;
	if ((v = getenv("TORTOS_SHARED"))) P_SHARED = v;
	if ((v = getenv("TORTOS_WEB")))    P_WEB = v;
	if ((v = getenv("TORTOS_FONT"))) P_FONT = v;
}

unsigned plat_now_ms(void) { return SDL_GetTicks(); }

void plat_set_btn(in_state *st, in_button b, bool down)
{
	if (b == IN_NONE) return;
	if (down && !st->down[b]) {
		st->pressed[b] = true;
		st->down_since[b] = SDL_GetTicks();
		st->last_repeat[b] = 0;
	}
	st->down[b] = down;
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
bool plat_terminating(void) { return g_terminating != 0; }

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
void plat_note_power_pressed(void) { run_power_pressed = true; }
void plat_set_power_pressed(bool on) { run_power_pressed = on; }

extern char **environ;

/* The environment a child starts with: this process's, with `envkv` laid over
 * it - each "KEY=VALUE" replacing any KEY already there. Built BEFORE the fork
 * and handed to execve, so between fork and exec the child calls nothing but
 * chdir, setsid, execve, write and _exit.
 *
 * Both forks below used to putenv each pair in the child, and one reported a
 * failed exec with fprintf. Neither is async-signal-safe, which is all POSIX
 * promises a child of a threaded process may call before it execs - and this
 * process has threads, the art resizer among them writing to stderr. Whether
 * glibc's own fork makes those two safe anyway was never checked; building the
 * environment first removes the question.
 *
 * The strings are borrowed, not copied: environ's and the caller's both outlive
 * the exec that uses them. Only the array is allocated, and the parent frees it
 * once the fork is done. */
static char **child_environ(const char *const envkv[])
{
	size_t have = 0, add = 0, i, k, n = 0;
	char **out;

	while (environ && environ[have]) have++;
	while (envkv && envkv[add]) add++;
	if (!(out = malloc((have + add + 1) * sizeof *out))) return NULL;
	for (i = 0; i < have; i++) {
		const char *eq = strchr(environ[i], '=');
		size_t klen = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
		bool replaced = false;

		for (k = 0; k < add && !replaced; k++)
			replaced = !strncmp(envkv[k], environ[i], klen) && envkv[k][klen] == '=';
		if (!replaced) out[n++] = environ[i];
	}
	for (k = 0; k < add; k++)
		if (strchr(envkv[k], '=')) out[n++] = (char *)envkv[k];
	out[n] = NULL;
	return out;
}

int plat_run(char *const argv[], const char *const envkv[], const char *workdir)
{
	pid_t pid;
	char **env = child_environ(envkv);
	size_t alen = strlen(argv[0]);

	if (!env) return -1;
	run_power_pressed = false;
	pid = fork();
	if (pid < 0) { free(env); return -1; }
	if (pid == 0) {
		ssize_t w;

		if (workdir) {
			if (chdir(workdir) != 0) { /* still try to run */ }
		}
		execve(argv[0], argv, env);
		/* write, not fprintf - see child_environ. */
		w = write(2, "execve ", 7);
		w = write(2, argv[0], alen);
		w = write(2, ": failed\n", 9);
		(void)w;
		_exit(127);
	}
	free(env);
	int status = 0;
#ifdef __linux__
	/* Escape hatch: a core that dead-ends (a bad ROM, a missing BIOS) leaves
	 * the child showing an error screen that eats no input. Watch the power
	 * button while waiting and end the child on a press. */
	device_power_pressed();          /* drain stale events */
	int ms_since_term = -1;
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) break;
		if (r < 0) return -1;
		usleep(100 * 1000);
		if (device_power_pressed() && ms_since_term < 0) {
			run_power_pressed = true;
			kill(pid, SIGTERM);
			ms_since_term = 0;
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
	char **env = child_environ(envkv);
	struct rlimit rl;
	int top = 1024, fd;
	pid_t pid;

	/* EVERYTHING ABOVE STDERR IS CLOSED IN THE CHILD, before it becomes the
	 * program asked for.
	 *
	 * Fork hands a child every descriptor this process has open: the resident
	 * emulator's socket, Muse's, the display, the GPU, four input devices,
	 * the font. A short-lived child drops them when it exits. A DAEMON keeps
	 * them for as long as it lives, and Muse is meant to outlive the launcher.
	 * That froze the Brick on 2026-09-18: Muse held a dead launcher's end of
	 * Diatom's socket, so Diatom never saw it close, and the restarted
	 * launcher blocked in connect before its first frame. Diatom's half is
	 * its ADR-0033; this is ours.
	 *
	 * By number up to the limit, not by listing /proc/self/fd: listing
	 * allocates, and after fork in a threaded process only async-signal-safe
	 * calls are safe - close is one. The limit is read here, before fork, for
	 * the same reason. 1024 on the Brick, with about thirty open. stdout and
	 * stderr stay: they are the log, and a daemon's lines belong in it. */
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
		top = rl.rlim_cur < 65536 ? (int)rl.rlim_cur : 65536;

	if (!env) return false;
	pid = fork();
	if (pid < 0) { free(env); return false; }
	if (pid == 0) {
		if (fork() == 0) {
			setsid();
			for (fd = 3; fd < top; fd++) close(fd);
			if (workdir) { if (chdir(workdir) != 0) { /* still try */ } }
			execve(argv[0], argv, env);
			_exit(127);
		}
		_exit(0);
	}
	free(env);
	waitpid(pid, NULL, 0);
	return true;
}

void plat_request_poweroff(void)
{
	FILE *f = fopen(TORTOS_POWEROFF_FLAG, "w");
	if (f) fclose(f);
}

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

/* One past the last tick plat_draw_osd still draws it on, since that test is
 * `>`: asking at exactly the window's end would still find it showing. */
Uint32 plat_osd_until(void)
{
	return osd_kind ? osd_shown_at + OSD_WINDOW_MS + 1 : UINT32_MAX;
}


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

/* ---- Date & Time ---------------------------------------------------------- */

bool plat_clock_set(time_t t)
{
	struct timeval tv = { .tv_sec = t, .tv_usec = 0 };

	if (settimeofday(&tv, NULL) != 0) {
		fprintf(stderr, "clock: settimeofday: %s\n", strerror(errno));
		return false;
	}
#ifdef __linux__
	/* The hardware clock keeps UTC, as the kernel reads it back at boot
	 * (CONFIG_RTC_HCTOSYS). Without this the time set here would last until
	 * the next power-off - on the Pixel 2, back to whatever the clock held
	 * before, 2018 on one never set. */
	{
		struct tm tm;
		struct rtc_time rt;
		int fd = open("/dev/rtc0", O_WRONLY | O_CLOEXEC);

		if (fd < 0) {
			fprintf(stderr, "clock: no hardware clock: %s\n", strerror(errno));
			return true;
		}
		gmtime_r(&t, &tm);
		memset(&rt, 0, sizeof rt);
		rt.tm_year = tm.tm_year; rt.tm_mon = tm.tm_mon; rt.tm_mday = tm.tm_mday;
		rt.tm_hour = tm.tm_hour; rt.tm_min = tm.tm_min; rt.tm_sec = tm.tm_sec;
		if (ioctl(fd, RTC_SET_TIME, &rt) != 0)
			fprintf(stderr, "clock: hardware clock: %s\n", strerror(errno));
		close(fd);
	}
#endif
	return true;
}

void plat_clock_zone(const char *id)
{
	if (!id || !id[0]) return;
	setenv("TZ", id, 1);
	tzset();
}
