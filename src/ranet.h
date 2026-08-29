/* SPDX-License-Identifier: 0BSD */
#ifndef TORTOS_RANET_H
#define TORTOS_RANET_H

#include <stdbool.h>
#include <stddef.h>

/* Talking to RetroAchievements.
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
void ra_set_ca_path(const char *path);

typedef struct { const char *k, *v; } ra_field;

/* Small replies - login, gameid, startsession. Returns the body length, or
 * -1: no curl, no network, HTTP error, or a timeout. The distinction between
 * those is logged, not returned, because every caller does the same thing
 * with it - carry on without. */
long ra_post_buf(const ra_field *f, int n, char *out, size_t outn, int timeout_s);

/* Large replies - a patch response runs past 100KB for a big set. Written
 * straight to `path` so nothing has to guess a buffer size. Writes through a
 * temporary and renames, so an interrupted fetch cannot leave a half a set
 * where a whole one is expected. */
bool ra_post_file(const ra_field *f, int n, const char *path, int timeout_s);

/* Whether there is any point trying: curl present and an address on a
 * non-loopback interface. Cheap, and it turns "achievements did not appear"
 * into something the menu can explain. */
bool ra_online(void);

#endif
