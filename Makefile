# TortOS. The device builds, for the TrimUI Brick and the GKD Pixel 2, are
# plastron's (github.com/ericreinsmidt/plastron): its Buildroot builds the
# launcher and Muse with mk/cross.mk and lays out the card with mk/payload.sh,
# and the version below is read from here. What this Makefile does is the rest:
# the offline checks, a host build of the launcher, the cores, and the assets.
# docs/building.md says how to work on a device.

.PHONY: all clean native vendor boot checkmark \
        check check-cheevos check-hare check-httpd check-idle check-rahash \
        check-raset check-xfer check-menus check-artscrape check-artrun check-audioout \
        check-db check-stats check-sort check-bt check-ss

all: check

# Everything offline, in one command. There was no umbrella target: every check
# had to be remembered by name, which is a suite in the same sense that a list
# of good intentions is a plan. A check nobody runs is a check that does not
# exist, and check-menus was about to join eight others in that state.
CHECKS = check-cheevos check-hare check-httpd check-idle check-rahash \
         check-raset check-xfer check-menus check-artscrape check-artrun check-audioout \
         check-db check-stats check-sort check-bt check-ss \
         check-muselib check-musequeue check-museart check-controls check-logpack \
         check-clock

check:
	@fail=0; for c in $(CHECKS); do \
		printf '\n=== %s ===\n' "$$c"; \
		$(MAKE) --no-print-directory "$$c" || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then printf '\nsome checks FAILED\n' >&2; exit 1; fi; \
	printf '\nok: every check passed\n'

# One version number: plastron reads it from here for the card and the About
# page (its package/tortos/tortos.mk).
VERSION ?= 1.6.0

# The ScreenScraper developer pair, from .screenscraper.env in this directory
# (gitignored, never committed). A value already in the environment wins, and
# no file is still a supported build: the defines come out empty and art falls
# back to libretro. plastron's build reads the same file for the devices.
SS_ENV := .screenscraper.env
ss_value = $(shell [ -f $(SS_ENV) ] && sed -n 's/^$(1)=//p' $(SS_ENV) | head -1)
SS_DEVID ?= $(call ss_value,SS_DEVID)
SS_DEVPASS ?= $(call ss_value,SS_DEVPASS)
export SS_DEVID SS_DEVPASS

# The check binaries are rebuilt every time, deliberately.
#
# They take about a second each, and make's mtime comparison is second-granular
# - so editing a source and re-running a check inside the same second silently
# tests the PREVIOUS binary. That is not theoretical: it happened three times on
# 2026-08-30 while checking whether a check actually catches the bug it claims
# to, and each time a clean source read as a failing one.
#
# A check that quietly tests the wrong binary is worse than no check.
FORCE:

# The achievement half, checked on the host. It parses a file and filters a
# list - no SDL, no device, no emulator - and every one of its claims fails
# invisibly on hardware, which is the argument for checking it here.
check-cheevos: build-native/cheevos-check
	@./build-native/cheevos-check

build-native/cheevos-check: tools/cheevos-check.c src/cheevos.c src/cheevos.h \
                            src/atomic.c src/atomic.h src/db.c src/db.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/cheevos-check.c src/cheevos.c src/atomic.c src/db.c -lsqlite3

# Hare's routes: is anything reachable without the PIN? check-xfer proves a
# path cannot climb out of a root; this proves the routes actually ask it, and
# that they are behind the lock - separate claims, and the ones that regress
# when an endpoint is added.
check-hare: build-native/hare-check
	@./build-native/hare-check

build-native/hare-check: tools/hare-check.c src/hare.c src/httpd.c src/xfer.c src/muselib.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/hare-check.c src/hare.c src/httpd.c src/xfer.c src/muselib.c

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

# What a menu contains, for a given state. The point of the menu design: a
# screen's build function takes state and produces rows with no renderer and
# no device, so this is answerable here instead of by looking at a handheld.
# Every menu defect this screen has had was some form of "nobody noticed the
# list was wrong", and every one of them needed a device to see.
#
# It links the pure halves of the screens and NOT SDL. If it ever needs SDL to
# link, the menu design has failed - reopen it rather than adding the flag.
check-menus: build-native/menu-check
	@./build-native/menu-check

# Where sound goes, given a cable, a headset and a setting. Eight combinations
# and a five-line rule, which is exactly the kind of thing that is obviously
# right until a headset connects mid-game and it is not.
check-audioout: build-native/audioout-check
	@./build-native/audioout-check

build-native/audioout-check: tools/audioout-check.c src/audioout.c \
                            src/audioout.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/audioout-check.c src/audioout.c

# What MENU > Controls says: every button on exactly one page, and rows short
# enough to sit still. The page is the device's own copy of the controls list,
# so what it omits is invisible until somebody goes looking for a button.
check-controls: build-native/controls-check
	@./build-native/controls-check

build-native/controls-check: tools/controls-check.c src/controls.c \
                            src/controls.h src/menu.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/controls-check.c src/controls.c

build-native/menu-check: tools/menu-check.c src/wifi_menu.c src/wifi.c \
                        src/sys_menu.c src/menu.c src/game_menu.c \
                        src/audioout.c src/wifi_menu.h src/sys_menu.h \
                        src/game_menu.h src/menu.h src/audioout.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -o $@ \
	      tools/menu-check.c src/wifi_menu.c src/wifi.c src/sys_menu.c \
	      src/menu.c src/game_menu.c src/audioout.c

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

# Date & Time: each row moving only itself, the 12-hour words, and the zone
# list running west to east - see tools/clock-check.c.
check-clock: build-native/clock-check
	@./build-native/clock-check

build-native/clock-check: tools/clock-check.c src/clock.c src/clock.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/clock-check.c src/clock.c

# The device's title matcher against the host tool's. A drift here does not
# fail, it just finds fewer games - see tools/artscrape-check.c.
check-artscrape: build-native/artscrape-check
	@ART_SCORING=1 ./build-native/artscrape-check
	@python3 tools/artscrape-check.py

build-native/artscrape-check: tools/artscrape-check.c src/artscrape.c src/artscrape.h src/urlenc.c
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -Isrc \
	      -DTORTOS_VERSION='"check"' \
	      -o $@ tools/artscrape-check.c src/artscrape.c src/net.c src/urlenc.c

# Replace fetches over a cover rather than deleting it first. The real scraper
# against a stubbed network, so a hit and a miss are both pinned with no device
# and no download - see tools/artrun-check.c.
check-artrun: build-native/artrun-check
	@./build-native/artrun-check

build-native/artrun-check: tools/artrun-check.c src/artscrape.c src/artscrape.h src/urlenc.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -Isrc -DSS_BUSY_WAIT=0 -DART_QUIET \
	      -o $@ tools/artrun-check.c src/artscrape.c src/urlenc.c

# The two hashers - one C for the device, one Python for the host tools - over
# every ROM in the library. A wrong rule does not crash; it produces a hash RA
# has never seen, which is indistinguishable from a game RA does not know.
check-rahash: build-native/rahash-check
	@python3 tools/rahash-check.py

build-native/rahash-check: tools/rahash-check.c src/rahash.c src/rahash.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/rahash-check.c src/rahash.c -lz

# The two set converters - one C on the device, one Python in the bulk tool -
# over real RetroAchievements responses. Needs credentials and a network;
# skips cleanly without them.
check-raset: build-native/raset-check
	@python3 tools/raset-check.py

build-native/raset-check: tools/raset-check.c src/rafetch.c src/rajson.c src/rahash.c src/net.c src/atomic.c src/db.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -DTORTOS_VERSION='"check"' \
	      -o $@ tools/raset-check.c src/rafetch.c src/rajson.c src/rahash.c src/net.c src/atomic.c src/db.c -lsqlite3 -lz

# Whether this build can reach ScreenScraper, and whether an account survives
# a restart. The live half needs a network and .screenscraper.env, and skips
# cleanly without them - the same bargain check-raset makes.
check-ss: build-native/ss-check
	@./build-native/ss-check

build-native/ss-check: tools/ss-check.c src/ss.c src/ss.h src/ssfetch.c src/ssfetch.h src/db.c src/net.c src/urlenc.c src/rajson.c FORCE
	@mkdir -p build-native
	@$(MAKE) -f mk/native.mk creds
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -Ibuild-native \
	      -DTORTOS_VERSION='"check"' \
	      -o $@ tools/ss-check.c src/ss.c src/ssfetch.c src/db.c src/net.c src/urlenc.c src/rajson.c -lsqlite3

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

# Fourteen config files became two databases, so properties that used to be
# obvious from looking at a text file now need asserting - chiefly that a
# shipped default never overwrites a choice, which is a bug this project
# already shipped once.
check-db: build-native/db-check
	@./build-native/db-check

build-native/db-check: tools/db-check.c src/db.c src/db.h src/atomic.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/db-check.c src/db.c src/atomic.c -lsqlite3

# Play time. The assertion that earns this its place is that a LAUNCH writes
# nothing: that is a claim about a path nobody watches, and it stops being true
# one reasonable-looking write at a time.
check-stats: build-native/stats-check
	@./build-native/stats-check

build-native/stats-check: tools/stats-check.c src/stats.c src/stats.h src/db.c src/atomic.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/stats-check.c src/stats.c src/db.c src/atomic.c -lsqlite3

# What order a shelf ends up in. Two of the four orders read the session rows,
# so this links stats and the database - but not library.c: game_entry is a
# struct in the header and nothing here scans a directory.
check-sort: build-native/sort-check
	@./build-native/sort-check

build-native/sort-check: tools/sort-check.c src/sort.h src/library.h src/stats.c \
                         src/stats.h src/db.c src/atomic.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/sort-check.c src/stats.c src/db.c src/atomic.c -lsqlite3

# Bluetooth. A device NAME is arbitrary bytes chosen by whoever owns the
# headset, arriving over the air into a process running as root; an ADDRESS is
# the one value handed back to bluetoothctl. This checks the boundary between
# them.
check-bt: build-native/bt-check
	@./build-native/bt-check

build-native/bt-check: tools/bt-check.c src/bt.c src/bt.h src/bt_menu.c src/bt_menu.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/bt-check.c src/bt.c src/bt_menu.c

# Muse's view of the Music folder: the two shapes it reads, the litter a Mac
# leaves beside every track, and where an album's cover is kept.
check-muselib: build-native/muselib-check
	@./build-native/muselib-check

build-native/muselib-check: tools/muselib-check.c src/muselib.c src/muselib.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/muselib-check.c src/muselib.c

# The logs pack: names and Bluetooth addresses masked, and a whole pack
# unpacked with the same tar and read back.
check-logpack: build-native/logpack-check
	@./build-native/logpack-check

build-native/logpack-check: tools/logpack-check.c src/logpack.c src/logpack.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/logpack-check.c src/logpack.c

# The play modes: the last track, the first, a shuffle running out, a track
# that will not open, and a mode changed under a playing track.
check-musequeue: build-native/musequeue-check
	@./build-native/musequeue-check

build-native/musequeue-check: tools/musequeue-check.c src/musequeue.c src/musequeue.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/musequeue-check.c src/musequeue.c

# Album covers from MusicBrainz: the rule that picks the record, against what
# MusicBrainz said about the first card's albums, and a run on stubbed network.
check-museart: build-native/museart-check
	@./build-native/museart-check

build-native/museart-check: tools/museart-check.c src/museart.c src/museart.h \
                            src/urlenc.c src/rajson.c tools/fixtures/musicbrainz/*.json FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/museart-check.c src/museart.c src/urlenc.c src/rajson.c

# src/main.c hand-keeps a copy of the mark that C cannot import from
# tools/markdef.py. Run it before a release, for the same reason payload.sh
# refuses a card whose systems.cfg names cores vendor/ does not have: the
# failure is silent otherwise, and it ships.
checkmark:
	python3 tools/checkmark.py

clean:
	rm -rf build build-native out
