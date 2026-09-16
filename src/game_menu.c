/* The game info screen's rows. See src/game_menu.h. */
#include <stddef.h>
#include <stdio.h>

#include "game_menu.h"

int gi_rows(menu_row *out, const game_info *gi, bool net)
{
	int n = 0;

	/* What is true of the GAME first, then what is true of your copy of it.
	 * Cheevos leads because it is the one fact that changes while you play,
	 * and it is what this screen is opened to check. Year and the synopsis
	 * belong directly under it when the scrape that fills them exists. */
	out[n++] = (menu_row){ "Cheevos",  gi->cheevos, false };
	/* Only once a scrape has spoken for this game. Before that these two rows
	 * would say "unknown" on every game on the card, which is a row asking to
	 * be ignored rather than a fact. A scrape that came back without a year
	 * still says nothing; one that came back without prose says "none", the
	 * way the in-game Cheevos row does, because the difference between "not
	 * looked up" and "looked up, nothing there" is worth a word. */
	if (gi->scraped && gi->year[0])
		out[n++] = (menu_row){ "Year",  gi->year, false };
	if (gi->scraped)
		out[n++] = (menu_row){ "Synopsis", gi->has_synopsis ? NULL : "none",
		                       gi->has_synopsis };
	out[n++] = (menu_row){ "File",     gi->file,    false };
	out[n++] = (menu_row){ "Size",     gi->size,    false };
	out[n++] = (menu_row){ "Saves",    gi->saves,   false };
	out[n++] = (menu_row){ "Box Art",  gi->art,     false };
	/* The two live rows last, under the facts, because they act on them. */
	out[n++] = (menu_row){ gi->has_art ? "Replace Box Art" : "Get Box Art",
	                       net ? NULL : "needs Wi-Fi", net };
	out[n++] = (menu_row){ "Favorite", gi->favorite ? "yes" : "no", true };
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
