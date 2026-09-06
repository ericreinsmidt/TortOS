/* SPDX-License-Identifier: MIT */
/* See stats.h. No SDL and no platform header, so the check can drive it
 * without a window - `now_ms` comes from the caller for the same reason
 * idle_check takes one.
 *
 * A row is "sess.<start>.<tag>\t<file>" -> "<seconds>\t<state>", where state
 * is "open" while the game is running and the EXIT reason afterwards. The tab
 * is the separator for the reason favorites use one: a ROM filename may
 * legally contain almost anything except a tab or a newline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "stats.h"

/* --- the session in progress -------------------------------------------- */
static char     g_tag[STATS_TAG_MAX];
static char     g_file[STATS_FILE_MAX];
static long     g_start;          /* unix time, the key's middle segment */
static unsigned g_t0;             /* now_ms at RUN, for the elapsed seconds */
static unsigned g_written;        /* now_ms of the last row written, 0 = none */
static bool     g_running;

/* The millisecond clock is in the key, not for ordering but for UNIQUENESS.
 * Two launches of the same game inside one wall-clock second would otherwise
 * share a key and the second would overwrite the first - which is what the
 * check found, by launching Contra twice in a row with no sleep between.
 *
 * It has to be something already in hand, because stats_begin does no I/O:
 * asking the database whether a key is taken would put a read on the launch
 * path, which is the one thing this design refuses. g_t0 is monotonic and
 * costs nothing. */
static void session_key(char *out, size_t n, long start, unsigned t0,
                        const char *tag, const char *file)
{
	snprintf(out, n, "sess.%ld.%u.%.*s\t%.*s", start, t0,
	         STATS_TAG_MAX - 1, tag, STATS_FILE_MAX - 1, file);
}

/* Elapsed in whole seconds. unsigned arithmetic, so a wrap of the millisecond
 * clock subtracts correctly rather than producing a session of 49 days. */
static long elapsed_s(unsigned now_ms)
{
	return (long)((now_ms - g_t0) / 1000u);
}

static void write_row(unsigned now_ms, const char *state)
{
	char key[STATS_TAG_MAX + STATS_FILE_MAX + 56], val[64];

	session_key(key, sizeof key, g_start, g_t0, g_tag, g_file);
	snprintf(val, sizeof val, "%ld\t%s", elapsed_s(now_ms), state);
	db_set_str(db_lib(), key, val);
	g_written = now_ms;
}

void stats_begin(const char *tag, const char *file, unsigned now_ms)
{
	g_running = false;
	if (!tag || !file || !*tag || !*file) return;
	if (strlen(tag) >= STATS_TAG_MAX || strlen(file) >= STATS_FILE_MAX) return;

	/* Everything here is memory. Nothing is opened, written or synced - the
	 * whole point of the design is that a launch costs nothing. */
	snprintf(g_tag, sizeof g_tag, "%s", tag);
	snprintf(g_file, sizeof g_file, "%s", file);
	g_start   = (long)time(NULL);
	g_t0      = now_ms;
	g_written = 0;
	g_running = true;
}

void stats_tick(unsigned now_ms)
{
	if (!g_running) return;

	/* Nothing at all until the session is worth recording, so backing
	 * straight out of a game leaves no row behind. */
	if (!g_written) {
		if (now_ms - g_t0 >= STATS_MARK_MS) write_row(now_ms, "open");
		return;
	}
	if (now_ms - g_written >= STATS_CKPT_MS) write_row(now_ms, "open");
}

void stats_end(const char *reason, unsigned now_ms)
{
	if (!g_running) return;
	g_running = false;

	/* A session too short to have written a marker still gets its row here,
	 * because by now we know it ended and how long it was. The marker exists
	 * for sessions that never reach this point, not for this one. */
	write_row(now_ms, reason && *reason ? reason : "quit");
}

/* --- recovery ------------------------------------------------------------ */

struct open_row { char key[STATS_TAG_MAX + STATS_FILE_MAX + 40];
                  char val[64]; struct open_row *next; };

static bool collect_open(const char *key, const char *value, void *ctx)
{
	struct open_row **head = ctx, *r;
	const char *tab = strchr(value, '\t');

	if (!tab || strcmp(tab + 1, "open") != 0) return true;
	if (!(r = malloc(sizeof *r))) return false;
	snprintf(r->key, sizeof r->key, "%s", key);
	snprintf(r->val, sizeof r->val, "%.*s\tlost", (int)(tab - value), value);
	r->next = *head;
	*head = r;
	return true;
}

void stats_recover(void)
{
	struct open_row *open = NULL, *r;

	/* Collected before anything is written. Rewriting rows while enumerating
	 * the same prefix is asking a question and changing the answer at once -
	 * the same care fav_save needs for its deletions. */
	db_each_prefix(db_lib(), "sess.", collect_open, &open);
	while ((r = open)) {
		open = r->next;
		/* The seconds stay as they were: the last checkpoint is the most that
		 * can honestly be claimed for a session nobody saw end. Marked so it
		 * is not mistaken for a clean quit later. */
		db_set_str(db_lib(), r->key, r->val);
		free(r);
	}
}

/* --- summarizing --------------------------------------------------------- */

typedef struct {
	char tag[STATS_TAG_MAX];
	char file[STATS_FILE_MAX];
	long seconds;
	int  launches;
} stats_row;

static stats_row g_rows[STATS_MAX];
static int  g_nrows;
static long g_total;
static int  g_launches;

static int find_row(const char *tag, const char *file)
{
	int i;
	for (i = 0; i < g_nrows; i++)
		if (!strcmp(g_rows[i].tag, tag) && !strcmp(g_rows[i].file, file))
			return i;
	return -1;
}

static bool fold(const char *key, const char *value, void *ctx)
{
	/* sess.<start>.<t0>.<tag>\t<file> - skip both numeric segments. */
	const char *rest = key + strlen("sess.");
	const char *dot = strchr(rest, '.');
	const char *tab, *sep = strchr(value, '\t');
	char tag[STATS_TAG_MAX];
	long secs;
	int i;

	(void)ctx;
	if (!dot || !sep) return true;
	if (!(dot = strchr(dot + 1, '.'))) return true;
	if (!(tab = strchr(dot + 1, '\t'))) return true;
	if ((size_t)(tab - dot - 1) >= STATS_TAG_MAX) return true;
	secs = strtol(value, NULL, 10);
	if (secs < 0) return true;

	snprintf(tag, sizeof tag, "%.*s", (int)(tab - dot - 1), dot + 1);
	i = find_row(tag, tab + 1);
	if (i < 0) {
		if (g_nrows >= STATS_MAX) return false;
		i = g_nrows++;
		snprintf(g_rows[i].tag, STATS_TAG_MAX, "%s", tag);
		snprintf(g_rows[i].file, STATS_FILE_MAX, "%s", tab + 1);
		g_rows[i].seconds = 0;
		g_rows[i].launches = 0;
	}
	g_rows[i].seconds += secs;
	g_rows[i].launches++;
	g_total += secs;
	g_launches++;
	return true;
}

static int by_seconds(const void *a, const void *b)
{
	const stats_row *x = a, *y = b;
	if (x->seconds != y->seconds) return x->seconds < y->seconds ? 1 : -1;
	return strcmp(x->file, y->file);      /* stable enough to read twice */
}

int stats_summarize(void)
{
	g_nrows = 0;
	g_total = 0;
	g_launches = 0;
	db_each_prefix(db_lib(), "sess.", fold, NULL);
	qsort(g_rows, g_nrows, sizeof g_rows[0], by_seconds);
	return g_nrows;
}

bool stats_at(int i, const char **tag, const char **file,
              long *seconds, int *launches)
{
	if (i < 0 || i >= g_nrows) return false;
	if (tag)      *tag      = g_rows[i].tag;
	if (file)     *file     = g_rows[i].file;
	if (seconds)  *seconds  = g_rows[i].seconds;
	if (launches) *launches = g_rows[i].launches;
	return true;
}

long stats_total_seconds(void) { return g_total; }
int  stats_total_launches(void) { return g_launches; }

void stats_format(long seconds, char *out, size_t n)
{
	if (!out || !n) return;
	if (seconds <= 0)      snprintf(out, n, "never");
	else if (seconds < 60) snprintf(out, n, "%lds", seconds);
	else if (seconds < 3600)
		snprintf(out, n, "%ldm %lds", seconds / 60, seconds % 60);
	else
		snprintf(out, n, "%ldh %ldm", seconds / 3600, (seconds % 3600) / 60);
}
