/* What does a menu contain, for a given state?
 *
 * This check is the point of ADR-0001. A screen's build function takes state
 * and produces rows, touching no renderer and no device, so the answer can be
 * had here rather than by looking at a handheld. Every menu defect this
 * project has had was some form of "nobody noticed the list was wrong", and
 * every one of them needed a device to see.
 *
 * If this file ever needs SDL to link, the decision has failed. Do not add it -
 * reopen ADR-0001 instead.
 */
#include "../src/wifi_menu.h"
#include "../src/sys_menu.h"

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
	const char *heading;
	int n;

	memset(&w, 0, sizeof w);
	w.on = false;
	n = wifi_build(&w, rows, 64, &heading);

	printf("radio off:\n");
	ck(heading && !strcmp(heading, "Wi-Fi"), "the screen names itself");
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
	const char *heading;
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
	n = wifi_build(&w, rows, 64, &heading);

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
	const char *heading;
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
	n = wifi_build(&w, rows, 64, &heading);

	printf("scan done, one unsaved network:\n");
	ck(n == 4, "switch, network, rule, note");
	ck(!strcmp(rows[1].value, "strong"), "-55 dBm reads as strong");
	ck(!strcmp(rows[3].label, "Y: rescan"), "no forget offered with nothing saved");

	w.nets[0].known = true;
	wifi_label(&w);
	n = wifi_build(&w, rows, 64, &heading);
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
	const char *heading;
	int n, i;

	memset(&w, 0, sizeof w);
	w.on = true;
	w.scanned = true;
	w.n = WIFI_MAX_NETS;
	for (i = 0; i < WIFI_MAX_NETS; i++)
		snprintf(w.nets[i].ssid, sizeof w.nets[i].ssid, "net%d", i);
	wifi_label(&w);
	n = wifi_build(&w, rows, 4, &heading);

	printf("a small buffer:\n");
	ck(n <= 4, "never returns more rows than it was offered");
}


/* ---------- the two MENU-button menus ------------------------------------- */

static const char *val(const menu_row *r) { return r->value ? r->value : ""; }

/* A device with no network. Three rows depend on one, and all three have to
 * say so rather than silently doing nothing: Over The Hare, Box Art, and Box
 * Art's counterpart in the system menu. */
static void tortos_menu_offline(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;
	int n;

	memset(&u, 0, sizeof u);
	u.wifi = WIFI_OFF;
	u.text_size = "100%";
	u.auto_off = 120;
	n = sys_menu_build(&u, rows, &b, &heading);

	printf("TortOS menu, radio off:\n");
	ck(n == PM_ROWS, "every row is filled");
	ck(!strcmp(heading, "TortOS"), "heading");
	ck(!strcmp(val(&rows[PM_WIFI]), "off"), "Wi-Fi reads off");
	ck(!strcmp(val(&rows[PM_XFER]), "needs Wi-Fi"), "OTH says why it is dead");
	ck(!rows[PM_XFER].live, "OTH is not selectable offline");
	ck(!strcmp(val(&rows[PM_SCRAPE]), "needs Wi-Fi"), "Box Art says why");
	ck(!rows[PM_SCRAPE].live, "Box Art is not selectable offline");
	ck(!strcmp(val(&rows[PM_ACHIEVEMENTS]), "sign in"), "Cheevos invites a sign in");
	ck(rows[PM_ACHIEVEMENTS].live, "Cheevos is reachable signed out");
	ck(!strcmp(val(&rows[PM_SLEEP]), "2m"), "120s reads as 2m");
	ck(!strcmp(val(&rows[PM_TEXT]), "100%"), "text size is passed through");
	ck(!rows[PM_BT].live, "Bluetooth is still a placeholder");
}

/* Connected and signed in. The Wi-Fi row shows the network's NAME - a settings
 * row says what the setting is, and the address lives on the About page. */
static void tortos_menu_online(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;
	int n;

	memset(&u, 0, sizeof u);
	u.wifi = WIFI_CONNECTED;
	u.ssid = "kitchen";
	u.ra_in = true;
	u.ra_name = "eric";
	u.text_size = "115%";
	u.auto_off = 0;
	n = sys_menu_build(&u, rows, &b, &heading);

	ck(n == PM_ROWS, "row count does not depend on the network");
	printf("TortOS menu, connected:\n");
	ck(!strcmp(val(&rows[PM_WIFI]), "kitchen"), "Wi-Fi shows the network name");
	ck(rows[PM_XFER].live && !rows[PM_XFER].value, "OTH is live and unqualified");
	ck(rows[PM_SCRAPE].live, "Box Art is live");
	ck(!strcmp(val(&rows[PM_ACHIEVEMENTS]), "eric"), "Cheevos shows the account");
	ck(!strcmp(val(&rows[PM_SLEEP]), "off"), "0s reads as off");
}

/* Connecting and idle are not the same as off, and the row must not flatten
 * them: "not connected" and "connecting" answer different questions. */
static void wifi_row_wording(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;

	memset(&u, 0, sizeof u);
	u.text_size = "100%";
	printf("the Wi-Fi row's three off states:\n");

	u.wifi = WIFI_CONNECTING;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_WIFI]), "connecting"), "connecting");

	u.wifi = WIFI_IDLE;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_WIFI]), "not connected"), "up but unassociated");

	/* Connected with no name yet: better to say nothing useful than to print
	 * an empty value where a network name belongs. */
	u.wifi = WIFI_CONNECTED;
	u.ssid = "";
	sys_menu_build(&u, rows, &b, &heading);
	ck(val(&rows[PM_WIFI])[0] != 0, "connected with no SSID still says something");
}

/* The system menu. Games and Core carry real values, so a wrong count here is
 * a row lying about the machine it describes. */
static void system_menu(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;
	int n;

	memset(&u, 0, sizeof u);
	u.games = true;
	u.wifi = WIFI_CONNECTED;
	u.sys_name = "Game Boy";
	u.sys_core = "gambatte";
	u.game_count = 412;
	u.dmode = "Sharp";
	n = sys_menu_build(&u, rows, &b, &heading);

	printf("system menu:\n");
	ck(n == SM_ROWS, "every row is filled");
	ck(!strcmp(heading, "Game Boy"), "heading is the system, not TortOS");
	ck(!strcmp(val(&rows[SM_GAMES]), "412"), "the count is the real one");
	ck(!strcmp(val(&rows[SM_CORE]), "gambatte"), "the core is the real one");
	ck(!strcmp(val(&rows[SM_DISPLAY]), "Sharp"), "display mode is passed through");
	ck(rows[SM_DISPLAY].live && rows[SM_RESCAN].live, "the two live rows are live");
	ck(!rows[SM_GAMES].live && !rows[SM_CORE].live, "reported facts are not rows to press");
	ck(rows[SM_BOXART].live, "Box Art is live on a network");

	/* The same menu with the radio down. Only Box Art changes. */
	u.wifi = WIFI_OFF;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!rows[SM_BOXART].live, "Box Art dies with the radio");
	ck(!strcmp(val(&rows[SM_BOXART]), "needs Wi-Fi"), "and says why");
	ck(rows[SM_RESCAN].live, "Rescan does not need a network");
}

/* The labels, on their own. A row that reads "90s" for a minute and a half
 * would be wrong in a way no screenshot makes obvious. */
static void auto_off_words(void)
{
	char s[16];
	struct { int sec; const char *want; } t[] = {
		{ 0, "off" }, { 30, "30s" }, { 60, "1m" },
		{ 120, "2m" }, { 300, "5m" }, { 600, "10m" },
	};
	size_t i;

	printf("auto off labels:\n");
	for (i = 0; i < sizeof t / sizeof t[0]; i++) {
		sys_menu_auto_off_label(t[i].sec, s, sizeof s);
		ck(!strcmp(s, t[i].want), t[i].want);
	}
}

int main(void)
{
	off_state();
	scanning_state();
	scanned_state();
	strength_words();
	respects_max();
	tortos_menu_offline();
	tortos_menu_online();
	wifi_row_wording();
	system_menu();
	auto_off_words();
	if (fails) { printf("\n%d menu check(s) failed\n", fails); return 1; }
	printf("\nok: menus contain what they should\n");
	return 0;
}
