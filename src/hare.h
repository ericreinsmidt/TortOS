/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_HARE_H
#define TORTOS_HARE_H

#include <stdbool.h>
#include <stddef.h>

#include "httpd.h"

/* Over The Hare: the routes, the PIN, and the file operations.
 *
 * httpd.c moves bytes and knows nothing about what they mean. xfer.c decides
 * what a path is allowed to be. This is the part in between: what a URL does,
 * who is allowed to do it, and what happens to the card when they do.
 *
 * THE PIN IS FOUR DIGITS, which is ten thousand guesses. On its own that is a
 * doorbell rather than a lock - a script exhausts it in seconds. What makes it
 * a lock is the lockout: a handful of wrong answers and the door stops
 * answering for a while, which turns ten thousand guesses into more hours than
 * anyone will spend on somebody else's ROM folder. The PIN is generated fresh
 * every time the screen opens and dies with it, so there is nothing to reuse
 * and nothing to leak.
 *
 * The threat this defends against is someone else on the same Wi-Fi, which is
 * the realistic one for a handheld on a home or cafe network. It is not
 * defense against someone who can watch the traffic: this is plain HTTP, the
 * PIN crosses the network in the clear, and so does everything else. TLS on a
 * LAN address is a certificate nobody can issue and a warning everybody clicks
 * through, which buys less than it costs. The mitigation that actually applies
 * is that the server exists only while the screen is open.
 */

/* Start and stop with the screen. Generates the PIN, clears any session, and
 * sweeps part-files left by a transfer that died.
 *
 * The three directories are arguments rather than the P_ globals so this whole
 * subsystem - routes, transport, path safety - links into a check without SDL
 * behind it. The launcher passes P_ROMS, P_CARD and its own res/web. */
bool hare_start(const char *roms_dir, const char *card_dir,
                const char *shared_dir, const char *web_dir);
void hare_stop(void);

/* Download logs: how the launcher packs them, which this file cannot do by
 * itself - the masking needs names only the launcher knows (see logpack.h).
 * The hook writes a .tar.gz somewhere temporary and says where, and what the
 * download should be called. Unset, the route answers that there are none. */
void hare_set_logs(bool (*pack)(char *path, size_t pn, char *name, size_t nn));

/* Called with a folder's absolute path just before it is deleted with
 * everything in it, so the launcher can stop Muse if it is playing a file in
 * there. Unset, nothing is told. */
void hare_set_before_delete(void (*fn)(const char *abs));

/* Called with a folder's old and new absolute paths after Tidy names renamed
 * it, so the launcher can move what it keeps under the old name: the cover in
 * .media, Album Art's memory of it, an audiobook's place. Unset, nothing is
 * told. */
void hare_set_after_rename(void (*fn)(const char *from, const char *to));

/* Box art for the web page, which the launcher answers because it knows the
 * shelves and the matching rule (hareart.c, artscrape.h). The browser fetches
 * the covers itself; these say which games want one and which each gets:
 *
 *   wanted  GET /api/art/wanted, the shelves with games missing covers, as
 *           the JSON the page reads
 *   match   POST /api/art/match?s=<folder>, the body a collection's names,
 *           one a line: that shelf's matches, as JSON
 *   crc     POST /api/art/crc?s=<folder>, the body the collection's No-Intro
 *           list: what matching by name left, matched by checksum
 *   nointro POST /api/art/nointro?s=<folder>, the same list: each missing
 *           zip's No-Intro name by checksum, for the page to ask for directly
 *
 * Each returns malloc'd text the server frees, or NULL with the reason in
 * `why`. Unset, the routes answer 404 and the page hides its button. */
typedef char *(*hare_art_fn)(const char *folder, const char *body,
                             size_t *len, char *why, size_t wn);
void hare_set_art(char *(*wanted)(size_t *len, char *why, size_t wn),
                  hare_art_fn match, hare_art_fn crc, hare_art_fn nointro);

/* Album covers the same way, from MusicBrainz and the Cover Art Archive:
 *
 *   wanted  GET /api/albums/wanted, the albums Album Art would ask about, each
 *           with the MusicBrainz searches to try, in order, as JSON
 *   pick    POST /api/albums/pick?id=N, the body a search's reply: the release
 *           group by Muse's track-count rule and where its cover goes, as
 *           JSON, or an empty "rg" when the reply is not the album
 *
 * Same contract as hare_set_art. */
void hare_set_albums(char *(*wanted)(size_t *len, char *why, size_t wn),
                     char *(*pick)(int id, const char *json, size_t n,
                                   size_t *len, char *why, size_t wn));

/* Called with a file's absolute path once an upload of it is in place, so the
 * launcher can shrink a cover. Unset, nothing is told. */
void hare_set_after_write(void (*fn)(const char *abs));

/* The most a delete takes at once, files and folders together. More than
 * this is almost certainly the wrong folder, and it is asked for in parts. */
#define HARE_DELETE_MAX 1000

/* What the screen puts on the panel. `ip` is the LAN address, from wifi. */
const char *hare_pin(void);
int         hare_port(void);

/* Drive it from the screen's frame loop. Returns non-zero when anything moved,
 * which is what keeps Auto Off from powering the device off mid-upload. */
int hare_poll(void);

/* For the screen: what is going on, in words a person can read. */
typedef struct {
	int           clients;      /* browsers currently connected */
	unsigned long in, out;      /* bytes since the last call */
	int           uploads;      /* transfers in flight */
	char          last[128];    /* the most recent thing that happened */
} hare_stats;

void hare_status(hare_stats *out);

/* Whether anything under the ROM, Music or Audiobooks root was written,
 * renamed or deleted since the screen opened. The shelf is scanned once at
 * startup and nothing watches the card, so a ROM, an album or a book arriving
 * over the network is
 * invisible until something rescans - which is what the screen uses this to
 * decide on the way out. */
bool hare_shelf_changed(void);

#endif
