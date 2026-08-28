# PlayOS's own toolchain: a stock Debian cross-compiler pinned by digest, with
# the device's SDL2 in sysroot/. Every dependency is PlayOS's own or the
# TrimUI's SDK. Build it once with `make toolchain`, and the sysroot once with
# `mk/fetch-sysroot.sh` (needs the device on adb).
IMAGE := tortos-toolchain
BRICK ?= 192.168.1.101
SSH := sshpass -p 'tina' ssh -o StrictHostKeyChecking=no root@$(BRICK)

.PHONY: all clean native toolchain vendor boot payload release install-card \
        adb adb-elf adb-res adb-vendor adb-restart adb-run adb-log \
        deploy restart logs

all: build/tortos.elf

build/tortos.elf: $(wildcard src/*.c) $(wildcard src/*.h) tools/setbright.c mk/cross.mk
	@docker image inspect $(IMAGE) > /dev/null 2>&1 || { \
		echo "toolchain image missing; run: make toolchain" >&2; exit 1; }
	@[ -d sysroot/usr/include/SDL2 ] || { \
		echo "no sysroot; run: mk/fetch-sysroot.sh (needs the device)" >&2; exit 1; }
	docker run --rm -v $(CURDIR):/work -w /work $(IMAGE) \
		make -f mk/cross.mk SYSROOT=/work/sysroot build/tortos.elf build/setbright

# One-time: the cross-compiler image. Pinned by digest, so it does not drift.
toolchain:
	docker build -f mk/toolchain.Dockerfile -t $(IMAGE) mk

# Host build of the launcher, for working on how the shelf looks.
native:
	$(MAKE) -f mk/native.mk

# The libretro cores PlayOS redistributes, hash-pinned from libretro's own
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

payload: all
	./mk/payload.sh

VERSION ?= 1.0
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
