/* SPDX-License-Identifier: MIT */
/* See hareart.h. */
#include "hareart.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "artscrape.h"
#include "artshrink.h"

static const systems_cfg *g_sys;
static char g_roms[1024];

void hareart_init(const systems_cfg *sys, const char *roms_dir)
{
	g_sys = sys;
	snprintf(g_roms, sizeof g_roms, "%s", roms_dir);
}

/* ---- JSON out, into a buffer that grows -------------------------------- */

void hj_put(hj *o, const char *s, size_t n)
{
	if (o->bad) return;
	if (o->used + n + 1 > o->cap) {
		size_t cap = o->cap ? o->cap : 4096;
		char *p;

		while (o->used + n + 1 > cap) cap *= 2;
		if (!(p = realloc(o->p, cap))) { o->bad = true; return; }
		o->p = p;
		o->cap = cap;
	}
	memcpy(o->p + o->used, s, n);
	o->used += n;
	o->p[o->used] = '\0';
}

void hj_lit(hj *o, const char *s) { hj_put(o, s, strlen(s)); }

/* Only what JSON requires, as hare.c's jstr: a name on this card is UTF-8 and
 * goes through as it is. */
void hj_str(hj *o, const char *s)
{
	hj_lit(o, "\"");
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		char esc[8];

		if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; hj_put(o, esc, 2); }
		else if (c < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", c); hj_put(o, esc, 6); }
		else hj_put(o, (const char *)&c, 1);
	}
	hj_lit(o, "\"");
}

void hj_int(hj *o, int v)
{
	char num[16];

	snprintf(num, sizeof num, "%d", v);
	hj_lit(o, num);
}

char *hj_done(hj *o, size_t *len, char *why, size_t wn)
{
	if (o->bad || !o->p) {
		free(o->p);
		snprintf(why, wn, "out of memory");
		return NULL;
	}
	*len = o->used;
	return o->p;
}

/* ---- the answers ------------------------------------------------------- */

static bool shelf_dir(const system_cfg *sc, char *dir, size_t n)
{
	struct stat st;

	if (snprintf(dir, n, "%s/%s", g_roms, sc->folder) >= (int)n) return false;
	return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

/* A missing game, for the page to ask for by its own name first. */
static void wanted_one(const char *stem, void *ctx)
{
	hj *o = ctx;

	if (o->used && o->p[o->used - 1] != '[') hj_lit(o, ",");
	hj_str(o, stem);
}

char *hareart_wanted(size_t *len, char *why, size_t wn)
{
	hj o = { 0 };
	bool first = true;
	int i, k;

	if (!g_sys) { snprintf(why, wn, "no shelves"); return NULL; }
	hj_lit(&o, "{\"shelves\":[");
	for (i = 0; i < g_sys->count; i++) {
		const system_cfg *sc = &g_sys->systems[i];
		char dir[1100];
		int missing;

		/* A shelf libretro has no collection for (Muse, or one added to
		 * systems.cfg later) has nothing to look in. */
		if (!art_collection(sc->folder, 0) || !shelf_dir(sc, dir, sizeof dir)) continue;
		if ((missing = art_missing(dir, sc->exts, NULL, NULL)) == 0) continue;
		if (!first) hj_lit(&o, ",");
		first = false;
		hj_lit(&o, "{\"name\":");
		hj_str(&o, sc->name);
		hj_lit(&o, ",\"folder\":");
		hj_str(&o, sc->folder);
		hj_lit(&o, ",\"collections\":[");
		for (k = 0; art_collection(sc->folder, k); k++) {
			if (k) hj_lit(&o, ",");
			hj_str(&o, art_collection(sc->folder, k));
		}
		hj_lit(&o, "],\"missing\":");
		hj_int(&o, missing);
		/* The games themselves, by stem: the page asks for each by its own
		 * name before anything else, which needs no listing at all. */
		hj_lit(&o, ",\"games\":[");
		art_missing(dir, sc->exts, wanted_one, &o);
		hj_lit(&o, "]}");
	}
	hj_lit(&o, "]}");
	return hj_done(&o, len, why, wn);
}

/* The collection's names from the last match, kept for the checksum pass that
 * may follow on the same shelf: the name a checksum gives still has to be
 * found among them. Replaced by the next match. */
static char *g_names;
static char  g_names_folder[256];

typedef struct {
	hj         *o;
	const char *folder, *names, *dat;
	const system_cfg *sc;
	char        dir[1100];
	int         found, left;
} matching;

static void add_match(matching *m, const char *stem, const char *name)
{
	char cover[1100];

	if (snprintf(cover, sizeof cover, "roms/%s/.media/%s.png", m->folder, stem)
	    >= (int)sizeof cover) { m->left++; return; }
	if (m->found++) hj_lit(m->o, ",");
	hj_lit(m->o, "{\"stem\":");
	hj_str(m->o, stem);
	hj_lit(m->o, ",\"name\":");
	hj_str(m->o, name);
	hj_lit(m->o, ",\"cover\":");
	hj_str(m->o, cover);
	hj_lit(m->o, "}");
}

/* By name: the ROM's own, exact and then normalized. */
static void match_one(const char *stem, void *ctx)
{
	matching *m = ctx;
	char name[512];

	if (art_match_list(stem, m->names, name, sizeof name)) add_match(m, stem, name);
	else m->left++;
}

/* By checksum: the zip's CRC named by the No-Intro list, and that name found
 * among the collection's, as a Box Art run's P_CRC pass does; and where that
 * names nothing, the loose pass, last of all, as there. */
static void crc_one(const char *stem, void *ctx)
{
	matching *m = ctx;
	char dname[512], name[512];
	uint32_t crc;

	if (art_rom_crc(m->dir, stem, m->sc->exts, &crc) &&
	    art_dat_name(m->dat, crc, dname, sizeof dname) &&
	    art_match_list(dname, m->names, name, sizeof name)) {
		fprintf(stderr, "art: %s is %s by checksum, cover %s\n", stem, dname, name);
		add_match(m, stem, name);
	} else if (art_match_loose(stem, m->names, name, sizeof name)) {
		fprintf(stderr, "art: %s loosely, cover %s\n", stem, name);
		add_match(m, stem, name);
	} else if (art_match_subtitle(stem, m->names, name, sizeof name)) {
		fprintf(stderr, "art: %s by its title alone, cover %s\n", stem, name);
		add_match(m, stem, name);
	} else {
		m->left++;
	}
}

/* By checksum alone: the No-Intro name for each zip the list knows, which is
 * the name libretro files its cover under. No listing needed; the page asks
 * for it directly, and what is not there goes on to the listing. */
static void nointro_one(const char *stem, void *ctx)
{
	matching *m = ctx;
	char dname[512];
	uint32_t crc;

	if (art_rom_crc(m->dir, stem, m->sc->exts, &crc) &&
	    art_dat_name(m->dat, crc, dname, sizeof dname))
		add_match(m, stem, dname);
	else
		m->left++;
}

static bool find_shelf(matching *m, char *why, size_t wn)
{
	int i;

	if (!g_sys) { snprintf(why, wn, "no shelves"); return false; }
	for (i = 0; i < g_sys->count; i++)
		if (!strcmp(g_sys->systems[i].folder, m->folder)) break;
	if (i == g_sys->count || !shelf_dir(&g_sys->systems[i], m->dir, sizeof m->dir)) {
		snprintf(why, wn, "no such shelf");
		return false;
	}
	m->sc = &g_sys->systems[i];
	return true;
}

/* {"matches":[...],"left":N}, the .media folder made when anything matched. */
static char *run(matching *m, void (*fn)(const char *, void *), size_t *len,
                 char *why, size_t wn)
{
	char media[1200];

	hj_lit(m->o, "{\"matches\":[");
	art_missing(m->dir, m->sc->exts, fn, m);
	hj_lit(m->o, "],\"left\":");
	hj_int(m->o, m->left);
	hj_lit(m->o, "}");
	if (m->found && snprintf(media, sizeof media, "%s/.media", m->dir) < (int)sizeof media)
		mkdir(media, 0755);
	return hj_done(m->o, len, why, wn);
}

char *hareart_match(const char *folder, const char *names, size_t *len,
                    char *why, size_t wn)
{
	hj o = { 0 };
	matching m = { .o = &o, .folder = folder, .names = names };

	if (!find_shelf(&m, why, wn)) return NULL;
	free(g_names);
	g_names = strdup(names);
	snprintf(g_names_folder, sizeof g_names_folder, "%s", folder);
	return run(&m, match_one, len, why, wn);
}

char *hareart_nointro(const char *folder, const char *dat, size_t *len,
                      char *why, size_t wn)
{
	hj o = { 0 };
	matching m = { .o = &o, .folder = folder, .dat = dat };

	if (!find_shelf(&m, why, wn)) return NULL;
	return run(&m, nointro_one, len, why, wn);
}

char *hareart_crc(const char *folder, const char *dat, size_t *len,
                  char *why, size_t wn)
{
	hj o = { 0 };
	matching m = { .o = &o, .folder = folder, .dat = dat };

	if (!g_names || strcmp(g_names_folder, folder)) {
		snprintf(why, wn, "match this shelf by name first");
		return NULL;
	}
	m.names = g_names;
	if (!find_shelf(&m, why, wn)) return NULL;
	return run(&m, crc_one, len, why, wn);
}

void hareart_after_write(const char *abs)
{
	size_t n = strlen(g_roms), len = strlen(abs);

	if (!g_roms[0] || strncmp(abs, g_roms, n) || abs[n] != '/') return;
	if (!strstr(abs + n, "/.media/")) return;
	if (len < 4 || strcasecmp(abs + len - 4, ".png")) return;
	art_shrink(abs);
}
