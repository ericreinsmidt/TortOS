/* SPDX-License-Identifier: 0BSD */
/* See artscrape.h for where the rules came from and why they are these. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "artscrape.h"
#include "config.h"
#include "library.h"
#include "net.h"

#define BASE "https://thumbnails.libretro.com"

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
 * A table rather than a guess. The names are a catalogue's, not a pattern:
 * nothing derives "Sega - Mega Drive - Genesis" from "Genesis". A folder
 * missing from here is REPORTED, not skipped quietly - systems.cfg gains
 * entries over time, and a system silently passed over looks exactly like a
 * system whose art is already complete. */
static const struct { const char *folder, *remote; } MAP[] = {
	{ "NES",              "Nintendo - Nintendo Entertainment System" },
	{ "SNES",             "Nintendo - Super Nintendo Entertainment System" },
	{ "Game Boy",         "Nintendo - Game Boy" },
	{ "Game Boy Color",   "Nintendo - Game Boy Color" },
	{ "Game Boy Advance", "Nintendo - Game Boy Advance" },
	{ "Genesis",          "Sega - Mega Drive - Genesis" },
	{ "Master System",    "Sega - Master System - Mark III" },
	{ "Game Gear",        "Sega - Game Gear" },
	{ "TurboGrafx-16",    "NEC - PC Engine - TurboGrafx 16" },
};
#define MAP_N ((int)(sizeof MAP / sizeof MAP[0]))

static const char *remote_for(const char *folder)
{
	int i;

	for (i = 0; i < MAP_N; i++)
		if (!strcmp(MAP[i].folder, folder)) return MAP[i].remote;
	return NULL;
}

/* ---- the normalisation, which is the whole matching rule ---------------- */

/* A title with every parenthesised tag removed, lowercased, and reduced to
 * single spaces between alphanumerics.
 *
 * Region, language, revision and dump tags are exactly what differs between
 * two catalogues of the same game, and they are never what distinguishes two
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
 * after an unmatched bracket - better behaviour by any reading, and wrong.
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

/* ---- percent-encoding a path segment ----------------------------------- */

/* For a URL, so spaces and the punctuation in "Sega - Mega Drive - Genesis"
 * survive. Unreserved characters pass; everything else is escaped, including
 * '/' - each of these is ONE segment, and a name containing a slash must not
 * become two. */
static void urlenc(const char *in, char *out, size_t outn)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;

	for (; *in && o + 4 < outn; in++) {
		unsigned char c = (unsigned char)*in;

		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
	}
	out[o] = '\0';
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
 * Two parallel arrays of decoded and normalised names used to sit beside this.
 * Once the real index sizes were known they would have been 13418 x 192 bytes
 * each - five megabytes of copies of strings already in the buffer, and an
 * entry cap that was the other half of the truncation bug. match() scans the
 * HTML instead: one pass per ROM, normalising each candidate as it goes. That
 * is 13418 short normalisations per ROM on the worst system, which is
 * microseconds, and there is nothing left to overflow. */
static char   *g_html;
static int     g_nnames;      /* what the index held; for the message only */

typedef struct { char folder[CFG_STR]; char exts[CFG_STR]; } sysrow;
static sysrow  g_sys[CFG_MAX_SYSTEMS];
static int     g_nsys;
static char    g_romdir[LIB_PATH];

/* Where the step machine is. Split this way because a request must not be
 * waited for: these are called from a screen's frame loop, which is also
 * where the power button is read, so anything that blocks for a timeout is a
 * device that has stopped answering its own power button. */
enum { P_SYSTEM, P_INDEX_WAIT, P_ROM, P_IMAGE_WAIT };
static int  g_phase;
static char g_pending[ARTPATH_MAX];       /* the image being fetched */

static int  g_si;               /* which system */
static int  g_ri;               /* which ROM inside it */
static bool g_running;

static char g_roms[ENTRIES_MAX][NAME_MAX_];
static int  g_nroms;

static art_progress g_st;

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
		dot = strrchr(g_roms[g_nroms], '.');
		if (dot) *dot = '\0';
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
	 * against a fraction of the catalogue and report the difference as
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

/* Exact filename first, then normalised. In that order because an exact hit is
 * unambiguous and a normalised one can collide - two dumps of the same game
 * normalise alike, and the first is as good an answer as any.
 *
 * One pass over the index per ROM, decoding and normalising candidates as it
 * goes. `out` takes the winning name exactly as libretro spells it, because
 * that is what the download URL needs. */
static bool match(const char *base, char *out, size_t outn)
{
	char nb[NAME_MAX_], cand[NAME_MAX_], cnorm[NAME_MAX_];
	const char *p;
	bool have_norm = false;

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

		if (!strcmp(cand, base)) {                  /* exact: nothing beats it */
			snprintf(out, outn, "%s", cand);
			return true;
		}
		if (!have_norm && nb[0]) {
			art_norm(cand, cnorm, sizeof cnorm);
			if (!strcmp(cnorm, nb)) {
				snprintf(out, outn, "%s", cand);
				have_norm = true;               /* but keep looking for one */
			}
		}
	}
	return have_norm;
}

/* ---- driving ------------------------------------------------------------ */

void art_cancel(void)
{
	g_running = false;
	free(g_html);  g_html = NULL;
	g_nnames = 0;
}

void art_begin(const systems_cfg *sys, const char *roms_dir)
{
	int i;

	art_cancel();
	memset(&g_st, 0, sizeof g_st);
	g_si = g_ri = 0;
	g_phase = P_SYSTEM;
	g_nsys = 0;
	snprintf(g_romdir, sizeof g_romdir, "%s", roms_dir ? roms_dir : "");

	if (!sys) return;
	for (i = 0; i < sys->count && g_nsys < CFG_MAX_SYSTEMS; i++) {
		char dir[ARTPATH_MAX];
		struct stat st;

		/* Only shelves with a ROM folder on the card. The launcher appends a
		 * Favorites shelf to this list - games drawn from every system, with
		 * no folder of its own - and counting it made the screen say "10 of
		 * 10" for a nine-system library, with one of the ten always skipped.
		 * A shelf that is not a folder has no art to fetch. */
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
		fprintf(stderr, "art: %s\n", g_st.now);
	}
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
	if (g_si >= g_nsys) { art_cancel(); return 0; }

	snprintf(dir, sizeof dir, "%s/%s", g_romdir, g_sys[g_si].folder);

	switch (g_phase) {
	case P_SYSTEM:
		if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
			return next_system("no such folder");
		remote = remote_for(g_sys[g_si].folder);
		/* Loudly. systems.cfg gains entries over time, and a system silently
		 * passed over looks exactly like one whose art is already complete. */
		if (!remote) return next_system("not in the table");
		read_roms(dir, g_sys[g_si].exts);
		if (g_nroms == 0) return next_system("no ROMs");

		/* Count what is missing BEFORE fetching the index, and skip the
		 * system entirely when nothing is.
		 *
		 * The skip-if-present test used to live per-ROM, after the index had
		 * already been downloaded - so a second run over a complete library
		 * still pulled nine multi-megabyte indexes to discover it had nothing
		 * to do. The commit that added this feature claimed running it again
		 * was cheap. It was not; this is what makes that true. */
		{
			int i, want = 0;

			for (i = 0; i < g_nroms; i++) {
				char have[ARTPATH_MAX];

				if (snprintf(have, sizeof have, "%s/.media/%s.png",
				             dir, g_roms[i]) >= (int)sizeof have) continue;
				if (stat(have, &st) == 0 && st.st_size > 0) g_st.skipped++;
				else want++;
			}
			if (want == 0) return next_system(NULL);
		}

		snprintf(g_st.now, sizeof g_st.now, "%s", g_sys[g_si].folder);
		if (!start_index(remote)) return next_system("could not start curl");
		g_phase = P_INDEX_WAIT;
		return 1;

	case P_INDEX_WAIT: {
		int r = net_async_poll();

		if (r == 0) return 1;                  /* still fetching */
		/* Distinguished on purpose: a network or TLS failure is not "this game
		 * has no art", and reporting it as one is how a certificate change
		 * gets mistaken for a library full of missing games. */
		if (r < 0) return next_system("index fetch failed");
		if (!parse_index()) return next_system("index unreadable or empty");
		g_ri = 0;
		g_phase = P_ROM;
		return 1;
	}

	case P_ROM:
		if (g_ri >= g_nroms) return next_system(NULL);

		if (snprintf(media, sizeof media, "%s/.media", dir)
		    >= (int)sizeof media ||
		    snprintf(dest, sizeof dest, "%s/%s.png", media, g_roms[g_ri])
		    >= (int)sizeof dest) {
			g_st.missing++;
			g_ri++;
			return 1;
		}

		/* Already there: no request. Counted by the pre-pass above, which is
		 * why this does not count it again. */
		if (stat(dest, &st) == 0 && st.st_size > 0) {
			g_ri++;
			return 1;
		}
		if (!match(g_roms[g_ri], hitbuf, sizeof hitbuf)) {
			g_st.missing++;
			snprintf(g_st.now, sizeof g_st.now, "no art for %s", g_roms[g_ri]);
			g_ri++;
			return 1;
		}
		{
			char url[ARTURL_MAX], er[384], en[NAME_MAX_ * 3 + 1];

			snprintf(g_st.now, sizeof g_st.now, "%s", g_roms[g_ri]);
			mkdir(media, 0777);
			urlenc(remote_for(g_sys[g_si].folder), er, sizeof er);
			urlenc(hitbuf, en, sizeof en);
			snprintf(url, sizeof url, BASE "/%s/Named_Boxarts/%s.png", er, en);
			snprintf(g_pending, sizeof g_pending, "%s", dest);
			if (!net_get_async(url, g_pending, 60)) {
				g_st.missing++;
				g_ri++;
				return 1;
			}
			g_phase = P_IMAGE_WAIT;
		}
		return 1;

	case P_IMAGE_WAIT: {
		int r = net_async_poll();

		if (r == 0) return 1;
		if (r > 0) g_st.found++;
		else       g_st.missing++;
		g_ri++;
		g_phase = P_ROM;
		return 1;
	}
	}
	return 1;
}

void art_status(art_progress *out)
{
	if (out) *out = g_st;
}
