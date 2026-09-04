/* The Wi-Fi screen, minus the screen.
 *
 * State and row building only. This file must never include SDL: ADR-0001 says
 * a build function has to be callable with no renderer and no device, and
 * tools/menu-check.c links it directly to prove it. If that stops being true
 * the decision has failed and should be reopened rather than the requirement
 * quietly dropped.
 *
 * The half that needs a screen - wifi_key, wifi_screen - stays in main.c.
 */
#include "wifi_menu.h"

#include <stdio.h>
#include <string.h>

/* Signal as a word. dBm is the honest number and it is also jargon; the list
 * is sorted strongest first anyway, so the word only has to separate "this
 * will work" from "this will not". */
const char *wifi_strength(int dbm)
{
	if (dbm >= -60) return "strong";
	if (dbm >= -72) return "good";
	return "weak";
}


/* The right-hand column for every row, rebuilt whenever the list changes.
 *
 * "connected" outranks "saved": after a successful connect the list was redrawn
 * saying only that the network was known, which is what it said before the
 * connect too, so the screen gave no sign anything had happened. */
void wifi_label(wifi_ui *w)
{
	char cur[WIFI_SSID_MAX];
	int i;

	if (wifi_status(cur, sizeof cur, NULL, 0) != WIFI_CONNECTED) cur[0] = '\0';
	for (i = 0; i < w->n; i++) {
		const char *state = (cur[0] && !strcmp(cur, w->nets[i].ssid))
		                    ? " - connected"
		                    : w->nets[i].known ? " - saved"
		                    : w->nets[i].secured ? "" : " - open";

		/* A saved network listed before the scan has no signal reading, and
		 * printing "weak" for one that has simply not been heard from yet
		 * would be a measurement we do not have. */
		if (!w->scanned && w->nets[i].known)
			snprintf(w->vals[i], sizeof w->vals[i], "saved");
		else
			snprintf(w->vals[i], sizeof w->vals[i], "%s%s",
			         wifi_strength(w->nets[i].signal), state);
	}
}

/* Start a scan and show what we already know while it runs.
 *
 * Entering this screen used to stand still behind a "Scanning..." panel for up
 * to nine seconds, because a scan settles slowly on purpose. The saved networks
 * need no scan at all - the supplicant is holding them - so they go up
 * immediately and the scan fills in around them. */
void wifi_begin_scan(wifi_ui *w)
{
	w->scanned = false;
	w->n = wifi_known(w->nets, WIFI_MAX_NETS);
	wifi_label(w);
	w->scanning = wifi_scan_start();
}

int wifi_build(void *ctx, menu_row *rows, int max)
{
	wifi_ui *w = ctx;
	int nrows = 0, i;
	bool any_saved = false;

	/* The scan is driven from here because this is the function that already
	 * runs every frame. wifi_scan_poll() does nothing until its next second is
	 * due, so calling it per frame costs a comparison. When results land the
	 * provisional list of saved networks is replaced by what the radio heard.
	 *
	 * This is still the only place rows are built, and it still touches no
	 * renderer - ADR-0001's requirement holds. */
	if (w->scanning && wifi_scan_poll() == 1) {
		int got = wifi_scan_take(w->nets, WIFI_MAX_NETS);

		w->scanning = false;
		w->scanned = true;
		if (got >= 0) w->n = got;
		wifi_label(w);
	}

	/* The switch is row 0, so the state of the radio is the first thing read
	 * and the first thing reachable. */
	rows[nrows++] = (menu_row){ "Wi-Fi", w->on ? "on" : "off", true };
	for (i = 0; i < w->n && nrows < max - 2; i++)
		rows[nrows++] = (menu_row){ w->nets[i].ssid, w->vals[i], true };

	/* These are footers, not list items - they say something about the list
	 * rather than offering anything - so they sit under a rule like the key
	 * legend does. */
	if (!w->on) {
		rows[nrows++] = MENU_RULE;
		rows[nrows++] = MENU_NOTE("Turn Wi-Fi on to scan");
		return nrows;
	}
	if (w->n == 0 && !w->scanning) {
		rows[nrows++] = MENU_RULE;
		rows[nrows++] = MENU_NOTE("No networks found");
		return nrows;
	}
	for (i = 0; i < w->n; i++)
		if (w->nets[i].known) { any_saved = true; break; }
	rows[nrows++] = MENU_RULE;
	/* What is happening, then what you can do. The footer is the honest place
	 * for progress: it is already outside the list, and swapping its text
	 * means the screen never has to put a modal in front of you. */
	rows[nrows++] = w->scanning
	              ? MENU_NOTE("Scanning...")
	              : MENU_NOTE(any_saved ? "Y: rescan   X: forget" : "Y: rescan");
	return nrows;
}

