/* Can this build talk to ScreenScraper, and does an account survive a restart?
 *
 * Two halves, deliberately. The first needs nothing: it asserts what a build
 * WITHOUT the developer pair promises, which is that ScreenScraper reports
 * itself unavailable rather than half-working - the condition every caller
 * falls back to libretro on.
 *
 * The second needs a network and an account, and skips cleanly without one,
 * the same way check-raset does. It is the only place the real request is
 * exercised: a sign-in that cannot be tested is a sign-in nobody finds out
 * about until a player types their password into it. Credentials come from
 * ~/.screenscraper.env, are never printed, and the password is wiped from
 * memory here as well as in the module.
 *
 * Links src/ss.c, src/db.c, src/net.c and src/rajson.c, and NOT SDL.
 */
#include "../src/ss.h"

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

#define DEV "/tmp/tortos-ss-check-device.db"
#define LIB "/tmp/tortos-ss-check-library.db"

static void scrub(void)
{
	const char *suffix[] = { "", "-wal", "-shm" };
	char p[256];
	size_t i;

	for (i = 0; i < sizeof suffix / sizeof suffix[0]; i++) {
		snprintf(p, sizeof p, "%s%s", DEV, suffix[i]); unlink(p);
		snprintf(p, sizeof p, "%s%s", LIB, suffix[i]); unlink(p);
	}
}

/* One value out of ~/.screenscraper.env, which is the host-side scaffolding's
 * file and is not in the repository. Absent is not a failure. */
static bool env_value(const char *key, char *out, size_t n)
{
	char path[512], line[512];
	const char *home = getenv("HOME");
	FILE *f;
	size_t klen = strlen(key);

	out[0] = '\0';
	if (!home) return false;
	snprintf(path, sizeof path, "%s/.screenscraper.env", home);
	f = fopen(path, "r");
	if (!f) return false;
	while (fgets(line, sizeof line, f)) {
		char *nl = strchr(line, '\n');

		if (nl) *nl = '\0';
		if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
			snprintf(out, n, "%s", line + klen + 1);
			break;
		}
	}
	fclose(f);
	memset(line, 0, sizeof line);
	return out[0] != '\0';
}

int main(void)
{
	char user[SS_USER_MAX], pass[SS_PASS_MAX], err[160] = "";
	bool have_account;

	printf("the developer pair:\n");
	if (ss_have_dev()) {
		printf("  built in - the live half of this check will run\n");
	} else {
		printf("  absent, which is a supported build\n");
		ck(!ss_signed_in(), "nothing is signed in without it");
	}

	scrub();
	if (!db_init(DEV, LIB, NULL)) {
		printf("  (no sqlite here, so storage is not checked)\n");
		printf("\nok: ScreenScraper reports what it can do\n");
		return fails ? 1 : 0;
	}

	printf("with no account stored:\n");
	ss_creds_clear();
	ck(!ss_creds_load(), "loading finds nothing");
	ck(!ss_signed_in(), "and nothing claims to be signed in");
	ck(ss_user()[0] == '\0', "the account name is empty");

	have_account = env_value("SS_USER", user, sizeof user) &&
	               env_value("SS_PASS", pass, sizeof pass);
	if (!ss_have_dev() || !have_account) {
		printf("the live sign-in:\n  skipped - %s\n",
		       !ss_have_dev() ? "this build has no developer pair"
		                      : "no SS_USER and SS_PASS in ~/.screenscraper.env");
		db_shutdown();
		scrub();
		printf("\nok: ScreenScraper reports what it can do\n");
		return fails ? 1 : 0;
	}

	printf("the live sign-in:\n");
	if (!ss_sign_in(user, pass, err, sizeof err)) {
		/* A network that is down is not a broken launcher, so this reports
		 * rather than fails. What it must never do is report success. */
		printf("  did not sign in: %s\n", err);
		ck(!ss_signed_in(), "and it did not claim to have");
	} else {
		ck(ss_signed_in(), "signed in");
		ck(strcmp(ss_user(), user) == 0, "under the account that was given");

		/* The point of storing it: a handheld that has been off still knows
		 * the account, because there is no token to re-ask for. */
		ss_creds_clear();
		ck(!ss_signed_in(), "clearing really clears it");
		ck(ss_creds_load(), "and it comes back from the database");
		ck(strcmp(ss_user(), user) == 0, "still the same account");
		/* Said out loud, because every assertion above is silent when it
		 * passes and a silent section reads exactly like a skipped one. The
		 * account name is not printed: it is half of a credential. */
		printf("  signed in, stored, and read back after a clear\n");
	}
	memset(pass, 0, sizeof pass);

	db_shutdown();
	scrub();
	if (fails) { printf("\n%d ScreenScraper check(s) failed\n", fails); return 1; }
	printf("\nok: ScreenScraper reports what it can do\n");
	return 0;
}
