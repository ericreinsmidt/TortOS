/* Bluetooth: the parts that take input from the air.
 *
 * A device name is arbitrary bytes chosen by whoever owns the headset, it
 * arrives over the radio, and this process is root. Nothing here ever hands a
 * name to a command - but an ADDRESS is handed to bluetoothctl, so the
 * validator that decides what counts as one is the boundary, and it is checked
 * rather than trusted.
 *
 * The PCM naming is checked because it has a SECOND implementation, in
 * launch.sh, which runs at boot before this binary exists. The two must agree
 * exactly or a headset gets a PCM under one name and is looked for under
 * another.
 *
 * Links src/bt.c and NOT SDL.
 */
#include "../src/bt.h"
#include "../src/bt_menu.h"

#include <stdio.h>
#include <string.h>

static int fails;
static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

static void addresses(void)
{
	printf("what counts as an address:\n");
	ck(bt_mac_valid("AA:BB:CC:DD:EE:FF"), "upper case");
	ck(bt_mac_valid("a0:b1:c2:d3:e4:f5"), "lower case");
	ck(bt_mac_valid("00:00:00:00:00:00"), "all zeroes");

	ck(!bt_mac_valid(NULL), "nothing");
	ck(!bt_mac_valid(""), "empty");
	ck(!bt_mac_valid("AA:BB:CC:DD:EE"), "too short");
	ck(!bt_mac_valid("AA:BB:CC:DD:EE:FF:"), "too long");
	ck(!bt_mac_valid("AA:BB:CC:DD:EE:GG"), "not hex");
	ck(!bt_mac_valid("AA-BB-CC-DD-EE-FF"), "wrong separator");
	ck(!bt_mac_valid("AABBCCDDEEFF"), "no separators");

	/* The reason this function exists. An address is the one value that goes
	 * back out as an argument, and execv means these could never have been a
	 * shell injection - but a value that is not an address has no business
	 * reaching bluetoothctl at all, whatever the transport. */
	ck(!bt_mac_valid("AA:BB:CC:DD:EE:FF; rm -rf /"), "a command after one");
	ck(!bt_mac_valid("$(reboot)"), "a substitution");
	ck(!bt_mac_valid("../../etc/passwd"), "a path");
}

static void device_lines(void)
{
	bt_device d[8];
	int n;

	printf("reading what bluetoothctl prints:\n");
	n = bt_parse_devices(
		"Device AA:BB:CC:DD:EE:FF Shokz OpenRun Pro\n"
		"Device 11:22:33:44:55:66 JBL Go\n", d, 8);
	ck(n == 2, "two devices");
	ck(!strcmp(d[0].mac, "AA:BB:CC:DD:EE:FF"), "the first address");
	ck(!strcmp(d[0].name, "Shokz OpenRun Pro"), "a name with spaces survives whole");
	ck(!strcmp(d[1].name, "JBL Go"), "and the second");

	printf("and what it does not:\n");
	n = bt_parse_devices("", d, 8);
	ck(n == 0, "nothing at all");
	n = bt_parse_devices("Agent registered\n[bluetooth]# \n", d, 8);
	ck(n == 0, "chatter that is not a device line");
	n = bt_parse_devices("Device NOTAMAC Something\n", d, 8);
	ck(n == 0, "a line whose address is not one");
	n = bt_parse_devices("Device AA:BB:CC:DD:EE:FF\n", d, 8);
	ck(n == 1 && !strcmp(d[0].name, "AA:BB:CC:DD:EE:FF"),
	   "a device with no name falls back to its address");

	/* A name is never handed to a command, so the only requirement is that a
	 * hostile one is carried as data and does not break the parse. */
	n = bt_parse_devices("Device AA:BB:CC:DD:EE:FF ; rm -rf / #\n", d, 8);
	ck(n == 1 && !strcmp(d[0].name, "; rm -rf / #"),
	   "a name that looks like a command is just a name");

	printf("the bound is respected:\n");
	n = bt_parse_devices(
		"Device 00:00:00:00:00:01 a\nDevice 00:00:00:00:00:02 b\n"
		"Device 00:00:00:00:00:03 c\n", d, 2);
	ck(n == 2, "it stops at max rather than writing past it");
}

static void labels(void)
{
	bt_device d = { "AA:BB:CC:DD:EE:FF", "Shokz", false, false };
	char v[32];

	printf("what the right-hand column says:\n");
	bt_label(&d, v, sizeof v);
	ck(!strcmp(v, "in range"), "seen but not paired");
	d.bonded = true;
	bt_label(&d, v, sizeof v);
	ck(!strcmp(v, "paired"), "paired but not connected");

	/* Connected has to outrank paired. A successful connect that redraws the
	 * list saying what it already said gives no sign anything happened, which
	 * is a mistake wifi_label made and this one is not repeating. */
	d.connected = true;
	bt_label(&d, v, sizeof v);
	ck(!strcmp(v, "connected"), "connected outranks paired");

	bt_label(NULL, v, sizeof v);
	ck(v[0] == '\0', "nothing at all says nothing");
}

static void pcm_names(void)
{
	char pcm[32];

	printf("the ALSA name, which launch.sh also computes:\n");
	bt_pcm_name("AA:BB:CC:DD:EE:FF", pcm, sizeof pcm);
	/* launch.sh: bt_pcm_name() { echo "bt_$(echo "$1" | tr ':' '_')"; } */
	ck(!strcmp(pcm, "bt_AA_BB_CC_DD_EE_FF"), "colons become underscores");
	bt_pcm_name("a0:b1:c2:d3:e4:f5", pcm, sizeof pcm);
	ck(!strcmp(pcm, "bt_a0_b1_c2_d3_e4_f5"), "case is left alone, as tr leaves it");
}

/* The rows themselves. This is why bt_menu.c exists as its own SDL-free file:
 * the first version of this screen built its rows inside main.c, where nothing
 * could reach them. ADR-0001 says a build function has to be callable with no
 * renderer and no device. */
static void rows_are_shaped_like_the_others(void)
{
	bt_ui u = { 0 };
	menu_row r[16];
	int n;

	printf("the row layout, which follows the Wi-Fi screen:\n");
	u.state = BT_READY;
	u.n = 2;
	snprintf(u.dev[0].name, BT_NAME_MAX, "Shokz");
	snprintf(u.dev[1].name, BT_NAME_MAX, "JBL");
	u.dev[0].bonded = true;
	u.cursor = 0;

	n = bt_menu_build(&u, r, 16, 7);
	ck(n == 5, "toggle, two devices, a rule and a note");
	ck(!strcmp(r[0].label, "Bluetooth") && !strcmp(r[0].value, "on"),
	   "the toggle leads, the way Wi-Fi's does");
	ck(!strcmp(r[1].label, "Shokz"), "then the devices");

	/* A footer says something about the list rather than offering anything,
	 * so it sits under a rule - the same reason the key legend does. */
	ck(r[n - 2].label == NULL && r[n - 2].value == NULL, "a rule before the footer");
	ck(ROW_IS_NOTE(r[n - 1]), "and the footer is a note, not a row");

	printf("every row that can be chosen is live:\n");
	/* `live` is selectable, not selected - passing the cursor here drew every
	 * other row quiet and highlighted none of them, on both new screens. */
	ck(r[1].live && r[2].live, "both devices, not just the one under the cursor");

	printf("the footer follows the cursor:\n");
	u.cursor = -1;
	ck(strstr(bt_menu_footer(&u), "turn Bluetooth off") != NULL,
	   "on the toggle it offers the toggle");
	u.cursor = 0;
	ck(!strcmp(bt_menu_footer(&u), "A: connect   Y: search   X: forget"),
	   "on a bonded device, connect and forget");
	u.cursor = 1;
	ck(!strcmp(bt_menu_footer(&u), "A: pair   Y: search"),
	   "on one merely in range, pair and nothing to forget");
	u.dev[1].connected = true;
	ck(strstr(bt_menu_footer(&u), "A: disconnect") != NULL,
	   "and connected offers disconnect");

	snprintf(u.note, sizeof u.note, "Forgotten");
	ck(!strcmp(bt_menu_footer(&u), "Forgotten"),
	   "a message about what just happened takes the same slot");
	u.note[0] = '\0';

	printf("the list is bounded:\n");
	u.n = BT_MAX;
	n = bt_menu_build(&u, r, 16, 7);
	ck(n <= 16, "it never writes past the array it was given");
	ck(ROW_IS_NOTE(r[n - 1]), "and the footer still lands");

	u.state = BT_POWERED_OFF;
	u.n = 0;
	n = bt_menu_build(&u, r, 16, 7);
	ck(!strcmp(r[0].value, "off"), "the toggle says off");
	ck(strstr(bt_menu_footer(&u), "turn Bluetooth on") != NULL,
	   "and the footer says how to fix it");
}

int main(void)
{
	addresses();
	device_lines();
	labels();
	pcm_names();
	rows_are_shaped_like_the_others();
	if (fails) { printf("\n%d FAILED\n", fails); return 1; }
	printf("\nok: addresses are validated and names are only ever data\n");
	return 0;
}
