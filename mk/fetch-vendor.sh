#!/bin/sh
# Populate vendor/ with the prebuilt tg5040 runtime PlayOS ships alongside its
# own binary: the three libretro cores, minarch's runtime libraries, and the
# handful of NextUI assets minarch resolves from compile-time paths.
#
# minarch.elf itself is NOT taken from here -- it is built from source by
# minarch/build.sh, because PlayOS's whole launch story is in the patches.
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

# minarch resolves /.system/res (its fonts and glyph sheet) and
# /.system/tg5040/shaders at COMPILE time and crashes at startup without them;
# governor.sh it calls by name out of $SYSTEM_PATH/bin.
mkdir -p "$VENDOR/system/res" "$VENDOR/system/tg5040/shaders" "$VENDOR/bin"
unzip -o -j "$DL/MinUI.zip" '.system/res/*' -d "$VENDOR/system/res" > /dev/null
unzip -o -j "$DL/MinUI.zip" '.system/tg5040/shaders/*' -d "$VENDOR/system/tg5040/shaders" > /dev/null
unzip -o -j "$DL/MinUI.zip" .system/tg5040/bin/governor.sh -d "$VENDOR/bin" > /dev/null

echo "vendor/ ready:"
ls "$VENDOR/cores" "$VENDOR/lib"
