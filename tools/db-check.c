/* Does the settings store keep its promises?
 *
 * Fourteen config files became two databases, and the properties that used to
 * be obvious from looking at a text file now have to be asserted. Chiefly:
 *
 *   - a shipped default never overwrites a choice the player made. That is not
 *     hypothetical - tortos.cfg was applied over the top of levels.cfg at every
 *     boot until 2026-08, which put the config ahead of the player.
 *   - a missing or unreadable database comes back as a working one, because
 *     the defaults are compiled in and there is no .db in the payload.
 *   - a value that is not a number is not a zero.
 *   - the two scopes stay apart. Flattening them would move a session token
 *     between handhelds, which is the one thing the old layout was careful of.
 *
 * Links src/db.c and NOT SDL. If it ever needs SDL, the split has failed.
 */
#include "../src/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

#define DEV "/tmp/tortos-db-check-device.db"
#define LIB "/tmp/tortos-db-check-library.db"

static void scrub(void)
{
	const char *suffix[] = { "", "-wal", "-shm" };
	char p[256];
	for (unsigned i = 0; i < 3; i++) {
		snprintf(p, sizeof p, "%s%s", DEV, suffix[i]); unlink(p);
		snprintf(p, sizeof p, "%s%s", LIB, suffix[i]); unlink(p);
	}
}

static void opens_and_seeds(void)
{
	db *d;
	char buf[64];

	printf("a fresh database arrives with the shipped defaults:\n");
	d = db_open(DEV, DB_DEVICE);
	ck(d != NULL, "it opens where nothing existed");
	if (!d) return;

	ck(db_get_int(d, "volume", -1) == 40, "volume seeded from config/tortos.cfg");
	ck(db_get_int(d, "brightness", -1) == 7, "brightness seeded");
	db_get_str(d, "audioout", buf, sizeof buf, "");
	ck(!strcmp(buf, "auto"), "audioout seeded to auto");
	ck(db_get_int(d, "autooff", -1) == 0, "auto off seeded to off");
	db_close(d);
}

static void a_default_never_beats_a_choice(void)
{
	db *d;

	printf("a default never overwrites a choice:\n");
	d = db_open(DEV, DB_DEVICE);
	ck(db_set_int(d, "volume", 12), "the player turns it down");
	db_close(d);

	d = db_open(DEV, DB_DEVICE);              /* seeds again on every open */
	ck(db_get_int(d, "volume", -1) == 12, "and it is still 12 after a reopen");
	ck(db_get_int(d, "brightness", -1) == 7, "while an untouched key keeps its default");
	db_close(d);
}

static void round_trips(void)
{
	db *d = db_open(DEV, DB_DEVICE);
	char buf[64];

	printf("values survive a close and reopen:\n");
	ck(db_set_str(d, "probe.text", "hello world"), "a string with a space writes");
	ck(db_set_int(d, "probe.int", -17), "a negative integer writes");
	db_close(d);

	d = db_open(DEV, DB_DEVICE);
	db_get_str(d, "probe.text", buf, sizeof buf, "");
	ck(!strcmp(buf, "hello world"), "the string comes back whole");
	ck(db_get_int(d, "probe.int", 0) == -17, "so does the integer");

	printf("a value that is not a number is not a zero:\n");
	db_set_str(d, "probe.junk", "seven");
	ck(db_get_int(d, "probe.junk", -99) == -99, "it falls back instead of reading 0");
	db_set_str(d, "probe.trailing", "12x");
	ck(db_get_int(d, "probe.trailing", -99) == -99, "and trailing rubbish is not 12");

	printf("absent keys and deletion:\n");
	ck(!db_has(d, "probe.never"), "a key nobody wrote is absent");
	ck(db_get_int(d, "probe.never", 5) == 5, "and reads as the fallback");
	db_get_str(d, "probe.never", buf, sizeof buf, "spare");
	ck(!strcmp(buf, "spare"), "including for strings");
	ck(db_has(d, "probe.text"), "a key that was written is present");
	ck(db_del(d, "probe.text"), "it deletes");
	ck(!db_has(d, "probe.text"), "and is gone afterwards");
	db_close(d);
}

static void a_lost_database_self_heals(void)
{
	db *d;

	printf("a deleted database comes back working:\n");
	scrub();
	d = db_open(DEV, DB_DEVICE);
	ck(d != NULL, "it reopens from nothing");
	ck(db_get_int(d, "volume", -1) == 40, "with the shipped default, not an empty value");
	ck(!db_has(d, "probe.int"), "and nothing from the old file");
	db_close(d);
}

static void the_scopes_stay_apart(void)
{
	db *dev, *lib;
	char buf[64];
	size_t nd, nl, i, j;
	const db_default *dd, *ld;

	printf("the two scopes do not bleed into each other:\n");
	dev = db_open(DEV, DB_DEVICE);
	lib = db_open(LIB, DB_LIBRARY);
	ck(dev && lib, "both open");
	if (!dev || !lib) return;

	db_get_str(lib, "timezone", buf, sizeof buf, "");
	ck(!strcmp(buf, "America/New_York"), "the library scope seeds its own defaults");
	ck(!db_has(dev, "timezone"), "which the device scope does not get");
	ck(!db_has(lib, "volume"), "and the device's do not leak the other way");

	/* A key in both would be ambiguous the first time a card moved between
	 * handhelds, which is exactly the failure the split exists to prevent. */
	dd = db_defaults(DB_DEVICE, &nd);
	ld = db_defaults(DB_LIBRARY, &nl);
	for (i = 0; i < nd; i++)
		for (j = 0; j < nl; j++)
			ck(strcmp(dd[i].key, ld[j].key) != 0,
			   "no key is declared in both scopes");

	/* Writing the same name either side must stay two separate values. */
	db_set_str(dev, "shared.name", "device");
	db_set_str(lib, "shared.name", "library");
	db_get_str(dev, "shared.name", buf, sizeof buf, "");
	ck(!strcmp(buf, "device"), "the same key in both files holds two values");
	db_get_str(lib, "shared.name", buf, sizeof buf, "");
	ck(!strcmp(buf, "library"), "and neither one wins");

	db_close(dev);
	db_close(lib);
}

static void every_default_is_readable(void)
{
	db *d;
	const db_default *def;
	size_t n, i;
	char buf[128];

	printf("every declared default is present and reads back:\n");
	scrub();
	for (int s = 0; s < 2; s++) {
		db_scope sc = s ? DB_LIBRARY : DB_DEVICE;
		d = db_open(s ? LIB : DEV, sc);
		if (!d) { ck(0, "scope opens"); continue; }
		def = db_defaults(sc, &n);
		ck(n > 0, "the scope declares at least one default");
		for (i = 0; i < n; i++) {
			ck(db_has(d, def[i].key), def[i].key);
			db_get_str(d, def[i].key, buf, sizeof buf, "");
			ck(!strcmp(buf, def[i].value), "seeded value matches the table");
		}
		db_close(d);
	}
}

int main(void)
{
	if (!db_available()) {
		/* Not a pass dressed as a skip: say which, and say it loudly. */
		printf("db-check: no libsqlite3 on this machine, so nothing was checked\n");
		return 77;
	}
	scrub();
	opens_and_seeds();
	a_default_never_beats_a_choice();
	round_trips();
	a_lost_database_self_heals();
	the_scopes_stay_apart();
	every_default_is_readable();
	scrub();

	if (fails) { printf("\n%d FAILED\n", fails); return 1; }
	printf("\nok: the settings store keeps its promises\n");
	return 0;
}
