/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_DEVICE_H
#define TORTOS_DEVICE_H

#include "platform.h"

/* The seam inside the platform layer, between the parts every device shares
 * (platform.c, resident.c) and the one device being built (src/device/<name>.c,
 * chosen by DEVICE= at build time). Nothing outside the platform layer
 * includes this: the rest of TortOS sees platform.h only.
 *
 * A device file defines everything platform.h declares that touches hardware -
 * video, input, levels, the backlight, LEDs, the battery, where things live on
 * the card - plus the two functions below. Kept this short on purpose: each
 * line here is something a second device has to answer. */

/* Drain the power key's queue and say whether a press was in it. Called while
 * a child or a game owns the screen, so it must not block. */
bool device_power_pressed(void);

/* A game just handed input back. Forget whatever was remembered about the
 * hardware while something else was driving: the headphone jack, a mute
 * switch. */
void device_levels_forget(void);

/* The display, around a game. Release is called just before Diatom is told to
 * present (RUN, and the wait that follows a RESUME), take once it has handed
 * the screen back (PAUSED, EXIT, or the game gone). Nothing on a device where
 * both can draw to one framebuffer by turns, which is the Brick; passing DRM
 * master on one that drives its display through KMS (Diatom's ADR-0036). */
void device_display_release(void);
void device_display_take(void);

/* Shared by the portable half, for the device file. */
void plat_set_btn(in_state *st, in_button b, bool down);   /* a button's edge */
bool plat_terminating(void);                    /* SIGTERM arrived */
void plat_set_power_pressed(bool on);           /* the last game ended on power */
/* One line to a running game, on the connection already open; connects for
 * nothing. */
bool plat_resident_tell(const char *fmt, ...);

#endif
