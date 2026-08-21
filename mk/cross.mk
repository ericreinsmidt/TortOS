# Runs inside the tg5040 toolchain container (CC/SYSROOT come from its env).
BUILD := build
SRC := $(wildcard src/*.c)

CFLAGS := -O2 -mcpu=cortex-a53 -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
          -I$(SYSROOT)/usr/include/SDL2 -D_GNU_SOURCE
LDLIBS := -lSDL2 -lSDL2_image -lSDL2_ttf -lm -ldl

$(BUILD)/playos.elf: $(SRC) $(wildcard src/*.h)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

# A libc-only helper with no SDL: setbright puts the panel at the configured
# brightness before the boot animation, while the launcher is still starting.
$(BUILD)/setbright: tools/setbright.c
	mkdir -p $(BUILD)
	$(CC) -O2 -mcpu=cortex-a53 -Wall -std=gnu11 -o $@ $<
