#!/bin/sh
# Assemble TortOS's card payload: under out/sd/ for the TrimUI Brick, or
# out/sd-pixel2/ with DEVICE=pixel2 for the GKD Pixel 2. plastron, the system
# on both, builds it into its card image and copies it onto the card on first
# boot, so its build sets where the binaries are and where the payload goes:
#
#   DEVICE=brick TORTOS_ELF=... MUSE_ELF=... DIATOM_ELF=... VENDOR_DIR=... \
#       BTPLAYER_ELF=... OUT=... mk/payload.sh
#
# BTPLAYER_ELF only for a device with Bluetooth: tools/btplayer.c, which
# plastron's /etc/init.d/bluetooth runs from the card.
#
# TrimUI's own firmware is no longer supported, so there is one Brick payload,
# plastron's. NATIVE=1, which used to choose it, is accepted and ignored. And
# no release zip: it was the Brick's install for TrimUI's firmware, and both
# devices now install from plastron's card image. ZIP= is ignored too.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEVICE=${DEVICE:-brick}
case $DEVICE in
brick)  OUT=${OUT:-$ROOT/out/sd};        LAUNCH=$ROOT/sd/brick/launch.sh ;;
pixel2) OUT=${OUT:-$ROOT/out/sd-pixel2}; LAUNCH=$ROOT/sd/pixel2/launch.sh ;;
*) echo "payload: unknown DEVICE $DEVICE (brick or pixel2)" >&2; exit 1 ;;
esac
P=$OUT/TortOS
TORTOS_ELF=${TORTOS_ELF:-$ROOT/build/tortos.elf}
MUSE_ELF=${MUSE_ELF:-$ROOT/build/muse}
VENDOR_DIR=${VENDOR_DIR:-$ROOT/vendor}

[ -f "$TORTOS_ELF" ] || { echo "run make first"; exit 1; }
DIATOM_ELF=${DIATOM_ELF:-$ROOT/../diatom/build/$DEVICE/diatom}
[ -f "$DIATOM_ELF" ] || { echo "no diatom at $DIATOM_ELF (set DIATOM_ELF)"; exit 1; }
[ -f "$VENDOR_DIR/cores/fceumm_libretro.so" ] || { echo "run mk/fetch-vendor.sh first"; exit 1; }

rm -rf "$OUT"
mkdir -p "$P/cards" "$P/cores" "$P/res/web" \
         "$OUT/Roms" "$OUT/Music" "$OUT/Audiobooks" "$OUT/Bios" "$OUT/Saves"

cp "$TORTOS_ELF" "$P/tortos.elf"
cp "$MUSE_ELF" "$P/muse"                  # the audio player's engine; Muse cannot play without it
[ -n "$BTPLAYER_ELF" ] && cp "$BTPLAYER_ELF" "$P/btplayer"   # a headset's keys and volume
cp "$LAUNCH" "$P/launch.sh"
# Which TortOS this is, for plastron: the Pixel's image copies its TortOS/ over
# the card's only when it is newer than the card's (/etc/init.d/tortos), so a
# system-only update brings the new TortOS with it. Only when the build says:
# a payload built without a version has no file, and is never copied over one.
if [ -n "${VERSION:-}" ]; then
	echo "$VERSION" > "$P/VERSION"
fi
# Only systems.cfg is shipped now. tortos.cfg, turbo.cfg and coreopts.cfg are
# compiled into the launcher and seed the settings database on first run, so
# there is no file to ship and none to drift from the code that reads it.
#
# systems.cfg stays a file because it is BUILD input, not a setting: the check
# below reads it to refuse a card whose cores are missing, and the loop further
# down reads it to create the ROM folders. Both run on the host, before any
# database exists.
cp "$ROOT/config/systems.cfg" "$P/"
cp -R "$ROOT/res/cards/." "$P/cards/"     # the classic/ and fancy/ sets
cp "$ROOT/res/fonts/menu.ttf" "$P/"       # the UI face, and the in-game menu's
cp "$ROOT/res/singles.png" "$P/"          # Muse's cover for Singles, the loose songs
# Over The Hare's page. The launcher serves these off the card at P_WEB, so a
# payload without them is a card whose transfer screen starts a server and then
# answers its own page with a 404, which only shows on a built card - the
# reason to build one before calling it a release. The font goes in twice
# rather than being kept in the repo twice: the launcher reads $P/menu.ttf and
# a browser asks for /web/menu.ttf.
cp "$ROOT/res/web/"* "$P/res/web/"
cp "$ROOT/res/fonts/menu.ttf" "$P/res/web/menu.ttf"
# The Pixel's files go over its USB cable, so its page carries the cable name,
# Down To The Wire (src/sys_menu.h). Only the page's own words: the browser
# code names nothing, and the server is the same.
if [ "$DEVICE" = pixel2 ]; then
	sed 's/Over The Hare/Down To The Wire/g' "$ROOT/res/web/index.html" > "$P/res/web/index.html"
fi
# The device has curl and OpenSSL but nothing to trust - see res/ssl/README.md.
# Without this, every HTTPS request fails verification and achievements never
# arrive, with an error that reads like the network being down.
cp "$ROOT/res/ssl/cacert.pem" "$P/"
# The notices for what the card carries that isn't TortOS's, in a Licenses
# folder of its own, where plastron puts its notices and Diatom's beside it
mkdir -p "$OUT/Licenses"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$OUT/Licenses/TortOS.md"
cp "$DIATOM_ELF" "$P/diatom"
cp "$VENDOR_DIR/cores/"*.so "$P/cores/"

# Every system on the shelf must have its core on the card.
#
# The copy above is a glob: it ships whatever vendor/cores happens to hold and
# says nothing about what is missing. That is how a card was nearly built with
# four of nine systems dead - systems.cfg gained SNES and the Sega machines,
# vendor/ still held the original three, and nothing failed. A shelf whose
# cards open onto nothing is a worse failure than a build that refuses.
missing=
for core in $(awk -F'|' '$1=="sys"{gsub(/^[ \t]+|[ \t]+$/,"",$4); print $4}' \
              "$ROOT/config/systems.cfg" | sort -u); do
	[ -f "$P/cores/${core}_libretro.so" ] || missing="$missing $core"
done
if [ -n "$missing" ]; then
	echo "payload: systems.cfg needs cores that vendor/ does not have:$missing" >&2
	echo "payload: run mk/fetch-vendor.sh" >&2
	exit 1
fi

chmod +x "$P/launch.sh" "$P/tortos.elf" "$P/diatom" "$P/muse"
[ -f "$P/btplayer" ] && chmod +x "$P/btplayer"

# The headsets' ALSA config: the Brick's launch.sh sources it at boot, and
# TortOS runs it after a pair or a forget.
[ "$DEVICE" = brick ] && cp "$ROOT/sd/tortos/bt-alsa.sh" "$P/"

# One ROM folder per system, with the .media folder box art goes in. Read from
# systems.cfg (awk, not sed: folder names contain spaces).
awk -F'|' '$1=="sys"{gsub(/^[ \t]+|[ \t]+$/,"",$3); print $3}' "$ROOT/config/systems.cfg" |
while IFS= read -r folder; do
	mkdir -p "$OUT/Roms/$folder/.media"
done
# Bios/ is created above and stays FLAT. It is handed to the core as its system
# directory and a core asks for a filename inside it - mgba wants gba_bios.bin,
# mednafen wants syscard3.pce - so a folder per system is a place a BIOS goes to
# be ignored. This line used to read `mkdir -p "$OUT/Bios/GBA"`, directly under a
# comment saying not to litter the card with empty Bios folders, and it shipped
# in v1.0: an empty GBA/ that contradicted the README two directories away.
# Nothing errors when a BIOS lands in it. mgba falls back to its built-in one and
# the only symptom is the boot animation you were trying to enable not appearing.

du -sh "$OUT"
echo "payload ready: $OUT"
