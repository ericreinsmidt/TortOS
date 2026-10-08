/* SPDX-License-Identifier: MIT */
/* See controls.h. The pages, and nothing else. */
#include "controls.h"

#include <stddef.h>

static bool fn_volume;   /* see ctl_set_fn_volume */

void ctl_set_fn_volume(bool on) { fn_volume = on; }

static const char *const NAMES[CTL_PAGES] = {
	[CTL_MOVING]   = "Moving",
	[CTL_SHELF]    = "On the shelf",
	[CTL_GAME]     = "In a game",
	[CTL_MUSE]     = "In Muse",
	[CTL_ANYWHERE] = "Anywhere",
};

const char *ctl_page_name(ctl_page p)
{
	return NAMES[(unsigned)p < CTL_PAGES ? p : CTL_MOVING];
}

/* A PAIR OF BUTTONS IS WRITTEN "L1/R1", with no air around the slash. Eric's,
 * 2026-09-20: spaced out it reads as two separate things rather than one
 * control with two ends, and it is not how he writes them.
 *
 * EVERY PAGE FITS, so none of them moves while it is being read.
 *
 * The panel holds seven rows under a one-line heading, and the last of the
 * seven is the footer. Six lines of content, measured against the shipped text
 * size 2026-09-19: at eight the footer was drawn half off the panel, and a
 * cursorless list that does not fit scrolls itself, which is right for a
 * synopsis and wrong for a table you are trying to read a button off.
 *
 * That ceiling is why the shelf is two pages. Its seven buttons split where
 * the hand does: the pad and the shoulders get you around, and the four face
 * buttons do something to what you have landed on. */
static int moving(ctl_dir dir, menu_row *out)
{
	int n = 0;

	/* Which axis does what is the whole of UI Direction. Cubic takes both -
	 * systems on one, games on the other - so it has no letter jump. */
	if (dir == CTL_CUBIC) {
		out[n++] = (menu_row){ "Up/Down",    "Console",        false };
		out[n++] = (menu_row){ "Left/Right", "Game",           false };
	} else if (dir == CTL_VERTICAL) {
		out[n++] = (menu_row){ "Up/Down",    "Move",           false };
		out[n++] = (menu_row){ "Left/Right", "Jump by letter", false };
	} else {
		out[n++] = (menu_row){ "Left/Right", "Move",           false };
		out[n++] = (menu_row){ "Up/Down",    "Jump by letter", false };
	}
	out[n++] = (menu_row){ "L1/R1", "A screenful", false };
	return n;
}

static int shelf(ctl_dir dir, menu_row *out)
{
	int n = 0;

	out[n++] = (menu_row){ "A", dir == CTL_CUBIC ? "Start the game"
	                                             : "Open or play",   false };
	/* On the cube there is nowhere to go back to - the cube IS the shelf - so
	 * B is the console's menu there instead. */
	out[n++] = (menu_row){ "B", dir == CTL_CUBIC ? "Console's menu"
	                                             : "Back",           false };
	out[n++] = (menu_row){ "X", "Game details", false };
	out[n++] = (menu_row){ "Y", "Favorite",     false };
	return n;
}

static int game(menu_row *out)
{
	int n = 0;

	/* Fast forward rides on MENU's row, because a seventh row does not fit
	 * (PAGE_MAX in tools/controls-check.c). MENU+R1 steps 2x, 3x, 4x and back
	 * to normal - Diatom's ADR-0034 - and MENU opens the menu on release so it
	 * can be held for it. Eric's wording, 2026-10-01, SELECT's with it. */
	out[n++] = (menu_row){ "MENU",   "Menu, +R1 for FF",   false };
	out[n++] = (menu_row){ "SELECT", "Muse in menu",       false };
	/* X and Y are Diatom's turbo on the nine two-button systems; L2 and R2
	 * are mGBA's own Turbo L and R, which work on GBA and do nothing on GB and
	 * GBC, which have no shoulders (found 2026-09-30, choosing a fast-forward
	 * button). One row for both, since the page is full. */
	out[n++] = (menu_row){ "X/Y/L2/R2", "Turbo A/B/L1/R1 *", false };
	out[n++] = (menu_row){ "POWER",  "Save and turn off",  false };
	/* The exceptions as a note rather than a longer row: X and Y are the pad's
	 * own buttons on Genesis and SNES, and L2 and R2 only do anything on GBA.
	 * Kept short - a note runs the width of the panel and is cut at about
	 * thirty characters. An asterisk on both ends, the row's value and this
	 * line, so the two read as a footnote and its mark rather than as two
	 * separate claims. Eric's, 2026-09-20; reworded by him 2026-10-01. */
	out[n++] = MENU_NOTE("* for some systems");
	return n;
}

static int muse(menu_row *out)
{
	int n = 0;

	/* A on Muse's SHELF opens an album, which the shelf page already says.
	 * These are the player's own buttons, the ones a shelf does not have. */
	out[n++] = (menu_row){ "A",          "Play or pause", false };
	out[n++] = (menu_row){ "L1/R1",      "Track or chapter", false };
	/* Ten seconds a tap and further held - see seek_step in main.c. Said as
	 * what it does, since the numbers do not fit in a value. */
	out[n++] = (menu_row){ "Left/Right", "Seek, faster held", false };
	/* "Mode" rather than "Mode, on Now Playing": you press it on Now Playing,
	 * where the mark beside the track count changes in front of you. Eric's,
	 * 2026-09-20 - the rest of that sentence was saying what the screen was
	 * about to show anyway. "Or speed" since 2026-09-30: on a book, which
	 * plays in order, Y is its speed instead. */
	out[n++] = (menu_row){ "Y",          "Mode or speed", false };
	/* B is not here. It goes back in Muse exactly as it does everywhere else,
	 * and the shelf's page already says so - and with the rule above the
	 * footer this page has room for five, not six. */
	/* The pocket lock (#51). It took SELECT's row: "Close Muse" is SELECT
	 * doing on Muse what the Anywhere page already says it does. F1/F2 are
	 * L3/R3 to anyone coming from another CFW; the guide says so. */
	out[n++] = (menu_row){ fn_volume ? "FN+both Vol" : "F1+F2", "Lock, held", false };
	return n;
}

static int anywhere(menu_row *out)
{
	int n = 0;

	out[n++] = (menu_row){ "SELECT",        "Muse",       false };
	/* MENU is here rather than on a shelf page because it is the same promise
	 * wherever you are: the settings for whatever is in front of you. In a
	 * game it is a different menu, and that page says so. */
	out[n++] = (menu_row){ "MENU",          "Settings",   false };
	out[n++] = (menu_row){ "Volume rocker", "Sound",      false };
	out[n++] = (menu_row){ fn_volume ? "FN+Vol" : "F1/F2", "Brightness", false };
	out[n++] = (menu_row){ "POWER",         "Turn off",   false };
	return n;
}

int ctl_rows(ctl_page p, ctl_dir dir, menu_row *out)
{
	int n;

	switch (p) {
	case CTL_SHELF:    n = shelf(dir, out);  break;
	case CTL_GAME:     n = game(out);        break;
	case CTL_MUSE:     n = muse(out);        break;
	case CTL_ANYWHERE: n = anywhere(out);    break;
	default:           n = moving(dir, out); break;
	}
	/* The page is one of several and says so, the way Play Time's footer names
	 * the keys that change its window, under the same rule that divides every
	 * other list from its footer. Eric's, 2026-09-20. The rule is a row here
	 * but a thin one - a few pixels and the air around them, against a row's
	 * full height - which is what lets these pages keep it and still fit. */
	out[n++] = MENU_RULE;
	out[n++] = MENU_NOTE("Left/Right: more");
	return n;
}
