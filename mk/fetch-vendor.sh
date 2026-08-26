#!/bin/sh
# Populate vendor/ with the prebuilt tg5040 runtime PlayOS ships alongside its
# own binary: the libretro cores and the device runtime libraries.
#
# The emulator itself is Diatom, built in its own repository; payload.sh takes
# the binary from DIATOM_ELF (default ../diatom/build/brick/diatom).
#
# Everything this pulls keeps its own license; see THIRD-PARTY-LICENSES.md.
#
# Usage: mk/fetch-vendor.sh [download-dir]
# Reuses NextUI-*-base.zip / NextUI-*-extras.zip in download-dir if present,
# otherwise downloads them with gh from LoveRetro/NextUI.
set -e
REL=v6.11.2
DL=${1:-/tmp/playos-vendor}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VENDOR=$ROOT/vendor

mkdir -p "$DL" "$VENDOR/cores" "$VENDOR/lib"

cd "$DL"
BASE=$(ls NextUI-*-base.zip 2> /dev/null | head -1 || true)
EXTRAS=$(ls NextUI-*-extras.zip 2> /dev/null | head -1 || true)
if [ -z "$BASE" ]; then
	gh release download $REL -R LoveRetro/NextUI -p 'NextUI-*-base.zip'
	BASE=$(ls NextUI-*-base.zip | head -1)
fi
if [ -z "$EXTRAS" ]; then
	gh release download $REL -R LoveRetro/NextUI -p 'NextUI-*-extras.zip'
	EXTRAS=$(ls NextUI-*-extras.zip | head -1)
fi

# the base zip nests the system payload as MinUI.zip
unzip -o -j "$BASE" MinUI.zip -d "$DL" > /dev/null

unzip -o -j "$DL/MinUI.zip" '.system/tg5040/lib/*' -d "$VENDOR/lib" > /dev/null
unzip -o -j "$DL/MinUI.zip" .system/tg5040/cores/fceumm_libretro.so \
	-d "$VENDOR/cores" > /dev/null
unzip -o -j "$EXTRAS" \
	Emus/tg5040/MGBA.pak/mgba_libretro.so \
	Emus/tg5040/PCE.pak/mednafen_pce_fast_libretro.so \
	-d "$VENDOR/cores" > /dev/null

# The SNES and Sega cores come from libretro's own buildbot rather than the
# NextUI release, pinned by sha256 - the same hashes Diatom's CORES.md pins,
# because they are the same verified binaries. The buildbot path is unpinned;
# the hashes are the pin.
BB=https://buildbot.libretro.com/nightly/linux/aarch64/latest
fetch_core() { # name sha256
	[ -f "$VENDOR/cores/${1}_libretro.so" ] && \
		[ "$(shasum -a 256 "$VENDOR/cores/${1}_libretro.so" | cut -d' ' -f1)" = "$2" ] && return
	curl -sSfL -o "$DL/$1.zip" "$BB/${1}_libretro.so.zip"
	unzip -o -j "$DL/$1.zip" -d "$VENDOR/cores" > /dev/null
	GOT=$(shasum -a 256 "$VENDOR/cores/${1}_libretro.so" | cut -d' ' -f1)
	[ "$GOT" = "$2" ] || { echo "$1: hash mismatch: $GOT"; exit 1; }
}
fetch_core snes9x2010      3933890f520abb9dbb0e5276460785b20ce54d25f552b369cafeca270b9dd44c
fetch_core genesis_plus_gx 3673a22b906509461e23a5a118b1d1bec15cbda105f260cbcbc08a16b2124e48

echo "vendor/ ready:"
ls "$VENDOR/cores" "$VENDOR/lib"
