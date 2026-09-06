/* SPDX-License-Identifier: MIT */
/* How long each game has been played.
 *
 * WALL CLOCK, from RUN to EXIT, and there is nothing to subtract: the device
 * has no suspend and is not getting one (see the Auto Off row in sys_menu.c).
 * Auto Off powers it OFF, so a session left running is bounded by that
 * interval rather than by how long someone was away.
 *
 * THE LAUNCH PATH DOES NO I/O. stats_begin notes a number in memory and
 * nothing else - no file opened, nothing written, nothing synced - because
 * launch speed is the point of this firmware and a millisecond bought here
 * would be the first of several. Everything else happens from the game tick,
 * which already runs at 10 Hz and is cheap by contract, or at exit.
 *
 * NOTHING IS FSYNCED WHILE A GAME RUNS. Measured on the card: an fsync is
 * 1.56 ms median but 16.4 ms at the tail, which is a dropped frame. The writes
 * during play are unsynced, so they cost 0.007 ms and reach the card when the
 * kernel gets to them - about 33 s later, measured. That window is the right
 * trade here and self-correcting: by the time a session is long enough to be
 * worth recording, its marker has been on the card for a long while.
 *
 * SESSIONS THAT NEVER REACH EXIT. A power cut or a flat battery means no EXIT,
 * so a marker is written a few seconds in and updated every minute or so. On
 * the next boot an unclosed row is closed at its last checkpoint - short by up
 * to that interval rather than lost entirely.
 *
 * Rows live in the LIBRARY database, not the device one: play time belongs to
 * the card the games are on, the same argument favorites and earned
 * achievements are stored under. Keyed
 * "sess.<start>.<clock>.<tag>\t<file>", where start is the unix time the
 * session began and clock is the millisecond clock at RUN. The second number
 * is there only to keep two launches of one game inside the same second from
 * sharing a key, and it has to be something already in hand: asking the
 * database whether a key is free would put a read on the launch path.
 */
#ifndef TORTOS_STATS_H
#define TORTOS_STATS_H

#include <stdbool.h>
#include <stddef.h>

/* A game is a system tag plus the ROM's launch path, the same key favorites
 * use, so a file of the same name on two systems counts as two games. */
#define STATS_TAG_MAX   16
#define STATS_FILE_MAX 544
#define STATS_MAX      512   /* distinct games held in memory when summarizing */

/* Marker after this long, so a launch someone backs straight out of writes
 * nothing at all. Then a checkpoint at this interval, which is also the most
 * a crash can lose. */
#define STATS_MARK_MS  (5 * 1000u)
#define STATS_CKPT_MS  (60 * 1000u)

/* RUN. In-memory only - see the header note. */
void stats_begin(const char *tag, const char *file, unsigned now_ms);

/* From the game tick. Writes at most one unsynced row, and usually nothing. */
void stats_tick(unsigned now_ms);

/* EXIT. The one write that is synced, on the path where the player is already
 * waiting for the shelf rather than for a game. `reason` is Diatom's own EXIT
 * reason, or NULL for a clean quit. */
void stats_end(const char *reason, unsigned now_ms);

/* Close any session left open by a power cut, at its last checkpoint. Call
 * once at startup, before anything reads a total. */
void stats_recover(void);

/* --- reading, for the menu ---------------------------------------------- */

/* Fold every session into one row per game, most-played first. Returns the
 * number of distinct games. */
int  stats_summarize(void);
bool stats_at(int i, const char **tag, const char **file,
              long *seconds, int *launches);
long stats_total_seconds(void);
int  stats_total_launches(void);

/* "12h 34m", "7m 12s", "never". `out` takes at least 16 bytes. */
void stats_format(long seconds, char *out, size_t n);

#endif
