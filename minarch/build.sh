#!/bin/sh
# Build PlayOS's minarch from NextUI source with PlayOS's overrides applied.
#
# minarch is the libretro host PlayOS hands a game to. We do not fork NextUI:
# this copies a pristine workspace, overlays only the files in overrides/, and
# builds in the tg5040 toolchain container. Output: vendor/minarch.elf.
#
# minarch.elf is GPL-3.0 (NextUI); PlayOS's own code is 0BSD. See
# minarch/overrides/README.md and THIRD-PARTY-LICENSES.md.
#
# Usage: minarch/build.sh
#   NEXTUI_SRC=/path/to/NextUI/workspace   (default ~/Projects/NextUI/workspace)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NEXTUI_SRC=${NEXTUI_SRC:-$HOME/Projects/NextUI/workspace}
IMAGE=ghcr.io/loveretro/tg5040-toolchain:latest
BUILD=${PLAYOS_MINARCH_BUILD:-/tmp/playos-minarch/workspace}

[ -d "$NEXTUI_SRC/all/minarch" ] || { echo "NextUI workspace not found at $NEXTUI_SRC"; exit 1; }

echo "copying workspace -> $BUILD"
rm -rf "$BUILD"; mkdir -p "$BUILD"
cp -R "$NEXTUI_SRC/." "$BUILD/"

echo "applying PlayOS overrides"
( cd "$ROOT/minarch/overrides" && cp -R . "$BUILD/" )

echo "building in container"
docker run --rm -v "$BUILD":/root/workspace "$IMAGE" /bin/bash -c '
	set -e
	source ~/.bashrc 2>/dev/null || true
	cd /root/workspace/tg5040/libmsettings && make
	cd /root/workspace/all/minarch && make PLATFORM=tg5040
'

OUT="$BUILD/all/minarch/build/tg5040/minarch.elf"
[ -f "$OUT" ] || { echo "build failed: no minarch.elf"; exit 1; }
mkdir -p "$ROOT/vendor"
cp "$OUT" "$ROOT/vendor/minarch.elf"

# A stale binary that deploys silently costs more time than any bug. Prove the
# thing that was just built is the thing that was just written.
if ! strings "$ROOT/vendor/minarch.elf" | grep -q -- "--resident"; then
	echo "build produced a minarch.elf without PlayOS's resident mode -- refusing it"
	exit 1
fi
echo "vendor/minarch.elf updated ($(wc -c < "$ROOT/vendor/minarch.elf") bytes)"
