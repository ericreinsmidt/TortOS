/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_SSFETCH_H
#define TORTOS_SSFETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "db.h"

/* Asking ScreenScraper about one game.
 *
 * The account and the build's key live in ss.h; this is what you do with them.
 * Everything here that CAN be pure is pure, because the parts worth getting
 * wrong are the parts a device cannot show you: which system id a folder maps
 * to, whether a reply is about the game you asked about, and which of the
 * covers on offer is the right one. tools/ssfetch-check.c holds all three to
 * real replies with no network in sight.
 *
 * WHY THE LOOKUP IS BY CHECKSUM. A file's name is what somebody typed; the
 * checksum is what the cartridge is. Measured over this card's 1,708 games:
 * every one of the 1,683 cartridges was known by checksum, against a name
 * match that fails on every fan translation.
 *
 * AND WHY IT IS NOT, FOR A DISC. A CHD is 160 to 456 MB and its file checksum
 * is not what any catalog files a CD game under. Measured 2026-09-16: all 25
 * disc images on this card were found by NAME, with covers and prose, and
 * systemeid 31 and 114 gave identical answers - so the CD system id is not
 * needed either. ss_lookup_url takes crc = 0 for those and asks by name. */

/* ScreenScraper's id for a shelf folder, or 0 for one they have no system for.
 *
 * A TABLE, not a lookup. Their systemesListe.php would answer it, at the cost
 * of one request and a large parse before any game could be asked about, for
 * numbers that have not moved. Measured against that endpoint 2026-09-16; if a
 * folder is ever added, ask it again rather than guessing. */
int ss_system_id(const char *folder);

/* What one reply is worth. */
typedef struct {
	bool      found;           /* they know this game */
	bool      name_ok;         /* and it is plausibly the one asked about */
	char      name[128];       /* what they call it - for a mismatch report */
	game_meta meta;            /* exactly what the card's games table holds */
	char      art[512];        /* the box-2D media URL, empty when none */
	char      art_region[8];   /* which region that cover is from */
} ss_result;

/* Read a jeuInfos reply. Pure: no network, no card, no clock.
 *
 * `file` is the ROM's file name, and it is needed here rather than only at the
 * call site because the name check is part of reading the reply: a result that
 * has not been checked is a result somebody will use unchecked. */
bool ss_parse(const char *json, size_t len, const char *file, ss_result *out);

/* Whether a reply's names are plausibly the game a file is named after.
 *
 * ONLY THE SEQUEL NUMBER IS COMPARED, and that is a measurement rather than a
 * preference. Over 79 replies on 2026-09-15, a half-or-better word overlap
 * flagged 35 of them and was wrong about nearly every one, because a fan
 * translation is filed under its romanized Japanese title - "Samurai Pizza
 * Cats" comes back as "Kyatto Ninden Teyandee", which is the right game. The
 * number survives translation and is what a misfile gets wrong: over the whole
 * card it flagged 9 games, of which 4 were genuinely another game, including
 * Shin Megami Tensei II answered with the first game and Mother 3 answered
 * with ZZZ(notgame):#NONGAME.
 *
 * Biased toward flagging on purpose. A false alarm costs a libretro cover in
 * place of a correct one; a miss puts another game's synopsis on the shelf,
 * where nothing about it looks wrong. A file with no number in it is not
 * checked, because there is nothing to check it with. */
bool ss_name_ok(const char *file, const char *const *names, int n);

/* The URL for one lookup, credentials included.
 *
 * Into a caller's buffer, and it is never logged or printed: it carries the
 * developer pair and the player's password as query parameters. Both net_get
 * paths write it into a 0600 config file rather than into argv, which is the
 * only reason passing it as a URL is acceptable at all.
 *
 * `crc` of 0 and `size` of 0 ask by name alone, which is what a disc image
 * needs. False when the build has no key, nobody is signed in, or the folder
 * has no system id. */
bool ss_lookup_url(char *out, size_t n, const char *folder, const char *file,
                   uint32_t crc, long size);

/* The whole round trip, waiting for it. NOT for a frame loop - that is where
 * the power button is read, and a ScreenScraper call measured 5.5 seconds per
 * game on a good connection. For tools, checks, and any caller that is already
 * sitting behind a wait panel. */
bool ss_lookup(const char *folder, const char *file, uint32_t crc, long size,
               ss_result *out);

#endif
