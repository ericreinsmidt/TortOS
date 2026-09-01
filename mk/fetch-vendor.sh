#!/bin/sh
# Populate vendor/cores with the libretro cores TortOS redistributes.
#
# That is the whole of it. TortOS carries no third-party runtime libraries;
# measurement says nothing needs them:
#
#   the cores need  libc libm librt libstdc++ libgcc_s ld-linux
#   tortos.elf needs libSDL2 libSDL2_image libSDL2_ttf libm libdl libc
#
# Every one of those ships in the device's own firmware under /usr/trimui/lib
# or /usr/lib. Settings are TortOS's own code in src/platform.c, against the
# device's ALSA control and display-engine interfaces.
#
# Cores come from libretro's own buildbot, pinned by sha256 - the same hashes
# Diatom's CORES.md verifies, because they are the same binaries. The
# buildbot path is unpinned; these hashes are the pin.
#
# Usage: mk/fetch-vendor.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VENDOR=$ROOT/vendor
DL=${1:-/tmp/tortos-vendor}
BB=https://buildbot.libretro.com/nightly/linux/aarch64/latest

command -v curl   > /dev/null || { echo "need curl" >&2; exit 1; }
command -v unzip  > /dev/null || { echo "need unzip" >&2; exit 1; }
command -v shasum > /dev/null || { echo "need shasum" >&2; exit 1; }

mkdir -p "$DL" "$VENDOR/cores"

fetch_core() { # name sha256
	if [ -f "$VENDOR/cores/${1}_libretro.so" ] &&
	   [ "$(shasum -a 256 "$VENDOR/cores/${1}_libretro.so" | cut -d' ' -f1)" = "$2" ]; then
		echo "  ok      $1"
		return
	fi
	curl -sSfL -o "$DL/$1.zip" "$BB/${1}_libretro.so.zip"
	unzip -o -j "$DL/$1.zip" -d "$VENDOR/cores" > /dev/null
	GOT=$(shasum -a 256 "$VENDOR/cores/${1}_libretro.so" | cut -d' ' -f1)
	if [ "$GOT" != "$2" ]; then
		echo "  MISMATCH $1" >&2
		echo "    want $2" >&2
		echo "    got  $GOT" >&2
		echo "  The buildbot republished this core. Verify it, then update the" >&2
		echo "  hash here and in Diatom's CORES.md together." >&2
		exit 1
	fi
	echo "  fetched $1"
}

fetch_core fceumm            1b13b00d4680394dad8000d5175f97be727107e0945bc9b412da91d70c07b267
fetch_core snes9x2010        3933890f520abb9dbb0e5276460785b20ce54d25f552b369cafeca270b9dd44c
fetch_core mgba              abde7a0764f08fa0cc2c7d3d9a29b9d1245a9f3b7df0e7a594b74df642ee53c6
fetch_core genesis_plus_gx   3673a22b906509461e23a5a118b1d1bec15cbda105f260cbcbc08a16b2124e48
fetch_core mednafen_pce_fast aca90a14b18108c86398da2267ef40d5145eaddbc1c1b310614d745b258552b1
fetch_core mednafen_ngp      a2015668f9a9403b8bf6941b550fae2c618f37b79173e8ba27c95b95f96bdd99

echo "vendor/cores ready:"
ls "$VENDOR/cores"
