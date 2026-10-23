#!/bin/sh
# TortOS on the GKD Pixel 2. plastron's init starts this once the card's
# games partition is mounted at /mnt/SDCARD.
#
# The Brick's launch.sh, less what the Pixel doesn't have: Bluetooth, and a
# radio of its own (its Wi-Fi is a USB adapter, when one is plugged in). The
# Pixel's system is plastron, ours from the kernel up, so there is nothing
# here to work around: start the emulator, start the launcher, power off when
# it is done.

TORTOS_DIR=/mnt/SDCARD/TortOS
SDCARD=/mnt/SDCARD

export BIOS_PATH=$SDCARD/Bios
export ROMS_PATH=$SDCARD/Roms
export SAVES_PATH=$SDCARD/Saves
export CORES_PATH=$TORTOS_DIR/cores
export USERDATA_PATH=$SDCARD/.userdata/pixel2
export SHARED_USERDATA_PATH=$SDCARD/.userdata/shared
export LOGS_PATH=$USERDATA_PATH/logs
export HOME=$USERDATA_PATH
export TORTOS_DIATOM_SOCKET=/tmp/diatom.sock

# Everything up to the launcher is on the boot's critical path, so it is done
# with the shell's own built-ins wherever one will do: every helper program
# (mkdir, pidof, rm, cut) is a process start, 5 to 15 ms each on this CPU
# while the rest of the boot runs, and they added up to 93 ms before the
# launcher started (measured 2026-10-01).

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
# for it, as on the Brick. The default is for a fresh card's first boot.
WIFI=0
BOOT_ENV=$USERDATA_PATH/boot.env
[ -f "$BOOT_ENV" ] && . "$BOOT_ENV"

# Wi-Fi on, as the Brick does at boot, when it was left on and an adapter is
# in (wifi_follow, below): the supplicant rejoins a saved network by itself,
# and once it has, the DHCP client, started exactly as src/wifi.c starts it
# (the same PID file, no -q, so it stays and renews the lease), so TortOS
# finds it running rather than starting a second. /etc/init.d/wpa_supplicant
# stops it with Wi-Fi.
WPA_SOCK=/var/run/wpa_supplicant
DHCP_PID=/var/run/udhcpc.wlan0.pid
wifi_on() {
	/etc/init.d/wpa_supplicant start > /dev/null 2>&1 || {
		echo "wifi: no adapter, or it did not come up" >> "$LOG"
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

start_resident
[ -e /tmp/tortos_poweroff ] && rm -f /tmp/tortos_poweroff

# Wi-Fi follows the adapter as it comes and goes: it isn't the Pixel's own, so
# it can be plugged in or pulled at any time, a game included. Looked for every
# 2 seconds with the shell's own test, no process started for it. When it
# appears, and Wi-Fi was left on (boot.env, read again, as TortOS rewrites it
# when the setting changes), the same as at boot; when it goes, the
# supplicant and the DHCP client stop, so the next one starts clean. From when
# the splash ends, as on the Brick, so the adapter's driver and the supplicant
# stay out of TortOS's way to its first frame; a splash that never ends holds
# it 20 s at most.
wifi_follow() {
	i=0
	while [ $i -lt 200 ] && pidof splash > /dev/null; do
		sleep 0.1
		i=$((i + 1))
	done
	had=
	while : ; do
		if [ -d /sys/class/net/wlan0 ]; then
			if [ -z "$had" ]; then
				had=1
				WIFI=0
				[ -f "$BOOT_ENV" ] && . "$BOOT_ENV"
				[ "$WIFI" = 1 ] && wifi_on &
			fi
		elif [ -n "$had" ]; then
			had=
			/etc/init.d/wpa_supplicant stop > /dev/null 2>&1
		fi
		sleep 2
	done
}
wifi_follow &

# A Mac or PC leaves its own files on the card. Swept after the launcher is up,
# as on the Brick, so it costs the boot nothing.
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
