/* SPDX-License-Identifier: MIT */
/* Muse: TortOS's audio player, the engine half.
 *
 * A daemon over a Unix socket - the Diatom relationship again, and for the
 * same reason: an engine that does one thing, with the policy in the launcher.
 * This half decodes, plays, seeks and says where it is. It knows files and
 * transport and nothing else: not what an album is, not what a book is, not
 * what should play next. Those are tortos.elf's, because that is where the
 * screen and the frame loop are.
 *
 * Separate from the launcher so that music survives the launcher restarting,
 * which is the one argument for a second process that did not go stale when
 * the launcher grew worker threads (BACKLOG, the Muse scoping).
 *
 * THE PROTOCOL, one line each way, tab-separated key=value after a verb:
 *
 *   in   PLAY    path=  [at=]  [speed=]   open and play, from `at` seconds
 *        PAUSE | RESUME | STOP
 *        SEEK    at=
 *        SPEED   x=                          0.5-2.0, pitch preserved
 *        SINK    device=                     an ALSA name; "default" is dmix
 *        STATUS
 *   out  READY   proto=1
 *        STATE   state=playing|paused|stopped  path=
 *        META    title= artist= album= len= chapters=
 *        CHAPTER i= at= title=                 one per chapter, after META
 *        POS     at= len=                      about once a second while playing
 *        END     path=                         the file ran out by itself
 *        ERROR   why=
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "dec.h"
#include "pcm.h"

#define SOCK_PATH "/tmp/muse.sock"
#define PATH_MAX_ 1024
#define CHUNK     1024                   /* frames per write: ~21 ms */

typedef enum { ST_STOPPED, ST_PLAYING, ST_PAUSED } st;
typedef enum { C_NONE, C_PLAY, C_PAUSE, C_RESUME, C_STOP, C_SEEK, C_SPEED,
               C_SINK } cmd;

/* Everything the two threads share, under one lock. The player thread owns
 * the decoder and the output; the main thread owns the socket. Neither
 * touches the other's. */
static struct {
	pthread_mutex_t mu;
	pthread_cond_t  cv;

	st     state;
	char   path[PATH_MAX_];
	double heard, len, speed;
	/* What the file says about itself, copied out when it opens so the
	 * socket thread never reaches into the decoder the player thread is
	 * using. 256 chapters is more than any book this card will carry. */
	char   title[256], artist[256], album[256];
	int    nch;
	double ch_at[256];
	char   ch_title[256][96];
	int    ev_meta, ev_end, ev_state;   /* for the main thread to announce */
	char   ev_err[160];
} S = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/* Commands waiting for the player thread, in order.
 *
 * A QUEUE, and not the single slot this started as. The slot let a second
 * command replace the first before the player had taken it, which was meant
 * for the case where it is right - two seeks, only the last matters - and was
 * wrong for every other pair: PLAY then SPEED sent together lost the PLAY, and
 * the book never started. Found by doing exactly that on 2026-09-18.
 *
 * So repeats of the SAME verb still merge, where the newest is the only one
 * that means anything - SEEK, SPEED, SINK - and everything else waits its turn. */
#define QCAP 16
typedef struct { cmd c; char path[PATH_MAX_]; char dev[128]; double at, speed; } qcmd;
static qcmd Q[QCAP];
static int  q_head, q_len;               /* under S.mu */

static void q_push(const qcmd *in)
{
	int last = (q_head + q_len - 1) % QCAP;

	if (q_len > 0 && Q[last].c == in->c &&
	    (in->c == C_SEEK || in->c == C_SPEED || in->c == C_SINK)) {
		Q[last] = *in;
		return;
	}
	if (q_len == QCAP) { q_head = (q_head + 1) % QCAP; q_len--; }  /* drop the oldest */
	Q[(q_head + q_len) % QCAP] = *in;
	q_len++;
}

static int   g_wake[2] = { -1, -1 };   /* player -> main: something to say */
static dec  *g_dec;                    /* player thread only */
static char  g_dev[128] = "default";   /* player thread only */
static volatile sig_atomic_t g_quit;

static void wake(void)
{
	char b = 1;
	if (write(g_wake[1], &b, 1) < 0) { /* full is fine: a wake is pending */ }
}

static void say(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "muse: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---- the player thread -------------------------------------------------- */

static void fail(const char *why)
{
	snprintf(S.ev_err, sizeof S.ev_err, "%s", why);
	S.state = ST_STOPPED;
	S.ev_state = 1;
	wake();
}

/* Open S.path at `at`, replacing whatever is open. Called with the lock held;
 * opening is a few milliseconds and nothing else is waiting on it. */
static int reopen(double at)
{
	char err[160];

	dec_close(g_dec);
	g_dec = dec_open(S.path, at, S.speed, err, sizeof err);
	if (!g_dec) { fail(err); return 0; }
	S.len = dec_len(g_dec);
	S.heard = at;
	return 1;
}

static void snapshot(void)
{
	int i;

	snprintf(S.title, sizeof S.title, "%s", dec_tag(g_dec, "title"));
	snprintf(S.artist, sizeof S.artist, "%s", dec_tag(g_dec, "artist"));
	snprintf(S.album, sizeof S.album, "%s", dec_tag(g_dec, "album"));
	S.nch = dec_chapters(g_dec);
	if (S.nch > 256) S.nch = 256;
	for (i = 0; i < S.nch; i++) {
		S.ch_at[i] = dec_chapter_at(g_dec, i);
		snprintf(S.ch_title[i], sizeof S.ch_title[i], "%s",
		         dec_chapter_title(g_dec, i));
	}
}

static void handle(const qcmd *q)
{
	switch (q->c) {
	case C_NONE:
		break;
	case C_PLAY:
		snprintf(S.path, sizeof S.path, "%s", q->path);
		/* A PLAY with no speed means normal speed. It used to mean "whatever
		 * the last one was", and speed is a property of a book, not of the
		 * daemon: a 1.5x set for one audiobook played every song after it
		 * rushed. Found on the device 2026-09-18, the first time the launcher
		 * rather than a test drove it. */
		S.speed = q->speed > 0 ? q->speed : 1.0;
		pcm_drop();
		if (reopen(q->at)) {
			snapshot();
			S.state = ST_PLAYING;
			S.ev_meta = S.ev_state = 1;
			wake();
		}
		break;
	case C_PAUSE:
		if (S.state != ST_PLAYING) break;
		/* What has been HEARD, not what the decoder reached: the queue is
		 * thrown away, so resuming from the decoder's position would skip
		 * the 200 ms that never played - a word, in an audiobook. */
		pcm_drop();
		S.state = ST_PAUSED;
		S.ev_state = 1;
		wake();
		break;
	case C_RESUME:
		if (S.state != ST_PAUSED) break;
		if (reopen(S.heard)) { S.state = ST_PLAYING; S.ev_state = 1; wake(); }
		break;
	case C_SEEK:
		if (S.state == ST_STOPPED) break;
		pcm_drop();
		if (!reopen(q->at)) break;
		wake();
		break;
	case C_SPEED:
		if (q->speed < 0.5 || q->speed > 2.0) break;
		S.speed = q->speed;
		if (S.state == ST_STOPPED) break;
		pcm_drop();
		reopen(S.heard);
		break;
	case C_SINK:
		snprintf(g_dev, sizeof g_dev, "%s", q->dev);
		if (!pcm_open(g_dev)) {
			say("%s; falling back to default", pcm_error());
			snprintf(g_dev, sizeof g_dev, "default");
			pcm_open(g_dev);
		}
		if (S.state == ST_PLAYING) reopen(S.heard);
		break;
	case C_STOP:
		pcm_drop();
		dec_close(g_dec);
		g_dec = NULL;
		S.state = ST_STOPPED;
		S.ev_state = 1;
		wake();
		break;
	}
}

static void *player(void *arg)
{
	static int16_t buf[CHUNK * DEC_CHANNELS];

	(void)arg;
	if (!pcm_open(g_dev)) say("%s", pcm_error());
	pthread_mutex_lock(&S.mu);
	while (!g_quit) {
		int n;

		while (q_len == 0 && S.state != ST_PLAYING && !g_quit)
			pthread_cond_wait(&S.cv, &S.mu);
		if (q_len > 0) {
			qcmd q = Q[q_head];

			q_head = (q_head + 1) % QCAP;
			q_len--;
			handle(&q);
			continue;
		}
		if (S.state != ST_PLAYING || !g_dec) continue;

		/* Decode and write WITHOUT the lock: the write blocks for as long as
		 * the device takes to drain, and a command arriving meanwhile must
		 * not wait behind it. */
		pthread_mutex_unlock(&S.mu);
		n = dec_read(g_dec, buf, CHUNK);
		if (n > 0 && !pcm_write(buf, n)) n = -1;
		pthread_mutex_lock(&S.mu);

		if (n > 0) {
			double h = dec_pos(g_dec) - (double)pcm_queued() * S.speed / DEC_RATE;

			S.heard = h > 0 ? h : 0;
			continue;
		}
		if (n == 0) {
			/* The end of the file. What is queued plays out on its own; the
			 * launcher hears END and decides what comes next. */
			dec_close(g_dec);
			g_dec = NULL;
			S.state = ST_STOPPED;
			S.ev_end = S.ev_state = 1;
			wake();
			continue;
		}
		fail(pcm_error()[0] ? pcm_error() : "the file stopped decoding");
		dec_close(g_dec);
		g_dec = NULL;
	}
	pthread_mutex_unlock(&S.mu);
	pcm_close();
	return NULL;
}

/* ---- the socket ---------------------------------------------------------- */

/* A value from `line`, or "" - `key=` up to the next tab. */
static void arg(const char *line, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	const char *p = line;

	out[0] = '\0';
	while ((p = strchr(p, '\t'))) {
		p++;
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t len = strcspn(p + kl + 1, "\t\r\n");

			if (len >= n) len = n - 1;
			memcpy(out, p + kl + 1, len);
			out[len] = '\0';
			return;
		}
	}
}

/* A value going OUT, with the two characters the protocol uses as structure
 * taken out of it. A tag is somebody else's text. */
static const char *clean(const char *s, char *buf, size_t n)
{
	size_t i;

	for (i = 0; s[i] && i + 1 < n; i++)
		buf[i] = (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') ? ' ' : s[i];
	buf[i] = '\0';
	return buf;
}

static void send_line(int fd, const char *fmt, ...)
{
	char line[2048];
	va_list ap;
	int n;

	if (fd < 0) return;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
	line[n++] = '\n';
	if (send(fd, line, (size_t)n, MSG_NOSIGNAL) < 0) { /* the client went */ }
}

static const char *st_name(st s)
{
	return s == ST_PLAYING ? "playing" : s == ST_PAUSED ? "paused" : "stopped";
}

static void announce(int fd, int all)
{
	char a[PATH_MAX_], b[256], c[256];

	pthread_mutex_lock(&S.mu);
	if (S.ev_meta || (all && S.state != ST_STOPPED)) {
		int i;

		send_line(fd, "META\ttitle=%s\tartist=%s\talbum=%s\tlen=%.1f\tchapters=%d",
		          clean(S.title, a, sizeof a), clean(S.artist, b, sizeof b),
		          clean(S.album, c, sizeof c), S.len, S.nch);
		for (i = 0; i < S.nch; i++)
			send_line(fd, "CHAPTER\ti=%d\tat=%.1f\ttitle=%s", i, S.ch_at[i],
			          clean(S.ch_title[i], a, sizeof a));
	}
	if (S.ev_err[0]) {
		send_line(fd, "ERROR\twhy=%s", clean(S.ev_err, a, sizeof a));
		say("error: %s", S.ev_err);
	}
	if (S.ev_end) send_line(fd, "END\tpath=%s", clean(S.path, a, sizeof a));
	if (S.ev_state || all)
		send_line(fd, "STATE\tstate=%s\tpath=%s", st_name(S.state),
		          clean(S.path, a, sizeof a));
	if (all || S.state == ST_PLAYING)
		send_line(fd, "POS\tat=%.1f\tlen=%.1f", S.heard, S.len);
	S.ev_meta = S.ev_end = S.ev_state = 0;
	S.ev_err[0] = '\0';
	pthread_mutex_unlock(&S.mu);
}

static void command(int fd, const char *line)
{
	static qcmd q;
	char v[64];

	memset(&q, 0, sizeof q);
	if (!strncmp(line, "PLAY", 4)) {
		arg(line, "path", q.path, sizeof q.path);
		arg(line, "at", v, sizeof v);    q.at = atof(v);
		arg(line, "speed", v, sizeof v); q.speed = atof(v);
		if (q.path[0]) q.c = C_PLAY;
	} else if (!strncmp(line, "PAUSE", 5))  q.c = C_PAUSE;
	else if (!strncmp(line, "RESUME", 6))   q.c = C_RESUME;
	else if (!strncmp(line, "STOP", 4))     q.c = C_STOP;
	else if (!strncmp(line, "SEEK", 4)) {
		arg(line, "at", v, sizeof v);
		q.at = atof(v);
		q.c = C_SEEK;
	} else if (!strncmp(line, "SPEED", 5)) {
		arg(line, "x", v, sizeof v);
		q.speed = atof(v);
		q.c = C_SPEED;
	} else if (!strncmp(line, "SINK", 4)) {
		arg(line, "device", q.dev, sizeof q.dev);
		if (q.dev[0]) q.c = C_SINK;
	}
	if (q.c != C_NONE) {
		pthread_mutex_lock(&S.mu);
		q_push(&q);
		pthread_cond_signal(&S.cv);
		pthread_mutex_unlock(&S.mu);
	}

	if (!strncmp(line, "STATUS", 6)) announce(fd, 1);
}

static void on_term(int sig) { (void)sig; g_quit = 1; }

int main(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	pthread_t th;
	int lfd, cfd = -1;
	char buf[4096];
	size_t have = 0;
	time_t last_pos = 0;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", SOCK_PATH);

	/* One Muse. A second one started by a launcher that did not know the
	 * first was alive finds it answering and leaves. */
	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (connect(lfd, (struct sockaddr *)&sa, sizeof sa) == 0) {
		say("already running");
		return 0;
	}
	close(lfd);
	unlink(SOCK_PATH);
	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(lfd, 2) < 0) {
		say("cannot listen on %s: %s", SOCK_PATH, strerror(errno));
		return 1;
	}
	if (pipe(g_wake) < 0) return 1;
	fcntl(g_wake[0], F_SETFL, O_NONBLOCK);
	fcntl(g_wake[1], F_SETFL, O_NONBLOCK);

	S.speed = 1.0;
	pthread_create(&th, NULL, player, NULL);
	say("listening on %s", SOCK_PATH);

	while (!g_quit) {
		struct pollfd p[3] = {
			{ lfd, POLLIN, 0 }, { g_wake[0], POLLIN, 0 }, { cfd, POLLIN, 0 },
		};
		int r = poll(p, cfd >= 0 ? 3 : 2, 250);

		if (r < 0 && errno != EINTR) break;

		if (p[0].revents & POLLIN) {
			/* The launcher, again: it restarted, and the newest connection is
			 * the one that is alive. */
			int n = accept(lfd, NULL, NULL);

			if (n >= 0) {
				if (cfd >= 0) close(cfd);
				cfd = n;
				have = 0;
				send_line(cfd, "READY\tproto=1");
				announce(cfd, 1);
			}
		}
		if (p[1].revents & POLLIN) {
			while (read(g_wake[0], buf, sizeof buf) > 0) { }
			announce(cfd, 0);
		}
		if (cfd >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t got = recv(cfd, buf + have, sizeof buf - have - 1, 0);

			if (got <= 0) { close(cfd); cfd = -1; have = 0; continue; }
			have += (size_t)got;
			buf[have] = '\0';
			for (;;) {
				char *nl = memchr(buf, '\n', have);
				size_t used;

				if (!nl) break;
				*nl = '\0';
				command(cfd, buf);
				used = (size_t)(nl - buf) + 1;
				memmove(buf, nl + 1, have - used);
				have -= used;
			}
			if (have >= sizeof buf - 1) have = 0;   /* a line with no end */
		}
		if (time(NULL) != last_pos) {
			last_pos = time(NULL);
			pthread_mutex_lock(&S.mu);
			r = S.state == ST_PLAYING;
			pthread_mutex_unlock(&S.mu);
			if (r) announce(cfd, 0);
		}
	}

	pthread_mutex_lock(&S.mu);
	g_quit = 1;
	pthread_cond_signal(&S.cv);
	pthread_mutex_unlock(&S.mu);
	pthread_join(th, NULL);
	if (cfd >= 0) close(cfd);
	close(lfd);
	unlink(SOCK_PATH);
	return 0;
}
