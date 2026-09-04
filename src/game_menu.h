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
