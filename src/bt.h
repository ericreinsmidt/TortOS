/* SPDX-License-Identifier: MIT */
/* Bluetooth audio, over the stock BlueZ. Like wifi.h, none of this is TortOS's
 * own stack - it is a client of firmware that already works, and the four
 * things that were not obvious about getting it working are in the backlog.
 *
 * Three of them matter to this file:
 *
 *   THE AGENT IS THE WHOLE BALLGAME. A headset will not pair without one, and
 *   bluetoothctl only registers a default agent in interactive mode. Every
 *   pair here passes --agent NoInputNoOutput explicitly.
 *
 *   JUDGE BY `info`, NEVER BY THE RETURN OF `connect`. bluetoothctl reports
 *   Failed for a2dp even when the link came up. Cost an evening.
 *
 *   BONDS LIVE IN /etc/lib/bluetooth/<adapter>/<device>/, not the
 *   /etc/bluetooth/keys/ that the init wrapper symlinks into being. bluetoothd
 *   has its storage path compiled in with --localstatedir=/etc and never reads
 *   the decoy. Reading the bonds directly is also how the paired list is built
 *   without forking anything.
 *
 * FORKING IS THE COST HERE. This process is ~119 MB, every query is a fork,
 * and the menu loop runs every frame - so nothing in here is called per frame.
 * The screen refreshes on an interval, the same shape menu_wifi uses.
 */
#ifndef TORTOS_BT_H
#define TORTOS_BT_H

#include <stdbool.h>
#include <stddef.h>

#define BT_MAC_MAX   18      /* AA:BB:CC:DD:EE:FF plus NUL */
#define BT_NAME_MAX  64
#define BT_MAX       24

typedef struct {
	char mac[BT_MAC_MAX];
	char name[BT_NAME_MAX];
	bool bonded;         /* trusted, and will reconnect on its own at boot */
	bool connected;
} bt_device;

typedef enum { BT_NO_ADAPTER, BT_POWERED_OFF, BT_READY } bt_state;

/* --- pure, and checked on the host ---------------------------------------
 * An address is the one thing here that is ever handed back to bluetoothctl as
 * an argument, so it is validated rather than trusted. A device NAME is
 * arbitrary bytes chosen by whoever owns the headset, arriving over the air
 * into a process running as root - it is never passed to anything. */
bool bt_mac_valid(const char *mac);

/* "Device AA:BB:CC:DD:EE:FF Some Name", one per line, as `bluetoothctl
 * devices` prints it. Returns how many were understood; anything else on a
 * line is skipped rather than guessed at. */
int bt_parse_devices(const char *text, bt_device *out, int max);

/* Mark every device in `list` that appears in `hcitool con` output. Returns
 * how many were marked.
 *
 * ONE call for ALL of them, which is the whole point. This started as
 * /tmp/tortos_btsink, which launch.sh writes on a twenty-second poll and so
 * knows nothing about a connect this screen just made; then as an `info` per
 * row, which was a fork each and which I limited to the row under the cursor
 * to keep the cost down. That made the label depend on where the CURSOR was -
 * move off the headset and it went back to saying "paired". Connection state
 * is a property of the device, not of the selection. Seen on the device
 * 2026-09-06. */
int bt_mark_connected(const char *hcitool_con, bt_device *list, int n);

/* The right-hand column for one device. Pure, so the wording is checkable:
 * "connected" has to outrank "paired", or a successful connect redraws the
 * list saying exactly what it said before and the screen gives no sign
 * anything happened. wifi_label learned that the hard way. */
void bt_label(const bt_device *d, char *out, size_t n);

bt_state bt_status(void);

/* Bonded devices, read from the bond directories - no fork. */
int bt_bonded(bt_device *out, int max);

/* Everything BlueZ currently knows about, bonded or merely seen. One fork. */
int bt_visible(bt_device *out, int max);

/* Scan in the background for `secs`, so the UI never blocks on it. Results
 * arrive in bt_visible as they are found. */
bool bt_scan(int secs);

/* Each of these forks once and blocks. `err` takes something worth showing. */
bool bt_pair(const char *mac, char *err, size_t n);
bool bt_connect(const char *mac, char *err, size_t n);
/* Is this one connected, asked of BlueZ rather than inferred. One fork, so
 * the caller decides how often - the screen asks only about the row under the
 * cursor.
 *
 * bt_visible marks connections from /tmp/tortos_btsink, which launch.sh writes
 * on a twenty-second poll. That is fine for a headset that reconnected on its
 * own and useless right after the UI connects one: the file has not caught up,
 * so the row redraws saying "paired" and the screen looks like it did nothing.
 * Seen on the device 2026-09-06. */
/* Power the adapter. This is the immediate half of the toggle and it only
 * works when the stack is already up - hciattach, bluetoothd and bluealsa are
 * started by launch.sh from the stored preference, so turning Bluetooth on
 * from a cold boot that had it off takes effect at the next boot. */
bool bt_power(bool on);

bool bt_connected(const char *mac);

bool bt_disconnect(const char *mac);
bool bt_forget(const char *mac);

/* The ALSA name a bonded device gets. Must agree with bt_pcm_name in
 * launch.sh - see bt_asoundrc. */
void bt_pcm_name(const char *mac, char *out, size_t n);

/* Rewrite .asoundrc from the current bonds, so a newly paired headset has a
 * PCM waiting for it.
 *
 * THIS IS A SECOND IMPLEMENTATION of launch.sh's bt_write_asoundrc, and the
 * duplication is deliberate rather than overlooked: the shell one runs at boot
 * before this binary exists, and calling the launcher from launch.sh to avoid
 * it would load SDL on the boot path to write four lines. The check pins the
 * exact bytes both are expected to produce.
 *
 * It also arrives too late to help the emulator that is already running.
 * alsa-lib loads its config once and never notices a file written afterwards,
 * measured 2026-09-05: a config written when a headset connects failed in
 * place and worked immediately on restarting the emulator with the file
 * already there. So a device paired now carries game audio from the next
 * emulator start, and the screen has to say so rather than look broken. */
bool bt_asoundrc(const char *userdata_dir);

#endif
