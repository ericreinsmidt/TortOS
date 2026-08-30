/* SPDX-License-Identifier: 0BSD */
/* Favorites. See favorites.h for the keying; this is the storage.
 *
 * Tab-separated, because a ROM filename may legally contain almost anything
 * except a tab or a newline - spaces, brackets, commas and parentheses are
 * routine in No-Intro names, and every one of those would have made a
 * separator that needed escaping.
 *
 * A linear scan over at most FAV_MAX entries. The list is read once per drawn
 * game card, which sounds like a lot until you notice that a shelf shows
 * about seven cards and a favorite list is realistically a dozen entries.
 * Sorting it or hashing it would be machinery bought for a problem nobody
 * has.
 */
#include <stdio.h>
#include <string.h>

#include "atomic.h"
#include "favorites.h"

typedef struct {
	char tag[FAV_TAG_MAX];
	char file[FAV_FILE_MAX];
} fav_entry;

static fav_entry g_fav[FAV_MAX];
static int g_count;

static int find(const char *tag, const char *file)
{
	int i;
	if (!tag || !file) return -1;
	for (i = 0; i < g_count; i++)
		if (!strcmp(g_fav[i].tag, tag) && !strcmp(g_fav[i].file, file))
			return i;
	return -1;
}

void fav_load(const char *path)
{
	char line[FAV_TAG_MAX + FAV_FILE_MAX + 8];
	FILE *f;

	g_count = 0;
	f = fopen(path, "r");
	if (!f) return;                       /* nobody has favorited anything */
	while (fgets(line, sizeof line, f) && g_count < FAV_MAX) {
		char *tab;
		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0] || line[0] == '#') continue;
		tab = strchr(line, '\t');
		if (!tab) continue;               /* not our format; skip it */
		*tab++ = '\0';
		if (!line[0] || !*tab) continue;
		/* Rejected rather than truncated. A tag cut to fit would still match
		 * something on the next lookup - just not the system it came from -
		 * and a favorite that silently points at the wrong shelf is worse
		 * than one that failed to load. */
		if (strlen(line) >= FAV_TAG_MAX || strlen(tab) >= FAV_FILE_MAX) continue;
		if (find(line, tab) >= 0) continue;               /* set, not a list */
		memcpy(g_fav[g_count].tag, line, strlen(line) + 1);
		memcpy(g_fav[g_count].file, tab, strlen(tab) + 1);
		g_count++;
	}
	fclose(f);
}

bool fav_save(const char *path)
{
	/* Atomically - see atomic.h. Favorites accumulate, and this is rewritten
	 * whole on every toggle. */
	FILE *f = atomic_open(path, 0644);
	int i;

	if (!f) return false;
	fprintf(f, "# TortOS favorites: one <system tag>\\t<rom file> per line.\n");
	for (i = 0; i < g_count; i++)
		fprintf(f, "%s\t%s\n", g_fav[i].tag, g_fav[i].file);
	if (!atomic_commit(f, path)) return false;
	return true;
}

bool fav_is(const char *tag, const char *file)
{
	return find(tag, file) >= 0;
}

bool fav_toggle(const char *tag, const char *file)
{
	int i = find(tag, file);

	if (i >= 0) {
		/* Order does not matter - the shelf sorts itself - so closing the gap
		 * with the last entry beats shuffling everything down. */
		g_fav[i] = g_fav[--g_count];
		return false;
	}
	if (g_count >= FAV_MAX || !tag || !file || !*tag || !*file) return false;
	if (strlen(tag) >= FAV_TAG_MAX || strlen(file) >= FAV_FILE_MAX) return false;
	memcpy(g_fav[g_count].tag, tag, strlen(tag) + 1);
	memcpy(g_fav[g_count].file, file, strlen(file) + 1);
	g_count++;
	return true;
}

int fav_count(void) { return g_count; }

bool fav_at(int i, const char **tag, const char **file)
{
	if (i < 0 || i >= g_count) return false;
	if (tag)  *tag  = g_fav[i].tag;
	if (file) *file = g_fav[i].file;
	return true;
}
