#!/bin/sh
# TortOS on the TrimUI Brick. plastron's init starts this once the card's
# games partition is mounted at /mnt/SDCARD, as on the GKD Pixel 2.
#
# The Pixel's launch.sh, plus what the Brick has and the Pixel doesn't: Wi-Fi
# and Bluetooth, started at boot when they were left on, and the loop that
# keeps a remembered headset connected and tells TortOS where sound can go.
# Everything that worked around TrimUI's firmware (its input daemon, LEDs,
# boot logo and splash, procd's services) went with it: the system is
# plastron, ours from the kernel up.

TORTOS_DIR=/mnt/SDCARD/TortOS
SDCARD=/mnt/SDCARD

export BIOS_PATH=$SDCARD/Bios
export ROMS_PATH=$SDCARD/Roms
export SAVES_PATH=$SDCARD/Saves
export CORES_PATH=$TORTOS_DIR/cores
export USERDATA_PATH=$SDCARD/.userdata/tg3040
export SHARED_USERDATA_PATH=$SDCARD/.userdata/shared
export LOGS_PATH=$USERDATA_PATH/logs
export HOME=$USERDATA_PATH
export TORTOS_DIATOM_SOCKET=/tmp/diatom.sock
export SDL_VIDEODRIVER=pvrfb SDL_RENDER_DRIVER=opengles2
# .asoundrc named as a top-level ALSA config file, and not only reached through
# alsa.conf's @hooks load: alsa-lib re-reads its config at the next open when a
# file in this list changes, and never checks files a hook loaded. So a headset
# paired after Diatom or Muse started, whose PCM bt_write_asoundrc has just
# added, was `Unknown PCM` to both until they restarted (measured 2026-09-25).
export ALSA_CONFIG_PATH=/usr/share/alsa/alsa.conf:$HOME/.asoundrc

# The system's own tool, which lights the panel: TortOS turns it off on its
# way out (a power-off, Auto Off), and the next pass needs it back on
BACKLIGHT=/usr/lib/brick/backlight

# Everything up to the launcher is on the boot's critical path, so it is done
# with the shell's own built-ins wherever one will do: every helper program is
# a process start, and on the Pixel they added up to 93 ms before the launcher
# started (measured 2026-10-01). The radios go in the background.

# Only on a fresh card are any of these missing
for d in "$BIOS_PATH" "$ROMS_PATH" "$SAVES_PATH" "$USERDATA_PATH" "$LOGS_PATH" \
         "$SHARED_USERDATA_PATH"; do
	[ -d "$d" ] || mkdir -p "$d"
done

# One log per boot, carrying the launcher and everything it starts. Ten boots
# kept: this one and tortos.log.1 to .9, newest first. One wasn't enough: a
# player's report can come several reboots after the fault. Only the last
# boot's log is moved out of the way here; the older ones shift along in the
# background, where nothing waits on them (in line, the renames took 260 ms of
# the boot on TrimUI's firmware, measured 2026-10-02).
LOG=$LOGS_PATH/tortos.log
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.prev"
: > "$LOG"
(
	i=9
	while [ $i -gt 1 ]; do
		[ -f "$LOG.$((i - 1))" ] && mv -f "$LOG.$((i - 1))" "$LOG.$i"
		i=$((i - 1))
	done
	[ -f "$LOG.prev" ] && mv -f "$LOG.prev" "$LOG.1"
) &

# The settings this script needs before TortOS runs, which TortOS writes out
# for it (there is no sqlite3 here to read its database). Sourced rather than
# parsed: parsing costs a fork per value. The defaults are for a fresh card's
# first boot, before TortOS has ever run.
WIFI=0
BLUETOOTH=0
BOOT_ENV=$USERDATA_PATH/boot.env
[ -f "$BOOT_ENV" ] && . "$BOOT_ENV"

# Wi-Fi at boot, when it was left on. The supplicant rejoins a saved network
# by itself, but nothing asks for an address: so once it has joined, the DHCP
# client, started exactly as src/wifi.c starts it (the same PID file, no -q,
# so it stays and renews the lease), and TortOS finds it running rather than
# starting a second. /etc/init.d/wpa_supplicant stops it with the radio.
WPA_SOCK=/var/run/wpa_supplicant
DHCP_PID=/var/run/udhcpc.wlan0.pid
wifi_on() {
	/etc/init.d/wpa_supplicant start > /dev/null 2>&1 || {
		echo "wifi: the radio did not come up" >> "$LOG"
		return 1
	}
	i=0
	while [ $i -lt 25 ]; do
		if wpa_cli -p $WPA_SOCK -i wlan0 status 2> /dev/null |
		   grep -q '^wpa_state=COMPLETED'; then
			p=$(cat $DHCP_PID 2> /dev/null)
			[ -n "$p" ] && [ -d "/proc/$p" ] && return 0
			udhcpc -i wlan0 -S -t 5 -T 7 -b -p $DHCP_PID > /dev/null 2>&1
			return 0
		fi
		sleep 1
		i=$((i + 1))
	done
	echo "wifi: no saved network joined after ${i}s" >> "$LOG"
}

# Bluetooth at boot, when it was left on: the system's script brings it up
# (the modules, dbus, the chip attached, bluetoothd, bluealsa), then the
# reconnect loop below. Off needs nothing: the radio is off from power-on.
bt_on() {
	/etc/init.d/bluetooth start > /dev/null 2>&1 || {
		echo "bt: Bluetooth did not come up" >> "$LOG"
		return 1
	}
	# The headsets' PCMs again, now that their bonds can be read: they live on
	# plastron's state partition, which Bluetooth's start mounts, so the write
	# before start_resident found none. The emulator and Muse see the new
	# file at their next open (ALSA_CONFIG_PATH, above)
	command -v bt_write_asoundrc > /dev/null && bt_write_asoundrc
	sleep 3          # let both profiles register with BlueZ before any connect
	bt_reconnect
}

# Whether bluealsa has an A2DP stream for this address yet. The link comes up
# first and the stream a moment after, and a headset published in between
# is one that nothing can open: Muse got `No such device` and fell back to
# the speaker, and stayed there until something happened to re-send the
# route (seen 2026-09-26, switching to the OpenFit). -l lists each device's
# PCMs under its own line, so the A2DP line has to be under THIS one's.
bt_a2dp_ready() {
	bluealsa-aplay -l 2> /dev/null |
		awk -v m="$1" '/^hci/ { on = index($0, m) > 0 } on && /A2DP/ { f = 1 } END { exit !f }'
}

# Keep a remembered headset connected, so powering it on reconnects it wherever
# you are rather than only at a screen that happens to be watching. And tell
# TortOS where the sound can go, in /tmp/tortos_btsink.
#
# Trusted devices with a key only, from bluetoothd's own /var/lib/bluetooth/
# <adapter>/. Judge success by `info`, never by the return of `connect`:
# bluetoothctl reports Failed for a2dp even when the link came up.
bt_reconnect() {
	adapter=$(hciconfig hci0 2> /dev/null | sed -n 's/.*BD Address: \([0-9A-F:]*\).*/\1/p')
	[ -n "$adapter" ] || return 0
	latest=
	prev_links=
	soon=0
	while :; do
		connected=
		bonds=
		for d in /var/lib/bluetooth/"$adapter"/*:*; do
			[ -d "$d" ] || continue
			grep -q '^Trusted=true' "$d/info" 2> /dev/null || continue
			# And a key, or it is not a bond: a memory-only pairing leaves
			# Trusted behind with no key after a restart, and dialing that
			# every pass connected it for four seconds at a time and published
			# a sink each time. See bond_name in src/bt.c.
			grep -q '^\[LinkKey\]' "$d/info" 2> /dev/null || continue
			bonds="$bonds $(basename "$d")"
		done
		# Whoever is already connected first, and nobody dialed while one is:
		# `connect` takes about five seconds to give up on a headset that is
		# off, and walking in address order made a live headset wait on a dead
		# one (measured 2026-09-25).
		#
		# And with two connected, the one connected LAST, as a phone does. The
		# order is kept here, pass to pass: a bond connected now and not last
		# pass, or on a different link handle (a reconnect), is the latest. Not
		# the handle's value itself, which is reused. A connect from the
		# Bluetooth screen asks for a pass at once, so the headset chosen there
		# takes the sound within a second.
		cons=$(hcitool con 2> /dev/null)
		links=
		for mac in $bonds; do
			bluetoothctl info "$mac" 2> /dev/null | grep -q 'Connected: yes' || continue
			h=$(echo "$cons" | sed -n "s/.*ACL $mac handle \([0-9]*\).*/\1/p" | head -1)
			links="$links $mac/$h"
			case " $prev_links " in *" $mac/$h "*) ;; *) latest=$mac ;; esac
		done
		prev_links=$links
		# The latest while it stays connected; when it goes, one that has not.
		case "$links " in
		*" $latest/"*) connected=$latest ;;
		*) set -- $links; connected=${1%%/*} ;;
		esac
		# Nothing connected: say so NOW, before dialing. Dialing a headset that
		# is off takes about five seconds each, and a song that had failed over
		# to the speaker stayed there when its headset came back meanwhile
		# (seen 2026-09-25).
		[ -n "$connected" ] || rm -f /tmp/tortos_btsink
		for mac in $bonds; do
			[ -z "$connected" ] || break
			bluetoothctl connect "$mac" > /dev/null 2>&1
			# Judge by info, NEVER by the return. Trusting the return published
			# "no sink" while a headset was connected, and every flip reopens
			# Diatom's audio device: nine route changes and 407236 dropped
			# audio frames in one game (2026-09-05).
			sleep 1
			bluetoothctl info "$mac" 2> /dev/null | grep -q 'Connected: yes' &&
				connected=$mac
		done
		# A file rather than TortOS asking, because asking means forking
		# bluetoothctl out of a large process. This loop already knows.
		if [ -n "$connected" ] && ! bt_a2dp_ready "$connected"; then
			# Connected, its stream not there yet: leave what is published
			# (during a switch that is the old headset, still playing) and look
			# again in a second rather than twenty. Fifteen times at most, for
			# a bond that never gets an audio stream at all.
			soon=$((soon + 1))
		elif [ -n "$connected" ]; then
			soon=0
			# The PCM's name from .asoundrc, not a bluealsa device string,
			# which SDL opens and then never drains (measured 2026-09-05). And
			# the link's ACL handle on a second line, new on every connection:
			# a headset that dropped and came back between two looks keeps its
			# name, and without the handle whoever had fallen back to the
			# speaker meanwhile was never sent back to it.
			link=$(hcitool con 2> /dev/null |
				sed -n "s/.*ACL $connected handle \([0-9]*\).*/\1/p" | head -1)
			printf '%s\n%s\n' "$(bt_pcm_name "$connected")" "$link" \
				> /tmp/tortos_btsink.tmp && mv /tmp/tortos_btsink.tmp /tmp/tortos_btsink
		else
			soon=0
			rm -f /tmp/tortos_btsink
		fi
		# Twenty seconds, or less when the Bluetooth screen asks: it touches
		# /tmp/tortos_btpass after a connect, a disconnect or a forget, so the
		# sink moves with the screen rather than up to a pass later. A file
		# and not a signal: TortOS has no pid for this loop, and a signal to
		# the wrong shell would be launch.sh itself.
		n=0
		wait=20
		[ $soon -gt 0 ] && [ $soon -le 15 ] && wait=1
		while [ $n -lt $wait ] && [ ! -e /tmp/tortos_btpass ]; do
			sleep 1
			n=$((n + 1))
		done
		rm -f /tmp/tortos_btpass
	done
}

# The headsets' ALSA config, bt_pcm_name and bt_write_asoundrc, in a file of
# its own so TortOS runs the same code after a pair or a forget. Tested first:
# `.` on a missing file ends a non-interactive shell, and this one ending
# would leave no TortOS. Without it the boot goes on, with game sound on the
# speaker.
[ -f "$TORTOS_DIR/bt-alsa.sh" ] && . "$TORTOS_DIR/bt-alsa.sh"

# The radios once TortOS has drawn its first frame, which is when plastron's
# splash ends: loading Wi-Fi's driver and firmware beside TortOS's startup
# made the shelf about 0.15 s later (measured 2026-10-18, three cold boots
# each way). They come up a second or so later for it, still long before
# anyone could open a menu. Bounded: a splash that never ends (no TortOS
# drawing) holds them 20 s at most.
radios_after_splash() {
	i=0
	while [ $i -lt 200 ] && pidof splash > /dev/null; do
		sleep 0.1
		i=$((i + 1))
	done
	[ "$WIFI" = 1 ] && wifi_on &
	[ "$BLUETOOTH" = 1 ] && bt_on &
}
{ [ "$WIFI" = 1 ] || [ "$BLUETOOTH" = 1 ]; } && radios_after_splash &

# Running already? By the pid file and /proc, rather than pidof's walk of
# every process. A pid reused after a crash is caught by the name.
diatom_running() {
	local pid comm
	[ -f /tmp/diatom.pid ] && read pid < /tmp/diatom.pid || return 1
	[ -r "/proc/$pid/comm" ] && read comm < "/proc/$pid/comm" || return 1
	[ "$comm" = diatom ]
}

start_resident() {
	diatom_running && return
	[ -e "$TORTOS_DIATOM_SOCKET" ] && rm -f "$TORTOS_DIATOM_SOCKET"
	"$TORTOS_DIR/diatom" --socket "$TORTOS_DIATOM_SOCKET" \
		--cores "$CORES_PATH" \
		--save "$SAVES_PATH" --system "$BIOS_PATH" >> "$LOG" 2>&1 &
	echo $! > /tmp/diatom.pid
}

# Whole seconds since boot into NOW, for telling a crash loop from a normal
# exit. A variable rather than $(...), which would fork a subshell for it.
now_s() {
	local up rest
	read up rest < /proc/uptime
	NOW=${up%%.*}
}

# BEFORE start_resident, and that order is the whole point: alsa-lib caches
# its config at the first PCM open, so a definition written afterwards is
# invisible to that process for as long as it lives. Only when bt-alsa.sh
# was there to source.
command -v bt_write_asoundrc > /dev/null && bt_write_asoundrc
start_resident
[ -e /tmp/tortos_poweroff ] && rm -f /tmp/tortos_poweroff

# A Mac or PC leaves its own files on the card: ._ files, .DS_Store,
# Thumbs.db and desktop.ini anywhere, and the folders macOS writes at the top
# of any card it mounts, .Trashes included (Eric's choice, 2026-09-30: on a
# handheld "deleted" means gone). Swept after the launcher is up, so it costs
# the boot nothing.
( sleep 8
  rm -rf /mnt/SDCARD/.Trashes /mnt/SDCARD/.Spotlight-V100 /mnt/SDCARD/.fseventsd \
         /mnt/SDCARD/.TemporaryItems
  find /mnt/SDCARD \( -name '._*' -o -name '.DS_Store' -o -name 'Thumbs.db' \
       -o -name 'desktop.ini' \) -type f -exec rm -f {} \;
) > /dev/null 2>&1 &

# The launcher, restarted if it exits on its own; five fast failures in a row
# and the device powers off rather than looping on a broken build.
cd "$TORTOS_DIR"
FAILS=0
while : ; do
	[ -x $BACKLIGHT ] && $BACKLIGHT on
	now_s; START=$NOW
	./tortos.elf >> "$LOG" 2>&1
	[ -f /tmp/tortos_poweroff ] && break
	now_s; END=$NOW
	if [ $((END - START)) -lt 5 ]; then
		FAILS=$((FAILS + 1))
		[ $FAILS -ge 5 ] && break
	else
		FAILS=0
	fi
	sleep 1
	# Bring the emulator back if it died, before the launcher restarts. Here
	# and not at the top of the loop: the first pass would ask the instant
	# after the start above, while the new process is still a copy of this
	# shell, and start a second Diatom. Measured 2026-10-01, two of them.
	start_resident
done
sync
poweroff
