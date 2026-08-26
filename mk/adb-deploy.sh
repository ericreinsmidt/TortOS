#!/bin/sh
# Push a build to the device over ADB-USB. No card pulling, no WiFi, no SSH.
#
# The TrimUI exposes "TRIMUI ADB" over USB whenever it is powered on, including
# when it has fallen back to stock. The SD card has to be IN the device and
# mounted at /mnt/SDCARD -- that is where everything lives.
#
# Usage: mk/adb-deploy.sh [all|elf|res|diatom]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WHAT=${1:-all}
P=/mnt/SDCARD/PlayOS

# Pick the TrimUI out of whatever else is plugged in.
SER=${ADB_SERIAL:-$(adb devices | awk '/\tdevice$/{print $1}' | while read s; do
	if adb -s "$s" shell 'grep -qi TG.040 /proc/cpuinfo && echo yes' 2>/dev/null | grep -q yes; then
		echo "$s"; break
	fi
done)}
[ -n "$SER" ] || { echo "no TrimUI found over adb (is it powered on?)"; exit 1; }
A="adb -s $SER"

# Is the card actually mounted? This has to be judged by OUTPUT, not by an exit
# status: `adb shell` returns 0 regardless of what the remote command exited
# with, so the obvious `adb shell '... | grep -q ...' || exit 1` guard is
# silently always-true. With the card dropped off the bus, that guard waves the
# deploy through and every file lands in the RAM overlay under the empty
# mountpoint -- reporting success and changing nothing.
case "$($A shell 'mount | grep -q " /mnt/SDCARD " && echo MOUNTED')" in
*MOUNTED*) ;;
*)
	echo "!! /mnt/SDCARD is not mounted on the device -- nothing was deployed."
	echo "   Anything written there now would go to RAM, not the card."
	echo "   Reseat the SD card and reboot the device, then run this again."
	exit 1
	;;
esac

$A shell "mkdir -p $P/cards $P/cores $P/lib $P/bin /mnt/SDCARD/.tmp_update \
          /mnt/SDCARD/.userdata/shared /mnt/SDCARD/.system/res \
          /mnt/SDCARD/.system/tg5040/shaders" > /dev/null

case $WHAT in elf|all)
	[ -f "$ROOT/build/playos.elf" ] || { echo "run make first"; exit 1; }
	$A push "$ROOT/build/playos.elf"        "$P/" > /dev/null
	$A push "$ROOT/build/setbright"         "$P/" > /dev/null
	$A push "$ROOT/config/systems.cfg"      "$P/" > /dev/null
	$A push "$ROOT/config/playos.cfg"       "$P/" > /dev/null
	$A push "$ROOT/sd/playos/launch.sh"     "$P/" > /dev/null
	$A push "$ROOT/sd/.tmp_update/updater"   /mnt/SDCARD/.tmp_update/ > /dev/null
	$A push "$ROOT/sd/.tmp_update/tg5040.sh" /mnt/SDCARD/.tmp_update/ > /dev/null
	$A shell "chmod +x $P/playos.elf $P/setbright $P/launch.sh \
	          /mnt/SDCARD/.tmp_update/updater /mnt/SDCARD/.tmp_update/tg5040.sh"
	echo "  + launcher"
esac
case $WHAT in res|all)
	$A push "$ROOT/res/cards/."             "$P/cards/" > /dev/null
	$A push "$ROOT/res/fonts/menu.ttf"      "$P/" > /dev/null
	$A push "$ROOT/res/boot/playos-boot.mp4" "$P/" > /dev/null
	echo "  + assets"
esac
case $WHAT in diatom|all)
	D=${DIATOM_ELF:-$ROOT/../diatom/build/brick/diatom}
	[ -f "$D" ] || { echo "no diatom at $D (set DIATOM_ELF)"; exit 1; }
	$A push "$D" "$P/diatom" > /dev/null
	$A shell "chmod +x $P/diatom"
	echo "  + diatom"
esac
case $WHAT in vendor)
	$A push "$ROOT/vendor/cores/."  "$P/cores/" > /dev/null
	echo "  + cores and runtime"
esac
$A shell sync
echo "deployed '$WHAT' to $SER"
