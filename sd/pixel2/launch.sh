#!/bin/sh
# TortOS on the GKD Pixel 2. TortOS-px2's init starts this once the card's
# games partition is mounted at /mnt/SDCARD.
#
# The Brick's launch.sh, less everything that belongs to the Brick's stock
# firmware - its LEDs, its input daemon, Wi-Fi and Bluetooth, the boot logo
# and splash it patches. The Pixel's system is TortOS-px2, ours from the
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

mkdir -p "$BIOS_PATH" "$ROMS_PATH" "$SAVES_PATH" "$USERDATA_PATH" "$LOGS_PATH" \
         "$SHARED_USERDATA_PATH"

LOG=$LOGS_PATH/tortos.log
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
: > "$LOG"

start_resident() {
	pidof diatom > /dev/null && return
	rm -f "$TORTOS_DIATOM_SOCKET"
	"$TORTOS_DIR/diatom" --socket "$TORTOS_DIATOM_SOCKET" \
		--cores "$CORES_PATH" \
		--save "$SAVES_PATH" --system "$BIOS_PATH" >> "$LOG" 2>&1 &
	echo $! > /tmp/diatom.pid
}

start_resident
rm -f /tmp/tortos_poweroff

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
	start_resident          # bring it back if it died
	START=$(cut -d. -f1 /proc/uptime)
	./tortos.elf >> "$LOG" 2>&1
	[ -f /tmp/tortos_poweroff ] && break
	END=$(cut -d. -f1 /proc/uptime)
	if [ $((END - START)) -lt 5 ]; then
		FAILS=$((FAILS + 1))
		[ $FAILS -ge 5 ] && break
	else
		FAILS=0
	fi
	sleep 1
done
sync
poweroff
