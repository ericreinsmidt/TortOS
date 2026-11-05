#!/bin/sh
# How the mGBA core TortOS ships was built. Every core TortOS ships comes from
# scutes (github.com/ericreinsmidt/scutes), fetched and hash-pinned by
# mk/fetch-vendor.sh, and five of them are the libretro buildbot's own. This
# made the sixth, the file on scutes, and stays as the record of how:
#
#   mGBA's buildbot core segfaults on the first read of cartridge RAM for every
#   MBC2 Game Boy cartridge - Kirby's Pinball Land, Wave Race, Golf, X, both
#   Final Fantasy Legends. A regression in mgba a1b2b23 (2026-07-30), reported
#   as mgba-emu/mgba#3859 and fixed upstream the SAME DAY in 543a1975.
#
#   The fix is in no core that can be downloaded. libretro builds from its own
#   fork, github.com/libretro/mgba, unsynced since 2026-08-06.
#
# So this builds libretro's own tree at the exact commit the pin is built from,
# applies only upstream's own one-hunk fix, and hardens it to match the official
# build. No patch of ours is in it.
#
# It stays, even once libretro syncs its fork. The cores don't change: a save
# state records the version of the core that wrote it and Diatom refuses any
# other, so libretro's build would cost every Game Boy, Game Boy Color and
# Game Boy Advance state. Whether this rebuilds scutes' file byte for byte
# hasn't been checked; it writes vendor/cores/mgba_libretro.so, and
# mk/fetch-vendor.sh puts scutes' copy back if the two differ.
#
# Needs: docker. Its image is mk/mgba-bridge.Dockerfile, built here; after
# the first time, Docker has it cached.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-/tmp/tortos-mgba-bridge}
PIN=e31759b24e7a4e3899285ff720d7b573ac328ae7   # libretro/mgba, what the buildbot's core was built from
FIX=543a197582c30364584d773a974d7f991892fa43   # mgba-emu/mgba, the upstream fix

command -v docker >/dev/null || { echo "need docker" >&2; exit 1; }
docker build -q -f "$ROOT/mk/mgba-bridge.Dockerfile" -t mgba-xbuild "$ROOT/mk" >/dev/null

if [ ! -d "$SRC/.git" ]; then
	git clone --depth 40 -b master https://github.com/libretro/mgba.git "$SRC"
fi
cd "$SRC"
git fetch --depth 40 origin master
git checkout -q "$PIN"
git remote get-url up >/dev/null 2>&1 || git remote add up https://github.com/mgba-emu/mgba.git
# The fix by its hash, not master's tip: it left the last few commits of
# master long ago. Two deep so git show has its parent to diff against.
git fetch -q --depth 2 up "$FIX"
git checkout -q -- src/gb/mbc.c
git show "$FIX" -- src/gb/mbc.c | git apply
# project(mGBA) declares C and CXX; there is no cross g++ in the image and the
# libretro core is pure C, so it does not need one. perl rather than sed -i,
# which takes a backup suffix on BSD and not on GNU - written with sed first,
# where it silently did nothing on macOS and CMake then asked for a C++
# compiler. No `|| true` either: if this edit fails the build must stop.
perl -pi -e 's/^project\(mGBA\)$/project(mGBA C)/' CMakeLists.txt
grep -q '^project(mGBA C)$' CMakeLists.txt || { echo "!! CMakeLists edit failed" >&2; exit 1; }

# -DZLIB_LIBRARIES is not a typo for the singular. mGBA's CMake has a gap: the
# libretro target appends $ZLIB_LIBRARIES to OS_LIB, but the bundled-zlib path
# sets ZLIB_LIBRARY, so zlib is compiled and never linked. The core then builds,
# loads, and dies at dlopen with "undefined symbol: crc32".
docker run --rm -v "$SRC:/src" mgba-xbuild bash -lc '
set -e
HARD="-D_FORTIFY_SOURCE=2 -fstack-protector-strong -O2"
cmake -S /src -B /src/bl -DBUILD_LIBRETRO=ON -DSKIP_LIBRARY=ON \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="$HARD" \
  -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON -DZLIB_LIBRARIES=zlibstatic \
  -DUSE_FFMPEG=OFF -DUSE_PNG=OFF -DUSE_LIBZIP=OFF -DUSE_SQLITE3=OFF \
  -DUSE_ELF=OFF -DUSE_EPOXY=OFF -DUSE_DISCORD_RPC=OFF -DUSE_LZMA=OFF \
  -DBUILD_QT=OFF -DBUILD_SDL=OFF >/dev/null
cmake --build /src/bl -j$(nproc) >/dev/null
aarch64-linux-gnu-strip --strip-unneeded /src/bl/mgba_libretro.so
# Never ship one that cannot load. Two cores were deployed and called "tested"
# before anyone noticed they had never linked at all.
[ "$(aarch64-linux-gnu-nm -D -u /src/bl/mgba_libretro.so | grep -c crc32)" = 0 ] \
  || { echo "!! undefined crc32 - zlib did not link" >&2; exit 1; }
[ "$(aarch64-linux-gnu-nm -D -u /src/bl/mgba_libretro.so | grep -cE "__stack_chk|_chk@")" -ge 9 ] \
  || { echo "!! not hardened - the official build has 9 such symbols" >&2; exit 1; }
'
mkdir -p "$ROOT/vendor/cores"
cp "$SRC/bl/mgba_libretro.so" "$ROOT/vendor/cores/mgba_libretro.so"
echo "  built  mgba (bridge, $PIN + $FIX)"
echo "  sha256 $(shasum -a 256 "$ROOT/vendor/cores/mgba_libretro.so" | cut -d' ' -f1)"
