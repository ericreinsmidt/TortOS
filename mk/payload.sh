#!/bin/sh
# Assemble the installable SD payload under out/sd/.
#
# Copy the CONTENTS of out/sd/ to the root of a FAT32 SD card, put it in a
# stock Brick and power on: the first boot installs the runtrimui.sh hook and
# every boot after that comes straight up in PlayOS.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out/sd
P=$OUT/PlayOS

[ -f "$ROOT/build/playos.elf" ] || { echo "run make first"; exit 1; }
DIATOM_ELF=${DIATOM_ELF:-$ROOT/../diatom/build/brick/diatom}
[ -f "$DIATOM_ELF" ] || { echo "no diatom at $DIATOM_ELF (set DIATOM_ELF)"; exit 1; }
[ -f "$ROOT/vendor/cores/fceumm_libretro.so" ] || { echo "run mk/fetch-vendor.sh first"; exit 1; }

rm -rf "$OUT"
mkdir -p "$P/cards" "$P/cores" "$P/lib" \
         "$OUT/.tmp_update" "$OUT/trimui/app" \
         "$OUT/Roms" "$OUT/Bios" "$OUT/Saves"

cp "$ROOT/build/playos.elf" "$P/"
cp "$ROOT/build/setbright" "$P/"          # brightness before the boot animation
cp "$ROOT/sd/playos/launch.sh" "$P/"
cp "$ROOT/config/systems.cfg" "$ROOT/config/playos.cfg" "$P/"
cp "$ROOT/res/cards/"*.png "$P/cards/"
cp "$ROOT/res/fonts/menu.ttf" "$P/"       # the UI face, and the in-game menu's
cp "$ROOT/res/boot/playos-boot.mp4" "$P/"
cp "$ROOT/res/boot/bootlogo.bmp" "$P/"    # u-boot splash, applied on first boot
cp "$ROOT/res/boot/splash.png" "$P/"      # the pic2fb loading splash, likewise
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$P/"  # notices for the redistributed software
cp "$DIATOM_ELF" "$P/diatom"
cp "$ROOT/vendor/cores/"*.so "$P/cores/"
cp "$ROOT/vendor/lib/"* "$P/lib/"

cp "$ROOT/sd/.tmp_update/updater" "$ROOT/sd/.tmp_update/tg5040.sh" "$OUT/.tmp_update/"
cp "$ROOT/sd/trimui/app/MainUI" "$ROOT/sd/trimui/app/runtrimui.sh" "$OUT/trimui/app/"

chmod +x "$OUT/.tmp_update/updater" "$OUT/.tmp_update/tg5040.sh" \
         "$OUT/trimui/app/MainUI" "$OUT/trimui/app/runtrimui.sh" \
         "$P/launch.sh" "$P/playos.elf" "$P/diatom" "$P/setbright"

# One ROM folder per system, with the .media folder box art goes in. Read from
# systems.cfg (awk, not sed: folder names contain spaces).
awk -F'|' '$1=="sys"{gsub(/^[ \t]+|[ \t]+$/,"",$3); print $3}' "$ROOT/config/systems.cfg" |
while IFS= read -r folder; do
	mkdir -p "$OUT/Roms/$folder/.media"
done
# Only mgba can use a BIOS, and only optionally; the other two never do, so
# there is no reason to litter the card with empty Bios folders.
mkdir -p "$OUT/Bios/GBA"

du -sh "$OUT"
echo "payload ready: $OUT"

VERSION=${VERSION:-1.0}
ZIP="$ROOT/out/PlayOS-v$VERSION.zip"
rm -f "$ZIP"
( cd "$OUT" && zip -qr "$ZIP" . -x '.DS_Store' '._*' )
echo "release zip:  $ZIP  ($(du -h "$ZIP" | cut -f1))"
