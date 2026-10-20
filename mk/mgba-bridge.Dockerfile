# The image mk/build-mgba-bridge.sh builds the mGBA core in, and goes when that
# script does. A stock Debian cross-compiler and cmake; the core is pure C and
# links its own zlib, so it needs no device libraries.
#
# Bullseye for its glibc: the image's glibc is the floor of what the core asks
# of the device, and 2.31 is below both devices'. Pinned by digest, and apt
# reads from a fixed snapshot: bullseye is past end of life, and its security
# updates left the live mirror while the index still named them. Valid-Until
# is off because a snapshot's Release file is past its own expiry by design;
# the signatures are still checked.
#
#   docker build -f mk/mgba-bridge.Dockerfile -t mgba-xbuild mk
FROM debian:bullseye-slim@sha256:f313b4bd62667092a59b3a664d7d3ab8b5e65f41675f48e81455a15dc5abe792

ARG DEBIAN_SNAPSHOT=20260901T000000Z
RUN printf '%s\n' \
        "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bullseye main" \
        "deb http://snapshot.debian.org/archive/debian-security/${DEBIAN_SNAPSHOT} bullseye-security main" \
        "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bullseye-updates main" \
        > /etc/apt/sources.list

RUN apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu \
        libc6-dev-arm64-cross \
        binutils-aarch64-linux-gnu \
        cmake \
        make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
