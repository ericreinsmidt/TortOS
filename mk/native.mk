# Native dev build (macOS/Linux host): make -f mk/native.mk
#
# Drivable with the arrow keys, Return and Escape. Point it at a staging tree:
#   TORTOS_ROOT=... TORTOS_ROMS=... TORTOS_FONT=res/fonts/menu.ttf build-native/tortos
# There is no emulator on the host, so launching a game does nothing useful --
# this build is for looking at the shelf while changing how it looks.
BUILD := build-native
SRC := $(wildcard src/*.c)

PKGS := sdl2 SDL2_image SDL2_ttf
CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE \
          $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm

$(BUILD)/tortos: $(SRC) $(wildcard src/*.h)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
