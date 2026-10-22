/* SPDX-License-Identifier: MIT */
/* btplayer: a media player registered with BlueZ that plays nothing, so that
 * a headset's volume reaches the Brick.
 *
 * BlueZ 5.54 negotiates AVRCP absolute volume with a headset and then drops
 * what the headset reports: avrcp_volume_changed() hands the value to the
 * target's player, and returns early when there is none. On a phone the music
 * app is that player. On the Brick nothing registered one, bluealsa included,
 * so the A2DP transport's Volume stayed at its initial -1, the property stayed
 * hidden, and bluealsa's --a2dp-volume failed with "No such property 'Volume'".
 * A headset with no buttons of its own had no volume control anywhere.
 *
 * Measured 2026-09-25 with the OpenFit: registering a player with an empty
 * property dict is the whole fix. The transport gains Volume, setting bluealsa's
 * `<name> - A2DP` control sends SetAbsoluteVolume, and the change is heard.
 *
 * It has to stay alive: BlueZ unregisters a player whose owner leaves the bus.
 * plastron's /etc/init.d/bluetooth starts it after bluetoothd and bluealsa and
 * stops it first, because a bluetoothd that restarts has forgotten it.
 *
 * Every call the headset makes on the player - play, pause, from its buttons -
 * is answered UnknownMethod by libdbus, which BlueZ passes on as a failure and
 * nothing more.
 *
 * It also mends a volume BlueZ lost. BlueZ 5.54 hands a headset's report only
 * to the A2DP transports that exist at that moment, and keeps the value in the
 * player so that a report of the same level again is dropped too
 * (media.c set_volume). A transport created AFTER the report - a reconnect
 * from the Bluetooth screen, 2026-09-26 - has no Volume, the keys stop
 * reaching the headset, and nothing says so. Re-reporting alone did not bring
 * it back, and a fresh player alone did not; the two together did, measured
 * the same day: Volume 70, the headset's own level. So each new transport is
 * looked at two seconds after it appears, and one without a Volume gets
 * exactly that - once, so a headset that cannot do absolute volume is tried
 * once and not forever.
 *
 * And it passes a headset's OWN volume changes on. BlueZ announces the
 * transport's Volume (PropertiesChanged) whenever the headset reports one:
 * the OpenRun announced every press of its buttons, measured 2026-09-26 -
 * and every value the Brick sent it as well, which the launcher tells apart
 * by time (bt_volume_follow). Written to /tmp/tortos_btvol as
 * `<PCM name> <0..127> <stamp>`, the stamp being this clock's milliseconds,
 * which only go up - so a new report never reads as the last one, as a
 * reused inode number could make it. Nothing for four seconds after a
 * transport appears or is mended: that is the headset reporting where it
 * already was, and at connect the Brick's level is the one that wins.
 *
 * And a headset's play, pause, next and previous. Not as calls on this player
 * - none came, from either headset - but as keys: BlueZ gives every AVRCP
 * headset a virtual keyboard named `<headset> (AVRCP)` and presses its keys.
 * The OpenRun sent KEY_PLAYCD for every press, never pause; the OpenFit sent
 * KEY_PAUSECD and KEY_PLAYCD as it saw fit, and KEY_NEXTSONG for a hold -
 * measured 2026-09-26. Gestures are the headset's own business and cannot be
 * seen from here; only what they mean arrives. Any `(AVRCP)` keyboard is
 * watched, and a press is written to /tmp/tortos_btkey as `<action> <stamp>`
 * for the launcher to hand to Muse: toggle, pause, next or prev.
 */
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PLAYER_PATH     "/org/tortos/player"

static DBusHandlerResult on_message(DBusConnection *conn, DBusMessage *msg, void *data)
{
	(void)conn; (void)msg; (void)data;
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* RegisterPlayer(o path, a{sv} {}). An empty dict is valid: BlueZ reads the
 * properties it is given and needs none. */
static bool register_player(DBusConnection *conn)
{
	const char *path = PLAYER_PATH;
	DBusMessage *msg, *reply;
	DBusMessageIter args, dict;
	DBusError err;

	dbus_error_init(&err);
	msg = dbus_message_new_method_call("org.bluez", "/org/bluez/hci0", "org.bluez.Media1", "RegisterPlayer");
	if (!msg) return false;
	dbus_message_iter_init_append(msg, &args);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
	dbus_message_iter_close_container(&args, &dict);
	reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
	dbus_message_unref(msg);
	if (!reply) {
		fprintf(stderr, "btplayer: RegisterPlayer: %s\n",
		        err.message ? err.message : "no reply");
		dbus_error_free(&err);
		return false;
	}
	dbus_message_unref(reply);
	return true;
}

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* A method call with up to two string-ish arguments, waiting for the reply.
 * The reply for the caller to read and unref, or NULL on any error. */
static DBusMessage *call(DBusConnection *conn, const char *path, const char *iface, const char *method,
                  int t1, const char *a1, int t2, const char *a2)
{
	DBusMessage *msg = dbus_message_new_method_call("org.bluez", path, iface, method), *reply;
	DBusMessageIter args;
	DBusError err;

	if (!msg) return NULL;
	dbus_message_iter_init_append(msg, &args);
	if (a1) dbus_message_iter_append_basic(&args, t1, &a1);
	if (a2) dbus_message_iter_append_basic(&args, t2, &a2);
	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
	dbus_message_unref(msg);
	if (!reply) dbus_error_free(&err);
	return reply;
}

static bool has_volume(DBusConnection *conn, const char *transport)
{
	DBusMessage *r = call(conn, transport, "org.freedesktop.DBus.Properties", "Get",
	               DBUS_TYPE_STRING, "org.bluez.MediaTransport1", DBUS_TYPE_STRING, "Volume");

	if (!r) return false;
	dbus_message_unref(r);
	return true;
}

/* The transport's device, e.g. /org/bluez/hci0/dev_C0_86_B3_A7_48_7F. */
static bool device_of(DBusConnection *conn, const char *transport, char *out, size_t n)
{
	DBusMessage *r = call(conn, transport, "org.freedesktop.DBus.Properties", "Get",
	               DBUS_TYPE_STRING, "org.bluez.MediaTransport1", DBUS_TYPE_STRING, "Device");
	DBusMessageIter it, var;
	const char *dev = NULL;

	if (!r) return false;
	if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&it, &var);
		if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_OBJECT_PATH) dbus_message_iter_get_basic(&var, &dev);
	}
	if (dev) snprintf(out, n, "%s", dev);
	dbus_message_unref(r);
	return dev != NULL;
}

/* A fresh player, so BlueZ has no stored level to call the report a repeat
 * of, then AVRCP again on the device, so the headset reports once more - now
 * that the transport exists to take it. AVRCP is only the control channel:
 * the audio does not stop, and the transport is not recreated by it. */
static void mend(DBusConnection *conn, const char *transport)
{
	static const char *avrcp = "0000110e-0000-1000-8000-00805f9b34fb";
	char dev[160];
	DBusMessage *r;

	if (!device_of(conn, transport, dev, sizeof dev)) return;
	if ((r = call(conn, "/org/bluez/hci0", "org.bluez.Media1", "UnregisterPlayer",
	              DBUS_TYPE_OBJECT_PATH, PLAYER_PATH, 0, NULL)))
		dbus_message_unref(r);
	if (!register_player(conn)) return;
	if ((r = call(conn, dev, "org.bluez.Device1", "DisconnectProfile",
	              DBUS_TYPE_STRING, avrcp, 0, NULL)))
		dbus_message_unref(r);
	sleep(1);
	if ((r = call(conn, dev, "org.bluez.Device1", "ConnectProfile",
	              DBUS_TYPE_STRING, avrcp, 0, NULL)))
		dbus_message_unref(r);
}

/* Transports to look at, and when. Stage 0 is the first look, two seconds
 * after it appears - a headset whose AVRCP comes up after the transport
 * reports into it by itself; stage 1 is after a mend, and the last. */
#define WATCHED 4
static struct { char path[160]; long due; int stage; } g_watch[WATCHED];

static long g_quiet_until;   /* no headset volumes passed on before this */

/* /org/bluez/hci0/dev_A8_F5_E1_4A_93_71/sep1/fd8 -> bt_A8_F5_E1_4A_93_71, the
 * name launch.sh publishes (bt_pcm_name in bt-alsa.sh). */
static void publish_volume(const char *transport, unsigned volume)
{
	const char *dev = strstr(transport, "/dev_");
	FILE *f;

	if (!dev || strlen(dev) < 5 + 17) return;
	if (!(f = fopen("/tmp/tortos_btvol.tmp", "w"))) return;
	fprintf(f, "bt_%.17s %u %ld\n", dev + 5, volume, now_ms());
	fclose(f);
	rename("/tmp/tortos_btvol.tmp", "/tmp/tortos_btvol");
}

/* PropertiesChanged(s interface, a{sv} changed, as invalidated), on a
 * transport: its Volume, if that is what changed. */
static void on_properties(DBusMessage *msg)
{
	DBusMessageIter args, dict, entry, var;
	const char *iface, *key, *path = dbus_message_get_path(msg);

	if (!path || !dbus_message_iter_init(msg, &args) || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING) return;
	dbus_message_iter_get_basic(&args, &iface);
	if (strcmp(iface, "org.bluez.MediaTransport1")) return;
	if (!dbus_message_iter_next(&args) || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) return;
	for (dbus_message_iter_recurse(&args, &dict); dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&dict)) {
		unsigned short volume;

		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		if (strcmp(key, "Volume") || !dbus_message_iter_next(&entry)) continue;
		dbus_message_iter_recurse(&entry, &var);
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_UINT16) continue;
		dbus_message_iter_get_basic(&var, &volume);
		if (now_ms() < g_quiet_until) continue;
		publish_volume(path, volume);
	}
}

static DBusHandlerResult on_signal(DBusConnection *conn, DBusMessage *msg, void *data)
{
	DBusMessageIter args, dict, entry;
	const char *path = NULL, *iface;
	int i;

	(void)conn; (void)data;
	if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
		on_properties(msg);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	if (!dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (!dbus_message_iter_init(msg, &args) || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_OBJECT_PATH) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	dbus_message_iter_get_basic(&args, &path);
	if (!dbus_message_iter_next(&args) || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	for (dbus_message_iter_recurse(&args, &dict); dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&dict)) {
		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &iface);
		if (strcmp(iface, "org.bluez.MediaTransport1")) continue;
		for (i = 0; i < WATCHED && g_watch[i].path[0]; i++) { }
		if (i == WATCHED) break;
		snprintf(g_watch[i].path, sizeof g_watch[i].path, "%s", path);
		g_watch[i].due = now_ms() + 2000;
		g_watch[i].stage = 0;
		g_quiet_until = now_ms() + 4000;
		break;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void look(DBusConnection *conn)
{
	int i;

	for (i = 0; i < WATCHED; i++) {
		char *p = g_watch[i].path;

		if (!p[0] || now_ms() < g_watch[i].due) continue;
		if (has_volume(conn, p)) {
			if (g_watch[i].stage) fprintf(stderr, "btplayer: %s has its volume back\n", p);
			p[0] = '\0';
		} else if (g_watch[i].stage == 0) {
			fprintf(stderr, "btplayer: %s came up without a volume; asking again\n", p);
			mend(conn, p);
			g_quiet_until = now_ms() + 4000;
			g_watch[i].stage = 1;
			g_watch[i].due = now_ms() + 3000;
		} else {
			fprintf(stderr, "btplayer: %s still has no volume; leaving it\n", p);
			p[0] = '\0';
		}
	}
}

/* The headsets' AVRCP keyboards. BlueZ makes one per connection and they come
 * and go with it, so the list is read again every two seconds; a node that
 * has gone reads as ENODEV and is closed. */
#define KEYBOARDS 4
static struct { int fd; char node[24]; } g_kbd[KEYBOARDS] = {
	{ -1, "" }, { -1, "" }, { -1, "" }, { -1, "" }
};

static void find_keyboards(void)
{
	FILE *f = fopen("/proc/bus/input/devices", "r");
	char line[256];
	bool avrcp = false;

	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char *ev, node[24];
		int i, free_slot = -1, have = 0;

		if (!strncmp(line, "N: Name=", 8)) {
			avrcp = strstr(line, " (AVRCP)\"") != NULL;
			continue;
		}
		if (!avrcp || strncmp(line, "H: Handlers=", 12) || !(ev = strstr(line, "event")))
			continue;
		avrcp = false;
		snprintf(node, sizeof node, "/dev/input/%.*s", (int)strspn(ev, "event0123456789"), ev);
		for (i = 0; i < KEYBOARDS; i++) {
			if (g_kbd[i].fd >= 0 && !strcmp(g_kbd[i].node, node)) have = 1;
			if (g_kbd[i].fd < 0 && free_slot < 0) free_slot = i;
		}
		if (have || free_slot < 0) continue;
		if ((g_kbd[free_slot].fd = open(node, O_RDONLY | O_NONBLOCK)) >= 0) {
			snprintf(g_kbd[free_slot].node, sizeof g_kbd[free_slot].node, "%s", node);
			fprintf(stderr, "btplayer: watching %s for a headset's keys\n", node);
		}
	}
	fclose(f);
}

/* Which of Muse's actions a key is. PLAYCD toggles, because the OpenRun sends
 * nothing else for play and for pause; PAUSECD and STOPCD only pause, so the
 * OpenFit, which says which it means, never starts music it meant to stop.
 * FASTFORWARD and REWIND are next and previous, for a headset that sends a
 * hold that way. */
static const char *key_action(unsigned code)
{
	switch (code) {
	case KEY_PLAYCD: case KEY_PLAYPAUSE: case KEY_PLAY: return "toggle";
	case KEY_PAUSECD: case KEY_STOPCD: case KEY_PAUSE:  return "pause";
	case KEY_NEXTSONG: case KEY_FASTFORWARD:           return "next";
	case KEY_PREVIOUSSONG: case KEY_REWIND:            return "prev";
	}
	return NULL;
}

static void read_keyboards(void)
{
	int i;

	for (i = 0; i < KEYBOARDS; i++) {
		struct input_event ev;
		ssize_t n;

		if (g_kbd[i].fd < 0) continue;
		while ((n = read(g_kbd[i].fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
			const char *act = ev.type == EV_KEY && ev.value == 1 ? key_action(ev.code) : NULL;
			FILE *f;

			if (!act || !(f = fopen("/tmp/tortos_btkey.tmp", "w"))) continue;
			fprintf(f, "%s %ld\n", act, now_ms());
			fclose(f);
			rename("/tmp/tortos_btkey.tmp", "/tmp/tortos_btkey");
		}
		if (n < 0 && errno != EAGAIN) {          /* the headset went */
			close(g_kbd[i].fd);
			g_kbd[i].fd = -1;
			g_kbd[i].node[0] = '\0';
		}
	}
}

int main(void)
{
	static const DBusObjectPathVTable vt = { .message_function = on_message };
	DBusConnection *conn;
	DBusError err;
	int tries;

	dbus_error_init(&err);
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (!conn) {
		fprintf(stderr, "btplayer: system bus: %s\n", err.message ? err.message : "?");
		return 1;
	}
	if (!dbus_connection_register_object_path(conn, PLAYER_PATH, &vt, NULL)) {
		fprintf(stderr, "btplayer: cannot export %s\n", PLAYER_PATH);
		return 1;
	}
	/* bluetoothd's media interface can come up a moment after bluetoothd
	 * itself, so a first refusal is not the last word. */
	for (tries = 0; tries < 10 && !register_player(conn); tries++)
		sleep(1);
	if (tries == 10) return 1;
	fprintf(stderr, "btplayer: registered %s\n", PLAYER_PATH);

	dbus_bus_add_match(conn, "type='signal',sender='org.bluez',"
	          "interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'", &err);
	dbus_connection_add_filter(conn, on_signal, NULL, NULL);
	dbus_bus_add_match(conn, "type='signal',sender='org.bluez',"
	          "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
	          "arg0='org.bluez.MediaTransport1'", &err);
	/* A tenth of a second, so a headset's key is seen as soon as it is
	 * pressed; the rest of the time this sleeps in the dispatch. */
	{
		long next_find = 0;

		while (dbus_connection_read_write_dispatch(conn, 100)) {
			look(conn);
			if (now_ms() >= next_find) {
				find_keyboards();
				next_find = now_ms() + 2000;
			}
			read_keyboards();
		}
	}
	return 0;
}
