/* SPDX-License-Identifier: 0BSD */
#ifndef TORTOS_RAFETCH_H
#define TORTOS_RAFETCH_H

#include <stdbool.h>
#include <stddef.h>

/* RetroAchievements, the parts that need the network.
 *
 * This is the normal client workflow, on the device, the way any other RA
 * frontend does it: hash the ROM, ask which game it is, fetch the set, and
 * cache it beside the ROM. tools/ra-sets.py does the same thing from a host
 * and writes the same file; it exists now only to pre-seed a library in bulk
 * or to work offline, not because the device cannot.
 *
 * The credential kept here is a TOKEN, not a password. RA's login returns one
 * and it is what every later call uses, so the password is typed once and
 * never stored.
 */

#define RA_TOKEN_MAX 64
#define RA_USER_MAX  64

/* Read and write .userdata/ra.cfg. Written 0600; it holds the token. */
bool ra_creds_load(const char *path);
bool ra_creds_save(const char *path);
void ra_creds_clear(void);
bool ra_signed_in(void);
const char *ra_user(void);

/* Exchange a password for a token. The password is used and dropped; nothing
 * writes it anywhere. `err` takes RA's own message when it refuses, which is
 * the difference between a wrong password and a site that is down. */
bool ra_sign_in(const char *user, const char *password, char *err, size_t errn);

/* 0: RetroAchievements does not know this hash, which for a fan translation is
 * the permanent and correct answer. -1: the question could not be asked, which
 * is a different thing and must not be cached as though it were an answer. */
long ra_game_for_rom(const char *rom_path, const char *tag);

/* The conversion on its own, so it can be checked against real responses with
 * no network involved. tools/raset-check.c does exactly that, against the
 * files tools/ra-sets.py writes from the same JSON. */
bool ra_set_from_json(const char *json, size_t len, long gameid,
                      const char *out_path);

/* Fetch a set and write it where chv_load will find it. The file is the same
 * format tools/ra-sets.py writes, because it is read by the same two
 * programs - see src/cheevos.h. */
bool ra_fetch_set(long gameid, const char *out_path);

/* The whole thing for one game: hash, identify, fetch, cache. False means no
 * set is available, for any of the ordinary reasons - offline, not signed in,
 * RA does not know the game, or it has no achievements. */
bool ra_ensure_set(const char *rom_path, const char *tag, const char *set_path);

/* ---- the account ---------------------------------------------------------
 *
 * Every call here blocks for as long as its timeout. None of them may be made
 * from inside the in-game wait loop, which is also the power button's
 * watchdog: a request in there would make the device stop answering it. They
 * belong on either side of a game, where the launcher owns the screen.
 */

/* What the account already holds for a game, softcore. Up to `max` ids into
 * `out`; returns how many, or -1 if the question could not be asked - a
 * different thing from "none", and one that must not be recorded as an answer.
 *
 * This is what makes a count mean something. Without it the launcher reports
 * what THIS DEVICE has seen, which looks exactly like an account total and is
 * not one - measured 2026-08-29 as 3/40 against the site's 13/40. */
int ra_account_unlocks(long gameid, int *out, int max);

/* Tell RetroAchievements a game has started. Fire and forget: it drives
 * "currently playing" on the site and nothing here depends on it. */
void ra_start_session(long gameid);

/* Submit one unlock, softcore. Signed the way rcheevos signs it -
 * md5(achievement id + username + hardcore flag) - because the server checks
 * it. `rom_hash` is optional and is what real clients send. */
bool ra_submit_unlock(int achievement_id, const char *rom_hash);

#endif
