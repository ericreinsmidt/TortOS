/* The game info screen's rows. See src/game_menu.h. */
#include <stddef.h>
#include <stdio.h>

#include "game_menu.h"

int gi_rows(menu_row *out, const game_info *gi, bool net)
{
	int n = 0;

	/* WHAT OPENS INTO SOMETHING COMES FIRST, then what is only worth reading.
	 *
	 * The two rows that lead are the two the cursor can act on, so the screen
	 * starts where the hand starts. Cheevos before the synopsis because it is
	 * the one fact here that changes while you play, and it is what this screen
	 * gets opened to check; the synopsis is the same every time.
	 *
	 * Cheevos opens the same list the in-game menu opens - which is where this
	 * row's own count comes from. Reading "4/54" and having nowhere to go with
	 * it is the question half-answered: which four, and what are the other
	 * fifty. A game with no set has nothing to open and says so. */
	out[n++] = (menu_row){ "Cheevos",  gi->cheevos, gi->has_cheevos };
	/* Both only once a scrape has spoken for this game. Before that they would
	 * say "unknown" on every game on the card, which is a row asking to be
	 * ignored rather than a fact. A scrape that came back without a year simply
	 * says nothing; one that came back without prose says "none", the way the
	 * in-game Cheevos row does, because the difference between "not looked up"
	 * and "looked up, nothing there" is worth a word. */
	if (gi->scraped)
		out[n++] = (menu_row){ "Synopsis", gi->has_synopsis ? NULL : "none",
		                       gi->has_synopsis };
	if (gi->scraped && gi->year[0])
		out[n++] = (menu_row){ "Year",  gi->year, false };
	/* WHAT THIS SCREEN NO LONGER SAYS, and why, because each was removed on a
	 * reason rather than to make the list shorter:
	 *
	 * File. The panel is headed by the game's name and the shelf behind it is
	 * showing the same game, so the row spent its width repeating what was
	 * already on screen twice over, plus a region tag and an extension. What
	 * goes with it is the one place the exact bytes of a filename could be
	 * read, which is a real loss on the day a scrape misses - and still not
	 * worth a permanent row that every player reads past every time.
	 *
	 * Size. The size of a cartridge ROM has no consequence on a card with room
	 * for a thousand of them, and it is not a number anybody acts on.
	 *
	 * Box Art, as a count of bytes. The cover is on the shelf behind this
	 * panel, so whether there is one is already answered by looking; the only
	 * part worth saying is whether the action below gets one or replaces one,
	 * which that row says in its own label.
	 *
	 * Favorite. Y does it from the shelf, on both screens that have a cursor,
	 * and a second way to do the same thing is a second thing to keep working.
	 * The row also had to rebuild the Favorites shelf under its own cursor and
	 * decide whether the game it was describing had moved out from under it. */
	out[n++] = (menu_row){ gi->has_art ? "Replace Box Art" : "Get Box Art",
	                       net ? NULL : "needs Wi-Fi", net };
	/* Last, and the only row here about your copy rather than the game. It is
	 * also the one nobody comes to this screen for: a save is something you
	 * notice from the carousel when you load, not something you look up. */
	out[n++] = (menu_row){ "Saves",    gi->saves,   false };
	return n;
}

/* The in-game rows, carrying the display mode's current label. */
int gm_rows(const gm_ui *u, menu_row *out, gm_bufs *b)
{
	static const char *label[GM_ROWS] = {
		"Continue", "Save", "Load", "Display", "Cheevos", "Reset", "Quit"
	};
	int i;

	for (i = 0; i < GM_ROWS; i++) out[i] = (menu_row){ label[i], NULL, true };
	out[GM_DISPLAY].value = u->dmode;

	/* Most of a library has no set, and a row that says so plainly is better
	 * than one that is missing: "none" answers the question the player opened
	 * the menu to ask. Drawn quiet, and does nothing when chosen - so the
	 * cursor steps over it rather than resting on a row that ignores A. */
	if (u->total > 0)
		snprintf(b->cheevos, sizeof b->cheevos, "%d / %d", u->earned, u->total);
	else {
		snprintf(b->cheevos, sizeof b->cheevos, "none");
		out[GM_CHEEVOS].live = false;
	}
	out[GM_CHEEVOS].value = b->cheevos;
	return GM_ROWS;
}
