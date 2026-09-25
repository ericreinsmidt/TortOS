/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_BTVOL_H
#define TORTOS_BTVOL_H
#include <stdbool.h>

/* Set the connected headset's own volume, 0..127 - AVRCP absolute volume, the
 * headset applying its own curve - through bluealsa's mixer control for it,
 * `<name> - A2DP`. False when there is no such control: no headset, or one
 * whose volume BlueZ has not been given (see tools/btplayer.c).
 *
 * One mixer open per call, so it is for changes, not for every frame. */
bool btvol_set(int level);

#endif
