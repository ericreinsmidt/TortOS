#!/bin/sh
# Populate vendor/cores with the libretro cores TortOS redistributes, and
# vendor/xz-embedded with the .xz decoder the updater is built with.
#
# That is the whole of it. TortOS carries no third-party runtime libraries;
# measurement says nothing needs them:
#
#   the cores need  libc libm librt libstdc++ libgcc_s ld-linux
#   tortos.elf needs libSDL2 libSDL2_image libSDL2_ttf libm libdl libc
#
# Every one of those is in plastron's system, under /usr/lib. Settings are
# TortOS's own code in src/device/, against each device's ALSA controls and
# display.
#
# Cores come from scutes (github.com/ericreinsmidt/scutes), which keeps the
# exact files on its v1 release, pinned by sha256 - the same hashes Diatom's
# CORES.md verifies, because they are the same binaries. What each was built
# from is in THIRD-PARTY-LICENSES.md here and in scutes' LICENSE.
#
# They don't change. A save state records the version of the core that wrote
# it and Diatom refuses any other, so a new core would cost players every state
# for that console. A core changes only to fix a bug players hit, as the same
# source plus the fix, the way mGBA's was.
#
# Usage: mk/fetch-vendor.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VENDOR=$ROOT/vendor
DL=${1:-/tmp/tortos-vendor}
SCUTES=https://github.com/ericreinsmidt/scutes/releases/download/v1

command -v curl   > /dev/null || { echo "need curl" >&2; exit 1; }
command -v shasum > /dev/null || { echo "need shasum" >&2; exit 1; }

mkdir -p "$DL" "$VENDOR/cores"

fetch_core() { # name sha256
	f=${1}_libretro.so
	if [ -f "$VENDOR/cores/$f" ] &&
	   [ "$(shasum -a 256 "$VENDOR/cores/$f" | cut -d' ' -f1)" = "$2" ]; then
		echo "  ok      $1"
		return
	fi
	curl -sSfL -o "$DL/$f" "$SCUTES/$f"
	GOT=$(shasum -a 256 "$DL/$f" | cut -d' ' -f1)
	if [ "$GOT" != "$2" ]; then
		rm -f "$DL/$f"
		echo "  MISMATCH $1" >&2
		echo "    want $2" >&2
		echo "    got  $GOT" >&2
		echo "  scutes' v1 files can't change (its releases are immutable): find" >&2
		echo "  out why before trusting it." >&2
		exit 1
	fi
	mv "$DL/$f" "$VENDOR/cores/$f"
	echo "  fetched $1"
}

fetch_core fceumm            1b13b00d4680394dad8000d5175f97be727107e0945bc9b412da91d70c07b267
fetch_core snes9x2010        3933890f520abb9dbb0e5276460785b20ce54d25f552b369cafeca270b9dd44c
# mGBA is our own build, not the buildbot's: mk/build-mgba-bridge.sh builds
# libretro/mgba e31759b plus mGBA's own fix 543a1975, without which every MBC2
# Game Boy cartridge segfaults (Kirby's Pinball Land, Wave Race, Golf, X, both
# Final Fantasy Legends; mgba-emu/mgba#3859). The script stays as the record of
# how this file was made.
fetch_core mgba              1c180827774f1941565ac82a173633edf4553ac2809a4851c9eee627f2775ff9
fetch_core genesis_plus_gx   3673a22b906509461e23a5a118b1d1bec15cbda105f260cbcbc08a16b2124e48
# Beetle PCE Fast's binary names no commit, unlike the others. It is
# beetle-pce-fast-libretro bebe2b13a840fbfd45cb501f5cd0efe01bdb1735: rebuilt from
# it on 2026-10-25 the way the buildbot built it (Ubuntu 20.04, GCC
# 12.5.0-8ubuntu2~20~ppa3 from ppa:ubuntu-toolchain-r/test, make platform=unix,
# then strip), it came out byte for byte this file
fetch_core mednafen_pce_fast aca90a14b18108c86398da2267ef40d5145eaddbc1c1b310614d745b258552b1
fetch_core mednafen_ngp      a2015668f9a9403b8bf6941b550fae2c618f37b79173e8ba27c95b95f96bdd99

echo "vendor/cores ready:"
ls "$VENDOR/cores"

# XZ Embedded, the small .xz decoder the Linux kernel unpacks itself with, for
# the updater (tools/tortos-update/): it unpacks a release's card image as it
# writes it, after the root it runs from is gone and no program is left to
# do it. Its own license (COPYING), with no conditions. It has no release
# files, only tags, so each file is taken at the tag's commit (v2024-12-30)
# and pinned here; a tarball GitHub makes from a tag isn't the same bytes
# from year to year. Only what a decoder needs: no BCJ filters, which the
# card images don't use.
XZ_COMMIT=ae63ae3a36ed01724674e8f3d750dc47bf125410
XZ_RAW=https://raw.githubusercontent.com/tukaani-project/xz-embedded/$XZ_COMMIT
mkdir -p "$VENDOR/xz-embedded"

fetch_xz() { # path-in-repository sha256
	f=$VENDOR/xz-embedded/${1##*/}
	if [ -f "$f" ] && [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" = "$2" ]; then
		echo "  ok      ${1##*/}"
		return
	fi
	curl -sSfL -o "$f" "$XZ_RAW/$1"
	GOT=$(shasum -a 256 "$f" | cut -d' ' -f1)
	if [ "$GOT" != "$2" ]; then
		rm -f "$f"
		echo "  MISMATCH ${1##*/}" >&2
		echo "    want $2" >&2
		echo "    got  $GOT" >&2
		echo "  A file at a pinned commit changed, which git doesn't allow: find out" >&2
		echo "  why before trusting it." >&2
		exit 1
	fi
	echo "  fetched ${1##*/}"
}

fetch_xz COPYING                    d9ff4f089509582e7b197e86f3d2d66948b89169f27c1f2349eb0d1ed4459309
fetch_xz linux/include/linux/xz.h   59196b9c8296815f6a029f26d202bcd404078e27fc6e9bc4077db5f6865664c7
fetch_xz linux/lib/xz/xz_private.h  9d8d116354cca408ef17a10cd544a955ea5d26e8f9f50885e99adab9135917d5
fetch_xz linux/lib/xz/xz_stream.h   89e80af27003ad39f3122ad447861ec574fa235a0c6339c26bb48a53c3d9e12b
fetch_xz linux/lib/xz/xz_lzma2.h    5681cf01bdcfab8907c02194eeaf9d4415166b3b5d48b50a34df809276f63aa1
fetch_xz linux/lib/xz/xz_crc32.c    e6373b6d1aa11535260e153d8d7c62485ce82fbcb1d9dd66e6d984025771eec6
fetch_xz linux/lib/xz/xz_crc64.c    cc6d80bc48c3d0ae963c5b61ac85f21a26efc733bc2ae61cbb8f39f09f5e85f6
fetch_xz linux/lib/xz/xz_dec_stream.c bb5d29c33bd29b7d7993ae6b1a60f1750c509b1935fe2d28d46dfd8c5f46e9ce
fetch_xz linux/lib/xz/xz_dec_lzma2.c  248dcd23e186b179a3fd1f5150c70cde13670521251e62bc44b15c99241491c4
fetch_xz userspace/xz_config.h      25fba71668254ba56709e9c176521a24a304b5521a9a337fb51d8334fa07e875

echo "vendor/xz-embedded ready:"
ls "$VENDOR/xz-embedded"
