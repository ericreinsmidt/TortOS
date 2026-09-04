/* What does a menu contain, for a given state?
 *
 * This check is the point of ADR-0001. A screen's build function takes state
 * and produces rows, touching no renderer and no device, so the answer can be
 * had here rather than by looking at a handheld. Before this, five defects in
 * one evening were all of the form "nobody noticed the list was wrong".
 *
 * If this file ever needs SDL to link, the decision has failed. Do not add it -
 * reopen ADR-0001 instead.
 */
#include "../src/wifi_menu.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

static int live_count(const menu_row *r, int n)
{
	int i, k = 0;
	for (i = 0; i < n; i++) if (r[i].live) k++;
	return k;
}

static int rule_at(const menu_row *r, int n)
{
	int i;
	for (i = 0; i < n; i++) if (!r[i].label) return i;
	return -1;
}

/* Every screen must be able to say "nothing here" without offering a row that
 * does nothing when pressed. The radio-off case is the one that used to draw a
 * selectable placeholder. */
static void off_state(void)
{
	wifi_ui w;
	menu_row rows[64];
	int n;

	memset(&w, 0, sizeof w);
	w.on = false;
	n = wifi_build(&w, rows, 64);

	printf("radio off:\n");
	ck(n == 3, "switch, rule, note");
	ck(rows[0].live, "the switch is live");
	ck(!strcmp(rows[0].label, "Wi-Fi"), "row 0 is the switch");
	ck(!strcmp(rows[0].value, "off"), "the switch reads off");
	ck(rule_at(rows, n) == 1, "the rule separates the footer");
	ck(ROW_IS_NOTE(rows[2]), "the footer is a note");
	ck(!rows[2].live, "the footer is not selectable");
	ck(live_count(rows, n) == 1, "only the switch can be chosen");
}

/* The state the async scan produces on entry: saved networks up immediately,
 * the footer saying what is happening, nothing claiming a signal it has not
 * measured. */
static void scanning_state(void)
{
	wifi_ui w;
	menu_row rows[64];
	int n;

	memset(&w, 0, sizeof w);
	w.on = true;
	w.scanning = true;
	w.scanned = false;
	w.n = 2;
	snprintf(w.nets[0].ssid, sizeof w.nets[0].ssid, "ReinFi");
	w.nets[0].known = true;
	snprintf(w.nets[1].ssid, sizeof w.nets[1].ssid, "Transport");
	w.nets[1].known = true;
	wifi_label(&w);
	n = wifi_build(&w, rows, 64);

	printf("scanning, two saved networks:\n");
	ck(n == 5, "switch, two networks, rule, note");
	ck(live_count(rows, n) == 3, "switch and both networks are choosable");
	ck(!strcmp(rows[1].label, "ReinFi"), "the first saved network is listed");
	ck(!strcmp(rows[1].value, "saved"), "no signal word before the scan lands");
	ck(rule_at(rows, n) == 3, "rule after the networks");
	ck(!strcmp(rows[4].label, "Scanning..."), "footer says what is happening");
	ck(ROW_IS_NOTE(rows[4]), "the progress line is a note");
}

/* After results land the footer becomes the key legend, and the legend only
 * offers forget when there is something to forget. */
static void scanned_state(void)
{
	wifi_ui w;
	menu_row rows[64];
	int n;

	memset(&w, 0, sizeof w);
	w.on = true;
	w.scanned = true;
	w.n = 1;
	snprintf(w.nets[0].ssid, sizeof w.nets[0].ssid, "Cafe");
	w.nets[0].known = false;
	w.nets[0].secured = true;
	w.nets[0].signal = -55;
	wifi_label(&w);
	n = wifi_build(&w, rows, 64);

	printf("scan done, one unsaved network:\n");
	ck(n == 4, "switch, network, rule, note");
	ck(!strcmp(rows[1].value, "strong"), "-55 dBm reads as strong");
	ck(!strcmp(rows[3].label, "Y: rescan"), "no forget offered with nothing saved");

	w.nets[0].known = true;
	wifi_label(&w);
	n = wifi_build(&w, rows, 64);
	ck(!strcmp(rows[3].label, "Y: rescan   X: forget"), "forget offered once saved");
	ck(!strcmp(rows[1].value, "strong - saved"), "a saved network in range shows both");
}

/* The bands are a judgment on a measured number and the boundary is not
 * validated - see BACKLOG. Pinned here so it cannot move by accident. */
static void strength_words(void)
{
	printf("signal words:\n");
	ck(!strcmp(wifi_strength(-40), "strong"), "-40 strong");
	ck(!strcmp(wifi_strength(-60), "strong"), "-60 is the strong boundary");
	ck(!strcmp(wifi_strength(-61), "good"),   "-61 good");
	ck(!strcmp(wifi_strength(-72), "good"),   "-72 is the good boundary");
	ck(!strcmp(wifi_strength(-73), "weak"),   "-73 weak, the measured cliff");
}

/* A build must never write past what it was given. */
static void respects_max(void)
{
	wifi_ui w;
	menu_row rows[4];
	int n, i;

	memset(&w, 0, sizeof w);
	w.on = true;
	w.scanned = true;
	w.n = WIFI_MAX_NETS;
	for (i = 0; i < WIFI_MAX_NETS; i++)
		snprintf(w.nets[i].ssid, sizeof w.nets[i].ssid, "net%d", i);
	wifi_label(&w);
	n = wifi_build(&w, rows, 4);

	printf("a small buffer:\n");
	ck(n <= 4, "never returns more rows than it was offered");
}

int main(void)
{
	off_state();
	scanning_state();
	scanned_state();
	strength_words();
	respects_max();
	if (fails) { printf("\n%d menu check(s) failed\n", fails); return 1; }
	printf("\nok: menus contain what they should\n");
	return 0;
}
