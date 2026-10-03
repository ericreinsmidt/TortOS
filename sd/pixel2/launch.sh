#!/bin/sh
# TortOS on the GKD Pixel 2. plastron's init starts this once the card's
# games partition is mounted at /mnt/SDCARD.
#
# The Brick's launch.sh, less everything that belongs to the Brick's stock
# firmware - its LEDs, its input daemon, Wi-Fi and Bluetooth, the boot logo
# and splash it patches. The Pixel's system is plastron, ours from the
# kernel up, so there is nothing here to work around: start the emulator,
# start the launcher, power off when it is done.

TORTOS_DIR=/mnt/SDCARD/TortOS
SDCARD=/mnt/SDCARD

export PLATFORM=pixel2
export DEVICE=pixel2
export SDCARD_PATH=$SDCARD
export BIOS_PATH=$SDCARD/Bios
export ROMS_PATH=$SDCARD/Roms
export SAVES_PATH=$SDCARD/Saves
export CHEATS_PATH=$SDCARD/Cheats
export SYSTEM_PATH=$TORTOS_DIR
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

LOG=$LOGS_PATH/tortos.log
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
: > "$LOG"

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
