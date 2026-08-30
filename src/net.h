/* SPDX-License-Identifier: 0BSD */
#ifndef TORTOS_NET_H
#define TORTOS_NET_H

#include <stdbool.h>
#include <stddef.h>

/* The launcher's HTTP client.
 *
 * It was ranet.c, and RetroAchievements was the only thing that talked. Box
 * art is fetched from libretro's thumbnail collection now and needs the same
 * curl, the same certificate store and the same fork-and-execv, so the file
 * that owns all three stopped being about one service. Nothing about the
 * transport was ever specific to RetroAchievements.
 *
 * Through the device's own curl, by fork and execv, the same way wifi.c drives
 * wpa_cli. The Brick ships curl 7.54.1 against OpenSSL 1.1.0i, so HTTPS is
 * already here; what it does not ship is anything to trust, which is why
 * res/ssl/cacert.pem travels with the launcher (see its README for the
 * measurement).
 *
 * THE REQUEST GOES IN A FILE, not on the command line. curl's -K reads options
 * from one, and an account token in argv is readable by anything that can list
 * processes. The file is written 0600 and unlinked straight after.
 *
 * Every call blocks for at most `timeout_s`. Nothing here is called from a
 * frame loop; the shelf fetches between frames and the launch path uses a
 * cached set when the network is slow or absent.
 */

/* Where the certificate store is. Set once at startup; without it every
 * handshake fails verification, because the device has no trust store of its
 * own (res/ssl/README.md). Passed in rather than read from platform.h so this
 * file does not drag SDL into a check that only wants to talk to a socket. */
void net_set_ca_path(const char *path);

typedef struct { const char *k, *v; } net_field;

/* Small replies - login, gameid, startsession. Returns the body length, or
 * -1: no curl, no network, HTTP error, or a timeout. The distinction between
 * those is logged, not returned, because every caller does the same thing
 * with it - carry on without. */
long net_post_buf(const net_field *f, int n, char *out, size_t outn, int timeout_s);

/* Large replies - a patch response runs past 100KB for a big set. Written
 * straight to `path` so nothing has to guess a buffer size. Writes through a
 * temporary and renames, so an interrupted fetch cannot leave a half a set
 * where a whole one is expected. */
bool net_post_file(const net_field *f, int n, const char *path, int timeout_s);

/* The same request, started and left to run. Nothing waits for it.
 *
 * This exists because a launch was 15ms warm and a request to RetroAchievements
 * is 310-460ms measured on the device, 150ms of which is the TLS handshake
 * alone - so anything on the launch path that waits for the network has
 * already lost. One in flight at a time, which is all this needs.
 *
 * net_async_poll: 1 finished and the file is there, 0 still running, -1 nothing
 * started or it failed. Reaping is the caller's job via poll; an unreaped
 * child is a zombie until then. */
bool net_post_async(const net_field *f, int n, const char *path, int timeout_s);
int  net_async_poll(void);

/* Plain GET, for things that are not RetroAchievements: a directory index
 * into a buffer, an image straight to a file. Same certificate store, same
 * timeout, same fork-and-execv.
 *
 * net_get_file writes through a temporary and renames, so an interrupted
 * download cannot leave half a PNG where later runs would read it as art that
 * is already there and skip it forever. */
long net_get_buf(const char *url, char *out, size_t outn, int timeout_s);
bool net_get_file(const char *url, const char *path, int timeout_s);

/* The same GET, started and left running, reaped through net_async_poll.
 *
 * For anything driven from a screen's frame loop. A blocking fetch there is
 * not merely slow: that loop is where the power button is read, so a request
 * sitting on its timeout is a handheld that has stopped answering its own
 * power button for a minute. One in flight at a time, like net_post_async.
 *
 * The file appears at `path` only when the poll says 1 - it is written
 * through a temporary, so a run that is cancelled or dies partway cannot
 * leave half a file where a later run would find it and skip the download. */
bool net_get_async(const char *url, const char *path, int timeout_s);

/* Give up on whatever is in flight: kill it, reap it, and free the slot.
 *
 * For a caller that stops caring - a screen the player closed mid-fetch. Not
 * optional politeness: there is ONE slot, and a request abandoned without
 * this holds it forever. Everything async then fails, permanently, and
 * silently. */
void net_async_abort(void);

/* Whether there is any point trying: curl present and an address on a
 * non-loopback interface. Cheap, and it turns "achievements did not appear"
 * into something the menu can explain. */
bool net_online(void);

#endif
