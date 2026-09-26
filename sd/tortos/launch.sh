#!/bin/sh
# TortOS boot entry. Called from .tmp_update/tg3040.sh; never returns.
#
# The order of things in here is the boot time. The animation runs in the
# background and everything else -- the launcher's whole startup and the
# resident emulator's -- happens behind it, so the animation costs its length
# and nothing else.

TORTOS_DIR=/mnt/SDCARD/TortOS
SDCARD=/mnt/SDCARD

export PLATFORM=tg3040
export DEVICE=brick
export SDCARD_PATH=$SDCARD
export BIOS_PATH=$SDCARD/Bios
export ROMS_PATH=$SDCARD/Roms
export SAVES_PATH=$SDCARD/Saves
export CHEATS_PATH=$SDCARD/Cheats
export SYSTEM_PATH=$TORTOS_DIR
export CORES_PATH=$TORTOS_DIR/cores
export USERDATA_PATH=$SDCARD/.userdata/tg3040
export SHARED_USERDATA_PATH=$SDCARD/.userdata/shared
export LOGS_PATH=$USERDATA_PATH/logs
export HOME=$USERDATA_PATH
# .asoundrc named as a top-level ALSA config file, and not only reached through
# alsa.conf's @hooks load. alsa-lib 1.1.8 re-reads its config at the next open
# when a file in this list changes, and it never checks files a hook loaded -
# so a headset paired after Diatom or Muse started, whose PCM bt_write_asoundrc
# has just added, was `Unknown PCM` to both until they restarted. Measured
# 2026-09-25 with a probe that opens once, waits, and opens a PCM added in
# between: not found without this, found with it, and a missing or empty
# .asoundrc at start is fine either way.
export ALSA_CONFIG_PATH=/usr/share/alsa/alsa.conf:$HOME/.asoundrc
export LD_LIBRARY_PATH=/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=/usr/trimui/bin:$PATH

# Cleared by the boot animation when it finishes. The launcher does all of its
# startup while the animation plays, then blocks on this and presents its first
# frame the moment the animation clears it.
TORTOS_ANIM_FLAG=/tmp/tortos_bootanim
export TORTOS_ANIM_FLAG

mkdir -p "$BIOS_PATH" "$ROMS_PATH" "$SAVES_PATH" "$USERDATA_PATH" "$LOGS_PATH" \
         "$SHARED_USERDATA_PATH"

# Settings live in a database now, and this script cannot read one: there is no
# sqlite3 binary on the device. So the launcher exports the five values needed
# before it exists, and this sources them. One read, no forks - it replaces six
# seds across four files, so the boot path got shorter rather than longer.
#
# The defaults below are for the very first boot of a fresh card, before the
# launcher has ever run. After that boot.env is rewritten on every start and
# whenever one of these changes.
#
# Sourced rather than parsed because parsing costs a fork per value. That trusts
# the file, which is the same trust this script already places in itself: both
# are on the card, and anyone who can edit one can edit the other.
BRIGHTNESS=7
WIFI=0
BLUETOOTH=0
TIMEZONE=America/New_York
BOOT_ENV=$USERDATA_PATH/boot.env
[ -f "$BOOT_ENV" ] && . "$BOOT_ENV"

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
# The level the player last chose, which the launcher already resolved against
# the shipped default before exporting it - so there is one value here now
# rather than a saved level and a fallback.
brightness_raw() {
	B=$BRIGHTNESS
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
		# Ask the image on the boot partition what it is, rather than asking a
		# marker on the card.
		#
		# The marker is the card's, the image is the device's, and a new card
		# means a missing marker over an already-applied image. Reinstalling on
		# a freshly formatted card then "backed up the stock logo" - which by
		# then was OUR logo - straight over the real one, and the genuine stock
		# image was gone for good on a device that had been reinstalled once.
		# Found by doing exactly that, 2026-08-30.
		if cmp -s /mnt/boot/bootlogo.bmp "$TORTOS_DIR/bootlogo.bmp"; then
			touch "$TORTOS_DIR/.bootlogo_applied"      # already ours
		else
			if [ -f /mnt/boot/bootlogo.bmp ] && [ ! -f "$TORTOS_DIR/bootlogo.stock.bmp" ]; then
				cp /mnt/boot/bootlogo.bmp "$TORTOS_DIR/bootlogo.stock.bmp"
			fi
			cp "$TORTOS_DIR/bootlogo.bmp" /mnt/boot/bootlogo.bmp && sync
			touch "$TORTOS_DIR/.bootlogo_applied"
		fi
		umount /mnt/boot
	fi
fi

# One-time: the stock "loading" splash that pic2fb blits from /etc/splash.png.
# Same black frame, same reason.
if [ -f "$TORTOS_DIR/splash.png" ] && [ ! -f "$TORTOS_DIR/.splash_applied" ]; then
	# Same trap as the bootlogo above, same answer: compare, do not assume.
	if cmp -s /etc/splash.png "$TORTOS_DIR/splash.png"; then
		touch "$TORTOS_DIR/.splash_applied"           # already ours
	else
		if [ -f /etc/splash.png ] && [ ! -f "$TORTOS_DIR/splash.stock.png" ]; then
			cp /etc/splash.png "$TORTOS_DIR/splash.stock.png"
		fi
		cp "$TORTOS_DIR/splash.png" /etc/splash.png && sync
		touch "$TORTOS_DIR/.splash_applied"
	fi
fi

# One-time: pic2fb blits that splash at the hardware default brightness, which
# is not ours, so the screen visibly steps partway through the boot. Call the
# brightness helper before it. Reversible: the stock init is backed up.
if [ -x "$TORTOS_DIR/setbright" ] && [ ! -f "$TORTOS_DIR/.brightboot_applied" ]; then
	cp "$TORTOS_DIR/setbright" /usr/trimui/bin/setbright 2> /dev/null && chmod +x /usr/trimui/bin/setbright
	cat > /usr/trimui/bin/tortos-bootbright.sh <<'BB'
#!/bin/sh
BR=$(sed -n "s/^BRIGHTNESS='\\(.*\\)'$/\\1/p" /mnt/SDCARD/.userdata/tg3040/boot.env 2> /dev/null | tail -1)
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
# builds its GL context and maps every core on the card.
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
	# page cache while nothing else is using the disk.
	#
	# The CORES are no longer read here. Diatom maps them itself at startup
	# with --cores, which is the same work for less: measured 2026-09-08,
	# reading all six through cost 480ms cold where mapping them costs 382ms,
	# because dlopen takes what it needs rather than every byte - and mapping
	# also pays the dynamic linker, which reading never did. A first launch
	# used to pay that: 179ms of cold dlopen for genesis_plus_gx alone.
	#
	# The emulator binary is still read here. Diatom cannot warm the file it
	# is about to be executed from.
	(
		cat "$TORTOS_DIR/diatom" > /dev/null 2>&1
	) &
fi

# Rumble off, mute-switch gpio readable
echo 227 > /sys/class/gpio/export 2> /dev/null
echo -n out > /sys/class/gpio/gpio227/direction 2> /dev/null
echo -n 0 > /sys/class/gpio/gpio227/value 2> /dev/null

# Timezone, before anything that writes a timestamp.
#
# The stock firmware points /tmp/localtime at Asia/Shanghai, which is eight
# hours out for anyone who did not buy the device there, and every date the
# launcher shows - the one under a save slot - is local time. The zoneinfo
# database is already on the device; this only chooses from it.
#
# The clock itself is not set here and does not need to be: /etc/rc.d/S98sysntpd
# runs ntpd, which corrects the time within a minute of Wi-Fi connecting. That
# is also why the device sat in 1970 until Wi-Fi worked - ntpd was running the
# whole time with nothing to reach.
TZNAME=$TIMEZONE
if [ -n "$TZNAME" ] && [ -f "/usr/share/zoneinfo/$TZNAME" ]; then
	ln -sf "/usr/share/zoneinfo/$TZNAME" /tmp/localtime
	rm -f /tmp/TZ
elif [ -n "$TZNAME" ]; then
	echo "tz: no zoneinfo for '$TZNAME', keeping the firmware default" \
	     >> "$LOGS_PATH/tortos.log"
fi

# Radio silence. TortOS has nothing to talk to yet: no downloads, no pairing,
# no achievements. Both radios are battery drain and boot time.
#
# Wi-Fi on in the settings -- or a .devwifi marker -- keeps WiFi up so a
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

# What THIS device was doing when it was last shut down, which is what someone
# who turned Wi-Fi on expects to find. The launcher resolved that against the
# shipped default before exporting, so $WIFI is already the answer.
if [ "$WIFI" = "1" ] || [ -f "$TORTOS_DIR/.devwifi" ]; then
	wifi_on &
else
	radio_off &
fi

# Bluetooth, on the same terms as WiFi above: off unless asked for, because
# both radios are battery drain and boot time, and the state the player left it
# in wins over the shipped default.
#
# The bring-up is the vendor's own, established on 2026-09-03. Three details in
# it are not guesses and should not be "simplified":
#
#   - The attach protocol is `xradio`. /etc/bluetooth/bt_init.sh on this rootfs
#     is the AIC variant and FAILS here ("bring up hci0 failed"; it also calls
#     hcidump_xr, which does not exist). /etc/init.d/hciattach has the right
#     invocation and is what this mirrors.
#   - rfkill needs a POWER CYCLE, not an unblock. After a failed attach the chip
#     wedges: hciattach runs happily and no hci0 ever appears. Every bt_init.sh
#     variant cycles the rail; /etc/init.d/hciattach does not, which is why the
#     boot service has never once succeeded - it is enabled as S80 and fails
#     against the radio we block four lines further down.
#   - hfp-ag is ASSUMED to matter, not measured. Another firmware on this
#     device reports that with a2dp-source alone a headset connects but behaves
#     oddly, and that the Hands-Free gateway role makes it connect the way it
#     would to a phone. That is plausible and costs nothing, so it is here - but
#     nobody has A/B'd it, and pairing was fixed on 2026-09-03 by an unrelated
#     change (the agent capability), so hfp-ag has never been shown to be doing
#     anything. To settle it: run bluealsa with -p a2dp-source alone, reconnect
#     the headset, and listen for whether it speaks or only beeps.
#
#     --a2dp-volume IS load-bearing and is not an assumption: it leaves volume
#     with the headset, so a BT sink stays outside both the speaker and jack
#     ladders in src/platform.c and must not be attenuated here.
bt_off() {
	# Before anything else: no radio means no sink, and a stale file would
	# leave the launcher routing sound at a device that is gone. The port
	# would fall back and say so (ADR-0029), but a launcher showing the wrong
	# answer for twenty seconds is a thing to avoid, not to recover from.
	rm -f /tmp/tortos_btsink
	# btplayer too: a bluetoothd started again later has forgotten its
	# registration, so bt_on starts a fresh one.
	killall -q btplayer bluealsa bluetoothd hciattach 2> /dev/null
	/etc/init.d/bluetooth stop 2> /dev/null
	rfkill block bluetooth 2> /dev/null
	echo 0 > /sys/class/rfkill/rfkill0/state 2> /dev/null
}

# Not start-stop-daemon, which would lose its log line: this is launch.sh and
# not an adb session, so `&` outlives nothing it should not.
bt_player() {
	pidof btplayer > /dev/null && return 0
	"$TORTOS_DIR/btplayer" >> "$LOGS_PATH/tortos.log" 2>&1 &
}

bt_on() {
	# Power cycle rather than unblock. See above.
	echo 0 > /sys/class/rfkill/rfkill0/state 2> /dev/null
	sleep 2
	echo 1 > /sys/class/rfkill/rfkill0/state 2> /dev/null
	sleep 2
	rfkill unblock bluetooth 2> /dev/null

	# Retried, not attempted once. Measured 2026-09-03: the identical command
	# takes ~4s and succeeds on a settled device, and FAILS outright when run
	# about two seconds after the unblock during boot - the chip is not ready
	# that early and the attach dies silently. A longer fixed sleep would be a
	# guess at how long boot happens to take on this card with this library;
	# a retry costs nothing when it works first time.
	#
	# The stock service is stopped first because /etc/rc.d/S80hciattach is
	# procd-managed and has already run and failed by the time we get here.
	# Leaving procd holding a failed instance means two things reaching for the
	# same UART.
	/etc/init.d/hciattach stop > /dev/null 2>&1
	killall -q hciattach 2> /dev/null

	try=0
	while [ $try -lt 3 ]; do
		start-stop-daemon -S -b -x /usr/bin/hciattach -- -n ttyS1 xradio > /dev/null 2>&1
		i=0
		while [ $i -lt 10 ]; do
			[ -d /sys/class/bluetooth/hci0 ] && break
			sleep 1
			i=$((i + 1))
		done
		[ -d /sys/class/bluetooth/hci0 ] && break
		try=$((try + 1))
		echo "bt: attach attempt $try produced no hci0" >> "$LOGS_PATH/tortos.log"
		# A dead attach leaves the chip wedged; only a rail cycle clears it.
		killall -q hciattach 2> /dev/null
		echo 0 > /sys/class/rfkill/rfkill0/state 2> /dev/null
		sleep 2
		echo 1 > /sys/class/rfkill/rfkill0/state 2> /dev/null
		sleep 2
	done
	if [ ! -d /sys/class/bluetooth/hci0 ]; then
		echo "bt: gave up after $try attach attempts" >> "$LOGS_PATH/tortos.log"
		return 1
	fi

	hciconfig hci0 up 2> /dev/null
	/etc/bluetooth/bluetoothd start > /dev/null 2>&1
	start-stop-daemon -S -b -x /usr/bin/bluealsa -- \
		-S -p a2dp-source -p hfp-ag --a2dp-volume > /dev/null 2>&1
	sleep 3          # let both profiles register with BlueZ before any connect

	# A media player registered with BlueZ, which does nothing else: without
	# one, BlueZ 5.54 drops the volume a headset reports and the transport
	# never gets a Volume, so no headset's volume can be set from here. See
	# tools/btplayer.c. Before bt_reconnect, so it is registered by the time a
	# headset connects: the headset only reports its volume at that moment.
	bt_player

	bt_reconnect &
}

# The Bluetooth headsets' ALSA config, bt_pcm_name and bt_write_asoundrc, in a
# file of its own so the launcher runs the same code after a pair or a forget.
#
# Tested first, and the test is not decoration: `.` on a missing file ends a
# non-interactive shell, and this one ending powers the device off (see
# .tmp_update/updater). Without the file the boot goes on: .asoundrc is not
# rewritten, and a connected headset's name comes out empty, which the launcher
# reads as no sink - so game audio stays on the speaker.
[ -f "$TORTOS_DIR/bt-alsa.sh" ] && . "$TORTOS_DIR/bt-alsa.sh"

start_resident() {
	pgrep -f "TortOS/diatom --socket" > /dev/null && return
	rm -f "$TORTOS_DIATOM_SOCKET"
	LD_LIBRARY_PATH=/usr/trimui/lib \
		"$TORTOS_DIR/diatom" --socket "$TORTOS_DIATOM_SOCKET" \
		--cores "$CORES_PATH" \
		--save "$SDCARD/Saves" --system "$SDCARD/Bios" >> "$LOG" 2>&1 &
	echo $! > /tmp/diatom.pid
}

# Keep a remembered headset connected, so powering it on reconnects it wherever
# you are rather than only at a screen that happens to be watching.
#
# Trusted devices with a key only: BlueZ writes those to
# /etc/lib/bluetooth/<adapter>/, NOT to /etc/bluetooth/keys/ - that directory
# is a decoy created by the bluetoothd init wrapper's `ln -snf ...
# /var/lib/bluetooth`, and bluetoothd never reads it because its storage path
# is compiled in with --localstatedir=/etc.
#
# Judge success by `info`, never by the return of `connect`: bluetoothctl reports
# Failed for a2dp even when the link came up.
bt_reconnect() {
	adapter=$(hciconfig hci0 2> /dev/null | sed -n 's/.*BD Address: \([0-9A-F:]*\).*/\1/p')
	[ -n "$adapter" ] || return 0
	latest=
	prev_links=
	while :; do
		# Back if it died. A restarted player is attached to the sessions that
		# exist, but a headset already connected reports its volume again only
		# when it reconnects.
		bt_player
		connected=
		bonds=
		for d in /etc/lib/bluetooth/"$adapter"/*:*; do
			[ -d "$d" ] || continue
			grep -q '^Trusted=true' "$d/info" 2> /dev/null || continue
			# And a key on the card, or it is not a bond: a memory-only pairing
			# leaves Trusted behind with no key after a restart, and dialing
			# that every pass connected it for four seconds at a time and
			# published a sink each time. See bond_name in src/bt.c.
			grep -q '^\[LinkKey\]' "$d/info" 2> /dev/null || continue
			bonds="$bonds $(basename "$d")"
		done
		# Whoever is already connected first, and nobody dialed while one is.
		# In one walk, a headset switched off but earlier in address order was
		# dialed before the connected one was even looked at: `connect` takes
		# about five seconds to give up, so publishing the OpenFit waited six
		# or seven on the OpenRun - measured 2026-09-25 - and the dead one was
		# paged every pass for as long as the live one was in use.
		#
		# And with two connected, the one connected LAST, as a phone does. Until
		# 2026-09-26 it was the first in address order, so with the OpenRun on
		# the OpenFit could be connected from the screen and the sound stayed on
		# the OpenRun. The order is kept here, pass to pass: a bond connected now
		# and not last pass, or on a different link handle - a reconnect - is
		# the latest. Not the handle's value itself, which is reused (the OpenFit
		# came back on 128 once the OpenRun's 128 was free). A connect from the
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
		# is off takes about five seconds each, and one switched back on while
		# that went on was republished under a name that had never been taken
		# away - no change, so nothing was re-routed, and a song that had failed
		# over to the speaker stayed there. Seen 2026-09-25: the OpenFit off for
		# 26 seconds and the sink never withdrawn.
		[ -n "$connected" ] || rm -f /tmp/tortos_btsink
		for mac in $bonds; do
			[ -z "$connected" ] || break
			bluetoothctl connect "$mac" > /dev/null 2>&1
			# Judge by info, NEVER by the return. connect reports Failed for
			# a2dp even when the link came up - measured 2026-09-03, and
			# written in the backlog before this loop was, then used anyway.
			#
			# Trusting the return made this publish "no sink" while a headset
			# was connected, so the launcher fell back to the speaker, then
			# picked the sink up on the next pass and switched again. Every
			# flip reopens Diatom's audio device: 2026-09-05 that was nine
			# route changes and 407236 dropped audio frames in one game.
			sleep 1
			bluetoothctl info "$mac" 2> /dev/null | grep -q 'Connected: yes' &&
				connected=$mac
		done
		# Tell the launcher where the sound can go, if anywhere.
		#
		# A file rather than the launcher asking, because asking means forking
		# bluetoothctl out of a 119 MB process - the exact mistake menu_wifi
		# exists to prevent, measured at 12 fps with a menu open. This loop
		# already knows the answer, so it writes it and the launcher stats a
		# path. The ALSA device string is built here for the same reason: the
		# MAC is here and Diatom must never learn what kind of thing it names.
		if [ -n "$connected" ]; then
			# Publish the PCM NAME, not a bluealsa device string.
			#
			# Measured 2026-09-05 with diatom/tools/btaudio.c - the same SDL,
			# the same writes, the same headset:
			#
			#   AUDIODEV=bluealsa:DEV=..,PROFILE=a2dp   queue climbs to 385 kB
			#                                           and never drains; the
			#                                           close never returns
			#   a named PCM of type bluealsa            drains to 0; close
			#                                           returns in ~105 ms
			#
			# Three runs each way. The device string goes through the plugin's
			# own argument parser and yields a PCM that SDL opens and then never
			# writes to; the config form goes through ALSA's normal path and
			# works. Wrapping the string form in `plug` does not help.
			#
			# The config itself is written at boot by bt_write_asoundrc, which
			# explains why it is not written here.
			#
			# And the link's ACL handle on a second line, which is new on every
			# connection even when the name is not. A headset that dropped and came
			# back between two looks here keeps its name, so without this the
			# launcher could not tell it had ever gone, and whoever had fallen back
			# to the speaker while it was away was never sent back to it.
			link=$(hcitool con 2> /dev/null |
				sed -n "s/.*ACL $connected handle \([0-9]*\).*/\1/p" | head -1)
			printf '%s\n%s\n' "$(bt_pcm_name "$connected")" "$link" \
				> /tmp/tortos_btsink.tmp && mv /tmp/tortos_btsink.tmp /tmp/tortos_btsink
		else
			rm -f /tmp/tortos_btsink
		fi
		# Twenty seconds, or less when the Bluetooth screen asks: it touches
		# /tmp/tortos_btpass after a connect, a disconnect or a forget, so the
		# sink moves with the screen rather than up to a pass later. Waiting a
		# whole pass sent anything pressed in between to the speaker - seen
		# 2026-09-25, a song resumed right after a pairing. A file and not a
		# signal, because the launcher has no pid for this loop and a signal
		# to the wrong shell would be launch.sh itself.
		n=0
		while [ $n -lt 20 ] && [ ! -e /tmp/tortos_btpass ]; do
			sleep 1
			n=$((n + 1))
		done
		rm -f /tmp/tortos_btpass
	done
}

if [ "$BLUETOOTH" = "1" ]; then
	bt_on &
else
	bt_off &
fi

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

# The resident emulator. It holds the GL context and every core between
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
# BEFORE start_resident, and that order is the whole point: alsa-lib caches its
# config at the first PCM open, so a definition written afterwards is invisible
# to this process for as long as it lives.
bt_write_asoundrc
start_resident

rm -f /tmp/tortos_poweroff
# Nothing is CONNECTED yet; bt_reconnect writes this when something is. The
# .asoundrc is not removed with it - it describes bonds, which outlive any
# connection, and removing it here would undo the line above.

# AppleDouble litter, swept in the background.
#
# Copying to the card from a Mac leaves ._name beside every file, and the
# project's own install does the same: 76 of them from a 43-file payload on a
# freshly formatted card, measured 2026-09-02. Harmless - lib_scan skips every
# dot-prefixed name in all three of its readdir loops, so they never become
# phantom games - but they double the directory entries on a FAT card and
# "._Contra (USA).zip" sitting beside the real one is confusing to read.
#
# Backgrounded behind a sleep because it is housekeeping and not a
# precondition: the scan already ignores them, so nothing waits on this. A
# card-wide sweep measured 190ms against a boot of about 900, which is too
# much to spend on the critical path and nothing at all once it is off it. The
# sleep also keeps it clear of the library scan's own I/O.
#
# ONLY the ._ prefix. .media, .cheevos and .tortos are ours and the whole
# library hangs off them. busybox find has no -delete, and -exec is used
# rather than xargs because these names contain spaces.
( sleep 8
  find /mnt/SDCARD -name '._*' -exec rm -f {} \; ) >/dev/null 2>&1 &

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
