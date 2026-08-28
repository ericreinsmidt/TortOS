#!/bin/sh
# PlayOS tg5040 boot: hand off to the launch loop on the SD card.

export LD_LIBRARY_PATH=/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=/usr/trimui/bin:$PATH

LAUNCH=/mnt/SDCARD/TortOS/launch.sh
if [ -x "$LAUNCH" ] || [ -f "$LAUNCH" ]; then
	sh "$LAUNCH"
fi
