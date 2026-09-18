/* SPDX-License-Identifier: MIT */
/* See musec.h. The protocol is src/muse/muse.c's. */
#include "musec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "muselib.h"
#include "platform.h"

#define MUSE_SOCK "/tmp/muse.sock"

static char    g_bin[256], g_root[256];
static int     g_fd = -1;
static char    g_in[4096];
static size_t  g_have;
static unsigned g_spawned_ms;
static int     g_spawned;

static char  **g_q;                 /* the queue, relative paths, ours */
static int     g_qn, g_qi;
static int     g_pending;           /* a PLAY waiting for the connection */
static int     g_skips;             /* consecutive tracks that would not open */
static char    g_artist[128], g_album[128];
static mu_now  g_now;

void musec_init(const char *muse_bin, const char *music_root)
{
	snprintf(g_bin, sizeof g_bin, "%s", muse_bin);
	snprintf(g_root, sizeof g_root, "%s", music_root);
}

static void drop(void)
{
	if (g_fd >= 0) close(g_fd);
	g_fd = -1;
	g_have = 0;
	if (g_now.state != MU_OFF) g_now.state = MU_OFF;
}

static void sendf(const char *fmt, ...)
{
	char line[1400];
	va_list ap;
	int n;

	if (g_fd < 0) return;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
	line[n++] = '\n';
	if (send(g_fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 &&
	    errno != EAGAIN)
		drop();                    /* it went away; the next poll starts it */
}

/* Connected, or on the way. The daemon is started at most once every two
 * seconds, so a Muse that cannot start does not become a fork a frame. */
static int connected(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	if (g_fd >= 0) return 1;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", MUSE_SOCK);
	/* Non-blocking by fcntl rather than SOCK_NONBLOCK, which is Linux's: the
	 * host build compiles this too, and a Unix-socket connect either answers
	 * at once or fails at once either way. */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
		g_fd = fd;
		g_have = 0;
		if (g_now.state == MU_OFF) g_now.state = MU_STOPPED;
		return 1;
	}
	if (fd >= 0) close(fd);
	if (!g_bin[0]) return 0;
	if (!g_spawned || plat_now_ms() - g_spawned_ms > 2000) {
		char *argv[] = { g_bin, NULL };

		plat_spawn_detached(argv, NULL, NULL);
		g_spawned = 1;
		g_spawned_ms = plat_now_ms();
		fprintf(stderr, "muse: started %s\n", g_bin);
	}
	return 0;
}

static void play_current(void)
{
	if (g_qi < 0 || g_qi >= g_qn) return;
	if (!connected()) { g_pending = 1; return; }
	g_pending = 0;
	sendf("PLAY\tpath=%s/%s", g_root, g_q[g_qi]);
	/* Until META says otherwise, the name the file has. */
	ml_track_name(strrchr(g_q[g_qi], '/') ? strrchr(g_q[g_qi], '/') + 1 : g_q[g_qi],
	              g_now.title, sizeof g_now.title);
	snprintf(g_now.artist, sizeof g_now.artist, "%s", g_artist);
	snprintf(g_now.album, sizeof g_now.album, "%s", g_album);
	g_now.at = 0;
	g_now.len = 0;
	g_now.index = g_qi;
	g_now.count = g_qn;
}

static void advance(void)
{
	if (g_qi + 1 < g_qn) { g_qi++; play_current(); }
	else g_now.state = MU_STOPPED;
}

/* `key=` in a tab-separated line, or "". */
static void field(const char *line, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	const char *p = line;

	out[0] = '\0';
	while ((p = strchr(p, '\t'))) {
		p++;
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t len = strcspn(p + kl + 1, "\t");

			if (len >= n) len = n - 1;
			memcpy(out, p + kl + 1, len);
			out[len] = '\0';
			return;
		}
	}
}

static void event(const char *line)
{
	char v[256];

	if (!strncmp(line, "READY", 5)) {
		if (g_pending) play_current();
	} else if (!strncmp(line, "STATE", 5)) {
		field(line, "state", v, sizeof v);
		g_now.state = !strcmp(v, "playing") ? MU_PLAYING
		            : !strcmp(v, "paused")  ? MU_PAUSED : MU_STOPPED;
		if (g_now.state == MU_PLAYING) g_skips = 0;
	} else if (!strncmp(line, "META", 4)) {
		/* The file's own tags win where it has them; the folder names stand
		 * in where it does not, which on this card is the artist almost
		 * every time - the rips carry an album tag and no artist one. */
		field(line, "title", v, sizeof v);
		if (v[0]) snprintf(g_now.title, sizeof g_now.title, "%s", v);
		field(line, "artist", v, sizeof v);
		if (v[0]) snprintf(g_now.artist, sizeof g_now.artist, "%s", v);
		field(line, "album", v, sizeof v);
		if (v[0]) snprintf(g_now.album, sizeof g_now.album, "%s", v);
		field(line, "len", v, sizeof v);
		g_now.len = atof(v);
	} else if (!strncmp(line, "POS", 3)) {
		field(line, "at", v, sizeof v);  g_now.at = atof(v);
		field(line, "len", v, sizeof v); if (atof(v) > 0) g_now.len = atof(v);
	} else if (!strncmp(line, "END", 3)) {
		advance();
	} else if (!strncmp(line, "ERROR", 5)) {
		field(line, "why", v, sizeof v);
		fprintf(stderr, "muse: %s\n", v);
		/* A track that will not open is skipped rather than ending the
		 * album - but not forever: a whole album of files that fail is a
		 * folder problem, and walking it at a frame a track says nothing. */
		if (++g_skips < 8) advance(); else g_now.state = MU_STOPPED;
	}
}

void musec_poll(void)
{
	ssize_t got;

	/* Reconnect whenever there is a queue to look after, not only when a
	 * play is waiting: a daemon that restarts mid-album would otherwise
	 * finish the track it was on and stop, because its END went to a socket
	 * nobody was reading. */
	if (g_fd < 0) {
		if ((!g_pending && g_qn == 0) || !connected()) return;
	}
	for (;;) {
		got = recv(g_fd, g_in + g_have, sizeof g_in - g_have - 1, MSG_DONTWAIT);
		if (got == 0) { drop(); return; }
		if (got < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) drop();
			break;
		}
		g_have += (size_t)got;
		g_in[g_have] = '\0';
		for (;;) {
			char *nl = memchr(g_in, '\n', g_have);
			size_t used;

			if (!nl) break;
			*nl = '\0';
			event(g_in);
			if (g_fd < 0) return;              /* the event dropped us */
			used = (size_t)(nl - g_in) + 1;
			memmove(g_in, nl + 1, g_have - used);
			g_have -= used;
		}
		if (g_have >= sizeof g_in - 1) g_have = 0;
	}
}

void musec_play(const char *const *paths, int n, int start,
                const char *artist, const char *album)
{
	int i, k = 0;

	for (i = 0; i < g_qn; i++) free(g_q[i]);
	free(g_q);
	g_q = calloc((size_t)(n > 0 ? n : 1), sizeof *g_q);
	g_qn = g_qi = 0;
	if (!g_q) return;
	for (i = 0; i < n; i++) {
		/* A tab or a newline is structure on the wire; a name holding one
		 * would arrive as a different path. Nobody names a song that way,
		 * and the one that did is left out rather than misplayed. */
		if (strpbrk(paths[i], "\t\n")) { if (i < start) start--; continue; }
		g_q[k] = strdup(paths[i]);
		if (g_q[k]) k++;
	}
	g_qn = k;
	g_qi = start >= 0 && start < k ? start : 0;
	g_skips = 0;
	snprintf(g_artist, sizeof g_artist, "%s", artist ? artist : "");
	snprintf(g_album, sizeof g_album, "%s", album ? album : "");
	play_current();
}

void musec_toggle(void)
{
	if (g_now.state == MU_PLAYING) sendf("PAUSE");
	else if (g_now.state == MU_PAUSED) sendf("RESUME");
	else if (g_qn > 0) play_current();
}

void musec_next(void)
{
	if (g_qi + 1 < g_qn) { g_qi++; play_current(); }
}

void musec_prev(void)
{
	/* The way every player does it: three seconds in, "back" means the start
	 * of this track; before that it means the one before. */
	if (g_now.at > 3.0 || g_qi == 0) sendf("SEEK\tat=0");
	else { g_qi--; play_current(); }
}

void musec_seek_by(double delta)
{
	double t = g_now.at + delta;

	if (g_now.state != MU_PLAYING && g_now.state != MU_PAUSED) return;
	if (t < 0) t = 0;
	if (g_now.len > 0 && t > g_now.len - 1) t = g_now.len - 1;
	g_now.at = t;                  /* shown at once, confirmed by the next POS */
	sendf("SEEK\tat=%.1f", t);
}

void musec_stop(void)
{
	sendf("STOP");
}

const mu_now *musec_now(void) { return &g_now; }

const char *musec_path(void)
{
	if (g_now.state == MU_OFF || g_now.state == MU_STOPPED) return "";
	return g_qi >= 0 && g_qi < g_qn ? g_q[g_qi] : "";
}

bool musec_playing(void) { return g_now.state == MU_PLAYING; }
