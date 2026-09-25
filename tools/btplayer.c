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
 * launch.sh's bt_on starts it after bluetoothd and bt_off stops it, because a
 * bluetoothd that restarts has forgotten it.
 *
 * libdbus is the device's own, reached with dlopen: the sysroot carries no
 * headers for it, so the handful of types used are declared here. Every call
 * the headset makes on the player - play, pause, from its buttons - is answered
 * UnknownMethod by libdbus, which BlueZ passes on as a failure and nothing more.
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

typedef struct { const char *name, *message; unsigned bits; void *pad; } dbus_error;
typedef struct { void *pad[16]; } dbus_iter;      /* at least DBusMessageIter's size */
typedef struct {
	void (*unregister_fn)(void *, void *);
	int  (*message_fn)(void *, void *, void *);
	void (*pad[4])(void *);
} dbus_vtable;

#define BUS_SYSTEM      1
#define TYPE_OBJECT     ((int)'o')
#define TYPE_ARRAY      ((int)'a')
#define NOT_HANDLED     1
#define PLAYER_PATH     "/org/tortos/player"

static void *(*bus_get)(int, dbus_error *);
static void  (*error_init)(dbus_error *);
static void  (*error_free)(dbus_error *);
static void *(*new_call)(const char *, const char *, const char *, const char *);
static void  (*message_unref)(void *);
static void  (*iter_init_append)(void *, dbus_iter *);
static int   (*append_basic)(dbus_iter *, int, const void *);
static int   (*open_container)(dbus_iter *, int, const char *, dbus_iter *);
static int   (*close_container)(dbus_iter *, dbus_iter *);
static void *(*send_block)(void *, void *, int, dbus_error *);
static int   (*register_path)(void *, const char *, const dbus_vtable *, void *);
static int   (*dispatch)(void *, int);

static int on_message(void *conn, void *msg, void *data)
{
	(void)conn; (void)msg; (void)data;
	return NOT_HANDLED;
}

static bool bind_libdbus(void)
{
	void *lib = dlopen("libdbus-1.so.3", RTLD_NOW);

	if (!lib) return false;
#define SYM(v, n) if (!(*(void **)&v = dlsym(lib, n))) return false
	SYM(bus_get,          "dbus_bus_get");
	SYM(error_init,       "dbus_error_init");
	SYM(error_free,       "dbus_error_free");
	SYM(new_call,         "dbus_message_new_method_call");
	SYM(message_unref,    "dbus_message_unref");
	SYM(iter_init_append, "dbus_message_iter_init_append");
	SYM(append_basic,     "dbus_message_iter_append_basic");
	SYM(open_container,   "dbus_message_iter_open_container");
	SYM(close_container,  "dbus_message_iter_close_container");
	SYM(send_block,       "dbus_connection_send_with_reply_and_block");
	SYM(register_path,    "dbus_connection_register_object_path");
	SYM(dispatch,         "dbus_connection_read_write_dispatch");
#undef SYM
	return true;
}

/* RegisterPlayer(o path, a{sv} {}). An empty dict is valid: BlueZ reads the
 * properties it is given and needs none. */
static bool register_player(void *conn)
{
	const char *path = PLAYER_PATH;
	void *msg, *reply;
	dbus_iter args, dict;
	dbus_error err;

	error_init(&err);
	msg = new_call("org.bluez", "/org/bluez/hci0", "org.bluez.Media1", "RegisterPlayer");
	if (!msg) return false;
	iter_init_append(msg, &args);
	append_basic(&args, TYPE_OBJECT, &path);
	open_container(&args, TYPE_ARRAY, "{sv}", &dict);
	close_container(&args, &dict);
	reply = send_block(conn, msg, 5000, &err);
	message_unref(msg);
	if (!reply) {
		fprintf(stderr, "btplayer: RegisterPlayer: %s\n",
		        err.message ? err.message : "no reply");
		error_free(&err);
		return false;
	}
	message_unref(reply);
	return true;
}

int main(void)
{
	static const dbus_vtable vt = { NULL, on_message, { NULL } };
	void *conn;
	dbus_error err;
	int tries;

	if (!bind_libdbus()) {
		fprintf(stderr, "btplayer: libdbus-1.so.3 is missing or incomplete\n");
		return 1;
	}
	error_init(&err);
	conn = bus_get(BUS_SYSTEM, &err);
	if (!conn) {
		fprintf(stderr, "btplayer: system bus: %s\n", err.message ? err.message : "?");
		return 1;
	}
	if (!register_path(conn, PLAYER_PATH, &vt, NULL)) {
		fprintf(stderr, "btplayer: cannot export %s\n", PLAYER_PATH);
		return 1;
	}
	/* bluetoothd's media interface can come up a moment after bluetoothd
	 * itself, so a first refusal is not the last word. */
	for (tries = 0; tries < 10 && !register_player(conn); tries++)
		sleep(1);
	if (tries == 10) return 1;
	fprintf(stderr, "btplayer: registered %s\n", PLAYER_PATH);

	while (dispatch(conn, -1)) { }
	return 0;
}
