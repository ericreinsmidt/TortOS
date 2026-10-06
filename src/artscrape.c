/* SPDX-License-Identifier: MIT */
/* See artscrape.h for where the rules came from and why they are these. */
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/stat.h>
#include <time.h>

#include "artscrape.h"
#include "artshrink.h"
#include "config.h"
#include "library.h"
#include "net.h"
#include "ss.h"
#include "ssrun.h"

#define BASE "https://thumbnails.libretro.com"

/* No-Intro's list for a collection, as libretro-database carries it: every dump
 * with its CRC32, under the name libretro files its covers by. Fetched when a
 * run needs it, like the index, rather than shipped. See the checksum pass. */
#define DAT_BASE "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/no-intro"

/* Composed paths and URLs get their own sizes rather than reusing LIB_PATH.
 *
 * A path here is a directory plus ".media" plus a ROM stem plus ".png", and a
 * URL is a host plus two encoded segments where encoding can triple a byte.
 * Sizing each from what actually goes in it is what stops the cross
 * compiler's -Wformat-truncation being a warning anybody has to learn to
 * ignore - and clang says nothing about any of this, so the device build is
 * the only place it shows up. */
#define NAME_MAX_   192
#define ARTPATH_MAX (LIB_PATH + NAME_MAX_ * 2 + 32)
#define ARTURL_MAX  (sizeof BASE + 384 + NAME_MAX_ * 3 + 32)

/* MEASURED, 2026-08-30, not guessed:
 *
 *     Nintendo - Nintendo Entertainment System   4.05 MB   13418 entries
 *     Nintendo - Super Nintendo Entertainment    0.97 MB    3676 entries
 *     Sega - Mega Drive - Genesis                0.62 MB    2355 entries
 *
 * The first version of this file said "a few hundred entries, the largest
 * measured was SNES at about 250KB" and sized the buffer at 512KB. Nothing
 * had been measured; the sentence was written as though it had. NES arrived
 * eight times over that, was truncated to its first eighth, and the match
 * rate fell from the tool's 97% to 72% - which looks exactly like libretro
 * not having the art.
 *
 * Eight megabytes because the largest real index is four and the collection
 * grows. Allocated only while the screen is open. */
#define INDEX_MAX (8 * 1024 * 1024)
#define ENTRIES_MAX 4096

/* The folder in systems.cfg, and what libretro calls the same machine.
 *
 * A table rather than a guess. The names are a catalog's, not a pattern:
 * nothing derives "Sega - Mega Drive - Genesis" from "Genesis". A folder
 * missing from here is REPORTED, not skipped quietly - systems.cfg gains
 * entries over time, and a system silently passed over looks exactly like a
 * system whose art is already complete. */
/* A shelf can map to MORE THAN ONE collection, because libretro files a
 * machine's disc games separately from its cartridges. TurboGrafx-16 is the
 * case that exposed it: the card had 27 games with no art and 23 of them were
 * sitting in "NEC - PC Engine CD - TurboGrafx-CD", 946 entries this scraper
 * had never looked at. Reported as missing art for months; it was a missing
 * lookup. Sega CD is the same shape.
 *
 * The second collection is only fetched if the first left something unmatched,
 * so a cartridge-only shelf costs exactly what it did before. */
static const struct { const char *folder, *remote, *remote2; } MAP[] = {
	{ "NES",              "Nintendo - Nintendo Entertainment System",
	                      "Nintendo - Family Computer Disk System" },
	{ "SNES",             "Nintendo - Super Nintendo Entertainment System", NULL },
	{ "Game Boy",         "Nintendo - Game Boy", NULL },
	{ "Game Boy Color",   "Nintendo - Game Boy Color", NULL },
	{ "Game Boy Advance", "Nintendo - Game Boy Advance", NULL },
	{ "Genesis",          "Sega - Mega Drive - Genesis",
	                      "Sega - Mega-CD - Sega CD" },
	{ "Master System",    "Sega - Master System - Mark III", NULL },
	{ "Game Gear",        "Sega - Game Gear", NULL },
	{ "TurboGrafx-16",    "NEC - PC Engine - TurboGrafx 16",
	                      "NEC - PC Engine CD - TurboGrafx-CD" },
	/* Two machines, two catalogs. The mono Pocket's art is NOT in the Color
	 * repo - checked, it 404s - which is the whole reason they are separate
	 * shelves rather than one mixed one. */
	{ "Neo Geo Pocket",       "SNK - Neo Geo Pocket", NULL },
	{ "Neo Geo Pocket Color", "SNK - Neo Geo Pocket Color", NULL },
};
#define MAP_N ((int)(sizeof MAP / sizeof MAP[0]))

/* The nth collection for a shelf, or NULL when it has no more. */
static const char *remote_nth(const char *folder, int n)
{
	int i;

	for (i = 0; i < MAP_N; i++)
		if (!strcmp(MAP[i].folder, folder))
			return n == 0 ? MAP[i].remote : n == 1 ? MAP[i].remote2 : NULL;
	return NULL;
}

static const char *remote_for(const char *folder)
{
	int i;

	for (i = 0; i < MAP_N; i++)
		if (!strcmp(MAP[i].folder, folder)) return MAP[i].remote;
	return NULL;
}

/* ---- the normalization, which is the whole matching rule ---------------- */

/* A title with every parenthesized tag removed, lowercased, and reduced to
 * single spaces between alphanumerics.
 *
 * Region, language, revision and dump tags are exactly what differs between
 * two catalogs of the same game, and they are never what distinguishes two
 * different games.
 *
 * This mirrors scrape-art.py's norm() EXACTLY, including where that is naive:
 *
 *     re.sub(r"\([^)]*\)", " ", s)
 *     re.sub(r"[^a-z0-9]+", " ", s.lower())
 *     " ".join(s.split())
 *
 * `\([^)]*\)` is not depth-aware and does not require a closing bracket to
 * exist. So "(a (b) c)" loses only "(a (b)" and leaves "c)", and an unclosed
 * "(tag" is left alone entirely and becomes "tag".
 *
 * The first version of this function tracked nesting and dropped everything
 * after an unmatched bracket - better behavior by any reading, and wrong.
 * The two sides have to reduce a title identically or a game that matched on
 * the host stops matching on the device, and the measured 97% is the Python's
 * number. tools/artscrape-check.c caught both on its first run. */
void art_norm(const char *in, char *out, size_t outn)
{
	size_t o = 0;
	bool sp = true;                 /* leading spaces are dropped */

	while (*in && o + 1 < outn) {
		unsigned char c = (unsigned char)*in;

		/* Only a bracket with a closer somewhere after it opens a tag, and
		 * the tag ends at the FIRST closer - not the balanced one. */
		if (c == '(') {
			const char *close = strchr(in, ')');

			if (close) {
				in = close + 1;
				if (!sp) { out[o++] = ' '; sp = true; }
				continue;
			}
		}

		in++;
		if (isalnum(c)) {
			out[o++] = (char)tolower(c);
			sp = false;
		} else if (!sp) {
			out[o++] = ' ';
			sp = true;
		}
	}
	while (o > 0 && out[o - 1] == ' ') o--;    /* and trailing ones */
	out[o] = '\0';
}

/* ---- choosing between candidates that normalize alike ------------------ */

#define TAG_MAX   24
#define TAGS_MAX  12

/* Two catalogs spell the same region differently. No-Intro says USA and
 * Europe; the TOSEC-style names libretro also carries say US and EU. Without
 * this, "Contra (1988-02)(Konami)(US)" shares nothing with "Contra (USA)" and
 * scores the same as the Japanese release. */
static const struct { const char *from, *to; } TAG_ALIAS[] = {
	{ "us",     "usa" },
	{ "eu",     "europe" },
	{ "jp",     "japan" },
	{ "world",  "usa" },        /* a World dump is the one a US card wants */
};

/* GoodTools' region codes, a letter a region and several run together: (U),
 * (J), (E), (UJE). Much of a card named before No-Intro looks like this, and
 * unread, a (U) game scored the US and Japanese covers alike and took
 * whichever came first: Vigilante (U) got Japan's box, 2026-10-05. Read only
 * from a tag of one to three of these letters and nothing else, and only once
 * TAG_ALIAS has passed on it, which already claims "us", "eu" and "jp". */
static const struct { char code; const char *region; } GOODTOOLS[] = {
	{ 'u', "usa" }, { 'j', "japan" }, { 'e', "europe" },
	{ 'w', "usa" },             /* World, as TAG_ALIAS reads it */
	{ 'k', "korea" },
};

static const char *goodtools_region(char code)
{
	size_t k;

	for (k = 0; k < sizeof GOODTOOLS / sizeof GOODTOOLS[0]; k++)
		if (GOODTOOLS[k].code == code) return GOODTOOLS[k].region;
	return NULL;
}

/* Tags likely to mean "not the release you have": prototypes, betas, and the
 * demo discs that share a title with the game. A candidate carrying one of
 * these is only picked when nothing else matched. */
static const char *TAG_BAD[] = {
	"beta", "proto", "prototype", "sample", "demo", "alpha", "hack", "unl",
};

/* Every parenthesized group in `s`, split on commas, lowercased. "Sonic (USA,
 * Europe, Brazil) (En)" gives usa, europe, brazil, en. */
static int name_tags(const char *s, char out[][TAG_MAX], int max)
{
	int n = 0;

	while (*s && n < max) {
		const char *e;
		size_t len;

		if (*s != '(') { s++; continue; }
		s++;
		e = strchr(s, ')');
		if (!e) break;
		while (s < e && n < max) {
			const char *c = s;
			size_t i, o = 0;

			while (c < e && *c != ',') c++;
			len = (size_t)(c - s);
			for (i = 0; i < len && o + 1 < TAG_MAX; i++) {
				unsigned char ch = (unsigned char)s[i];

				if (isalnum(ch)) out[n][o++] = (char)tolower(ch);
				else if (o && out[n][o - 1] != ' ') out[n][o++] = ' ';
			}
			while (o && out[n][o - 1] == ' ') o--;
			out[n][o] = '\0';
			if (o) {
				size_t k;
				bool aliased = false;

				for (k = 0; k < sizeof TAG_ALIAS / sizeof TAG_ALIAS[0]; k++)
					if (!strcmp(out[n], TAG_ALIAS[k].from)) {
						snprintf(out[n], TAG_MAX, "%s", TAG_ALIAS[k].to);
						aliased = true;
						break;
					}
				/* A GoodTools code becomes one tag a region it names. */
				if (!aliased && o <= 3) {
					char codes[4];

					snprintf(codes, sizeof codes, "%s", out[n]);
					for (k = 0; codes[k] && goodtools_region(codes[k]); k++) { }
					if (!codes[k]) {
						for (k = 0; codes[k] && n < max; k++)
							snprintf(out[n++], TAG_MAX, "%s", goodtools_region(codes[k]));
						s = c < e ? c + 1 : e;
						continue;
					}
				}
				n++;
			}
			s = c < e ? c + 1 : e;
		}
		s = e + 1;
	}
	return n;
}

int art_tag_score(const char *want, const char *cand)
{
	char w[TAGS_MAX][TAG_MAX], c[TAGS_MAX][TAG_MAX];
	int nw = name_tags(want, w, TAGS_MAX);
	int nc = name_tags(cand, c, TAGS_MAX);
	int i, j, score = 0;

	/* REGION HAS TO DOMINATE, and the weights are what make it. This is box
	 * art: what the box looks like is decided by which region pressed it, so
	 * a prototype of the right release still shows the right box while a
	 * different region shows a different one.
	 *
	 * The first version penalized a bad-dump tag by 8, enough to drop
	 * "Blaster Master (USA) (Beta)" below "Blaster Master (Japan) (Virtual
	 * Console)" - a Japanese box for a US card, chosen deliberately. The
	 * check caught it on its first run. A dump tag breaks ties inside a
	 * region now; it never flips one.
	 *
	 *     shared tag   +3     the region matched
	 *     stray tag    -1     it carries something we did not ask for
	 *     missing tag  -2     we asked for something it does not have
	 *     bad dump     -2     beta, proto, sample - on top of the stray
	 */
	for (i = 0; i < nc; i++) {
		bool shared = false;
		size_t k;

		for (j = 0; j < nw; j++)
			if (!strcmp(c[i], w[j])) { shared = true; break; }
		if (shared) { score += 3; continue; }
		score -= 1;

		for (k = 0; k < sizeof TAG_BAD / sizeof TAG_BAD[0]; k++)
			if (!strcmp(c[i], TAG_BAD[k])) { score -= 2; break; }
	}
	/* A tag we wanted and did not get costs more than a stray one, or an
	 * untagged "Contra" would beat "Contra (USA)" for a USA card. */
	for (j = 0; j < nw; j++) {
		bool shared = false;

		for (i = 0; i < nc; i++)
			if (!strcmp(c[i], w[j])) { shared = true; break; }
		if (!shared) score -= 2;
	}
	return score;
}

/* ---- percent-encoding a path segment ----------------------------------- */

/* For a URL, so spaces and the punctuation in "Sega - Mega Drive - Genesis"
 * survive. Unreserved characters pass; everything else is escaped, including
 * '/' - each of these is ONE segment, and a name containing a slash must not
 * become two. */
/* Moved to net.c when a second scraper needed it. Kept under the old name so
 * the call sites below read as they did. */
static void urlenc(const char *in, char *out, size_t outn)
{
	net_urlencode(in, out, outn);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void urldec(const char *in, char *out, size_t outn)
{
	size_t o = 0;

	while (*in && o + 1 < outn) {
		if (*in == '%' && hexval(in[1]) >= 0 && hexval(in[2]) >= 0) {
			out[o++] = (char)(hexval(in[1]) * 16 + hexval(in[2]));
			in += 3;
		} else {
			out[o++] = *in++;
		}
	}
	out[o] = '\0';
}

/* ---- state -------------------------------------------------------------- */

/* The whole index, held as it arrived.
 *
 * Two parallel arrays of decoded and normalized names used to sit beside this.
 * Once the real index sizes were known they would have been 13418 x 192 bytes
 * each - five megabytes of copies of strings already in the buffer, and an
 * entry cap that was the other half of the truncation bug. match() scans the
 * HTML instead: one pass per ROM, normalizing each candidate as it goes. That
 * is 13418 short normalizations per ROM on the worst system, which is
 * microseconds, and there is nothing left to overflow. */
static char   *g_html;
static int     g_nnames;      /* what the index held; for the message only */

/* The collection's No-Intro list, while its checksum pass runs, and whether
 * that pass has run for the collection in hand. */
static char   *g_dat;
static bool    g_crc_tried;

typedef struct { char folder[CFG_STR]; char exts[CFG_STR]; } sysrow;
static sysrow  g_sys[CFG_MAX_SYSTEMS];
static int     g_nsys;
static char    g_romdir[LIB_PATH];

/* Where the step machine is. Split this way because a request must not be
 * waited for: these are called from a screen's frame loop, which is also
 * where the power button is read, so anything that blocks for a timeout is a
 * device that has stopped answering its own power button. */
/* Three passes over a system, each only for what the one before it left, and
 * the order is the point.
 *
 *   P_TRY / P_TRY_WAIT     ask for <rom name>.png directly
 *   P_INDEX / P_INDEX_WAIT fetch the catalog, but only if something missed
 *   P_FUZZY / P_FUZZY_WAIT match the leftovers against it
 *   P_DAT / P_DAT_WAIT     fetch the collection's No-Intro list, if anything
 *                          is still unnamed
 *   P_CRC / P_CRC_WAIT     name those by their checksum, and match that name
 *
 * The checksum pass exists because a file's name can be one the catalog no
 * longer uses. Measured 2026-09-14 on the card: 67 games had no art under the
 * names they carry - every Neo Geo Pocket game among them - and libretro had a
 * cover for all 67 under a newer or fuller one. The CRC in a zip's header,
 * looked up in No-Intro's list, gives the name libretro uses for 29 of them.
 *
 * The index used to come first, always. It is what makes fuzzy matching
 * possible - you cannot normalize against a catalog you have not got - and
 * it is worth 97% against 83% for exact names alone. But 83% of the library
 * needs no catalog at all, and the NES index is four megabytes: adding one
 * game to a shelf downloaded four megabytes to learn a name it could have
 * simply asked for.
 *
 * So the direct request goes first and the catalog is the fallback. A first
 * run over an empty library costs the same as before plus a handful of 404s.
 * Adding a few games - the ordinary case, and the one Over The Hare makes
 * ordinary - usually costs no index at all. */
enum { P_SYSTEM, P_SS, P_SS_WAIT, P_TRY, P_TRY_WAIT, P_INDEX, P_INDEX_WAIT,
       P_FUZZY, P_FUZZY_WAIT, P_DAT, P_DAT_WAIT, P_CRC, P_CRC_WAIT };

/* ---- pass zero: ScreenScraper ------------------------------------------
 *
 * BACKLOG 27 settled the order and Replace Box Art has followed it for one
 * game since 2026-09-16: ScreenScraper when the player has an account,
 * libretro when they do not and libretro again whenever they cannot answer.
 * This is that order over a whole shelf.
 *
 * It costs nothing when there is nothing to do, because P_SYSTEM has already
 * counted what is missing and skipped the system if the answer is none. A
 * cover it fetches lands at the same .media path libretro's would, so pass one
 * skips that game without a request - the two sources compose rather than
 * competing.
 *
 * It also brings the year and the synopsis, which libretro has none of. That
 * is the real reason it goes first rather than being a fallback: a cover
 * fetched from libretro is a cover, where one fetched here is a cover and a
 * row in the games table, and asking again later for text that was in hand
 * now would cost a second run over the whole card.
 *
 * WHEN IT STOPS, which is the part that had to be got right before a run over
 * 1,708 games could exist at all:
 *
 *   the day is spent      every reply states the account's own counters, so
 *                         the run reads what is left rather than discovering
 *                         it by being refused. Under SS_DAY_FLOOR it stops and
 *                         libretro finishes the job.
 *   "too many at once"    a 429, which is the server saying not now rather
 *                         than not this game. Waits SS_BUSY_WAIT and asks the
 *                         same game once more; refused twice, it stops.
 *   anything else         that game is one they do not have. Ordinary, and the
 *                         run carries straight on to the next.
 *
 * The first two are different sentences to the same 429 status, which is why
 * the budget is read rather than inferred: a quota that is spent does not come
 * back in two seconds, and a client that retries it is hammering a server that
 * has already said no. */
#define SS_DAY_FLOOR 50    /* stop with this many of the day's requests left */
/* Seconds before a refused game is asked about again. Overridable only so the
 * check can pin the RULE - retry once, then stop - without spending the wait:
 * a suite that sleeps is a suite people stop running. */
#ifndef SS_BUSY_WAIT
#define SS_BUSY_WAIT  2
#endif
static int  g_phase;
static char g_pending[ARTPATH_MAX];       /* the image being fetched */

/* ROMs still without art after the direct pass, as indices into g_roms. */
static int  g_retry[ENTRIES_MAX];
static int  g_nretry, g_qi;

/* What one collection could not name, carried to the next one. A game is only
 * counted missing once every collection for its shelf has been tried. */
static int  g_left[ENTRIES_MAX];
static int  g_nleft;
static int  g_rem;              /* which collection, 0 or 1 */

static int  g_si;               /* which system */
static int  g_ri;               /* which ROM inside it */
static bool g_running;

static char g_roms[ENTRIES_MAX][NAME_MAX_];
/* The extension each stem came with, dot included, because ScreenScraper is
 * asked by the ROM's FILE name where libretro is asked by its stem. Rebuilding
 * it from the shelf's extension list would be a guess - a folder allows five
 * and a game carries one - and reopening the directory per game to look is a
 * readdir inside a frame. */
static char g_rext[ENTRIES_MAX][12];
static int  g_nroms;

/* The one game a replace is about, or "" for an ordinary run. See art_begin. */
static char g_only[NAME_MAX_];

/* The ScreenScraper pass, for this run only. `stop` is sticky: once the
 * account's day is spent or the server has refused twice, every remaining
 * system goes straight to libretro rather than asking again per shelf. */
static bool   g_ss_stop;
static bool   g_ss_retried;       /* this game already had its one retry */
static time_t g_ss_until;         /* not before this, after a 429 */
static char   g_ss_note[64];

static art_progress g_st;

/* ---- the log --------------------------------------------------------------
 *
 * Every request, what came back and how long it took, plus a line per system
 * and one for the run. BACKLOG 47: a scrape that felt slow on a fresh card had
 * nothing in the log but the resizes, so there was nothing to measure it by.
 *
 * The check defines ART_QUIET. It runs whole scrapes against a stub, and the
 * stub's traffic would bury its own report. */
static long g_run_t0, g_sys_t0;
static int  g_sys_found0, g_sys_missing0;
static bool g_sys_on;                  /* a system's work has begun */
static char g_asked[NAME_MAX_];        /* the name the image in flight was asked by */

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void say(const char *fmt, ...)
{
#ifdef ART_QUIET
	(void)fmt;
#else
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
#endif
}

/* How the request that just finished went: "HTTP 404, 312 ms", or "no answer
 * (curl 28), 60004 ms" when it never reached a server. */
static const char *answer(void)
{
	static char buf[64];

	if (net_async_http() == 0)
		snprintf(buf, sizeof buf, "no answer (curl %d), %d ms",
		         net_async_exit(), net_async_ms());
	else
		snprintf(buf, sizeof buf, "HTTP %d, %d ms",
		         net_async_http(), net_async_ms());
	return buf;
}

/* ---- reading the library ------------------------------------------------ */

static bool ext_allowed(const char *name, const char *exts)
{
	const char *dot = strrchr(name, '.');
	char e[16], list[160], *tok;

	if (!exts[0]) return true;
	if (!dot || !dot[1]) return false;
	snprintf(e, sizeof e, "%s", dot + 1);
	for (tok = e; *tok; tok++) *tok = (char)tolower((unsigned char)*tok);

	snprintf(list, sizeof list, "%s", exts);
	for (tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
		while (*tok == ' ') tok++;
		if (!strcmp(tok, e)) return true;
	}
	return false;
}

/* The ROM stems in one folder. `.media/` and any dotfile are not games, and
 * neither is a stray text file - which is what the extension list in
 * systems.cfg is for. Without it this reports missing art for things that
 * were never going to have any. */
static void read_roms(const char *dir, const char *exts)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	g_nroms = 0;
	if (!d) return;
	while ((e = readdir(d)) && g_nroms < ENTRIES_MAX) {
		char full[ARTPATH_MAX], *dot;
		struct stat st;

		if (e->d_name[0] == '.') continue;
		/* A path or a name cut short does not crash, it names a DIFFERENT
		 * file - so truncation is skipped rather than survived. */
		if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name)
		    >= (int)sizeof full) continue;
		if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		if (!ext_allowed(e->d_name, exts)) continue;
		if (snprintf(g_roms[g_nroms], NAME_MAX_, "%s", e->d_name)
		    >= NAME_MAX_) continue;
		g_rext[g_nroms][0] = '\0';
		dot = strrchr(g_roms[g_nroms], '.');
		if (dot) {
			snprintf(g_rext[g_nroms], sizeof g_rext[0], "%s", dot);
			*dot = '\0';
		}
		g_nroms++;
	}
	closedir(d);
}

/* ---- the index ---------------------------------------------------------- */

/* Every .png href in one directory listing.
 *
 * A scan for `href="..."` ending in .png rather than an HTML parse, which is
 * what the Python does and is all this needs: the page is a generated file
 * listing, not a document. */
#define INDEX_TMP "/tmp/tortos-artindex"

static bool start_index(const char *remote)
{
	char url[ARTURL_MAX], enc[384];

	urlenc(remote, enc, sizeof enc);
	snprintf(url, sizeof url, BASE "/%s/Named_Boxarts/", enc);
	return net_get_async(url, INDEX_TMP, 60);
}

/* Every .png href in one directory listing.
 *
 * A scan for `href="..."` ending in .png rather than an HTML parse, which is
 * what the Python does and is all this needs: the page is a generated file
 * listing, not a document. */
static bool parse_index(void)
{
	FILE *f = fopen(INDEX_TMP, "rb");
	const char *p;
	size_t n;

	g_nnames = 0;
	if (!f) return false;
	n = fread(g_html, 1, INDEX_MAX - 1, f);
	fclose(f);
	remove(INDEX_TMP);
	g_html[n] = '\0';

	/* A full buffer means the index outgrew it and the tail is missing, which
	 * shows up as games libretro "does not have". Refuse, rather than match
	 * against a fraction of the catalog and report the difference as
	 * missing art. */
	if (n >= INDEX_MAX - 1) return false;

	for (p = g_html; (p = strstr(p, "href=\"")); ) {
		const char *q;

		p += 6;
		q = strchr(p, '"');
		if (!q) break;
		if (q - p >= 5 && !strncmp(q - 4, ".png", 4)) g_nnames++;
	}
	return g_nnames > 0;
}

/* Exact filename first, then normalized. In that order because an exact hit is
 * unambiguous and a normalized one can collide - two dumps of the same game
 * normalize alike, and the first is as good an answer as any.
 *
 * One pass over the index per ROM, decoding and normalizing candidates as it
 * goes. `out` takes the winning name exactly as libretro spells it, because
 * that is what the download URL needs. */
/* One catalog name weighed against the ROM: true for an exact hit, which ends
 * the search; a normalized one is kept in `out` if it scores best so far. The
 * rule both index forms share - libretro's listing page here, a list of names
 * from the web page in art_match_list - so there is one copy of it. */
static bool consider(const char *base, const char *nb, const char *cand,
                     char *out, size_t outn, bool *have_norm, int *best)
{
	char cnorm[NAME_MAX_];

	if (!strcmp(cand, base)) {                      /* exact: nothing beats it */
		snprintf(out, outn, "%s", cand);
		return true;
	}
	if (nb[0]) {
		art_norm(cand, cnorm, sizeof cnorm);
		if (!strcmp(cnorm, nb)) {
			/* Several entries normalize alike - 1703 of NES's 13418 - and
			 * taking the first meant taking whichever sorted first, which
			 * is a Japanese release as often as not. Score them on how
			 * well their region and language tags overlap the card's. */
			int sc = art_tag_score(base, cand);

			if (!*have_norm || sc > *best) {
				*best = sc;
				snprintf(out, outn, "%s", cand);
				*have_norm = true;
			}
		}
	}
	return false;
}

static bool match(const char *base, char *out, size_t outn)
{
	char nb[NAME_MAX_], cand[NAME_MAX_];
	const char *p;
	bool have_norm = false;
	int best = 0;

	art_norm(base, nb, sizeof nb);

	for (p = g_html; (p = strstr(p, "href=\"")); ) {
		const char *q;
		size_t len;
		char raw[NAME_MAX_ * 2];

		p += 6;
		q = strchr(p, '"');
		if (!q) break;
		len = (size_t)(q - p);
		if (len < 5 || len >= sizeof raw) continue;
		if (strncmp(q - 4, ".png", 4) != 0) continue;

		snprintf(raw, sizeof raw, "%.*s", (int)(len - 4), p);   /* drop .png */
		urldec(raw, cand, sizeof cand);
		if (consider(base, nb, cand, out, outn, &have_norm, &best)) return true;
	}
	return have_norm;
}

/* ---- the loose pass ------------------------------------------------------
 *
 * The last try, only for a game the exact, normalized and checksum passes all
 * missed, so what they match is unchanged. art_norm stays the Python's rule;
 * this reads past what separates the naming conventions instead:
 *
 *   GoodTools' [...] flags   [!] [c] [a1] [b1] [T+Eng], dropped like (...)
 *   "The"                    first or last: The Legend of Zelda is Legend
 *                            of Zelda, The
 *   "&" and "and"            libretro files & as _, so Sonic & Knuckles and
 *                            Sonic and Knuckles both lose the word
 *   accents                  Pokémon is Pokemon, where art_norm drops the
 *                            letter and splits the word
 *
 * Eric's ask, 2026-10-05. Measured on the GKD Pixel 2's card the same day:
 * see tools/artscrape-check.c. */

/* A Latin letter for an accented one, UTF-8 Latin-1 range (C3 xx), or 0. */
static char unaccent(unsigned char b)
{
	static const char MAP[65] =
		"AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"
		"aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";

	return b >= 0x80 && b <= 0xBF ? MAP[b - 0x80] : 0;
}

void art_norm_loose(const char *in, char *out, size_t outn)
{
	char flat[NAME_MAX_ * 2], words[NAME_MAX_];
	size_t o = 0, w = 0;
	int depth = 0;
	char *tok, *save, *first = NULL, *last = NULL, *v[64];
	int nv = 0, i;

	/* Tags out, both kinds, accents folded, & to a space. And a "The" that
	 * opens a subtitle: No-Intro writes SNK vs. Capcom - The Match of the
	 * Millennium where a card has SNK Vs Capcom - Match of The Millennium
	 * (2026-10-05); "the" first or last is dropped below, once the " - " that
	 * marks a subtitle is gone. */
	for (; *in && o + 1 < sizeof flat; in++) {
		unsigned char c = (unsigned char)*in;

		if (!depth && !strncmp(in, " - ", 3) && !strncasecmp(in + 3, "the ", 4)) {
			flat[o++] = ' ';
			in += 6;                                   /* past " - the" */
			continue;
		}

		if (c == '(' || c == '[') { depth++; continue; }
		if ((c == ')' || c == ']') && depth) { depth--; flat[o++] = ' '; continue; }
		if (depth) continue;
		if (c == 0xC3 && in[1] && unaccent((unsigned char)in[1])) {
			unsigned char u = (unsigned char)unaccent((unsigned char)*++in);

			flat[o++] = isalnum(u) ? (char)tolower(u) : ' ';
			continue;
		}
		flat[o++] = isalnum(c) ? (char)tolower(c) : ' ';
	}
	flat[o] = '\0';

	/* "and" anywhere, "the" first or last. */
	for (tok = strtok_r(flat, " ", &save); tok && nv < 64; tok = strtok_r(NULL, " ", &save))
		if (strcmp(tok, "and")) v[nv++] = tok;
	if (nv > 1 && !strcmp(v[0], "the")) first = v[0];
	if (nv > 1 && !strcmp(v[nv - 1], "the")) last = v[nv - 1];
	for (i = 0; i < nv; i++) {
		size_t len = strlen(v[i]);

		if (v[i] == first || v[i] == last) continue;
		if (w + len + 2 > sizeof words) break;
		if (w) words[w++] = ' ';
		memcpy(words + w, v[i], len);
		w += len;
	}
	words[w] = '\0';
	snprintf(out, outn, "%s", words);
}

/* consider(), loosely: a hit is kept if its tags score best so far. */
static void consider_loose(const char *base, const char *lb, const char *cand,
                           char *out, size_t outn, bool *have, int *best)
{
	char cl[NAME_MAX_];
	int sc;

	art_norm_loose(cand, cl, sizeof cl);
	if (!lb[0] || strcmp(cl, lb)) return;
	sc = art_tag_score(base, cand);
	if (!*have || sc > *best) {
		*best = sc;
		snprintf(out, outn, "%s", cand);
		*have = true;
	}
}

/* The loose pass against libretro's listing page, as match() reads it. */
static bool match_loose(const char *base, char *out, size_t outn)
{
	char lb[NAME_MAX_], cand[NAME_MAX_];
	const char *p;
	bool have = false;
	int best = 0;

	art_norm_loose(base, lb, sizeof lb);
	for (p = g_html; p && (p = strstr(p, "href=\"")); ) {
		const char *q;
		size_t len;
		char raw[NAME_MAX_ * 2];

		p += 6;
		q = strchr(p, '"');
		if (!q) break;
		len = (size_t)(q - p);
		if (len < 5 || len >= sizeof raw) continue;
		if (strncmp(q - 4, ".png", 4) != 0) continue;
		snprintf(raw, sizeof raw, "%.*s", (int)(len - 4), p);
		urldec(raw, cand, sizeof cand);
		consider_loose(base, lb, cand, out, outn, &have, &best);
	}
	return have;
}

bool art_match_loose(const char *base, const char *names, char *out, size_t outn)
{
	char lb[NAME_MAX_], cand[NAME_MAX_];
	const char *p, *eol;
	bool have = false;
	int best = 0;

	art_norm_loose(base, lb, sizeof lb);
	for (p = names; *p; p = *eol ? eol + 1 : eol) {
		size_t len;

		eol = strchr(p, '\n');
		if (!eol) eol = p + strlen(p);
		len = (size_t)(eol - p);
		if (len && p[len - 1] == '\r') len--;
		if (!len || len >= sizeof cand) continue;
		memcpy(cand, p, len);
		cand[len] = '\0';
		consider_loose(base, lb, cand, out, outn, &have, &best);
	}
	return have;
}

/* ---- the subtitle rule ---------------------------------------------------
 *
 * The very last try, after the loose pass: a card title with no subtitle
 * matching a catalog title that adds one. libretro files the Neo Geo Pocket's
 * games under No-Intro's full names - Baseball Stars - Pocket Sports Series
 * (Japan, Europe) (En,Ja) - where a card carries Baseball Stars (World), and
 * three of the GKD Pixel 2 card's seven were found no other way (Eric's ask,
 * 2026-10-05).
 *
 * Only when every catalog title it fits is the SAME game, its title and
 * subtitle alike, differing only in tags: Mega Man fits Mega Man - Dr. Wily's
 * Revenge and Mega Man - Xtreme, two games, and matches neither. A wrong cover
 * is worse than none. What it cannot rule out is a catalog that has only a
 * different game under the same main title, which is why it is last. */

/* A title without its tags, and whether it has a subtitle there: a " - "
 * before the first "(" or "[". */
static size_t title_len(const char *s)
{
	const char *tag = strpbrk(s, "([");

	return tag ? (size_t)(tag - s) : strlen(s);
}

static const char *subtitle_at(const char *s)
{
	const char *sub = strstr(s, " - ");

	return sub && (size_t)(sub - s) < title_len(s) ? sub : NULL;
}

bool art_match_subtitle(const char *base, const char *names, char *out, size_t outn)
{
	char lb[NAME_MAX_], cand[NAME_MAX_], part[NAME_MAX_], cl[NAME_MAX_];
	char game[NAME_MAX_] = "";
	const char *p, *eol, *sub;
	bool have = false;
	int best = 0;

	if (subtitle_at(base)) return false;
	art_norm_loose(base, lb, sizeof lb);
	if (!lb[0]) return false;
	for (p = names; *p; p = *eol ? eol + 1 : eol) {
		size_t len;
		int sc;

		eol = strchr(p, '\n');
		if (!eol) eol = p + strlen(p);
		len = (size_t)(eol - p);
		if (len && p[len - 1] == '\r') len--;
		if (!len || len >= sizeof cand) continue;
		memcpy(cand, p, len);
		cand[len] = '\0';
		if (!(sub = subtitle_at(cand))) continue;

		snprintf(part, sizeof part, "%.*s", (int)(sub - cand), cand);
		art_norm_loose(part, cl, sizeof cl);
		if (strcmp(cl, lb)) continue;
		/* The whole title, subtitle and all, untagged: which game it is. */
		snprintf(part, sizeof part, "%.*s", (int)title_len(cand), cand);
		art_norm_loose(part, cl, sizeof cl);
		if (!game[0]) snprintf(game, sizeof game, "%s", cl);
		else if (strcmp(game, cl)) return false;          /* two games: neither */
		sc = art_tag_score(base, cand);
		if (!have || sc > best) {
			best = sc;
			snprintf(out, outn, "%s", cand);
			have = true;
		}
	}
	return have;
}

bool art_match_list(const char *base, const char *names, char *out, size_t outn)
{
	char nb[NAME_MAX_], cand[NAME_MAX_];
	const char *p, *eol;
	bool have_norm = false;
	int best = 0;

	art_norm(base, nb, sizeof nb);
	for (p = names; *p; p = *eol ? eol + 1 : eol) {
		size_t len;

		eol = strchr(p, '\n');
		if (!eol) eol = p + strlen(p);
		len = (size_t)(eol - p);
		if (len && p[len - 1] == '\r') len--;
		if (!len || len >= sizeof cand) continue;
		memcpy(cand, p, len);
		cand[len] = '\0';
		if (consider(base, nb, cand, out, outn, &have_norm, &best)) return true;
	}
	return have_norm;
}

const char *art_collection(const char *folder, int n)
{
	return remote_nth(folder, n);
}

/* Whether there is a cover at `path`: a file that starts as a PNG or a JPEG
 * does. Not merely one that is there - libretro's GitHub mirror stores a
 * duplicate cover as a symbolic link, and its raw files answer with the link's
 * text, the name of the file it points at. Four of those landed as "covers"
 * on 2026-10-05 and, counted as present, were never fetched again. */
static bool is_cover(const char *path)
{
	unsigned char head[4];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f) return false;
	n = fread(head, 1, sizeof head, f);
	fclose(f);
	if (n < 4) return false;
	return (head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') ||
	       (head[0] == 0xFF && head[1] == 0xD8);
}

int art_missing(const char *dir, const char *exts,
                void (*fn)(const char *stem, void *ctx), void *ctx)
{
	int i, n = 0;

	read_roms(dir, exts);
	for (i = 0; i < g_nroms; i++) {
		char have[ARTPATH_MAX];

		/* The same test a Box Art run counts by. */
		if (snprintf(have, sizeof have, "%s/.media/%s.png", dir, g_roms[i])
		    >= (int)sizeof have) continue;
		if (is_cover(have)) continue;
		if (fn) fn(g_roms[i], ctx);
		n++;
	}
	return n;
}

/* ---- the checksum ------------------------------------------------------- */

#define DAT_TMP "/tmp/tortos-artdat"

/* Eight megabytes, like the index: the largest list, NES, was 3.2MB on
 * 2026-09-14, and a list cut short would name nothing past the cut. */
#define DAT_MAX (8 * 1024 * 1024)

static bool start_dat(const char *remote)
{
	char url[ARTURL_MAX], enc[384];

	urlenc(remote, enc, sizeof enc);
	snprintf(url, sizeof url, DAT_BASE "/%s.dat", enc);
	return net_get_async(url, DAT_TMP, 60);
}

/* Into g_dat, or g_dat stays NULL. A disc collection has no No-Intro list and
 * 404s, and that is the ordinary case, not a failure. */
static void load_dat(void)
{
	FILE *f = fopen(DAT_TMP, "rb");
	long n;

	free(g_dat);
	g_dat = NULL;
	if (!f) return;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < DAT_MAX &&
	    fseek(f, 0, SEEK_SET) == 0 && (g_dat = malloc((size_t)n + 1))) {
		if (fread(g_dat, 1, (size_t)n, f) == (size_t)n) {
			g_dat[n] = '\0';
		} else {
			free(g_dat);
			g_dat = NULL;
		}
	}
	fclose(f);
	remove(DAT_TMP);
}

static uint32_t le16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

/* The CRC32 of the ROM inside <dir>/<stem>.zip, as the zip records it.
 *
 * From the central directory, so nothing is decompressed and nothing hashed:
 * a few kilobytes read, which is why this can run inside a frame. The entry
 * taken is the first one the shelf's extensions allow - a zip that carries a
 * readme beside the ROM must be named by the ROM - or the only file, when the
 * list allows none of them. Zips only: 1,680 of the card's ROMs are zips, and
 * a loose file would have to be read whole. */
bool art_rom_crc(const char *dir, const char *stem, const char *exts,
                 uint32_t *crc)
{
	static unsigned char tail[65536 + 22];
	char path[ARTPATH_MAX], name[NAME_MAX_ * 2];
	unsigned char *cd = NULL;
	FILE *f;
	long size, want, i;
	uint32_t entries, cdsize, cdoff, off, only = 0;
	int files = 0;
	bool found = false;

	if (snprintf(path, sizeof path, "%s/%s.zip", dir, stem) >= (int)sizeof path)
		return false;
	if (!(f = fopen(path, "rb"))) return false;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 22) goto out;

	/* The end-of-central-directory record: the last thing in the file, save
	 * for a comment of up to 65535 bytes after it. */
	want = size < (long)sizeof tail ? size : (long)sizeof tail;
	if (fseek(f, size - want, SEEK_SET) != 0 ||
	    fread(tail, 1, (size_t)want, f) != (size_t)want) goto out;
	for (i = want - 22; i >= 0; i--)
		if (le32(tail + i) == 0x06054b50) break;
	if (i < 0) goto out;
	entries = le16(tail + i + 10);
	cdsize  = le32(tail + i + 12);
	cdoff   = le32(tail + i + 16);
	if (cdsize == 0 || cdsize > 1024 * 1024 || (long)cdoff + (long)cdsize > size)
		goto out;

	if (!(cd = malloc(cdsize)) || fseek(f, (long)cdoff, SEEK_SET) != 0 ||
	    fread(cd, 1, cdsize, f) != cdsize) goto out;

	for (off = 0; entries-- > 0 && off + 46 <= cdsize; ) {
		uint32_t nl, xl, cl;

		if (le32(cd + off) != 0x02014b50) break;
		nl = le16(cd + off + 28);
		xl = le16(cd + off + 30);
		cl = le16(cd + off + 32);
		if (off + 46 + nl > cdsize || nl >= sizeof name) break;
		memcpy(name, cd + off + 46, nl);
		name[nl] = '\0';
		if (nl && name[nl - 1] != '/') {
			if (files++ == 0) only = le32(cd + off + 16);
			if (ext_allowed(name, exts)) {
				*crc = le32(cd + off + 16);
				found = true;
				break;
			}
		}
		off += 46 + nl + xl + cl;
	}
	if (!found && files == 1) {
		*crc = only;
		found = true;
	}
out:
	free(cd);
	fclose(f);
	return found;
}

/* The name No-Intro gives the dump with this CRC, from g_dat.
 *
 * The list is clrmamepro text: a `game (` block holding its `name "..."` and
 * one `rom ( ... crc XXXXXXXX ... )` line per file. So the CRC is found as
 * text and the name is the block's own, the nearest `game (` above it. */
static bool dat_name(uint32_t crc, char *out, size_t outn)
{
	return art_dat_name(g_dat, crc, out, outn);
}

bool art_dat_name(const char *dat, uint32_t crc, char *out, size_t outn)
{
	char needle[20];
	const char *hit, *game, *p, *q;

	if (!dat) return false;
	snprintf(needle, sizeof needle, " crc %08X ", (unsigned)crc);
	if (!(hit = strstr(dat, needle))) {
		snprintf(needle, sizeof needle, " crc %08x ", (unsigned)crc);
		if (!(hit = strstr(dat, needle))) return false;
	}
	for (game = hit; game > dat; game--)
		if (game[-1] == '\n' && !strncmp(game, "game (", 6)) break;
	if (strncmp(game, "game (", 6) != 0) return false;
	if (!(p = strstr(game, "name \"")) || p > hit) return false;
	p += 6;
	if (!(q = strchr(p, '"')) || q > hit || (size_t)(q - p) >= outn) return false;
	memcpy(out, p, (size_t)(q - p));
	out[q - p] = '\0';
	return true;
}

/* ---- driving ------------------------------------------------------------ */

void art_cancel(void)
{
	/* Whatever was in flight is not wanted, and leaving it in flight holds
	 * the one async slot the launcher has. */
	net_async_abort();
	/* The ScreenScraper pass holds the same one async slot through its own
	 * driver, so abandoning the run has to tell that half too. */
	ss_run_cancel();
	g_running = false;
	free(g_html);  g_html = NULL;
	free(g_dat);   g_dat = NULL;
	g_nnames = 0;
}

void art_begin(const systems_cfg *sys, const char *roms_dir, const char *only)
{
	int i;

	art_cancel();
	memset(&g_st, 0, sizeof g_st);
	g_si = g_ri = 0;
	g_nretry = g_qi = 0;
	g_nleft = g_rem = 0;
	g_ss_stop = g_ss_retried = false;
	g_ss_until = 0;
	g_ss_note[0] = '\0';
	g_run_t0 = now_ms();
	g_sys_on = false;
	g_phase = P_SYSTEM;
	g_nsys = 0;
	snprintf(g_romdir, sizeof g_romdir, "%s", roms_dir ? roms_dir : "");
	if (!sys) return;

	/* A name cut short names a different game, so it is refused rather than
	 * run - and refused as a run with nothing in it, which finishes on the
	 * first step, not by leaving g_only empty, which would quietly become a
	 * whole-system scrape. */
	g_only[0] = '\0';
	if (only && *only &&
	    snprintf(g_only, sizeof g_only, "%s", only) >= (int)sizeof g_only) {
		g_only[0] = '\0';
		sys = NULL;
	}

	for (i = 0; sys && i < sys->count && g_nsys < CFG_MAX_SYSTEMS; i++) {
		char dir[ARTPATH_MAX];
		struct stat st;

		/* Only shelves with a ROM folder on the card. The launcher appends a
		 * Favorites shelf to this list - games drawn from every system, with
		 * no folder of its own - and counting it made the screen say "10 of
		 * 10" for a nine-system library, with one of the ten always skipped.
		 * A shelf that is not a folder has no art to fetch.
		 *
		 * The empty name is tested FIRST, and on its own. Asking whether the
		 * folder exists looks like it covers this and does not: Favorites has
		 * folder "", so the path built below is the ROM root with a trailing
		 * slash, which exists and is a directory - so the check passed it
		 * through and the count went back to ten the moment anyone favorited
		 * a game. A shelf with no folder is not a shelf with a folder that
		 * happens to be missing. */
		if (!sys->systems[i].folder[0]) continue;
		if (snprintf(dir, sizeof dir, "%s/%s", g_romdir,
		             sys->systems[i].folder) >= (int)sizeof dir) continue;
		if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

		snprintf(g_sys[g_nsys].folder, CFG_STR, "%s", sys->systems[i].folder);
		snprintf(g_sys[g_nsys].exts,   CFG_STR, "%s", sys->systems[i].exts);
		g_nsys++;
	}
	g_st.systems = g_nsys;

	g_html = malloc(INDEX_MAX);
	if (!g_html) { art_cancel(); return; }
	g_running = true;
}

/* <dir>/.media and <dir>/.media/<stem>.png, or false if either would be cut
 * short - a truncated path names a different file, which is worse than
 * failing. */
static bool art_paths(const char *dir, const char *stem,
                      char *media, size_t mn, char *dest, size_t dn)
{
	if (snprintf(media, mn, "%s/.media", dir) >= (int)mn) return false;
	if (snprintf(dest, dn, "%s/%s.png", media, stem) >= (int)dn) return false;
	return true;
}

/* Ask for one image by the name libretro would file it under. Used by both
 * passes: pass one guesses the card's own name, pass two passes the name the
 * catalog gave. */
static bool start_image(const char *remote, const char *name, const char *dest)
{
	char url[ARTURL_MAX], er[384], en[NAME_MAX_ * 3 + 1];

	urlenc(remote, er, sizeof er);
	urlenc(name, en, sizeof en);
	snprintf(url, sizeof url, BASE "/%s/Named_Boxarts/%s.png", er, en);
	snprintf(g_pending, sizeof g_pending, "%s", dest);
	snprintf(g_asked, sizeof g_asked, "%s", name);
	return net_get_async(url, g_pending, 60);
}

/* Finish with this system and move to the next, saying why.
 *
 * Every one of these paths used to be silent, and a run where all of them
 * fired looked identical to a run with nothing to do: ten systems, zero
 * found, zero missing, zero already. That is not a state a person can debug
 * from, and it is the state the first device run produced. */
static int next_system(const char *why)
{
	if (why) {
		snprintf(g_st.now, sizeof g_st.now, "%s: %s",
		         g_si < g_nsys ? g_sys[g_si].folder : "?", why);
		snprintf(g_st.problem, sizeof g_st.problem, "%s", g_st.now);
		fprintf(stderr, "art: %s\n", g_st.now);
	}
	if (g_sys_on)
		say("art: %s: %d found, %d missing, in %ld s\n", g_sys[g_si].folder,
		    g_st.found - g_sys_found0, g_st.missing - g_sys_missing0,
		    (now_ms() - g_sys_t0 + 500) / 1000);
	g_sys_on = false;
	g_si++;
	g_st.systems_done++;
	g_phase = P_SYSTEM;
	return 1;
}

int art_step(void)
{
	char dir[ARTPATH_MAX], media[ARTPATH_MAX], dest[ARTPATH_MAX];
	const char *remote;
	char        hitbuf[NAME_MAX_];
	struct stat st;

	if (!g_running) return -1;
	if (g_si >= g_nsys) {
		say("art: finished in %ld s: %d found, %d missing, %d had one\n",
		    (now_ms() - g_run_t0 + 500) / 1000, g_st.found, g_st.missing,
		    g_st.skipped);
		art_cancel();
		return 0;
	}

	snprintf(dir, sizeof dir, "%s/%s", g_romdir, g_sys[g_si].folder);

	switch (g_phase) {
	case P_SYSTEM:
		if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
			return next_system("no such folder");
		remote = remote_for(g_sys[g_si].folder);
		/* Loudly. systems.cfg gains entries over time, and a system silently
		 * passed over looks exactly like one whose art is already complete. */
		if (!remote) return next_system("not in the table");
		g_rem = 0;
		g_nleft = 0;
		g_crc_tried = false;
		/* A replace is the one game, taken as named rather than found in the
		 * folder: a disc game is a folder of its own, which read_roms does not
		 * list, and its cover lives at the same .media path all the same. */
		if (g_only[0]) {
			snprintf(g_roms[0], NAME_MAX_, "%s", g_only);
			g_nroms = 1;
		} else {
			read_roms(dir, g_sys[g_si].exts);
		}
		if (g_nroms == 0) return next_system("no ROMs");

		/* Count what is missing BEFORE any request, and skip the system when
		 * nothing is. The skip-if-present test used to live per-ROM, after
		 * the index had already downloaded, so a second run over a complete
		 * library still pulled every index to learn it had nothing to do.
		 * A replace wants its game whether or not it has a cover. */
		{
			int i, want = 0;

			for (i = 0; i < g_nroms && g_only[0]; i++) want++;
			for (i = 0; i < g_nroms && !g_only[0]; i++) {
				char have[ARTPATH_MAX];

				if (snprintf(have, sizeof have, "%s/.media/%s.png",
				             dir, g_roms[i]) >= (int)sizeof have) continue;
				if (is_cover(have)) g_st.skipped++;
				else want++;
			}
			if (want == 0) return next_system(NULL);
		}

		snprintf(g_st.now, sizeof g_st.now, "%s", g_sys[g_si].folder);
		g_sys_t0 = now_ms();
		g_sys_found0 = g_st.found;
		g_sys_missing0 = g_st.missing;
		g_sys_on = true;
		g_ri = 0;
		g_nretry = 0;
		/* A replace keeps its own driver in the screen for now, which asks
		 * ScreenScraper before it ever gets here. */
		g_phase = (!g_only[0] && !g_ss_stop && ss_signed_in()) ? P_SS : P_TRY;
		return 1;

	/* ---- pass zero: ScreenScraper, for an account that has one --------- */
	case P_SS:
		if (g_ss_stop || g_ri >= g_nroms) {
			g_ri = 0;
			g_phase = P_TRY;
			return 1;
		}
		if (ss_run_left() >= 0 && ss_run_left() < SS_DAY_FLOOR) {
			g_ss_stop = true;
			snprintf(g_ss_note, sizeof g_ss_note,
			         "ScreenScraper: today's quota is spent");
			say("art: %s\n", g_ss_note);
			return 1;
		}
		if (g_ss_until && time(NULL) < g_ss_until) return 1;
		g_ss_until = 0;
		if (!art_paths(dir, g_roms[g_ri], media, sizeof media,
		               dest, sizeof dest)) { g_ri++; return 1; }
		if (is_cover(dest)) { g_ri++; return 1; }
		snprintf(g_st.now, sizeof g_st.now, "%s", g_roms[g_ri]);
		mkdir(media, 0777);
		{
			char file[NAME_MAX_ + 16];

			snprintf(file, sizeof file, "%s%s", g_roms[g_ri], g_rext[g_ri]);
			if (!ss_run_begin(g_sys[g_si].folder, file, g_roms[g_ri], dir,
			                  g_sys[g_si].exts)) {
				/* No account, or a system they have no id for. Either way
				 * this shelf is libretro's, and asking per game would be
				 * 337 identical refusals. */
				g_phase = P_TRY;
				g_ri = 0;
				return 1;
			}
		}
		g_phase = P_SS_WAIT;
		return 1;

	case P_SS_WAIT: {
		int r = ss_run_step();

		if (r == 1) return 1;
		if (r == 0) {
			g_st.found++;
			g_ri++;
			g_ss_retried = false;
		} else if (ss_run_too_many() && !g_ss_retried) {
			/* Not now, rather than not this game: wait a beat and ask once
			 * more about the SAME game, which is why g_ri does not move. */
			g_ss_retried = true;
			g_ss_until = time(NULL) + SS_BUSY_WAIT;
		} else if (ss_run_too_many()) {
			g_ss_stop = true;
			snprintf(g_ss_note, sizeof g_ss_note,
			         "ScreenScraper: too many requests, stopped");
			say("art: %s\n", g_ss_note);
		} else {
			g_ri++;
			g_ss_retried = false;
		}
		ss_run_cancel();
		g_phase = P_SS;
		return 1;
	}

	/* ---- pass one: ask for the name the card already has ---------------- */
	case P_TRY:
		if (g_ri >= g_nroms) {
			/* Nothing missed, so the catalog is never fetched. */
			if (g_nretry == 0) return next_system(NULL);
			g_phase = P_INDEX;
			return 1;
		}
		if (!art_paths(dir, g_roms[g_ri], media, sizeof media,
		               dest, sizeof dest)) {
			g_st.missing++;
			g_ri++;
			return 1;
		}
		if (!g_only[0] && is_cover(dest)) { g_ri++; return 1; }

		snprintf(g_st.now, sizeof g_st.now, "%s", g_roms[g_ri]);
		mkdir(media, 0777);
		if (!start_image(remote_nth(g_sys[g_si].folder, 0), g_roms[g_ri], dest)) {
			g_retry[g_nretry++] = g_ri;
			g_ri++;
			return 1;
		}
		g_phase = P_TRY_WAIT;
		return 1;

	case P_TRY_WAIT: {
		int r = net_async_poll();

		if (r == 0) return 1;
		say("art: %s from libretro by its own name: %s, %s\n", g_roms[g_ri],
		    r > 0 ? "found" : "not there", answer());
		/* A miss here is ordinary - it means libretro spells this game
		 * differently, which is exactly what the catalog is for. It is not
		 * counted as missing until the fuzzy pass has also failed. */
		if (r > 0) { art_shrink(g_pending); g_st.found++; }
		else       g_retry[g_nretry++] = g_ri;
		g_ri++;
		g_phase = P_TRY;
		return 1;
	}

	/* ---- pass two: the catalog, for what pass one could not name ------ */
	case P_INDEX:
		snprintf(g_st.now, sizeof g_st.now, "%s catalog",
		         g_sys[g_si].folder);
		if (!start_index(remote_nth(g_sys[g_si].folder, g_rem)))
			return next_system("could not start curl");
		g_phase = P_INDEX_WAIT;
		return 1;

	case P_INDEX_WAIT: {
		int r = net_async_poll();

		if (r == 0) return 1;
		say("art: %s catalog %s: %s\n", g_sys[g_si].folder,
		    remote_nth(g_sys[g_si].folder, g_rem), answer());
		/* Distinguished on purpose: a network or TLS failure is not "these
		 * games have no art", and reporting it as one is how a certificate
		 * change gets mistaken for a library full of missing games. */
		if (r < 0) { g_st.missing += g_nretry; return next_system("index fetch failed"); }
		if (!parse_index()) {
			g_st.missing += g_nretry;
			return next_system("index unreadable or empty");
		}
		g_qi = 0;
		g_phase = P_FUZZY;
		return 1;
	}

	case P_FUZZY:
		if (g_qi >= g_nretry) {
			/* Names have run out for this collection; its checksums are next,
			 * for whatever is still unnamed. Once per collection: the checksum
			 * pass ends by coming back here with g_crc_tried set. */
			if (g_nleft > 0 && !g_crc_tried) {
				g_crc_tried = true;
				memcpy(g_retry, g_left, (size_t)g_nleft * sizeof g_left[0]);
				g_nretry = g_nleft;
				g_nleft = 0;
				g_qi = 0;
				g_phase = P_DAT;
				return 1;
			}
			/* Another collection for this shelf, and something still unnamed?
			 * Try it before calling anything missing. A game is only missing
			 * once every catalog its machine has has been asked. */
			if (g_nleft > 0 && remote_nth(g_sys[g_si].folder, g_rem + 1)) {
				memcpy(g_retry, g_left, (size_t)g_nleft * sizeof g_left[0]);
				g_nretry = g_nleft;
				g_nleft = 0;
				g_qi = 0;
				g_rem++;
				g_crc_tried = false;
				g_phase = P_INDEX;
				return 1;
			}
			g_st.missing += g_nleft;
			g_nleft = 0;
			return next_system(NULL);
		}
		if (!art_paths(dir, g_roms[g_retry[g_qi]], media, sizeof media,
		               dest, sizeof dest)) {
			g_st.missing++;
			g_qi++;
			return 1;
		}
		if (!match(g_roms[g_retry[g_qi]], hitbuf, sizeof hitbuf)) {
			g_left[g_nleft++] = g_retry[g_qi];
			snprintf(g_st.now, sizeof g_st.now, "no art for %s",
			         g_roms[g_retry[g_qi]]);
			g_qi++;
			return 1;
		}
		snprintf(g_st.now, sizeof g_st.now, "%s", g_roms[g_retry[g_qi]]);
		mkdir(media, 0777);
		if (!start_image(remote_nth(g_sys[g_si].folder, g_rem), hitbuf, dest)) {
			g_left[g_nleft++] = g_retry[g_qi];
			g_qi++;
			return 1;
		}
		g_phase = P_FUZZY_WAIT;
		return 1;

	case P_FUZZY_WAIT: {
		int r = net_async_poll();

		if (r == 0) return 1;
		say("art: %s from libretro as \"%s\": %s, %s\n", g_roms[g_retry[g_qi]],
		    g_asked, r > 0 ? "found" : "not there", answer());
		if (r > 0) { art_shrink(g_pending); g_st.found++; }
		else       g_left[g_nleft++] = g_retry[g_qi];
		g_qi++;
		g_phase = P_FUZZY;
		return 1;
	}

	/* ---- pass three: the checksum, for what no name reached ----------- */
	case P_DAT:
		snprintf(g_st.now, sizeof g_st.now, "%s checksums",
		         g_sys[g_si].folder);
		free(g_dat);
		g_dat = NULL;
		/* Without the list every game below simply stays unnamed, which is
		 * where it was before this pass existed. */
		g_phase = start_dat(remote_nth(g_sys[g_si].folder, g_rem))
		          ? P_DAT_WAIT : P_CRC;
		return 1;

	case P_DAT_WAIT: {
		int r = net_async_poll();

		if (r == 0) return 1;
		say("art: %s checksum list: %s\n", g_sys[g_si].folder, answer());
		if (r > 0) load_dat();
		g_phase = P_CRC;
		return 1;
	}

	case P_CRC: {
		char     dname[NAME_MAX_];
		uint32_t crc;
		int      ri;

		/* Done: back to the end of the fuzzy pass, which now knows the
		 * checksums have had their turn. */
		if (g_qi >= g_nretry) { g_phase = P_FUZZY; return 1; }
		ri = g_retry[g_qi];
		if (!art_paths(dir, g_roms[ri], media, sizeof media, dest, sizeof dest)) {
			g_left[g_nleft++] = ri;
			g_qi++;
			return 1;
		}
		/* Logged, because a checksum that names a game is a claim worth
		 * being able to check: it says which file became which cover. And
		 * where the checksum names nothing, the loose pass, last of all. */
		if (g_dat && art_rom_crc(dir, g_roms[ri], g_sys[g_si].exts, &crc) &&
		    dat_name(crc, dname, sizeof dname) && match(dname, hitbuf, sizeof hitbuf)) {
			fprintf(stderr, "art: %s is %s by checksum, cover %s\n",
			        g_roms[ri], dname, hitbuf);
		} else if (match_loose(g_roms[ri], hitbuf, sizeof hitbuf)) {
			fprintf(stderr, "art: %s loosely, cover %s\n", g_roms[ri], hitbuf);
		} else {
			g_left[g_nleft++] = ri;
			g_qi++;
			return 1;
		}
		snprintf(g_st.now, sizeof g_st.now, "%s", g_roms[ri]);
		mkdir(media, 0777);
		if (!start_image(remote_nth(g_sys[g_si].folder, g_rem), hitbuf, dest)) {
			g_left[g_nleft++] = ri;
			g_qi++;
			return 1;
		}
		g_phase = P_CRC_WAIT;
		return 1;
	}

	case P_CRC_WAIT: {
		int r = net_async_poll();

		if (r == 0) return 1;
		say("art: %s from libretro as \"%s\": %s, %s\n", g_roms[g_retry[g_qi]],
		    g_asked, r > 0 ? "found" : "not there", answer());
		if (r > 0) { art_shrink(g_pending); g_st.found++; }
		else       g_left[g_nleft++] = g_retry[g_qi];
		g_qi++;
		g_phase = P_CRC;
		return 1;
	}
	}
	return 1;
}

void art_status(art_progress *out)
{
	if (!out) return;
	*out = g_st;
	/* Why the second source stopped, when nothing worse has been said. It is
	 * a run-level fact rather than a system-level one - the pass stops for
	 * the whole run - so it cannot be written when it happens: the next
	 * system's own report would overwrite it a moment later. */
	if (g_ss_note[0] && !out->problem[0])
		snprintf(out->problem, sizeof out->problem, "%s", g_ss_note);
}
