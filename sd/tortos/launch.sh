#!/bin/sh
# TortOS boot entry. Called from .tmp_update/tg5040.sh; never returns.
#
# The order of things in here is the boot time. The animation runs in the
# background and everything else -- the launcher's whole startup and the
# resident emulator's -- happens behind it, so the animation costs its length
# and nothing else.

TORTOS_DIR=/mnt/SDCARD/TortOS
SDCARD=/mnt/SDCARD

export PLATFORM=tg5040
export DEVICE=brick
export SDCARD_PATH=$SDCARD
export BIOS_PATH=$SDCARD/Bios
export ROMS_PATH=$SDCARD/Roms
export SAVES_PATH=$SDCARD/Saves
export CHEATS_PATH=$SDCARD/Cheats
export SYSTEM_PATH=$TORTOS_DIR
export CORES_PATH=$TORTOS_DIR/cores
export USERDATA_PATH=$SDCARD/.userdata/tg5040
export SHARED_USERDATA_PATH=$SDCARD/.userdata/shared
export LOGS_PATH=$USERDATA_PATH/logs
export HOME=$USERDATA_PATH
export LD_LIBRARY_PATH=/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=/usr/trimui/bin:$PATH

# Cleared by the boot animation when it finishes. The launcher does all of its
# startup while the animation plays, then blocks on this and presents its first
# frame the moment the animation clears it.
TORTOS_ANIM_FLAG=/tmp/tortos_bootanim
export TORTOS_ANIM_FLAG

mkdir -p "$BIOS_PATH" "$ROMS_PATH" "$SAVES_PATH" "$USERDATA_PATH" "$LOGS_PATH" \
         "$SHARED_USERDATA_PATH"

CFG=$TORTOS_DIR/tortos.cfg
getcfg() { [ -f "$CFG" ] && sed -n "s/^$1=//p" "$CFG" | tail -1; }

# All LEDs off: TortOS shows no chrome, and the lights are pure battery drain.
# A function, because trimui_inputd re-lights them when it starts.
leds_off() {
	echo 0 > /sys/class/led_anim/effect_enable 2> /dev/null
	for g in l r lr m f1 f2; do echo "000000 " > /sys/class/led_anim/effect_rgb_hex_$g 2> /dev/null; done
	echo 0 > /sys/class/led_anim/max_scale 2> /dev/null
	echo 0 > /sys/class/led_anim/max_scale_lr 2> /dev/null
	echo 0 > /sys/class/led_anim/max_scale_f1f2 2> /dev/null
	for f in /sys/class/leds/sunxi_led*/brightness; do echo 0 > "$f" 2> /dev/null; done
}

# Apply the configured brightness now, so the boot animation is not dimmer than
# everything after it. tortos.elf has not started yet and cannot do it.
#
# This ladder is TortOS's own and is shared verbatim with platform.c and with
# the emulator: twelve geometric rungs, the first being the panel's measured
# floor (0 and 1 are black on this display). A saved level in
# .userdata/levels.cfg wins over the config default, because that is the
# level the player last chose.
brightness_raw() {
	B=$(sed -n 's/^brightness=//p' "$USERDATA_PATH/levels.cfg" 2> /dev/null | head -1)
	[ -n "$B" ] || B=$(getcfg brightness)
	case "$B" in
		0) echo 2;;  1) echo 4;;   2) echo 8;;   3) echo 16;;
		4) echo 32;; 5) echo 48;;  6) echo 72;;  7) echo 96;;
		8) echo 128;; 9) echo 160;; 10) echo 192;; 11) echo 255;;
		*) echo 96;;
	esac
}
[ -x "$TORTOS_DIR/setbright" ] && "$TORTOS_DIR/setbright" "$(brightness_raw)"
leds_off

# One-time: replace the stock u-boot splash with a black frame, so the handoff
# into the boot animation is seamless rather than a TrimUI logo followed by a
# cut. Guarded by a marker; the stock logo is backed up first.
if [ -f "$TORTOS_DIR/bootlogo.bmp" ] && [ ! -f "$TORTOS_DIR/.bootlogo_applied" ]; then
	mkdir -p /mnt/boot
	if mount -t vfat /dev/mmcblk0p1 /mnt/boot 2> /dev/null; then
		if [ -f /mnt/boot/bootlogo.bmp ] && [ ! -f "$TORTOS_DIR/bootlogo.stock.bmp" ]; then
			cp /mnt/boot/bootlogo.bmp "$TORTOS_DIR/bootlogo.stock.bmp"
		fi
		cp "$TORTOS_DIR/bootlogo.bmp" /mnt/boot/bootlogo.bmp && sync
		umount /mnt/boot && touch "$TORTOS_DIR/.bootlogo_applied"
	fi
fi

# One-time: the stock "loading" splash that pic2fb blits from /etc/splash.png.
# Same black frame, same reason.
if [ -f "$TORTOS_DIR/splash.png" ] && [ ! -f "$TORTOS_DIR/.splash_applied" ]; then
	if [ -f /etc/splash.png ] && [ ! -f "$TORTOS_DIR/splash.stock.png" ]; then
		cp /etc/splash.png "$TORTOS_DIR/splash.stock.png"
	fi
	cp "$TORTOS_DIR/splash.png" /etc/splash.png && sync && touch "$TORTOS_DIR/.splash_applied"
fi

# One-time: pic2fb blits that splash at the hardware default brightness, which
# is not ours, so the screen visibly steps partway through the boot. Call the
# brightness helper before it. Reversible: the stock init is backed up.
if [ -x "$TORTOS_DIR/setbright" ] && [ ! -f "$TORTOS_DIR/.brightboot_applied" ]; then
	cp "$TORTOS_DIR/setbright" /usr/trimui/bin/setbright 2> /dev/null && chmod +x /usr/trimui/bin/setbright
	cat > /usr/trimui/bin/tortos-bootbright.sh <<'BB'
#!/bin/sh
BR=$(sed -n 's/^brightness=//p' /mnt/SDCARD/TortOS/tortos.cfg 2> /dev/null | tail -1)
case "$BR" in
	0) R=1;; 1) R=8;; 2) R=16;; 3) R=32;; 4) R=48;; 5) R=72;;
	6) R=96;; 7) R=128;; 8) R=160;; 9) R=192;; 10) R=255;; *) R=160;;
esac
[ -x /usr/trimui/bin/setbright ] && /usr/trimui/bin/setbright "$R"
BB
	chmod +x /usr/trimui/bin/tortos-bootbright.sh
	if [ -f /etc/init.d/runtrimui ] && ! grep -q tortos-bootbright /etc/init.d/runtrimui; then
		cp /etc/init.d/runtrimui /etc/init.d/runtrimui.tortos-bak
		awk '/pic2fb/ && !d {print "/usr/trimui/bin/tortos-bootbright.sh"; d=1} {print}' \
			/etc/init.d/runtrimui.tortos-bak > /etc/init.d/runtrimui
		sh -n /etc/init.d/runtrimui 2> /dev/null || cp /etc/init.d/runtrimui.tortos-bak /etc/init.d/runtrimui
		chmod +x /etc/init.d/runtrimui
	fi
	sync
	touch "$TORTOS_DIR/.brightboot_applied"
fi

# The boot animation, played in the BACKGROUND.
#
# An animation that adds its own length to the boot is just a delay with a
# picture on it. This one plays while the launcher does its entire startup --
# card scan, GL init, font and card decode -- and while the resident emulator
# builds its GL context and opens three cores.
#
# ffmpeg and the launcher both write to /dev/fb0, so they must never draw at
# the same time: last writer wins, and they would fight at 30fps against 60.
# The marker file is the handshake -- the launcher initializes freely, blocks
# on the marker, and presents the moment the animation clears it. Removed in
# the same subshell so it goes even if the decoder dies.
if [ -f "$TORTOS_DIR/tortos-boot.mp4" ]; then
	: > "$TORTOS_ANIM_FLAG"
	(
		ffmpeg -hide_banner -loglevel quiet -re -i "$TORTOS_DIR/tortos-boot.mp4" \
		       -pix_fmt bgra -f fbdev /dev/fb0 2> /dev/null
		rm -f "$TORTOS_ANIM_FLAG"
	) &

	# Free seconds: pull what the first launch needs off the card and into the
	# page cache while nothing else is using the disk. Cold reads of the
	# emulator, the cores and their libraries measure ~190ms against ~30ms warm.
	(
		cat "$TORTOS_DIR/diatom" "$TORTOS_DIR/cores/"*.so > /dev/null 2>&1
	) &
fi

# Rumble off, mute-switch gpio readable
echo 227 > /sys/class/gpio/export 2> /dev/null
echo -n out > /sys/class/gpio/gpio227/direction 2> /dev/null
echo -n 0 > /sys/class/gpio/gpio227/value 2> /dev/null

# Radio silence. TortOS has nothing to talk to yet: no downloads, no pairing,
# no achievements. Both radios are battery drain and boot time.
#
# `wifi=1` in tortos.cfg -- or a .devwifi marker -- keeps WiFi up so a
# development unit stays reachable over ssh. Without it there is no way to
# diagnose a problem on hardware except by pulling the card, which is a bad
# place to be when something does not come up.
#
# Stop the supplicant and drop the interface rather than rfkill-blocking, so
# the radio is left in a state the firmware understands.
#
# Both halves of this were dead until 2026-08-29, and measurably so: the
# supplicant was running on a device whose own log said radio silence.
#
# The `on` half called /etc/wifi/wifi_init.sh, which does not exist anywhere
# on the device and never has, so it failed silently and the flag did nothing.
# The stock service is procd-managed, so starting it is what the init script
# is for.
#
# The `off` half lost a race. /etc/rc.d/S96wpa_supplicant is USE_PROCD=1 and
# procd is pid 1, so procd owns the process and respawns a bare kill. The
# init script's `stop` handles that correctly -- but its `start_service`
# retries `ifconfig wlan0 up` five times with usleep 500000 between, so S96
# is still inside that loop when S99 runs us, and it finishes and starts the
# supplicant after we asked for it to be stopped. PIDs told the story:
# launch.sh 1839, tortos.elf 2156, wpa_supplicant 2286.
#
# So the stop is repeated across a window rather than once, and backgrounded,
# because waiting out someone else's usleeps is not worth the boot time.
#
# The window, and not just a retry-until-gone loop, because stopping it once
# is not the same as it staying stopped. Measured on 2026-08-29: a loop that
# exited on the first clear reading left the supplicant gone at 20s and back
# at 37s, restarted after tortos.elf was already up. It then exited on its
# own around 90s, but only because wlan0 was down underneath it -- which is
# luck, not a mechanism. Keep stopping it for the whole window instead.
radio_off() {
	i=0
	while [ $i -lt 20 ]; do
		if pgrep -f '[w]pa_supplicant' > /dev/null; then
			/etc/init.d/wpa_supplicant stop > /dev/null 2>&1
			killall -q udhcpc 2> /dev/null
		fi
		ifconfig wlan0 down 2> /dev/null
		sleep 1
		i=$((i + 1))
	done
	pgrep -f '[w]pa_supplicant' > /dev/null || return 0
	# $LOG is not set this early and would not survive the rotation below;
	# LOGS_PATH is exported at the top and the rotation happens seconds before
	# this line can ever run.
	echo "wifi: supplicant still up after ${i}s, giving up" >> "$LOGS_PATH/tortos.log"
}

# Bring the radio up and take a lease. Associating is not connecting: the
# supplicant joins a saved network on its own, but nothing on this device runs
# a DHCP client at boot, so without this the interface comes up with no address
# and every fetch fails in a way that looks like a dead network rather than a
# missing lease.
wifi_on() {
	i=0
	/etc/init.d/wpa_supplicant start > /dev/null 2>&1
	while [ $i -lt 25 ]; do
		if wpa_cli -p /etc/wifi/sockets -i wlan0 status 2> /dev/null \
		   | grep -q '^wpa_state=COMPLETED'; then
			udhcpc -i wlan0 -S -t 5 -T 7 -b -q > /dev/null 2>&1
			return 0
		fi
		sleep 1
		i=$((i + 1))
	done
	echo "wifi: no association after ${i}s" >> "$LOGS_PATH/tortos.log"
}

# The state the player left it in wins over the shipped default, the same way
# a saved brightness level does. tortos.cfg says what a fresh card does;
# .userdata/wifi.cfg says what THIS device was doing when it was last shut
# down, which is what someone who turned wifi on expects to find.
WIFI=$(sed -n 's/^wifi=//p' "$USERDATA_PATH/wifi.cfg" 2> /dev/null | head -1)
[ -n "$WIFI" ] || WIFI=$(getcfg wifi)
if [ "$WIFI" = "1" ] || [ -f "$TORTOS_DIR/.devwifi" ]; then
	wifi_on &
else
	radio_off &
fi

# Bluetooth off, always.
killall -q bluealsa bluetoothd 2> /dev/null
/etc/init.d/bluetooth stop 2> /dev/null
rfkill block bluetooth 2> /dev/null
echo 0 > /sys/class/rfkill/rfkill0/state 2> /dev/null

# CPU: interactive scaling
echo interactive > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null

# EVERY button and the d-pad arrive through the stock GPIO input daemon's
# virtual joystick at /dev/input/event3. Without this daemon there is no d-pad
# and no face buttons at all -- only volume and power, which come from real
# kernel devices at event0/event1. It is the single line the whole control
# scheme depends on.
pgrep trimui_inputd > /dev/null || trimui_inputd &

# LEDs off again: trimui_inputd re-enables them when it starts.
sleep 1
leds_off

# One log per boot, carrying the launcher AND everything it starts. Without
# this a failure inside a game goes to a console nobody reads.
LOG=$LOGS_PATH/tortos.log
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
: > "$LOG"

# The resident emulator. It holds the GL context and all three cores between
# games, which takes a launch from ~1100ms to ~200ms. Started here so its ~1s
# of setup happens during the boot animation alongside the launcher's own --
# and so the clearing it does while creating its context is hidden under the
# animation rather than flashing over the launcher.
#
# Nothing depends on it: the launcher checks for its fifos and runs a game the
# old way, one process per game, when they are not there. That is what happens
# for a launch in the first second after boot, and if this ever dies.
# The resident emulator is Diatom. It preloads nothing: a core is mapped the
# first time a game needs it and kept for the life of the process, so there
# is no core list to hand over and nothing here changes when a system is
# added. The fallback for a resident that dies mid-session is the same
# binary run standalone by the launcher - one emulator, held two ways.
export TORTOS_DIATOM_SOCKET=/tmp/diatom.sock
start_resident() {
	pgrep -f "TortOS/diatom --socket" > /dev/null && return
	rm -f "$TORTOS_DIATOM_SOCKET"
	LD_LIBRARY_PATH=/usr/trimui/lib \
		"$TORTOS_DIR/diatom" --socket "$TORTOS_DIATOM_SOCKET" \
		--save "$SDCARD/Saves" --system "$SDCARD/Bios" >> "$LOG" 2>&1 &
	echo $! > /tmp/diatom.pid
}
start_resident

rm -f /tmp/tortos_poweroff

# Restart loop: only ever exits for a power-off.
cd "$TORTOS_DIR"
FAILS=0
while : ; do
	leds_off
	start_resident          # bring it back if it died
	START=$(cut -d. -f1 /proc/uptime)
	./tortos.elf >> "$LOG" 2>&1
	[ -f /tmp/tortos_poweroff ] && break
	END=$(cut -d. -f1 /proc/uptime)
	# A launcher that dies immediately, five times running, is not going to
	# start on the sixth. Stop rather than strobe.
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
sleep 10
