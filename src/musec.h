/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_MUSEC_H
#define TORTOS_MUSEC_H

#include <stdbool.h>

/* The launcher's side of Muse: the connection to the daemon, and the play
 * queue.
 *
 * The daemon (src/muse/) knows one file at a time. What comes next is policy,
 * and policy is the launcher's - so this holds the queue, and when the daemon
 * says END it sends the next PLAY itself.
 *
 * Polled, never waited on: musec_poll reads whatever has arrived and returns.
 * It is called from the shelf's main loop, from Muse's own screen, and from
 * the 10 Hz tick the launcher gets while a game runs - which is what lets an
 * album carry on through a game, the way EROS's Muse did. The mixing itself
 * needs nothing from anybody: both processes open `default`, which is dmix. */

typedef enum { MU_OFF, MU_STOPPED, MU_PLAYING, MU_PAUSED } mu_state;

typedef struct {
	mu_state state;
	char     title[128], artist[128], album[128];
	double   at, len;
	int      index, count;      /* where in the queue, and how long it is */
} mu_now;

/* Where the daemon lives, and the folder paths are relative to. Starting it
 * is lazy: the first call that wants it spawns it if it is not answering. */
void musec_init(const char *muse_bin, const char *music_root);

void musec_poll(void);

/* Queue `n` tracks - paths relative to the music root - and play from
 * `start`. The launcher's copies; the caller's array may go away. */
void musec_play(const char *const *paths, int n, int start,
                const char *artist, const char *album);

void musec_toggle(void);       /* pause or resume */
void musec_next(void);
void musec_prev(void);         /* to the start of this track, or the one before */
void musec_seek_by(double delta);
void musec_stop(void);

const mu_now *musec_now(void);

/* The queued track, relative to the music root, or "" - so a list can mark
 * the row that is playing without comparing titles, which tags can change. */
const char *musec_path(void);

/* Playing right now, for Auto Off: music is somebody using the device with
 * nobody touching it, the same as the charger. */
bool musec_playing(void);

#endif
