/* SPDX-License-Identifier: MIT */
/* The Diatom transport: the launcher's side of the socket to the resident
 * emulator. Nothing in it knows which device it runs on. */
#include "device.h"
#include "platform.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

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
/* Diatom's QUIET, its ADR-0032: what the launcher wants, and what this
 * connection was last told. -1 is "never told", which a new connection is. */
static int    d_quiet, d_quiet_said = -1;

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
	/* send and MSG_NOSIGNAL, not write. Nothing in this process ignores
	 * SIGPIPE, so a write into a connection Diatom had closed - it restarted,
	 * or a newer client displaced this one (its ADR-0033) - killed the
	 * launcher outright, and launch.sh had to bring it back. A closed socket
	 * is an answer here, not a crash: the next call connects again. */
	if (send(dsock, line, (size_t)n, MSG_NOSIGNAL) != n) { dclose(); return false; }
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

static char d_audio_dev[128];
static bool d_audio_known;

/* Bumped on every successful connect. State the launcher pushed into a PREVIOUS
 * Diatom has to be pushed again into a new one, and this is how a caller tells
 * "still the same emulator" from "a different one that knows nothing". */
static unsigned d_generation;

unsigned plat_resident_generation(void) { return d_generation; }

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

	/* A new socket may be a new Diatom, which starts on its default output.
	 * Forgetting what the old one reported is what stops the launcher from
	 * believing a sink that this process was never told about. The generation
	 * bump is how a caller notices the same thing without polling for it. */
	d_audio_known = false;
	d_audio_dev[0] = '\0';
	d_quiet_said = -1;
	d_generation++;

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



static void (*d_on_unlock)(int id);

void plat_resident_on_unlock(void (*fn)(int id)) { d_on_unlock = fn; }

static void (*d_on_speed)(int x);

void plat_resident_on_speed(void (*fn)(int x)) { d_on_speed = fn; }

static void (*d_on_tick)(void);

void plat_resident_on_tick(void (*fn)(void)) { d_on_tick = fn; }

/* Has THIS GAME reported RUNNING yet?
 *
 * A session-long fact, so it cannot live in diatom_wait: the launcher calls
 * that in a loop, returning to it after every in-game menu, and a local reset
 * to 0 on each re-entry made a running game look like one that never started.
 * The cost was not cosmetic - see the ERROR arm in diatom_wait. */
static int d_got_running;

bool plat_resident_send(const char *tag, const char *core, const char *rom,
                        const char *resume, const char *exit_state,
                        const char *preview,
                        int console, const char *cheevos)
{
	plat_set_power_pressed(false);
	/* A new game has not reported RUNNING yet. Cleared HERE rather than in
	 * diatom_wait, because the launcher re-enters that after every in-game
	 * menu and the answer must survive those. */
	d_got_running = 0;

	{
		char *l;
		int vol, bri;

		if (!dconnect()) return false;
		while ((l = dline(0))) { }                 /* drop stale events */
		/* The drain is also what finds a connection Diatom closed under us:
		 * it reads the EOF and closes our end. Connect again rather than
		 * send a game into nothing - which failed the first launch after
		 * Diatom restarted, or after something displaced this connection. */
		if (!dconnect()) return false;
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

		/* Quiet BEFORE RUN, and on every RUN. Diatom holds it across RUN
		 * (its ADR-0032), so sent first it is in force from the game's first
		 * sample - a game started while music plays never makes a sound -
		 * and sent every time, a resident that restarted cannot start loud. */
		if (dsend("SETQUIET\ton=%d", d_quiet)) d_quiet_said = d_quiet;

		/* console and cheevos on RUN rather than after it, so a set is
		 * watched from the first frame - an achievement can fire in the
		 * opening seconds and Diatom cannot evaluate what it has not been
		 * given yet. Both are ignored by an older Diatom, which is what
		 * ADR-0009 promises about unknown keys. */
		/* The display goes to Diatom before the game does: it presents its
		 * first frame as soon as the load is done, and cannot while this
		 * process holds it. */
		device_display_release();
		if (!dsend("RUN\tcore=%s\trom=%s\ttag=%s"
		           "\tresume=%s\texit_state=%s\tpreview=%s"
		           "\tconsole=%d\tcheevos=%s",
		           core, rom, tag,
		           resume ? resume : "", exit_state ? exit_state : "",
		           preview ? preview : "",
		           console, cheevos ? cheevos : "")) {
			device_display_take();      /* no game is coming */
			return false;
		}

		/* AFTER RUN, never before: RUN resets the map to identity (Diatom's
		 * ADR-0020, so a table sent for one game cannot silently govern the
		 * next), and a map sent first would be discarded by the very launch it
		 * was meant for. The race is benign - no input reaches a core before
		 * RUNNING, which Diatom emits once the load completes.
		 *
		 * Logged either way. A map that does nothing looks identical to one
		 * that was never sent, one that was refused, and a test rig that could
		 * not press the button - which cost an hour on 2026-08-31. Diatom logs
		 * the receiving half for the same reason. */
		{
			const char *tm = plat_turbo_map(tag);
			fprintf(stderr, "turbo: %s %s\n", tag ? tag : "?",
			        tm && *tm ? tm : "(none)");
			if (tm && *tm) dsend("SETMAP\tmap=%s", tm);
		}

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
		/* And the mute, ON EVERY RUN and not only when it changes. A state
		 * sent only on transition is wrong exactly once - the first time -
		 * which here means a game started with the switch already down comes
		 * up loud. Diatom's ADR-0031. */
		dsend("SETMUTE\ton=%d", plat_muted() ? 1 : 0);
		return true;
	}

}

void plat_resident_quiet(bool on)
{
	d_quiet = on ? 1 : 0;
	/* Only over a connection that exists. Asking to connect for this would
	 * start nothing useful - with no connection there is no game to quiet,
	 * and the next RUN sends it anyway. */
	if (dsock >= 0 && d_quiet != d_quiet_said && dsend("SETQUIET\ton=%d", d_quiet))
		d_quiet_said = d_quiet;
}

bool plat_resident_line(const char *fmt, ...)
{
	char line[1600];
	va_list ap;

	/* Connect if we are not already. dsend refuses on a closed socket, and
	 * before the first game of a session there IS no socket - the connection
	 * was only ever made by plat_resident_send on the way into a game.
	 *
	 * That made every state write from the shelf a silent no-op. ADR-0029's
	 * audio output is the first state the launcher sets while nothing is
	 * running, and it spent 2026-09-05 announcing routes in the log that
	 * Diatom had never been told about, because the line went nowhere and
	 * nobody looked at the return.
	 *
	 * Cheap when it fails: connecting to a unix socket that is not there
	 * returns immediately. */
	if (!dconnect()) return false;

	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	/* RESUME hands the display back to the game, which presents the moment it
	 * reads the line: let go first, the same as before RUN. */
	if (!strncmp(line, "RESUME", 6)) device_display_release();
	if (dsend("%s", line)) return true;
	/* Closed under us - Diatom restarted, or a newer client displaced this
	 * connection (its ADR-0033). dsend has closed our end, so this is a fresh
	 * connection, and nothing of the line reached the old one to repeat. */
	if (!dconnect()) return false;
	return dsend("%s", line);
}

const char *plat_resident_last_preview(void) { return d_preview; }

/* Block until the game is over, watching the power button throughout -- the
 * same job plat_run does for the one-shot binary, except the signal ends the
 * GAME rather than the process, so residency survives to serve the next one. */
/* One LEVEL line: "LEVEL\tkind=volume\tindex=8\tcount=21". The values are
 * held rather than applied - Diatom owns the hardware while the game runs
 * (its ADR-0020), and applying over the top is exactly the fight the state
 * plane exists to end. They are applied when EXIT or PAUSED hands ownership
 * back. */
/* Where Diatom says the sound ACTUALLY is (its ADR-0029). Not necessarily
 * where it was told to put it: a sink that will not open, or one that died
 * under it, makes the port fall back and say so. Reported to the launcher so a
 * menu can show the truth instead of the request. */
static void d_note_audio(const char *l)
{
	const char *d = strstr(l, "device=");

	if (!d) return;
	snprintf(d_audio_dev, sizeof d_audio_dev, "%s", d + 7);
	d_audio_dev[strcspn(d_audio_dev, "\r\n")] = '\0';
	d_audio_known = true;
}

int plat_resident_volume(int *count)
{
	if (count) *count = d_pend_vol_n;
	return d_pend_vol_n > 1 ? d_pend_vol : -1;
}

void plat_resident_audio_asked(void) { d_audio_known = false; }

bool plat_resident_audio(char *out, size_t cap)
{
	if (!d_audio_known) return false;
	if (out && cap) snprintf(out, cap, "%s", d_audio_dev);
	return true;
}

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
		else if (strncmp(l, "AUDIO\t", 6) == 0) d_note_audio(l);
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
	int sent_stop = 0;
	unsigned stop_at = 0, start = plat_now_ms();
	int autostop_s = getenv("TORTOS_AUTOSTOP_S") ? atoi(getenv("TORTOS_AUTOSTOP_S")) : 0;

	device_power_pressed();          /* drain stale presses */

	for (;;) {
		char *l;

		if (dsock < 0) return RES_DEAD;
		/* THE MUTE SWITCH IS READ DURING A GAME TOO, and this is the half
		 * that matters: a player reaching for it is almost always reaching
		 * for it because a game is loud. plat_input_poll is not called from
		 * here, so without this the switch does nothing for as long as
		 * anything is playing.
		 *
		 * This loop turns every 100ms - dline's timeout - which is the whole
		 * reason the launcher can own the switch at all. An earlier design
		 * assumed this side was blocked and handed the job to the emulator.
		 *
		 * The cut reaches the game's audio because HpSpeaker Switch sits
		 * below the mixer: measured 2026-09-16 by muting a tone played from a
		 * second process while Diatom held the dmix slave. See BACKLOG 28 and
		 * Diatom's ADR-0031. */
		plat_mute_poll(false);
		while ((l = dline(100))) {
			if      (strncmp(l, "RUNNING", 7) == 0) d_got_running = 1;
			/* The menu is the launcher's, and so are the levels while it is
			 * up: taken at PAUSED as at EXIT. Taken only at EXIT, a press in
			 * the menu stepped from the level before the game - seen on the
			 * GKD Pixel 2 2026-10-02, raised to 18 in a game, and the menu's
			 * first press went to 3. */
			else if (strncmp(l, "PAUSED", 6) == 0)  { d_apply_levels(); return RES_PAUSED; }
			else if (strncmp(l, "PREVIEW\tpath=", 13) == 0)
				snprintf(d_preview, sizeof d_preview, "%s", l + 13);
			else if (strncmp(l, "LEVEL\t", 6) == 0) d_note_level(l);
			else if (strncmp(l, "AUDIO\t", 6) == 0) d_note_audio(l);
			else if (strncmp(l, "DISPLAY\t", 8) == 0) d_note_display(l);
			else if (strncmp(l, "CHEEVO\t", 7) == 0) d_note_cheevo(l);
			else if (strncmp(l, "SPEED\tx=", 8) == 0) {
				if (d_on_speed) d_on_speed(atoi(l + 8));
			}
			/* Nobody has pressed anything for as long as the player asked.
			 * Handled exactly as a power press: STOP the game, and the
			 * launcher's existing after-the-game check powers the device
			 * down. No new path, and the autosave happens either way. */
			else if (strncmp(l, "IDLE", 4) == 0 && !sent_stop) {
				/* Said out loud: a shutdown from here left nothing in the
				 * log to tell it from a power press, which is how the one
				 * Diatom sent seven frames into a game went unexplained. */
				fprintf(stderr, "idle: the emulator reports nobody there, "
				                "powering off\n");
				plat_set_power_pressed(true);
				sent_stop = 1;
				stop_at = plat_now_ms();
				dsend("STOP");
			}
			else if (strncmp(l, "EXIT", 4) == 0) {
				/* A crash and a quit arrive on the SAME line, and only the
				 * reason tells them apart. This threw the line away and
				 * returned, so a game that died of SIGSEGV was logged as
				 * "exit: back in 50 ms" - identical to backing out of a
				 * game normally, and the only hint that anything was wrong
				 * came later, from the resident being gone.
				 *
				 * Diatom has always sent it: on_crash emits
				 * "EXIT reason=crash signal=SIGSEGV" from the signal
				 * handler, and ADR-0009 makes `signal=` free to add because
				 * unknown keys are ignored. It was reported and discarded,
				 * which is worse than never having been sent - it looks
				 * like a firmware with nothing to say about the failure.
				 *
				 * Only the ones that are not a clean quit. Logging every
				 * EXIT would put a line in the log for every game anyone
				 * ever finishes, and a log that says something on every
				 * exit says nothing about the interesting ones. */
				if (strncmp(l, "EXIT\treason=user", 16) != 0)
					fprintf(stderr, "diatom: %s\n", l);
				d_apply_levels();
				return RES_EXIT;
			}
			else if (strncmp(l, "ERROR", 5) == 0) {
				fprintf(stderr, "diatom: %s\n", l);
				/* Before RUNNING it means the game never started and the
				 * display is still ours. After, it is a failed menu op the
				 * menu already showed; the game is still going.
				 *
				 * Getting this wrong is expensive, which is why the flag is
				 * no longer a local. Reported 2026-09-06: Load -> Auto asked
				 * for a state that could not exist, the ERROR arrived on a
				 * re-entry where the local had reset, and a failed menu
				 * operation was read as a dead emulator. The launcher then
				 * started a SECOND emulator while the first still held the
				 * framebuffer - two presenters, which wedges the display
				 * engine in-kernel, and the supervisor powered the device
				 * off. */
				if (!d_got_running) return RES_DEAD;
			}
		}
		if (dsock < 0) return RES_DEAD;            /* EOF mid-game */

		/* The launcher's own slice of the game session. Bounded work only -
		 * see plat_resident_on_tick. */
		if (d_on_tick) d_on_tick();

		if (device_power_pressed() && !sent_stop) {
			plat_set_power_pressed(true);
			sent_stop = 1;
			stop_at = plat_now_ms();
			dsend("STOP");
		}
		/* A termination arriving mid-game. Without this the loop waits for a
		 * game that nobody is going to end, and the process cannot be
		 * signaled out of it - which is how `killall tortos.elf` came to do
		 * nothing at all while a game was up. STOP, then the existing
		 * escalation below applies if the core will not honor it. */
		if (plat_terminating() && !sent_stop) {
			sent_stop = 1;
			stop_at = plat_now_ms();
			dsend("STOP");
		}

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
	int r;

	/* Here too, for a RESUME: the in-game menu took the display back, and
	 * the game is about to present again. Releasing twice is harmless. */
	device_display_release();
	r = diatom_wait();
	/* Back from Diatom, by whatever route - PAUSED and EXIT come after it has
	 * let go, and a dead emulator holds nothing. */
	device_display_take();

	/* Input ownership just came back to this process, so anything remembered
	 * about the jack was formed while something else was driving.
	 *
	 * A cable that went in or out DURING the game was seen by Diatom and not
	 * by this side, which leaves jack_was describing a world that is over. The
	 * poll then compares the hardware against that memory, finds them equal,
	 * and returns without re-mapping - so a headphone-window value stays in
	 * the register with the jack out. Measured 2026-09-05: 29, against a
	 * speaker window whose quiet end is 39, which is a working speaker at
	 * almost no volume.
	 *
	 * It is here rather than at the call site because forgetting it is not
	 * optional and a caller cannot be relied on to remember. Diatom does the
	 * same on its side, in diatom_port_level_invalidate. The device file
	 * forgets the mute switch's position here too, for the same reason. */
	device_levels_forget();
	return r;
}

/* One line on the connection that is already open, connecting for nothing.
 * For the device file, which tells a running game about the mute switch the
 * moment it moves, and has no business starting a connection to do it. */
bool plat_resident_tell(const char *fmt, ...)
{
	char line[1600];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	return dsend("%s", line);
}
