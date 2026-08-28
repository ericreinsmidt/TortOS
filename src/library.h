/* SPDX-License-Identifier: 0BSD */
#ifndef TORTOS_LIBRARY_H
#define TORTOS_LIBRARY_H

#include <stdbool.h>

#define LIB_NAME 256
#define LIB_PATH 544

typedef struct {
	char name[LIB_NAME];  /* the filename without its extension. Box art is
	                       * looked up by this and the shelf is sorted by it,
	                       * so it stays exactly as the file is named. */
	char title[LIB_NAME]; /* what the shelf shows: `name` with the trailing
	                       * region and dump tags cut off, so a card says
	                       * "Chrono Trigger" rather than the cataloguing that
	                       * follows it. */
	char file[LIB_PATH];  /* launch path relative to Roms/<folder>: a filename,
	                       * or "<folder>/<disc>" for a disc-folder game */
	/* Whether an autosave exists for this game, so the card can say so and
	 * the launch can be honest about resuming. Resolved on first use, not at
	 * scan time: it is two lookups per game and a card of a thousand games
	 * would spend the whole boot animation on them. */
	signed char state_known;
	signed char has_state;
} game_entry;

typedef struct {
	game_entry *items;
	int count;
	bool scanned;
} game_list;

/* Scan Roms/<folder> for files whose extension appears in exts (a comma or
 * space separated list, without dots; empty means take everything). */
bool lib_scan(const char *roms_root, const char *folder, const char *exts,
              game_list *out);
void lib_free(game_list *l);

#endif
