# Cross-toolchain for the TrimUI Brick (tg5040 platform).
#
# Nothing in here comes from NextUI, MinUI, LoveRetro or TrimUI's SDK. A stock
# Debian cross-compiler is the whole toolchain; the platform's SDL2 libraries
# and their headers live in the sysroot (mk/fetch-sysroot.sh), not in this
# image, so the image never needs rebuilding when the device libraries change.
#
# This replaces ghcr.io/loveretro/tg5040-toolchain:latest, which built PlayOS
# correctly but was someone else's image on an UNPINNED tag: a republish would
# have changed every build here with nothing failing and no record of it.
#
# Base is bullseye because of glibc, not preference: the image's glibc is the
# FLOOR of what the output binary demands, and it must stay at or below the
# device's 2.33. Bullseye's 2.31 clears that; bookworm's 2.36 would not.
#
# Pinned by digest, never :latest. The digest is the multi-arch manifest list,
# so on an arm64 host this runs natively with no emulation.
#
#   docker build -f mk/toolchain.Dockerfile -t playos-toolchain mk
FROM debian:bullseye-slim@sha256:f313b4bd62667092a59b3a664d7d3ab8b5e65f41675f48e81455a15dc5abe792

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu \
        libc6-dev-arm64-cross \
        binutils-aarch64-linux-gnu \
        make \
    && rm -rf /var/lib/apt/lists/*

ENV CC=aarch64-linux-gnu-gcc
WORKDIR /work
