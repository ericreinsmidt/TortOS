#!/bin/sh
# PlayOS boot entry. Called from .tmp_update/tg5040.sh; never returns.
#
# The order of things in here is the boot time. The animation runs in the
# background and everything else -- the launcher's whole startup and the
# resident emulator's -- happens behind it, so the animation costs its length
# and nothing else.

PLAYOS_DIR=/mnt/SDCARD/PlayOS
SDCARD=/mnt/SDCARD

export PLATFORM=tg5040
export DEVICE=brick
export SDCARD_PATH=$SDCARD
export BIOS_PATH=$SDCARD/Bios
export ROMS_PATH=$SDCARD/Roms
export SAVES_PATH=$SDCARD/Saves
export CHEATS_PATH=$SDCARD/Cheats
export SYSTEM_PATH=$PLAYOS_DIR
export CORES_PATH=$PLAYOS_DIR/cores
export USERDATA_PATH=$SDCARD/.userdata/tg5040
export SHARED_USERDATA_PATH=$SDCARD/.userdata/shared
export LOGS_PATH=$USERDATA_PATH/logs
export HOME=$USERDATA_PATH
export LD_LIBRARY_PATH=/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=/usr/trimui/bin:$PATH

# Cleared by the boot animation when it finishes. The launcher does all of its
# startup while the animation plays, then blocks on this and presents its first
# frame the moment the animation clears it.
PLAYOS_ANIM_FLAG=/tmp/playos_bootanim
export PLAYOS_ANIM_FLAG

mkdir -p "$BIOS_PATH" "$ROMS_PATH" "$SAVES_PATH" "$USERDATA_PATH" "$LOGS_PATH" \
         "$SHARED_USERDATA_PATH"

CFG=$PLAYOS_DIR/playos.cfg
getcfg() { [ -f "$CFG" ] && sed -n "s/^$1=//p" "$CFG" | tail -1; }

# All LEDs off: PlayOS shows no chrome, and the lights are pure battery drain.
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
# everything after it. playos.elf has not started yet and cannot do it.
#
# This ladder is PlayOS's own and is shared verbatim with platform.c and with
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
[ -x "$PLAYOS_DIR/setbright" ] && "$PLAYOS_DIR/setbright" "$(brightness_raw)"
leds_off

# One-time: replace the stock u-boot splash with a black frame, so the handoff
# into the boot animation is seamless rather than a TrimUI logo followed by a
# cut. Guarded by a marker; the stock logo is backed up first.
if [ -f "$PLAYOS_DIR/bootlogo.bmp" ] && [ ! -f "$PLAYOS_DIR/.bootlogo_applied" ]; then
	mkdir -p /mnt/boot
	if mount -t vfat /dev/mmcblk0p1 /mnt/boot 2> /dev/null; then
		if [ -f /mnt/boot/bootlogo.bmp ] && [ ! -f "$PLAYOS_DIR/bootlogo.stock.bmp" ]; then
			cp /mnt/boot/bootlogo.bmp "$PLAYOS_DIR/bootlogo.stock.bmp"
		fi
		cp "$PLAYOS_DIR/bootlogo.bmp" /mnt/boot/bootlogo.bmp && sync
		umount /mnt/boot && touch "$PLAYOS_DIR/.bootlogo_applied"
	fi
fi

# One-time: the stock "loading" splash that pic2fb blits from /etc/splash.png.
# Same black frame, same reason.
if [ -f "$PLAYOS_DIR/splash.png" ] && [ ! -f "$PLAYOS_DIR/.splash_applied" ]; then
	if [ -f /etc/splash.png ] && [ ! -f "$PLAYOS_DIR/splash.stock.png" ]; then
		cp /etc/splash.png "$PLAYOS_DIR/splash.stock.png"
	fi
	cp "$PLAYOS_DIR/splash.png" /etc/splash.png && sync && touch "$PLAYOS_DIR/.splash_applied"
fi

# One-time: pic2fb blits that splash at the hardware default brightness, which
# is not ours, so the screen visibly steps partway through the boot. Call the
# brightness helper before it. Reversible: the stock init is backed up.
if [ -x "$PLAYOS_DIR/setbright" ] && [ ! -f "$PLAYOS_DIR/.brightboot_applied" ]; then
	cp "$PLAYOS_DIR/setbright" /usr/trimui/bin/setbright 2> /dev/null && chmod +x /usr/trimui/bin/setbright
	cat > /usr/trimui/bin/playos-bootbright.sh <<'BB'
#!/bin/sh
BR=$(sed -n 's/^brightness=//p' /mnt/SDCARD/PlayOS/playos.cfg 2> /dev/null | tail -1)
case "$BR" in
	0) R=1;; 1) R=8;; 2) R=16;; 3) R=32;; 4) R=48;; 5) R=72;;
	6) R=96;; 7) R=128;; 8) R=160;; 9) R=192;; 10) R=255;; *) R=160;;
esac
[ -x /usr/trimui/bin/setbright ] && /usr/trimui/bin/setbright "$R"
BB
	chmod +x /usr/trimui/bin/playos-bootbright.sh
	if [ -f /etc/init.d/runtrimui ] && ! grep -q playos-bootbright /etc/init.d/runtrimui; then
		cp /etc/init.d/runtrimui /etc/init.d/runtrimui.playos-bak
		awk '/pic2fb/ && !d {print "/usr/trimui/bin/playos-bootbright.sh"; d=1} {print}' \
			/etc/init.d/runtrimui.playos-bak > /etc/init.d/runtrimui
		sh -n /etc/init.d/runtrimui 2> /dev/null || cp /etc/init.d/runtrimui.playos-bak /etc/init.d/runtrimui
		chmod +x /etc/init.d/runtrimui
	fi
	sync
	touch "$PLAYOS_DIR/.brightboot_applied"
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
if [ -f "$PLAYOS_DIR/tortos-boot.mp4" ]; then
	: > "$PLAYOS_ANIM_FLAG"
	(
		ffmpeg -hide_banner -loglevel quiet -re -i "$PLAYOS_DIR/tortos-boot.mp4" \
		       -pix_fmt bgra -f fbdev /dev/fb0 2> /dev/null
		rm -f "$PLAYOS_ANIM_FLAG"
	) &

	# Free seconds: pull what the first launch needs off the card and into the
	# page cache while nothing else is using the disk. Cold reads of the
	# emulator, the cores and their libraries measure ~190ms against ~30ms warm.
	(
		cat "$PLAYOS_DIR/diatom" "$PLAYOS_DIR/cores/"*.so > /dev/null 2>&1
	) &
fi

# Rumble off, mute-switch gpio readable
echo 227 > /sys/class/gpio/export 2> /dev/null
echo -n out > /sys/class/gpio/gpio227/direction 2> /dev/null
echo -n 0 > /sys/class/gpio/gpio227/value 2> /dev/null

# Radio silence. PlayOS has nothing to talk to: no downloads, no pairing, no
# achievements. Both radios are battery drain and boot time.
#
# `wifi=1` in playos.cfg -- or a .devwifi marker -- keeps WiFi up so a
# development unit stays reachable over ssh. Without it there is no way to
# diagnose a problem on hardware except by pulling the card, which is a bad
# place to be when something does not come up.
#
# Stop the supplicant and drop the interface rather than rfkill-blocking, so
# the radio is left in a state the firmware understands.
WIFI=$(getcfg wifi)
if [ "$WIFI" = "1" ] || [ -f "$PLAYOS_DIR/.devwifi" ]; then
	sh /etc/wifi/wifi_init.sh start > /dev/null 2>&1 &
else
	/etc/init.d/wpa_supplicant stop 2> /dev/null
	killall -q udhcpc wpa_supplicant 2> /dev/null
	ifconfig wlan0 down 2> /dev/null
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
LOG=$LOGS_PATH/playos.log
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
export PLAYOS_DIATOM_SOCKET=/tmp/diatom.sock
start_resident() {
	pgrep -f "PlayOS/diatom --socket" > /dev/null && return
	rm -f "$PLAYOS_DIATOM_SOCKET"
	LD_LIBRARY_PATH=/usr/trimui/lib \
		"$PLAYOS_DIR/diatom" --socket "$PLAYOS_DIATOM_SOCKET" \
		--save "$SDCARD/Saves" --system "$SDCARD/Bios" >> "$LOG" 2>&1 &
	echo $! > /tmp/diatom.pid
}
start_resident

rm -f /tmp/playos_poweroff

# Restart loop: only ever exits for a power-off.
cd "$PLAYOS_DIR"
FAILS=0
while : ; do
	leds_off
	start_resident          # bring it back if it died
	START=$(cut -d. -f1 /proc/uptime)
	./playos.elf >> "$LOG" 2>&1
	[ -f /tmp/playos_poweroff ] && break
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
