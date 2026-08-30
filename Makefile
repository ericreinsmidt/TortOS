# TortOS's own toolchain: a stock Debian cross-compiler pinned by digest, with
# the device's SDL2 in sysroot/. Every dependency is TortOS's own or the
# TrimUI's SDK. Build it once with `make toolchain`, and the sysroot once with
# `mk/fetch-sysroot.sh` (needs the device on adb).
IMAGE := tortos-toolchain
BRICK ?= 192.168.1.101
SSH := sshpass -p 'tina' ssh -o StrictHostKeyChecking=no root@$(BRICK)

.PHONY: all clean native toolchain vendor boot checkmark payload release install-card \
        adb adb-elf adb-res adb-vendor adb-restart adb-run adb-log \
        check-cheevos check-hare check-httpd check-idle check-rahash check-raset check-xfer \
        deploy restart logs

all: build/tortos.elf

# One version number: the zip name and the About page both read it from here.
VERSION ?= 1.0

build/tortos.elf: $(wildcard src/*.c) $(wildcard src/*.h) tools/setbright.c mk/cross.mk
	@docker image inspect $(IMAGE) > /dev/null 2>&1 || { \
		echo "toolchain image missing; run: make toolchain" >&2; exit 1; }
	@[ -d sysroot/usr/include/SDL2 ] || { \
		echo "no sysroot; run: mk/fetch-sysroot.sh (needs the device)" >&2; exit 1; }
	docker run --rm -v $(CURDIR):/work -w /work $(IMAGE) \
		make -f mk/cross.mk SYSROOT=/work/sysroot VERSION=$(VERSION) build/tortos.elf build/setbright

# The check binaries are rebuilt every time, deliberately.
#
# They take about a second each, and make's mtime comparison is second-granular
# - so editing a source and re-running a check inside the same second silently
# tests the PREVIOUS binary. That is not theoretical: it happened three times on
# 2026-08-30 while checking whether a check actually catches the bug it claims
# to, and each time a clean source read as a failing one. mk/cross.mk carries a
# staleness warning for the same reason on the device side, where a rebuild is
# too slow to just repeat.
#
# A check that quietly tests the wrong binary is worse than no check.
FORCE:

# The achievement half, checked on the host. It parses a file and filters a
# list - no SDL, no device, no emulator - and every one of its claims fails
# invisibly on hardware, which is the argument for checking it here.
check-cheevos: build-native/cheevos-check
	@./build-native/cheevos-check

build-native/cheevos-check: tools/cheevos-check.c src/cheevos.c src/cheevos.h \
                            src/atomic.c src/atomic.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/cheevos-check.c src/cheevos.c src/atomic.c

# Hare's routes: is anything reachable without the PIN? check-xfer proves a
# path cannot climb out of a root; this proves the routes actually ask it, and
# that they are behind the lock - separate claims, and the ones that regress
# when an endpoint is added.
check-hare: build-native/hare-check
	@./build-native/hare-check

build-native/hare-check: tools/hare-check.c src/hare.c src/httpd.c src/xfer.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/hare-check.c src/hare.c src/httpd.c src/xfer.c

# Hare's transport, driven by a real client over a real socket. An HTTP parser
# is where "looks right" and "is right" part company: every browser sends the
# well-formed case, and only the interesting failures send anything else.
check-httpd: build-native/httpd-check
	@./build-native/httpd-check

build-native/httpd-check: tools/httpd-check.c src/httpd.c src/httpd.h src/xfer.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/httpd-check.c src/httpd.c

# Hare's path safety: can a browser on the LAN climb out of the three roots?
# Every other check in here protects a feature; this one protects the device.
check-xfer: build-native/xfer-check
	@./build-native/xfer-check

build-native/xfer-check: tools/xfer-check.c src/xfer.c src/xfer.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/xfer-check.c src/xfer.c

# Auto Off's clock. It has broken five times, once in this arithmetic and four
# times in wiring; this covers the arithmetic, and `grep -n idle_due src/main.c`
# covers the wiring by listing every screen that honors it.
check-idle: build-native/idle-check
	@./build-native/idle-check

build-native/idle-check: tools/idle-check.c src/idle.c src/idle.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/idle-check.c src/idle.c

# The two hashers - one C for the device, one Python for the host tools - over
# every ROM in the library. A wrong rule does not crash; it produces a hash RA
# has never seen, which is indistinguishable from a game RA does not know.
check-rahash: build-native/rahash-check
	@python3 tools/rahash-check.py

build-native/rahash-check: tools/rahash-check.c src/rahash.c src/rahash.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/rahash-check.c src/rahash.c

# The two set converters - one C on the device, one Python in the bulk tool -
# over real RetroAchievements responses. Needs credentials and a network;
# skips cleanly without them.
check-raset: build-native/raset-check
	@python3 tools/raset-check.py

build-native/raset-check: tools/raset-check.c src/rafetch.c src/rajson.c src/rahash.c src/ranet.c src/atomic.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -DTORTOS_VERSION='"check"' \
	      -o $@ tools/raset-check.c src/rafetch.c src/rajson.c src/rahash.c src/ranet.c src/atomic.c

# One-time: the cross-compiler image. Pinned by digest, so it does not drift.
toolchain:
	docker build -f mk/toolchain.Dockerfile -t $(IMAGE) mk

# Host build of the launcher, for working on how the shelf looks.
native:
	$(MAKE) -f mk/native.mk VERSION=$(VERSION)

# The libretro cores TortOS redistributes, hash-pinned from libretro's own
# buildbot. Needed once, before the first payload. No runtime libraries: the
# cores need only what the device's firmware already has.
vendor:
	./mk/fetch-vendor.sh

# Assets in res/ are committed ready to ship; nothing needs generating to build.
#
# The boot animation still has a generator. The system cards no longer do: they
# are drawn by hand, gencards.py could not reproduce any of the nine, and its
# only remaining effect would have been to overwrite three of them. Their accent
# rule is kept in step with config/systems.cfg by tools/recolor-cards.py.
boot:
	python3 tools/genboot.py

# src/main.c hand-keeps a copy of the mark that C cannot import from
# tools/markdef.py. payload depends on this for the same reason payload.sh
# refuses a card whose systems.cfg names cores vendor/ does not have: the
# failure is silent otherwise, and it ships.
checkmark:
	python3 tools/checkmark.py

payload: all checkmark
	./mk/payload.sh

release: payload
	@echo "out/TortOS-v$(VERSION).zip"

# Copy the assembled payload onto a mounted FAT32 card. CARD must be set.
install-card: payload
	@[ -n "$(CARD)" ] || { echo "usage: make install-card CARD=/Volumes/YOURCARD"; exit 1; }
	./mk/install-card.sh "$(CARD)"

# --- ADB over USB: works with no WiFi and no SSH, whenever the device is on.
adb: all
	./mk/adb-deploy.sh all
adb-elf: all
	./mk/adb-deploy.sh elf
adb-res:
	./mk/adb-deploy.sh res
adb-vendor:
	./mk/adb-deploy.sh vendor

# Kill the launcher so launch.sh's restart loop picks up a freshly pushed
# build. NEVER kill launch.sh itself: the boot hook's failsafe powers the
# device off when the launch loop exits.
adb-restart:
	adb shell 'killall -q tortos.elf; exit 0'

# Restart the resident emulator too -- needed after pushing a new diatom.
adb-restart-all:
	adb shell 'killall -q diatom; killall -q tortos.elf; exit 0'

# Run the launcher by hand with its output on your terminal: the fastest way
# to tell "the scan is wrong" from "the display is wrong".
adb-run:
	adb shell 'killall -q tortos.elf; cd /mnt/SDCARD/TortOS && \
	  ROMS_PATH=/mnt/SDCARD/Roms \
	  LD_LIBRARY_PATH=/mnt/SDCARD/TortOS/lib:/usr/trimui/lib \
	  ./tortos.elf 2>&1' | head -60

adb-log:
	adb shell 'tail -60 /mnt/SDCARD/.userdata/tg5040/logs/tortos.log 2>/dev/null'

# --- Push to a running device over SSH. BRICK=<ip> to override.
deploy: all
	tar -cf - -C build tortos.elf setbright -C ../config systems.cfg tortos.cfg \
	    -C ../sd/tortos launch.sh -C ../res/fonts menu.ttf | \
	    $(SSH) 'tar -xf - -C /mnt/SDCARD/TortOS'

restart:
	$(SSH) 'killall -q tortos.elf; exit 0'

logs:
	$(SSH) 'tail -60 /mnt/SDCARD/.userdata/tg5040/logs/tortos.log 2>/dev/null'

clean:
	rm -rf build build-native out
