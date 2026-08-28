#!/bin/sh
# Regenerate the Brick sysroot TortOS links against.
#
# Deliberately NOT committed: the libraries are TrimUI's binaries and the
# headers are upstream SDL's, so the repository records how to obtain them
# rather than a copy of them. This script is that record.
#
#   Libraries: pulled over ADB from the device's own /usr/trimui/lib. What
#              links is byte-identical to what runs - which is the point, and
#              is why no SDL is built from source here.
#   Headers:   upstream libsdl-org releases, version-matched to the libraries
#              the firmware ships and pinned by sha256.
#
# Needs a connected Brick (adb), curl and tar.
#
#   mk/fetch-sysroot.sh [sysroot-dir]        default: sysroot
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/sysroot}
DL=/tmp/tortos-sysroot-dl

# SDL2 headers are version-matched to the library the firmware ships
# (libSDL2-2.0.so.0.3000.8 = 2.30.8).
#
# SDL2_image and SDL2_ttf are NOT version-matched, deliberately. The firmware
# ships 2.0.3 and 2.0.14, which predate GitHub release assets and cannot be
# fetched reproducibly. These are the oldest fetchable releases, and their
# headers declare a superset of the old API. That is safe here for a reason
# rather than by hope: the link is against the DEVICE'S OWN libraries, pulled
# above, so a call to anything the shipped library lacks is a link error, not
# a runtime surprise. If TortOS ever reaches for a newer SDL_image or SDL_ttf
# function, the build fails and says so.
#
# Hashes are trust-on-first-use against the release assets, recorded
# 2026-08-26. They guard against a changed tarball, not a malicious first fetch.
SDL_VER=2.30.8
SDL_SHA=380c295ea76b9bd72d90075793971c8bcb232ba0a69a9b14da4ae8f603350058
IMG_VER=2.6.3
IMG_SHA=931c9be5bf1d7c8fae9b7dc157828b7eee874e23c7f24b44ba7eff6b4836312c
TTF_VER=2.20.2
TTF_SHA=9dc71ed93487521b107a2c4a9ca6bf43fb62f6bddd5c26b055e6b91418a22053

command -v adb  > /dev/null || { echo "need adb" >&2; exit 1; }
command -v curl > /dev/null || { echo "need curl" >&2; exit 1; }
adb get-state > /dev/null 2>&1 || { echo "no device over adb" >&2; exit 1; }

mkdir -p "$OUT/usr/lib" "$OUT/usr/include/SDL2" "$DL"

# Named rather than pulled wholesale: /usr/trimui/lib contains at least one
# dangling symlink (libfreeimage.so.3 -> nothing), and `adb pull` of the whole
# directory fails on it. Naming what TortOS links against is more honest
# anyway - the list IS the dependency set.
echo "pulling libraries from the device"
for lib in libSDL2-2.0.so.0 libSDL2_image-2.0.so.0 libSDL2_ttf-2.0.so.0; do
	adb pull "/usr/trimui/lib/$lib" "$OUT/usr/lib/$lib" > /dev/null 2>&1 ||
		{ echo "missing $lib on the device" >&2; exit 1; }
done

# The linker wants the SONAME-less names; the device only ships versioned ones.
for base in SDL2-2.0 SDL2_image-2.0 SDL2_ttf-2.0; do
	short=$(echo "$base" | cut -d- -f1)
	[ -e "$OUT/usr/lib/lib${short}.so" ] ||
		ln -sf "lib${base}.so.0" "$OUT/usr/lib/lib${short}.so"
done

fetch_headers() { # name version sha url subdir
	f="$DL/$1-$2.tar.gz"
	[ -f "$f" ] || curl -sSfL -o "$f" "$4"
	if [ "$3" != "demand" ]; then
		got=$(shasum -a 256 "$f" | cut -d' ' -f1)
		[ "$got" = "$3" ] || { echo "$1: hash mismatch: $got" >&2; exit 1; }
	fi
	rm -rf "${DL:?}/$5"
	tar -xzf "$f" -C "$DL"
	# SDL2 keeps its headers in include/; SDL2_image and SDL2_ttf are a
	# single header at the tarball root. Take whichever the tarball has.
	cp "$DL/$5"/include/*.h "$OUT/usr/include/SDL2/" 2> /dev/null || true
	cp "$DL/$5"/SDL_*.h     "$OUT/usr/include/SDL2/" 2> /dev/null || true
	[ -n "$(ls "$OUT/usr/include/SDL2" 2> /dev/null)" ] ||
		{ echo "$1: no headers extracted" >&2; exit 1; }
}

echo "fetching headers"
fetch_headers SDL2 "$SDL_VER" "$SDL_SHA" \
	"https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" \
	"SDL2-$SDL_VER"
# SDL2_image and SDL2_ttf are one public header each; take them from the
# matching release tarball rather than the whole tree.
fetch_headers SDL2_image "$IMG_VER" "$IMG_SHA" \
	"https://github.com/libsdl-org/SDL_image/releases/download/release-$IMG_VER/SDL2_image-$IMG_VER.tar.gz" \
	"SDL2_image-$IMG_VER"
fetch_headers SDL2_ttf "$TTF_VER" "$TTF_SHA" \
	"https://github.com/libsdl-org/SDL_ttf/releases/download/release-$TTF_VER/SDL2_ttf-$TTF_VER.tar.gz" \
	"SDL2_ttf-$TTF_VER"

echo "sysroot ready: $OUT"
ls "$OUT/usr/include/SDL2" | head -5
