/* SPDX-License-Identifier: 0BSD */
#ifndef TORTOS_RAHASH_H
#define TORTOS_RAHASH_H

#include <stdbool.h>

/* RetroAchievements identifies a game by an MD5 over the ROM - but not always
 * over the WHOLE ROM. Each console has its own rule about what to skip, and
 * getting one wrong produces a hash RA has never seen, which is indistinguish-
 * able from a game it does not know.
 *
 * Two couplings worth stating, because both fail silently:
 *
 *   - THE ENTRY. A zipped archive is hashed on its LARGEST entry, which is the
 *     one Diatom loads (its src/zip.c). Taking the first instead would hash one
 *     game and run another, and every achievement would be for something else.
 *   - THE RULES. They are RetroAchievements', restated here in C and in
 *     tools/ra-check.py. Both are checked against the same ROMs; see
 *     `make check-rahash`.
 */

/* Writes 32 lowercase hex digits plus a NUL, so `out` needs 33 bytes.
 * `tag` is a systems.cfg tag - NES, SFC, MD, GB. False means the file could
 * not be read or the archive could not be opened. */
bool ra_hash_rom(const char *path, const char *tag, char *out);

#endif
