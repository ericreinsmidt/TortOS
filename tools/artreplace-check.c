/* SPDX-License-Identifier: MIT */
/* Does Replace keep a cover it cannot better?
 *
 *     make check-artreplace
 *
 * Replace on a game's info screen used to delete the cover and then run the
 * scrape to fill the gap. For a game libretro has no art for - a translation,
 * homebrew, a cover added by hand - there was nothing to fill it with, and the
 * cover was simply gone. Now the scrape runs over that one game without
 * deleting anything, and a download is renamed over the old cover only once it
 * has arrived.
 *
 * So this drives the real src/artscrape.c through a whole run against a
 * stubbed network - no device, no download - and pins both outcomes: a cover
 * libretro has replaces the old one, and a cover it has not leaves the old one
 * exactly as it was. Plus the two ways a one-game run could leak into more:
 * touching another game on the shelf, or an over-long name becoming a
 * whole-system scrape.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "../src/artscrape.h"
#include "../src/net.h"

static int failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		if (!(cond)) {                                                        \
			printf("  FAIL: ");                                               \
			printf(__VA_ARGS__);                                              \
			printf("\n        at %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			failures++;                                                       \
		}                                                                     \
	} while (0)

/* The real one needs SDL_image, and nothing here is about resizing. */
void art_shrink(const char *path);
void art_shrink(const char *path) { (void)path; }

/* ---- the network, stubbed ------------------------------------------------
 *
 * The same contract as src/net.c: a GET is started and polled, and the file
 * appears at `path` only when the poll says 1 - written beside it first and
 * renamed over it, which is what makes a replace atomic. A request for anything
 * not being served answers -1 and leaves nothing behind. */
static const char *g_serve_image;    /* "Hit%20Game" is served, or NULL */
static char g_pend_url[2048], g_pend_path[2048];
static bool g_pending;
static char g_urls[4096];            /* every URL asked for, one per line */
static int  g_nreq;

bool net_get_async(const char *url, const char *path, int timeout_s)
{
	(void)timeout_s;
	snprintf(g_pend_url, sizeof g_pend_url, "%s", url);
	snprintf(g_pend_path, sizeof g_pend_path, "%s", path);
	g_pending = true;
	g_nreq++;
	if (strlen(g_urls) + strlen(url) + 2 < sizeof g_urls) {
		strcat(g_urls, url);
		strcat(g_urls, "\n");
	}
	return true;
}

static bool write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");

	if (!f) return false;
	fputs(text, f);
	return fclose(f) == 0;
}

int net_async_poll(void)
{
	char part[2100];
	const char *body = NULL;
	size_t n = strlen(g_pend_url);

	if (!g_pending) return -1;
	g_pending = false;

	if (n && g_pend_url[n - 1] == '/') {
		/* The catalog: other games only, so a fuzzy pass finds nothing. */
		body = "<a href=\"Unrelated%20Title%20%28USA%29.png\">x</a>\n";
	} else if (g_serve_image && strstr(g_pend_url, g_serve_image)) {
		body = "NEW";
	}
	if (!body) return -1;

	snprintf(part, sizeof part, "%s.part", g_pend_path);
	if (!write_file(part, body)) return -1;
	return rename(part, g_pend_path) == 0 ? 1 : -1;
}

void net_async_abort(void) { g_pending = false; }

/* ---- the card, in /tmp --------------------------------------------------- */

static char g_root[256], g_shelf[320], g_media[400];

static void make_card(void)
{
	char p[512];

	snprintf(g_root, sizeof g_root, "/tmp/tortos-artreplace-%ld", (long)getpid());
	snprintf(g_shelf, sizeof g_shelf, "%s/Game Boy", g_root);
	snprintf(g_media, sizeof g_media, "%s/.media", g_shelf);
	mkdir(g_root, 0755);
	mkdir(g_shelf, 0755);
	mkdir(g_media, 0755);
	snprintf(p, sizeof p, "%s/Hit Game.gb", g_shelf);    write_file(p, "rom");
	snprintf(p, sizeof p, "%s/Other Game.gb", g_shelf);  write_file(p, "rom");
}

static void set_cover(const char *text)
{
	char p[512];

	snprintf(p, sizeof p, "%s/Hit Game.png", g_media);
	write_file(p, text);
}

static const char *cover(void)
{
	static char buf[64];
	char p[512];
	FILE *f;
	size_t n;

	snprintf(p, sizeof p, "%s/Hit Game.png", g_media);
	if (!(f = fopen(p, "rb"))) return "(gone)";
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

static bool part_left(void)
{
	char p[512];
	struct stat st;

	snprintf(p, sizeof p, "%s/Hit Game.png.part", g_media);
	return stat(p, &st) == 0;
}

/* A whole run, to the end. Bounded, so a step machine that never finishes is a
 * failing check rather than a hung one. */
static int run(const char *only, art_progress *out)
{
	systems_cfg sys;
	int steps = 0, r;

	memset(&sys, 0, sizeof sys);
	sys.count = 1;
	snprintf(sys.systems[0].folder, sizeof sys.systems[0].folder, "Game Boy");
	snprintf(sys.systems[0].exts, sizeof sys.systems[0].exts, "gb");

	g_urls[0] = '\0';
	g_nreq = 0;
	art_begin(&sys, g_root, only);
	while ((r = art_step()) == 1 && steps < 1000) steps++;
	art_status(out);
	return r;
}

int main(void)
{
	art_progress st;
	char big[1024];

	printf("artreplace: Replace keeps what it cannot better\n");
	make_card();

	printf("  a cover libretro has replaces the old one:\n");
	set_cover("OLD");
	g_serve_image = "Hit%20Game";
	CHECK(run("Hit Game", &st) == 0, "the run did not finish");
	CHECK(!strcmp(cover(), "NEW"), "the cover reads \"%s\", wanted NEW", cover());
	CHECK(st.found == 1, "found %d, wanted 1", st.found);
	CHECK(!strstr(g_urls, "Other%20Game"),
	      "a one-game replace asked about another game:\n%s", g_urls);
	CHECK(!part_left(), "a part-file was left beside the cover");

	printf("  a cover libretro has not got leaves the old one alone:\n");
	set_cover("OLD");
	g_serve_image = NULL;
	CHECK(run("Hit Game", &st) == 0, "the run did not finish");
	CHECK(!strcmp(cover(), "OLD"),
	      "the cover reads \"%s\" - a miss must keep the one it had", cover());
	CHECK(st.found == 0, "found %d on a miss", st.found);
	CHECK(strstr(g_urls, "Hit%20Game.png") != NULL,
	      "the game was never asked for, so the miss proves nothing:\n%s", g_urls);
	CHECK(!part_left(), "a part-file was left beside the cover");

	printf("  the ordinary run still skips a game that has art:\n");
	set_cover("OLD");
	g_serve_image = NULL;
	CHECK(run(NULL, &st) == 0, "the run did not finish");
	CHECK(!strstr(g_urls, "Hit%20Game"),
	      "the ordinary run asked for a game that already has art:\n%s", g_urls);
	CHECK(strstr(g_urls, "Other%20Game") != NULL,
	      "the ordinary run did not ask for the game with no art:\n%s", g_urls);
	CHECK(!strcmp(cover(), "OLD"), "the ordinary run changed a cover it skipped");

	printf("  a name too long to hold is refused, not widened to the shelf:\n");
	memset(big, 'x', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	CHECK(run(big, &st) == 0, "the run did not finish");
	CHECK(g_nreq == 0, "%d request(s) for a refused name:\n%s", g_nreq, g_urls);

	{
		char cmd[400];
		snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
		if (system(cmd) != 0) printf("  (could not remove %s)\n", g_root);
	}

	if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
	printf("\nok: every check passed\n");
	return 0;
}
