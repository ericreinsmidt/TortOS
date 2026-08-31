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
P=/mnt/SDCARD/TortOS

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
          /mnt/SDCARD/.userdata/shared /mnt/SDCARD/.system/res" > /dev/null

case $WHAT in elf|all)
	[ -f "$ROOT/build/tortos.elf" ] || { echo "run make first"; exit 1; }
	$A push "$ROOT/build/tortos.elf"        "$P/" > /dev/null
	$A push "$ROOT/build/setbright"         "$P/" > /dev/null
	$A push "$ROOT/config/systems.cfg"      "$P/" > /dev/null
	$A push "$ROOT/config/tortos.cfg"       "$P/" > /dev/null
	$A push "$ROOT/config/coreopts.cfg"     "$P/" > /dev/null
	$A push "$ROOT/sd/tortos/launch.sh"     "$P/" > /dev/null
	$A push "$ROOT/sd/.tmp_update/updater"   /mnt/SDCARD/.tmp_update/ > /dev/null
	$A push "$ROOT/sd/.tmp_update/tg3040.sh" /mnt/SDCARD/.tmp_update/ > /dev/null
	$A shell "chmod +x $P/tortos.elf $P/setbright $P/launch.sh \
	          /mnt/SDCARD/.tmp_update/updater /mnt/SDCARD/.tmp_update/tg3040.sh"
	echo "  + launcher"
esac
case $WHAT in res|all)
	$A push "$ROOT/res/cards/."             "$P/cards/" > /dev/null
	$A push "$ROOT/res/fonts/menu.ttf"      "$P/" > /dev/null
	# Over The Hare's page, served off the card so it can be restyled without
	# a rebuild. The font goes in twice rather than being kept in the repo
	# twice: the launcher reads $P/menu.ttf and a browser asks for
	# /web/menu.ttf, and one copy in git is worth two on a 117 GB card.
	$A shell "mkdir -p $P/res/web" > /dev/null
	$A push "$ROOT/res/web/."               "$P/res/web/" > /dev/null
	$A push "$ROOT/res/fonts/menu.ttf"      "$P/res/web/menu.ttf" > /dev/null
	$A push "$ROOT/res/boot/tortos-boot.mp4" "$P/" > /dev/null
	# The bootloader splash and the pic2fb splash. launch.sh installs these
	# ONCE, guarded by .bootlogo_applied / .splash_applied, so pushing them
	# is not enough on a device that has already been set up - clear the
	# markers too when the animation's opening color changes, or the
	# handoff into it steps from the old color to the new one.
	$A push "$ROOT/res/boot/bootlogo.bmp"   "$P/" > /dev/null
	$A push "$ROOT/res/boot/splash.png"     "$P/" > /dev/null
	# The device has curl and OpenSSL but no trust store, so without this
	# every HTTPS request fails verification - see res/ssl/README.md.
	$A push "$ROOT/res/ssl/cacert.pem"      "$P/" > /dev/null
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
