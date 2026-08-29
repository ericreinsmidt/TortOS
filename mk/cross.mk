# Runs inside the TortOS toolchain container. CC comes from the image; SYSROOT
# is passed in and holds the device's own SDL2 libraries, so what links is
# byte-identical to what runs.
BUILD := build
SRC := $(wildcard src/*.c)
CC ?= aarch64-linux-gnu-gcc

VERSION ?= 0.0
CFLAGS := -DTORTOS_VERSION='"$(VERSION)"' -O2 -mcpu=cortex-a53 -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
          -I$(SYSROOT)/usr/include/SDL2 -D_GNU_SOURCE
# --allow-shlib-undefined: the device's SDL2_ttf pulls FT_* out of freetype,
# and SDL2/SDL2_image reach for more of the firmware besides. Those resolve on
# the device at runtime, where they exist. The flag permits undefined symbols
# in SHARED LIBRARIES only - anything THIS code calls and cannot find is still
# a link error, which is what makes the link a real check that TortOS uses no
# SDL function the shipped libraries lack.
LDFLAGS := -L$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/usr/lib \
           -Wl,--allow-shlib-undefined
LDLIBS := -lSDL2 -lSDL2_image -lSDL2_ttf -lm -ldl

$(BUILD)/tortos.elf: $(SRC) $(wildcard src/*.h)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

# A libc-only helper with no SDL: setbright puts the panel at the configured
# brightness before the boot animation, while the launcher is still starting.
$(BUILD)/setbright: tools/setbright.c
	mkdir -p $(BUILD)
	$(CC) -O2 -mcpu=cortex-a53 -Wall -std=gnu11 -o $@ $<
