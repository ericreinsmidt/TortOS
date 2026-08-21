#!/bin/sh
# Installed to /usr/trimui/bin/runtrimui.sh by the first-boot installer.
# Waits for the SD card, then runs its updater (PlayOS) if present, else stock.

mounted=$(cat /proc/mounts | grep -i SDCARD)
cnt=0
while [ "$mounted" = "" ] && [ $cnt -lt 6 ]; do
	sleep 0.5
	cnt=$((cnt + 1))
	mounted=$(cat /proc/mounts | grep -i SDCARD)
done

UPDATER=/mnt/SDCARD/.tmp_update/updater
if [ -f "$UPDATER" ]; then
	"$UPDATER"
else
	/usr/trimui/bin/runtrimui-original.sh
fi
