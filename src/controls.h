/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_CONTROLS_H
#define TORTOS_CONTROLS_H

#include "menu.h"

/* Every button, and what it does where: the one place ON THE DEVICE that says
 * so. Eric's, 2026-09-18, backlog 36.
 *
 * The controls that need it most are the ones no screen announces. SELECT
 * opens Muse from anywhere, the volume keys and F1/F2 work everywhere, and Y
 * sets the play mode on one Muse screen of four. Each of those is learned, or
 * read in a README that is not on the Brick. This page is where a player who
 * has only the handheld can look them up.
 *
 * THE SHELF'S PAGE FOLLOWS UI DIRECTION rather than listing all three. Which
 * axis moves and which jumps by letter IS that setting's effect, so a page
 * describing the two directions you are not in is a page to read past.
 *
 * The device is the source. The README's Controls section is a summary and
 * docs/guide.md is the long version, but this is what the hardware in the
 * player's hands says, and it is the copy that cannot be out of date by being
 * on a different device.
 *
 * Split out of main.c under ADR-0001, the way sys_menu.c is: no SDL here, so
 * tools/controls-check.c can link it and read what these pages contain
 * without a renderer. */

/* Five pages, because six rows is what a panel holds under its heading and
 * the shelf's seven buttons do not fit on one: the pad and the shoulders move
 * you, the face buttons act on what you moved to. */
typedef enum {
	CTL_MOVING, CTL_SHELF, CTL_GAME, CTL_MUSE, CTL_ANYWHERE, CTL_PAGES
} ctl_page;

/* Which way the shelves run - UI Direction, as cards.h stores it. */
typedef enum { CTL_HORIZONTAL, CTL_VERTICAL, CTL_CUBIC } ctl_dir;

/* Room for the longest page and its footer, with a little spare. */
#define CTL_MAX_ROWS 10

/* What a page is called, under the heading. */
const char *ctl_page_name(ctl_page p);

/* One page's rows, the rule and footer note included, and how many there are.
 * Every button the Brick has is named on some page, and no page names one
 * twice - what tools/controls-check.c holds this to. A button can appear on
 * two pages when it does two things: A opens a game on the shelf and pauses a
 * track in Muse. */
int ctl_rows(ctl_page p, ctl_dir dir, menu_row *out);

/* Whether brightness is FUNCTION with the volume keys, on a device with no
 * brightness keys of its own (the Pixel 2, plat_brightness_on_volume): the
 * pages then name those where the Brick's say F1/F2, and Muse's lock is
 * FUNCTION with both. The Brick's names until it is set. */
void ctl_set_fn_volume(bool on);

#endif
