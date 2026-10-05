/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_HAREART_H
#define TORTOS_HAREART_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"

/* JSON out, into a buffer that grows, for these answers and the album ones
 * main.c gives (it holds Muse's library). Start from { 0 }; hj_done hands the
 * text over, or frees it and says why. */
typedef struct { char *p; size_t used, cap; bool bad; } hj;
void  hj_put(hj *o, const char *s, size_t n);
void  hj_lit(hj *o, const char *s);
void  hj_str(hj *o, const char *s);     /* quoted and escaped */
void  hj_int(hj *o, int v);
char *hj_done(hj *o, size_t *len, char *why, size_t wn);

/* The web page's Box art, on the device's side: the answers to hare.h's art
 * hooks, and the shrink for a cover once it lands.
 *
 * Down to the Wire only, Eric's call 2026-10-05: the GKD Pixel 2 has no
 * network of its own, so the browser at the other end of the cable fetches
 * libretro's covers from their GitHub mirror. The Brick fetches its own, with
 * ScreenScraper and the checksum besides, so a second way there would only be
 * a second thing to explain. This says which games want a cover and which
 * cover each gets from a collection's names, by artscrape's rule; the page
 * then uploads each to the path given, like any other file. */

/* The shelves and where their folders are. Kept, not copied: the launcher's
 * config outlives the transfer screen. */
void hareart_init(const systems_cfg *sys, const char *roms_dir);

/* {"shelves":[{"name":..,"folder":..,"collections":[..],"missing":N,
 * "games":[stem,..]}]}, the shelves with games missing covers, the libretro
 * collections to look in, and the games by stem, for the page to ask for by
 * their own names first.
 *
 * THE ORDER THE PAGE TAKES, best-first (Eric, 2026-10-05): the game's own
 * name, then its checksum's No-Intro name (hareart_nointro), both asked for
 * directly, then for what is left the collection's listing - GitHub's API, 60
 * requests an hour - matched by name, checksum and loosely. A No-Intro name is
 * what libretro files a cover under, so the two exact ways need no listing. */
char *hareart_wanted(size_t *len, char *why, size_t wn);

/* {"matches":[...],"left":N} as hareart_match, by checksum alone: each zip's
 * No-Intro name from `dat`, unchecked against any listing - the page asks for
 * it directly, and a miss goes on to the listing. */
char *hareart_nointro(const char *folder, const char *dat, size_t *len,
                      char *why, size_t wn);

/* {"matches":[{"stem":..,"name":..,"cover":"roms/<folder>/.media/<stem>.png"}],
 * "left":N}: one match for each game on the shelf still missing a cover that
 * `names` has a match for, and how many it had none for. Makes the shelf's
 * .media folder when there is anything to put in it, since an upload goes
 * into a folder that already exists. Keeps `names` for hareart_crc. */
char *hareart_match(const char *folder, const char *names, size_t *len,
                    char *why, size_t wn);

/* The same, by checksum, for what matching by name left: each game's zip CRC
 * named by `dat`, a collection's No-Intro list, and that name looked for among
 * the names the last hareart_match on this shelf was given. Box Art's P_CRC
 * pass, for a file named the way the catalog no longer names it. */
char *hareart_crc(const char *folder, const char *dat, size_t *len,
                  char *why, size_t wn);

/* An upload in place: a cover in a shelf's .media is shrunk to the size the
 * shelf draws it at, as a fetched one is. Anything else is left alone. Set on
 * every device, so a cover uploaded by hand is shrunk too. */
void hareart_after_write(const char *abs);

#endif
