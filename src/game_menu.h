/* What the game info screen contains, as rows rather than as pixels.
 *
 * Split out of main.c under ADR-0001 so a check can ask what this screen says
 * about a game without a card, a renderer or a device. Nothing here may
 * include SDL - tools/menu-check.c links it without one.
 *
 * gi_gather stays in main.c, because reading a card is exactly the half that
 * does not belong here. */
#ifndef GAME_MENU_H
#define GAME_MENU_H

#include <stdbool.h>

#include "menu.h"
#include "library.h"

/* Save slots. Six the player picks from, plus the resume slot the exit funnel
 * owns - which is NAMED "auto" and is not one of the numbered ones.
 *
 * Here rather than in main.c so a check can see it. The carousel used to map
 * its Auto entry to a hardcoded 9, MinUI's number for the same idea, and when
 * the resume slot was renamed on 2026-08-27 every use of the CONSTANT was
 * updated and that literal was not. Loading Auto from the in-game menu asked
 * for a `.9.state` for ten days, and the state_rejected that came back was
 * misread as a dead emulator - which started a second one over the top of the
 * first and wedged the display. A number nothing could test. */
#define GM_SLOTS  6
#define SLOT_AUTO (GM_SLOTS + 1)

/* The carousel shows Auto first, then the numbered slots. This is the only
 * place that mapping is written down. */
static inline int gm_slot_at(int carousel_index)
{
	return carousel_index == 0 ? SLOT_AUTO : carousel_index;
}

/* What the info screen offers to do, beyond telling you things. */
typedef enum { GI_ART, GI_FAV, GI_ROWS } gi_row;

#define GI_MAX 7          /* five facts, two actions */

/* Everything worth saying about one game, gathered once.
 *
 * Gathered rather than watched: this screen is a still. It reads the card, the
 * save directory and the achievement set when it opens, and again after an
 * action changes one of them. Polling would mean a stat storm every frame for
 * numbers that only move when the player does something. */
typedef struct {
	char  file[LIB_PATH];
	char  size[32];
	char  saves[32];
	char  cheevos[64];
	char  art[32];
	bool  favorite;
	bool  has_art;
} game_info;

/* `net` is whether there is a network, asked by the caller and passed in: the
 * two rows that need one say so instead of quietly doing nothing. */
int gi_rows(menu_row *out, const game_info *gi, bool net);

/* ---------- the in-game menu ---------------------------------------------- */

typedef enum {
	GM_CONTINUE, GM_SAVE, GM_LOAD, GM_DISPLAY, GM_CHEEVOS, GM_RESET, GM_QUIT,
	GM_ROWS
} gm_row;

/* What the in-game menu needs to know, gathered by the caller. */
typedef struct {
	const char *dmode;    /* the display mode's label */
	int         earned;
	int         total;    /* 0: this game has no achievement set */
} gm_ui;

/* Where the Cheevos row's text lives; the caller owns it, because a row holds
 * a pointer rather than a copy. */
typedef struct { char cheevos[24]; } gm_bufs;

int gm_rows(const gm_ui *u, menu_row *out, gm_bufs *b);

#endif
