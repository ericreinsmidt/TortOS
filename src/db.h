/* SPDX-License-Identifier: MIT */
/* One store for settings, replacing fourteen files that had drifted into
 * three different scopes and four different formats.
 *
 * TWO databases, not one, and the split is the one the files already had. It
 * is load-bearing and each half of it is reasoned in the code this replaces:
 *
 *   DB_DEVICE   per handheld. ra's session token lives here, and "a card moved
 *               to another device should not carry one with it". So do the
 *               levels, the display modes and the Wi-Fi preference, which
 *               describe a screen and a speaker rather than a library.
 *   DB_LIBRARY  per card, shared between handhelds. Favorites "travel with the
 *               saves they belong next to", cheevos because "what a player has
 *               earned belongs to them, not to the card it was earned on", and
 *               play time for the same reason.
 *
 * Flattening those into one file would silently move a session token between
 * devices, which is the one thing the old layout was careful about.
 *
 * There is no sqlite3.h on the device and none in the sysroot, so this dlopens
 * libsqlite3 and declares the entry points it needs - the same way the host
 * reaches zlib. The device ships 3.12.2 in /usr/lib.
 *
 * Measured on the card, 2026-09-05, against what this replaces: a settings
 * write through atomic_open/atomic_commit costs 3.03 ms median with a 9.27 ms
 * tail, and every volume nudge pays it while a game is running. The same write
 * here is 0.48 ms with a 0.59 ms tail. See tools/storeprobe.c.
 */
#ifndef TORTOS_DB_H
#define TORTOS_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct db db;

typedef enum { DB_DEVICE, DB_LIBRARY } db_scope;

/* False when libsqlite3 cannot be loaded at all. Callers decide what that
 * means; nothing here pretends a missing library is an empty database. */
bool db_available(void);

/* Opens, creates the schema if absent, and seeds any key the compiled-in
 * defaults for this scope declare and the file does not already hold. A
 * deleted or unreadable database therefore comes back as a fresh one with
 * shipped defaults rather than as a failure - there is no .db in the payload
 * to drift from this code. NULL if it cannot be opened. */
db *db_open(const char *path, db_scope scope);
void db_close(db *d);

/* A missing key is not an error: every getter takes what to say instead. That
 * is what makes adding a setting a one-line change rather than a migration. */
bool db_get_str(db *d, const char *key, char *out, size_t n, const char *fallback);
int  db_get_int(db *d, const char *key, int fallback);
bool db_has(db *d, const char *key);

bool db_set_str(db *d, const char *key, const char *val);
bool db_set_int(db *d, const char *key, int val);
bool db_del(db *d, const char *key);

/* Inspectable without being editable. Nothing on the device can read a
 * database - there is no sqlite3 binary - and losing the ability to SEE what a
 * setting is would be a real loss where losing the ability to edit it is not.
 * Reached from the launcher as --dump. */
void db_dump(db *d, FILE *out);

/* What db_open would seed, for a scope. Exposed so a check can assert that the
 * defaults and the code that reads them agree on every key, which is the thing
 * that rots first once these stop being files anyone looks at. */
typedef struct { const char *key; const char *value; } db_default;
const db_default *db_defaults(db_scope scope, size_t *count);

/* --- the two open handles -------------------------------------------------
 * Paths are passed in rather than read from platform.h, so this file stays
 * SDL-free and the check can link it without a window. */
bool db_init(const char *device_path, const char *library_path,
             const char *boot_env_path);
void db_shutdown(void);
db  *db_dev(void);      /* per handheld  */
db  *db_lib(void);      /* per card      */

/* --- boot.env -------------------------------------------------------------
 * launch.sh needs five values before tortos.elf exists: the panel brightness
 * for before the boot animation, the timezone, and whether each radio should
 * come up. It is POSIX shell with busybox sed, and there is no sqlite3 binary
 * on the device, so it cannot read a database.
 *
 * So the launcher exports what the shell needs. The database stays
 * authoritative and this file is derived - deleting it costs nothing but a
 * boot at the shipped defaults, and the next settings change rewrites it.
 *
 * It is also FASTER than what it replaces. launch.sh used to run six seds
 * across four files, each one a fork; this is a single `.` of one file.
 *
 * Rewritten whenever one of the five changes, not at shutdown: a handheld is
 * switched off by holding a button or by running the battery flat, and neither
 * is a chance to save anything. */
bool db_write_boot_env(void);

#endif
