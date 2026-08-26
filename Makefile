IMAGE := ghcr.io/loveretro/tg5040-toolchain:latest
BRICK ?= 192.168.1.101
SSH := sshpass -p 'tina' ssh -o StrictHostKeyChecking=no root@$(BRICK)

.PHONY: all clean native vendor boot cards payload release install-card \
        adb adb-elf adb-res adb-vendor adb-restart adb-run adb-log \
        deploy restart logs

all: build/playos.elf

build/playos.elf: $(wildcard src/*.c) $(wildcard src/*.h) tools/setbright.c mk/cross.mk
	docker run --rm -v $(CURDIR):/work -w /work $(IMAGE) \
		/bin/bash -c 'source ~/.bashrc && make -f mk/cross.mk build/playos.elf build/setbright'

# Host build of the launcher, for working on how the shelf looks.
native:
	$(MAKE) -f mk/native.mk

# The cores and runtime libraries PlayOS redistributes, pulled from a NextUI
# release. Needed once, before the first payload.
vendor:
	./mk/fetch-vendor.sh

# Assets in res/ are committed ready to ship; nothing needs generating to
# build. These regenerate them.
boot:
	python3 tools/genboot.py
cards:
	python3 tools/gencards.py

payload: all
	./mk/payload.sh

VERSION ?= 1.0
release: payload
	@echo "out/PlayOS-v$(VERSION).zip"

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
	adb shell 'killall -q playos.elf; exit 0'

# Restart the resident emulator too -- needed after pushing a new diatom.
adb-restart-all:
	adb shell 'killall -q diatom; killall -q playos.elf; exit 0'

# Run the launcher by hand with its output on your terminal: the fastest way
# to tell "the scan is wrong" from "the display is wrong".
adb-run:
	adb shell 'killall -q playos.elf; cd /mnt/SDCARD/PlayOS && \
	  ROMS_PATH=/mnt/SDCARD/Roms \
	  LD_LIBRARY_PATH=/mnt/SDCARD/PlayOS/lib:/usr/trimui/lib \
	  ./playos.elf 2>&1' | head -60

adb-log:
	adb shell 'tail -60 /mnt/SDCARD/.userdata/tg5040/logs/playos.log 2>/dev/null'

# --- Push to a running device over SSH. BRICK=<ip> to override.
deploy: all
	tar -cf - -C build playos.elf setbright -C ../config systems.cfg playos.cfg \
	    -C ../sd/playos launch.sh -C ../res/fonts menu.ttf | \
	    $(SSH) 'tar -xf - -C /mnt/SDCARD/PlayOS'

restart:
	$(SSH) 'killall -q playos.elf; exit 0'

logs:
	$(SSH) 'tail -60 /mnt/SDCARD/.userdata/tg5040/logs/playos.log 2>/dev/null'

clean:
	rm -rf build build-native out
