/* What a session record actually costs to commit.
 *
 * The game-stats work needs one durable write per game exit, and the argument
 * for how to store it rested on counting fsyncs rather than measuring them.
 * This measures them. Two candidate backends, on whichever filesystem the path
 * argument lands in:
 *
 *   append   open(O_APPEND), write 64 bytes, fsync, close. One fsync.
 *   sqlite   INSERT through a prepared statement, in DELETE and WAL journal
 *            modes. DELETE is the default and costs journal-create, page
 *            write, db write and journal-delete, each with its own barrier.
 *
 * Plus two controls, because a difference is only meaningful against them:
 *
 *   fsync    one fsync on an already-open fd, nothing else. The floor.
 *   nosync   the same write with no fsync at all. What durability costs.
 *
 * And the read path, which is the half the append design is supposed to lose:
 * folding every record versus asking SQL to group them.
 *
 * libsqlite3 is on the device already (3.12.2, /usr/lib), so this dlopens it
 * rather than linking, the same way the host reaches zlib. Nothing here needs
 * sqlite3.h - the ABI is stable and the eight entry points are declared below.
 *
 *   storeprobe <dir> [iterations]
 *
 * It writes only inside <dir>/storeprobe.tmp and removes it on the way out.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* --- the eight sqlite entry points this needs, declared rather than included */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
static int  (*sq_open)(const char *, sqlite3 **);
static int  (*sq_close)(sqlite3 *);
static int  (*sq_exec)(sqlite3 *, const char *, void *, void *, char **);
static int  (*sq_prepare)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
static int  (*sq_step)(sqlite3_stmt *);
static int  (*sq_reset)(sqlite3_stmt *);
static int  (*sq_finalize)(sqlite3_stmt *);
static int  (*sq_bind_int64)(sqlite3_stmt *, int, long long);
static const char *(*sq_errmsg)(sqlite3 *);
#define SQ_ROW  100
#define SQ_DONE 101

static int sqlite_load(void)
{
	void *h = dlopen("libsqlite3.so.0", RTLD_NOW);
	if (!h) h = dlopen("libsqlite3.so", RTLD_NOW);
	if (!h) h = dlopen("libsqlite3.dylib", RTLD_NOW);   /* so the host can smoke-test it */
	if (!h) return 0;
	sq_open       = dlsym(h, "sqlite3_open");
	sq_close      = dlsym(h, "sqlite3_close");
	sq_exec       = dlsym(h, "sqlite3_exec");
	sq_prepare    = dlsym(h, "sqlite3_prepare_v2");
	sq_step       = dlsym(h, "sqlite3_step");
	sq_reset      = dlsym(h, "sqlite3_reset");
	sq_finalize   = dlsym(h, "sqlite3_finalize");
	sq_bind_int64 = dlsym(h, "sqlite3_bind_int64");
	sq_errmsg     = dlsym(h, "sqlite3_errmsg");
	return sq_open && sq_close && sq_exec && sq_prepare && sq_step &&
	       sq_reset && sq_finalize && sq_bind_int64;
}

/* --- timing ------------------------------------------------------------- */
static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* Median and p90 rather than a mean. One 300 ms stall in a hundred writes is
 * the thing a mean hides and the thing a player feels. */
static void report(const char *name, double *v, int n, const char *note)
{
	qsort(v, n, sizeof *v, cmp_d);
	double sum = 0;
	for (int i = 0; i < n; i++) sum += v[i];
	printf("  %-22s %8.3f %8.3f %8.3f %8.3f   %7.1f  %s\n",
	       name, v[0], v[n / 2], v[(int)(n * 0.9)], v[n - 1], sum, note ? note : "");
	fflush(stdout);
}

/* A session record: game id, start, seconds, exit reason. 64 bytes is the
 * shape the stats log would actually write, so the probe writes that. */
static int record(char *buf, int i)
{
	return snprintf(buf, 96, "%08x %ld %6d %-8s\n",
	                (unsigned)(i * 2654435761u), (long)1757000000 + i * 900,
	                60 + i % 3600, i % 7 ? "quit" : "crash");
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: storeprobe <dir> [iterations]\n"); return 2; }
	int n = argc > 2 ? atoi(argv[2]) : 100;
	if (n < 5) n = 5;

	char dir[512], path[640], sql[256];
	snprintf(dir, sizeof dir, "%s/storeprobe.tmp", argv[1]);
	if (mkdir(dir, 0755) < 0 && access(dir, W_OK) < 0) {
		fprintf(stderr, "storeprobe: cannot write in %s\n", argv[1]);
		return 1;
	}

	double *v = malloc(n * sizeof *v);
	char rec[96];
	int len = record(rec, 0);

	printf("\n%s  (%d iterations, %d-byte records)\n", argv[1], n, len);
	printf("  %-22s %8s %8s %8s %8s   %7s\n",
	       "", "min", "median", "p90", "max", "total");

	/* --- control: the same write with no durability at all -------------- */
	snprintf(path, sizeof path, "%s/nosync.log", dir);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
	for (int i = 0; i < n; i++) {
		len = record(rec, i);
		double t0 = now_ms();
		if (write(fd, rec, len) < 0) { perror("write"); return 1; }
		v[i] = now_ms() - t0;
	}
	close(fd);
	report("write, no fsync", v, n, "control: what durability is bought against");

	/* --- control: the fsync floor, fd already open ---------------------- */
	snprintf(path, sizeof path, "%s/floor.log", dir);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
	for (int i = 0; i < n; i++) {
		len = record(rec, i);
		double t0 = now_ms();
		if (write(fd, rec, len) < 0) { perror("write"); return 1; }
		fsync(fd);
		v[i] = now_ms() - t0;
	}
	close(fd);
	report("write + fsync", v, n, "the floor any durable design pays");

	/* --- candidate: the append log, opened and closed per record -------- */
	snprintf(path, sizeof path, "%s/append.log", dir);
	unlink(path);
	for (int i = 0; i < n; i++) {
		len = record(rec, i);
		double t0 = now_ms();
		int f = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (f < 0) { perror("open"); return 1; }
		if (write(f, rec, len) < 0) { perror("write"); return 1; }
		fsync(f);
		close(f);
		v[i] = now_ms() - t0;
	}
	report("APPEND LOG", v, n, "open, write, fsync, close");

	/* --- candidate: sqlite, both journal modes --------------------------- */
	if (!sqlite_load()) {
		printf("  (no libsqlite3 here, so the sqlite rows are skipped)\n");
	} else {
		/* Three modes, and the third is the one that makes the comparison
		 * honest. WAL with synchronous=NORMAL does not fsync on commit at
		 * all - it defers to a checkpoint - so a power cut loses the last
		 * transactions. That is a real option, but it is not the same
		 * promise the append log makes, and putting it beside DELETE
		 * without saying so would be comparing durability against speed
		 * and calling one of them faster. WAL/FULL is the like-for-like. */
		const char *modes[3]   = { "DELETE", "WAL", "WAL" };
		const char *sync_of[3] = { "FULL",   "FULL", "NORMAL" };
		for (int m = 0; m < 3; m++) {
			sqlite3 *db = NULL;
			sqlite3_stmt *st = NULL;
			char *err = NULL;
			snprintf(path, sizeof path, "%s/stats-%s-%s.db", dir, modes[m], sync_of[m]);
			unlink(path);
			if (sq_open(path, &db) != 0) { printf("  sqlite open failed\n"); break; }
			snprintf(sql, sizeof sql, "PRAGMA journal_mode=%s;", modes[m]);
			sq_exec(db, sql, NULL, NULL, &err);
			snprintf(sql, sizeof sql, "PRAGMA synchronous=%s;", sync_of[m]);
			sq_exec(db, sql, NULL, NULL, &err);
			sq_exec(db, "CREATE TABLE s(game INTEGER, started INTEGER, "
			            "secs INTEGER, reason INTEGER);", NULL, NULL, &err);
			if (sq_prepare(db, "INSERT INTO s VALUES(?,?,?,?);", -1, &st, NULL) != 0) {
				printf("  sqlite prepare failed: %s\n", sq_errmsg ? sq_errmsg(db) : "?");
				sq_close(db); break;
			}
			for (int i = 0; i < n; i++) {
				double t0 = now_ms();
				sq_bind_int64(st, 1, (i * 2654435761u) & 0xffffffff);
				sq_bind_int64(st, 2, 1757000000 + i * 900);
				sq_bind_int64(st, 3, 60 + i % 3600);
				sq_bind_int64(st, 4, i % 7 ? 1 : 2);
				if (sq_step(st) != SQ_DONE) { printf("  insert failed\n"); break; }
				sq_reset(st);
				v[i] = now_ms() - t0;
			}
			sq_finalize(st);
			sq_close(db);
			char label[40];
			snprintf(label, sizeof label, "SQLITE %s/%s", modes[m], sync_of[m]);
			snprintf(sql, sizeof sql, "one INSERT, synchronous=%s", sync_of[m]);
			report(label, v, n, sql);
		}
	}

	/* --- the read path, on a realistic year of sessions ------------------ */
	int many = 4000;
	printf("\n  reading back %d sessions:\n", many);

	snprintf(path, sizeof path, "%s/fold.log", dir);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	for (int i = 0; i < many; i++) { len = record(rec, i); if (write(fd, rec, len) < 0) break; }
	fsync(fd); close(fd);

	{	/* Fold: read it all, sum seconds per game into a small open hash. */
		double t0 = now_ms();
		FILE *f = fopen(path, "rb");
		static unsigned key[8192]; static long tot[8192];
		memset(key, 0, sizeof key); memset(tot, 0, sizeof tot);
		char line[128]; long games = 0;
		while (fgets(line, sizeof line, f)) {
			unsigned g; long st_, se;
			if (sscanf(line, "%x %ld %ld", &g, &st_, &se) != 3) continue;
			unsigned h = g & 8191;
			while (key[h] && key[h] != g) h = (h + 1) & 8191;
			if (!key[h]) { key[h] = g; games++; }
			tot[h] += se;
		}
		fclose(f);
		double ms = now_ms() - t0;
		printf("  %-22s %8.3f ms   %ld distinct games\n", "fold the log", ms, games);
	}

	if (sq_open) {
		sqlite3 *db = NULL; sqlite3_stmt *st = NULL; char *err = NULL;
		snprintf(path, sizeof path, "%s/read.db", dir);
		unlink(path);
		sq_open(path, &db);
		sq_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err);
		sq_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, &err);
		sq_exec(db, "CREATE TABLE s(game INTEGER, started INTEGER, secs INTEGER, reason INTEGER);",
		        NULL, NULL, &err);
		sq_exec(db, "BEGIN;", NULL, NULL, &err);
		sq_prepare(db, "INSERT INTO s VALUES(?,?,?,?);", -1, &st, NULL);
		for (int i = 0; i < many; i++) {
			sq_bind_int64(st, 1, (i * 2654435761u) & 0xffffffff);
			sq_bind_int64(st, 2, 1757000000 + i * 900);
			sq_bind_int64(st, 3, 60 + i % 3600);
			sq_bind_int64(st, 4, i % 7 ? 1 : 2);
			sq_step(st); sq_reset(st);
		}
		sq_finalize(st);
		sq_exec(db, "COMMIT;", NULL, NULL, &err);
		sq_close(db);

		/* Reopened cold, because the launcher would not hold it open. */
		double t0 = now_ms();
		sq_open(path, &db);
		sq_prepare(db, "SELECT game, SUM(secs) FROM s GROUP BY game;", -1, &st, NULL);
		long games = 0;
		while (sq_step(st) == SQ_ROW) games++;
		sq_finalize(st);
		sq_close(db);
		double ms = now_ms() - t0;
		printf("  %-22s %8.3f ms   %ld distinct games\n", "sqlite GROUP BY", ms, games);
	}

	/* Sizes matter for a card and for what has to be read at startup. */
	printf("\n  on disk:\n");
	const char *names[] = { "fold.log", "read.db", "stats-DELETE-FULL.db",
	                        "stats-WAL-FULL.db", "stats-WAL-NORMAL.db" };
	for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
		struct stat sb;
		snprintf(path, sizeof path, "%s/%s", dir, names[i]);
		if (stat(path, &sb) == 0)
			printf("  %-22s %8lld bytes\n", names[i], (long long)sb.st_size);
	}

	/* Leave nothing behind. */
	char cmd[640];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	if (system(cmd) != 0) fprintf(stderr, "storeprobe: could not remove %s\n", dir);
	free(v);
	return 0;
}
