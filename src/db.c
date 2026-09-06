/* SPDX-License-Identifier: MIT */
/* See db.h. No SDL and no platform header, deliberately, so the check can link
 * it without a window - the same reason atomic.c is kept clean. */
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include "atomic.h"
#include "db.h"

/* --- libsqlite3, declared rather than included ---------------------------
 * There is no sqlite3.h on the device or in the sysroot. These nine entry
 * points are the whole surface this file uses, and the C ABI for them has been
 * stable since 3.x. The device ships 3.12.2. */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

static int  (*sq_open_v2)(const char *, sqlite3 **, int, const char *);
static int  (*sq_close)(sqlite3 *);
static int  (*sq_exec)(sqlite3 *, const char *, void *, void *, char **);
static int  (*sq_prepare)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
static int  (*sq_step)(sqlite3_stmt *);
static int  (*sq_finalize)(sqlite3_stmt *);
static int  (*sq_bind_text)(sqlite3_stmt *, int, const char *, int, void *);
static const unsigned char *(*sq_column_text)(sqlite3_stmt *, int);
static void (*sq_free)(void *);

#define SQ_OK    0
#define SQ_ROW   100
#define SQ_DONE  101
#define SQ_OPEN_RW_CREATE 0x00000006      /* READWRITE | CREATE */
#define SQ_TRANSIENT ((void *)-1)

struct db { sqlite3 *h; };

static int loaded;      /* 0 not tried, 1 ok, -1 failed */

static void *dl_sqlite(void)
{
	static const char *names[] = {
		"libsqlite3.so.0",      /* what the device actually ships */
		"libsqlite3.so",
		"libsqlite3.dylib",     /* so the check runs on a dev machine */
	};
	for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
		void *h = dlopen(names[i], RTLD_NOW);
		if (h) return h;
	}
	return NULL;
}

bool db_available(void)
{
	void *h;

	if (loaded) return loaded > 0;
	loaded = -1;
	if (!(h = dl_sqlite())) return false;

	sq_open_v2    = dlsym(h, "sqlite3_open_v2");
	sq_close      = dlsym(h, "sqlite3_close");
	sq_exec       = dlsym(h, "sqlite3_exec");
	sq_prepare    = dlsym(h, "sqlite3_prepare_v2");
	sq_step       = dlsym(h, "sqlite3_step");
	sq_finalize   = dlsym(h, "sqlite3_finalize");
	sq_bind_text  = dlsym(h, "sqlite3_bind_text");
	sq_column_text = dlsym(h, "sqlite3_column_text");
	sq_free       = dlsym(h, "sqlite3_free");

	if (!sq_open_v2 || !sq_close || !sq_exec || !sq_prepare || !sq_step ||
	    !sq_finalize || !sq_bind_text || !sq_column_text)
		return false;
	loaded = 1;
	return true;
}

/* --- the defaults, compiled in ------------------------------------------
 * Shipped in the binary rather than as a .db in the payload, so there is no
 * second artifact to drift from this code and a deleted database self-heals
 * into a working one. These values are what config/tortos.cfg shipped. */
static const db_default device_defaults[] = {
	/* A RUNG, 0..PLAT_VOL_MAX, which is what a nudge stores. config/tortos.cfg
	 * shipped 40 percent and the launcher converted; keeping the percentage
	 * here would put two units under one key, and 15 percent would then be
	 * indistinguishable from rung 15. (40 * 20 + 50) / 100 = 8. */
	{ "volume",     "8"  },
	{ "brightness", "7"  },
	{ "audioout",   "auto" },  /* auto | speaker - see src/audioout.h */
	{ "autooff",    "120" },   /* seconds without input, 0 is off */
	{ "textsize",   "1.0" },
	{ "wifi",       "0"  },    /* what THIS device was last doing, not the */
	{ "bluetooth",  "0"  },    /* shipped default - seeding merges the two */
};

static const db_default library_defaults[] = {
	{ "timezone",       "America/New_York" },
	{ "startup_system", "NES" },

	/* Turbo, per system tag, from config/turbo.cfg. `x:a~3,y:b~3` says X is a
	 * turbo A and Y a turbo B, three frames pressed and three released - about
	 * ten presses a second at 60 Hz. Listed only where X and Y are SPARE: nine
	 * of the eleven consoles had two face buttons, but a Genesis six-button
	 * pad and a SNES pad use X and Y for real, so MD and SFC are absent on
	 * purpose rather than by oversight. The full reasoning, including why PC
	 * Engine is here despite its core having a turbo of its own, is in
	 * docs/turbo.md. */
	{ "turbo.NES",  "x:a~3,y:b~3" },
	{ "turbo.SMS",  "x:a~3,y:b~3" },
	{ "turbo.PCE",  "x:a~3,y:b~3" },
	{ "turbo.GB",   "x:a~3,y:b~3" },
	{ "turbo.GBC",  "x:a~3,y:b~3" },
	{ "turbo.NGP",  "x:a~3,y:b~3" },
	{ "turbo.NGPC", "x:a~3,y:b~3" },
	{ "turbo.GBA",  "x:a~3,y:b~3" },
	{ "turbo.GG",   "x:a~3,y:b~3" },

	/* Core options, from config/coreopts.cfg. The segment after "coreopt." is
	 * the system tag and an EMPTY one means global, which is why the first key
	 * has two dots. A tagged entry overrides a global of the same name. */
	{ "coreopt..mgba_sgb_borders",  "OFF" },
	{ "coreopt.GB.mgba_gb_model",   "Game Boy" },
	{ "coreopt.GB.mgba_gb_colors",  "DMG Green" },
};

const db_default *db_defaults(db_scope scope, size_t *count)
{
	if (scope == DB_DEVICE) {
		*count = sizeof device_defaults / sizeof *device_defaults;
		return device_defaults;
	}
	*count = sizeof library_defaults / sizeof *library_defaults;
	return library_defaults;
}

/* --- schema --------------------------------------------------------------
 * WAL and synchronous=NORMAL for both. NORMAL leaves the same ~33 s writeback
 * window an unsynced write has, which is the right trade for a volume level:
 * losing the last nudge to a power cut costs nothing, and FULL costs 5x per
 * write to prevent it. Measured, see tools/storeprobe.c. */
static const char SCHEMA[] =
	"PRAGMA journal_mode=WAL;"
	"PRAGMA synchronous=NORMAL;"
	"CREATE TABLE IF NOT EXISTS settings("
	"  key TEXT PRIMARY KEY NOT NULL,"
	"  value TEXT NOT NULL"
	");";

static bool run(db *d, const char *sql)
{
	char *err = NULL;
	int rc = sq_exec(d->h, sql, NULL, NULL, &err);
	if (err && sq_free) sq_free(err);
	return rc == SQ_OK;
}

db *db_open(const char *path, db_scope scope)
{
	db *d;
	const db_default *def;
	size_t n, i;

	if (!path || !*path || !db_available()) return NULL;
	if (!(d = calloc(1, sizeof *d))) return NULL;

	if (sq_open_v2(path, &d->h, SQ_OPEN_RW_CREATE, NULL) != SQ_OK) {
		if (d->h) sq_close(d->h);
		free(d);
		return NULL;
	}
	if (!run(d, SCHEMA)) { db_close(d); return NULL; }

	/* Seed only what is absent, so a default never overwrites a choice. That
	 * is the bug the old tortos.cfg had until 2026-08: it was applied over
	 * the top of levels.cfg at every boot, putting the config ahead of the
	 * player. */
	def = db_defaults(scope, &n);
	for (i = 0; i < n; i++)
		if (!db_has(d, def[i].key))
			db_set_str(d, def[i].key, def[i].value);

	return d;
}

void db_close(db *d)
{
	if (!d) return;
	if (d->h) sq_close(d->h);
	free(d);
}

bool db_get_str(db *d, const char *key, char *out, size_t n, const char *fallback)
{
	sqlite3_stmt *st = NULL;
	const unsigned char *v;
	bool got = false;

	if (out && n) out[0] = '\0';
	if (d && key && out && n &&
	    sq_prepare(d->h, "SELECT value FROM settings WHERE key=?;", -1, &st, NULL) == SQ_OK) {
		sq_bind_text(st, 1, key, -1, SQ_TRANSIENT);
		if (sq_step(st) == SQ_ROW && (v = sq_column_text(st, 0))) {
			snprintf(out, n, "%s", (const char *)v);
			got = true;
		}
		sq_finalize(st);
	}
	if (!got && out && n) snprintf(out, n, "%s", fallback ? fallback : "");
	return got;
}

int db_get_int(db *d, const char *key, int fallback)
{
	char buf[64];
	char *end;
	long v;

	if (!db_get_str(d, key, buf, sizeof buf, NULL) || !buf[0]) return fallback;
	v = strtol(buf, &end, 10);
	/* A value that is not a number is not a zero. Saying so beats silently
	 * turning a typo into a setting. */
	if (end == buf || *end) return fallback;
	return (int)v;
}

bool db_has(db *d, const char *key)
{
	sqlite3_stmt *st = NULL;
	bool found = false;

	if (!d || !key) return false;
	if (sq_prepare(d->h, "SELECT 1 FROM settings WHERE key=?;", -1, &st, NULL) != SQ_OK)
		return false;
	sq_bind_text(st, 1, key, -1, SQ_TRANSIENT);
	found = sq_step(st) == SQ_ROW;
	sq_finalize(st);
	return found;
}

bool db_set_str(db *d, const char *key, const char *val)
{
	sqlite3_stmt *st = NULL;
	bool ok;

	if (!d || !key || !val) return false;
	/* INSERT OR REPLACE rather than UPSERT: 3.12.2 predates ON CONFLICT,
	 * and the device is the oldest sqlite this has to run against. */
	if (sq_prepare(d->h, "INSERT OR REPLACE INTO settings(key,value) VALUES(?,?);",
	               -1, &st, NULL) != SQ_OK)
		return false;
	sq_bind_text(st, 1, key, -1, SQ_TRANSIENT);
	sq_bind_text(st, 2, val, -1, SQ_TRANSIENT);
	ok = sq_step(st) == SQ_DONE;
	sq_finalize(st);
	return ok;
}

bool db_set_int(db *d, const char *key, int val)
{
	char buf[32];
	snprintf(buf, sizeof buf, "%d", val);
	return db_set_str(d, key, buf);
}

bool db_del(db *d, const char *key)
{
	sqlite3_stmt *st = NULL;
	bool ok;

	if (!d || !key) return false;
	if (sq_prepare(d->h, "DELETE FROM settings WHERE key=?;", -1, &st, NULL) != SQ_OK)
		return false;
	sq_bind_text(st, 1, key, -1, SQ_TRANSIENT);
	ok = sq_step(st) == SQ_DONE;
	sq_finalize(st);
	return ok;
}

void db_each_prefix(db *d, const char *prefix, db_each_fn fn, void *ctx)
{
	sqlite3_stmt *st = NULL;
	const unsigned char *k, *v;
	char hi[256];
	size_t n;

	if (!d || !prefix || !fn) return;
	/* A range rather than LIKE: no escaping question for keys that contain
	 * the wildcards, and it uses the primary key index. The upper bound is
	 * the prefix with its last byte incremented, which is the next key that
	 * cannot share it. */
	n = strlen(prefix);
	if (!n || n + 1 >= sizeof hi) return;
	memcpy(hi, prefix, n + 1);
	hi[n - 1]++;

	if (sq_prepare(d->h, "SELECT key,value FROM settings "
	                     "WHERE key >= ? AND key < ? ORDER BY key;",
	               -1, &st, NULL) != SQ_OK)
		return;
	sq_bind_text(st, 1, prefix, -1, SQ_TRANSIENT);
	sq_bind_text(st, 2, hi, -1, SQ_TRANSIENT);
	while (sq_step(st) == SQ_ROW) {
		k = sq_column_text(st, 0);
		v = sq_column_text(st, 1);
		if (!fn(k ? (const char *)k : "", v ? (const char *)v : "", ctx)) break;
	}
	sq_finalize(st);
}

void db_dump(db *d, FILE *out)
{
	sqlite3_stmt *st = NULL;
	const unsigned char *k, *v;

	if (!d || !out) return;
	if (sq_prepare(d->h, "SELECT key,value FROM settings ORDER BY key;",
	               -1, &st, NULL) != SQ_OK) {
		fprintf(out, "  (cannot read settings)\n");
		return;
	}
	while (sq_step(st) == SQ_ROW) {
		k = sq_column_text(st, 0);
		v = sq_column_text(st, 1);
		fprintf(out, "  %-16s %s\n", k ? (const char *)k : "?",
		        v ? (const char *)v : "");
	}
	sq_finalize(st);
}

/* --- the two open handles ------------------------------------------------ */
static db *g_dev, *g_lib;
static char g_boot_env[1024];

bool db_init(const char *device_path, const char *library_path,
             const char *boot_env_path)
{
	db_shutdown();
	g_dev = db_open(device_path, DB_DEVICE);
	g_lib = db_open(library_path, DB_LIBRARY);
	snprintf(g_boot_env, sizeof g_boot_env, "%s", boot_env_path ? boot_env_path : "");
	return g_dev && g_lib;
}

void db_shutdown(void)
{
	db_close(g_dev); g_dev = NULL;
	db_close(g_lib); g_lib = NULL;
	g_boot_env[0] = '\0';
}

db *db_dev(void) { return g_dev; }
db *db_lib(void) { return g_lib; }

/* --- boot.env, see db.h -------------------------------------------------- */
bool db_write_boot_env(void)
{
	FILE *f;
	char tz[64];

	if (!g_dev || !g_lib || !g_boot_env[0]) return false;
	if (!(f = atomic_open(g_boot_env, 0644))) return false;

	/* Shell-sourced, so every value is quoted and nothing is computed here.
	 * The launcher already resolved "the player's choice, else the shipped
	 * default" by seeding, which is why this is four lines and launch.sh no
	 * longer needs a fallback for each one. */
	fprintf(f, "# Written by TortOS from the settings database. Derived, not\n");
	fprintf(f, "# authoritative: delete it and the next settings change\n");
	fprintf(f, "# rewrites it. Sourced by launch.sh before tortos.elf runs.\n");
	fprintf(f, "BRIGHTNESS='%d'\n", db_get_int(g_dev, "brightness", 7));
	fprintf(f, "WIFI='%d'\n",       db_get_int(g_dev, "wifi", 0) ? 1 : 0);
	fprintf(f, "BLUETOOTH='%d'\n",  db_get_int(g_dev, "bluetooth", 0) ? 1 : 0);
	db_get_str(g_lib, "timezone", tz, sizeof tz, "America/New_York");
	/* A single quote in a timezone name would break the shell that sources
	 * this. None exist, and a value that could is dropped rather than
	 * escaped, because a wrong TZ is cheaper than an unbootable launch.sh. */
	fprintf(f, "TIMEZONE='%s'\n", strchr(tz, '\'') ? "UTC" : tz);

	return atomic_commit(f, g_boot_env);
}
