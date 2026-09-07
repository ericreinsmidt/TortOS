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
#include "../src/game_menu.h"

#include <stdio.h>
#include <stdbool.h>
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
	/* Live since 2026-09-06, when the pairing screen landed. The row used to
	 * be dead and read "not yet", which was true of the screen and false of
	 * the feature - game audio had been going to a headset for three days. */
	ck(rows[PM_BT].live, "Bluetooth is reachable now that pairing exists");
	/* Always reachable, even with no radio and no cable: it is the row you go
	 * to in order to say "not Bluetooth", so it must not vanish with the thing
	 * it refuses. */
	ck(rows[PM_AUDIO].live, "Audio Output is reachable offline");
	ck(!strcmp(val(&rows[PM_AUDIO]), "auto (speaker)"),
	   "and Auto names where it landed rather than only saying Auto");
}

/* The row has to say a PLACE. "Auto" alone makes the player guess which of
 * three they are about to hear. */
static void audio_row(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;

	memset(&u, 0, sizeof u);
	u.text_size = "100%";
	printf("the Audio Output row:\n");

	u.audio_policy = AOUT_AUTO;
	u.audio_dest = AOUT_WIRED;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_AUDIO]), "auto (wired)"), "auto, on a cable");

	u.audio_dest = AOUT_BT;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_AUDIO]), "auto (bluetooth)"), "auto, on a headset");

	/* Pinned says the place with no "auto", because there is no rule left to
	 * describe - it is just where the sound is. */
	u.audio_policy = AOUT_SPEAKER;
	u.audio_dest = AOUT_SPK;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_AUDIO]), "speaker"), "pinned reads as the place");

	/* Pinned to Speaker with a cable in is still the cable: the pin refuses
	 * Bluetooth, never the jack. */
	u.audio_dest = AOUT_WIRED;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_AUDIO]), "wired"),
	   "and a cable still shows through the Speaker pin");
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


/* ---------- where the cursor can go --------------------------------------- */

/* Walk the whole menu and collect every row the cursor can actually rest on.
 * A row that is drawn but cannot be selected is fine; a row that CAN be
 * selected and then does nothing when pressed is the defect. */
static int reachable(const menu_row *rows, int n, int *out, int max)
{
	int sel = 0, k = 0, i;

	if (!rows[0].live) sel = menu_step_sel(rows, n, 0, +1);
	if (!rows[sel].live) return 0;      /* nothing live at all */
	for (i = 0; i < n && k < max; i++) {
		out[k++] = sel;
		sel = menu_step_sel(rows, n, sel, +1);
		if (sel == out[0]) break;       /* wrapped */
	}
	return k;
}

static bool holds(const int *v, int n, int want)
{
	int i;
	for (i = 0; i < n; i++) if (v[i] == want) return true;
	return false;
}

/* The migration of the system menu onto the runner changed this on purpose:
 * the cursor used to walk every row, including the four that only report a
 * fact. Now it visits what can be pressed. */
static void cursor_reaches(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;
	int got[MENU_MAX_ROWS], n, k;

	memset(&u, 0, sizeof u);
	u.games = true;
	u.wifi = WIFI_CONNECTED;
	u.sys_name = "NES"; u.sys_core = "nestopia";
	u.game_count = 3; u.dmode = "Native";
	n = sys_menu_build(&u, rows, &b, &heading);
	k = reachable(rows, n, got, MENU_MAX_ROWS);

	printf("system menu, what the cursor can reach:\n");
	ck(k == 3, "three rows, not seven");
	ck(holds(got, k, SM_DISPLAY), "Display Mode");
	ck(holds(got, k, SM_BOXART), "Box Art");
	ck(holds(got, k, SM_RESCAN), "Rescan Folder");
	ck(!holds(got, k, SM_GAMES) && !holds(got, k, SM_CORE),
	   "the reported facts are not stops");

	memset(&u, 0, sizeof u);
	u.wifi = WIFI_OFF;
	u.text_size = "100%";
	n = sys_menu_build(&u, rows, &b, &heading);
	k = reachable(rows, n, got, MENU_MAX_ROWS);

	printf("TortOS menu offline, what the cursor can reach:\n");
	/* No network needed to pair a headset, so it stays reachable offline -
	 * unlike the three rows above, which do need one. */
	ck(holds(got, k, PM_BT), "Bluetooth is reachable with no network");
	ck(!holds(got, k, PM_XFER), "OTH is skipped with no network");
	ck(!holds(got, k, PM_SCRAPE), "Box Art is skipped with no network");
	ck(holds(got, k, PM_WIFI), "Wi-Fi is reachable, which is how you fix that");
	ck(holds(got, k, PM_ABOUT), "About is reachable");
	/* Play Time reads what is already stored and asks nothing of the network,
	 * so it stays reachable when everything else is greyed out. */
	ck(holds(got, k, PM_STATS), "Play Time is reachable offline");
}

/* The bound is the point. An all-dead menu must terminate, not spin. */
static void step_terminates(void)
{
	menu_row dead[3];
	int i;

	for (i = 0; i < 3; i++) dead[i] = (menu_row){ "x", NULL, false };
	printf("a menu with nothing live:\n");
	ck(menu_step_sel(dead, 3, 1, +1) == 1, "forward stays put");
	ck(menu_step_sel(dead, 3, 1, -1) == 1, "backward stays put");
	ck(menu_step_sel(dead, 0, 0, +1) == 0, "an empty menu is survivable");
}


/* ---------- the game info screen ------------------------------------------ */

/* Five facts and two actions. The facts are not stops for the cursor; the two
 * actions are, except that fetching art needs a network and says so. */
static void info_rows(void)
{
	game_info gi;
	menu_row rows[GI_MAX];
	int got[GI_MAX], n, k;

	memset(&gi, 0, sizeof gi);
	snprintf(gi.file, sizeof gi.file, "Chrono Trigger.sfc");
	snprintf(gi.size, sizeof gi.size, "4.0 MB");
	snprintf(gi.saves, sizeof gi.saves, "2");
	snprintf(gi.cheevos, sizeof gi.cheevos, "12 of 78");
	snprintf(gi.art, sizeof gi.art, "yes");
	gi.has_art = true;
	gi.favorite = false;

	n = gi_rows(rows, &gi, true);
	printf("game info, art present and a network:\n");
	ck(n == GI_MAX, "five facts and two actions");
	ck(!strcmp(rows[0].label, "File"), "File leads");
	ck(!strcmp(val(&rows[0]), "Chrono Trigger.sfc"), "and names the file");
	ck(!strcmp(rows[GI_MAX - 2].label, "Replace Box Art"),
	   "art present offers a replace");
	ck(!strcmp(val(&rows[GI_MAX - 1]), "no"), "Favorite reads its state");
	k = reachable(rows, n, got, GI_MAX);
	ck(k == 2, "only the two actions are stops");
	ck(!holds(got, k, 0) && !holds(got, k, 4), "no fact is a stop");

	gi.has_art = false;
	gi.favorite = true;
	n = gi_rows(rows, &gi, false);
	printf("game info, no art and no network:\n");
	ck(!strcmp(rows[GI_MAX - 2].label, "Get Box Art"),
	   "no art offers a get, not a replace");
	ck(!strcmp(val(&rows[GI_MAX - 2]), "needs Wi-Fi"), "and says why it is dead");
	ck(!rows[GI_MAX - 2].live, "which it is");
	ck(!strcmp(val(&rows[GI_MAX - 1]), "yes"), "Favorite follows the flag");
	k = reachable(rows, n, got, GI_MAX);
	ck(k == 1, "Favorite is the only stop left");
	ck(holds(got, k, GI_MAX - 1), "and it is Favorite");
}


/* ---------- the in-game menu ---------------------------------------------- */

/* Cheevos is the only row that can be dead, and it is dead exactly when the
 * game has no set. Everything else is always something A does. */
static void ingame_rows(void)
{
	gm_ui u;
	gm_bufs b;
	menu_row rows[GM_ROWS];
	int got[GM_ROWS], n, k;

	u.dmode = "Native"; u.earned = 12; u.total = 40;
	n = gm_rows(&u, rows, &b);

	printf("in-game menu, a game with a set:\n");
	ck(n == GM_ROWS, "seven rows");
	ck(!strcmp(rows[GM_CONTINUE].label, "Continue"), "Continue leads");
	ck(!strcmp(val(&rows[GM_DISPLAY]), "Native"), "Display carries the mode");
	ck(!strcmp(val(&rows[GM_CHEEVOS]), "12 / 40"), "Cheevos counts the set");
	ck(rows[GM_CHEEVOS].live, "and is reachable");
	k = reachable(rows, n, got, GM_ROWS);
	ck(k == GM_ROWS, "every row is a stop");

	u.earned = 0; u.total = 0;
	n = gm_rows(&u, rows, &b);
	printf("in-game menu, a game with no set:\n");
	ck(!strcmp(val(&rows[GM_CHEEVOS]), "none"), "Cheevos says none");
	ck(!rows[GM_CHEEVOS].live, "and does nothing");
	k = reachable(rows, n, got, GM_ROWS);
	ck(k == GM_ROWS - 1, "so the cursor steps over it");
	ck(!holds(got, k, GM_CHEEVOS), "and never rests on it");
	ck(holds(got, k, GM_QUIT) && holds(got, k, GM_CONTINUE),
	   "the rows either side of it still work");
}

/* The save slots, and the one constant that survived a rename by being written
 * out as a number.
 *
 * The carousel's Auto entry mapped to a hardcoded 9 - MinUI's number for the
 * resume slot. When it was renamed to "auto" on 2026-08-27 every use of the
 * CONSTANT was updated and the literal was missed, so Load -> Auto asked for a
 * `.9.state` that could not exist. Ten days later that turned into a wedged
 * device, because the rejection it caused was misread as a dead emulator.
 *
 * Cheap to assert, and it is the assertion that was missing. */
static void slots(void)
{
	int i;

	printf("save slots:\n");
	ck(gm_slot_at(0) == SLOT_AUTO, "the carousel's first entry is the resume slot");
	ck(gm_slot_at(0) != 9, "and is not MinUI's number for it");
	for (i = 1; i <= GM_SLOTS; i++)
		ck(gm_slot_at(i) == i, "a numbered slot maps to itself");

	/* The resume slot must not collide with one the player can pick, or a
	 * save to the last slot would overwrite the state the exit funnel owns. */
	ck(SLOT_AUTO > GM_SLOTS, "the resume slot is outside the numbered range");
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
	cursor_reaches();
	slots();
	step_terminates();
	info_rows();
	ingame_rows();
	audio_row();
	if (fails) { printf("\n%d menu check(s) failed\n", fails); return 1; }
	printf("\nok: menus contain what they should\n");
	return 0;
}
