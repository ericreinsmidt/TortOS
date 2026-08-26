# PlayOS

A custom firmware for the **TrimUI Brick / Brick Hammer** (tg5040) that plays
NES, TurboGrafx-16 and Game Boy Advance games, and does nothing else.

No store, no scraper, no achievements, no music player, no settings screen. No
WiFi and no Bluetooth — both radios are shut down at boot and never come back.
What is on screen is a row of cards, the name of the thing under the cursor,
and a rail saying where you are in the list.

The design metric is **speed**. Every decision in here was made by measuring
first.

---

## Where the time goes

Starting a game on this hardware costs about **1100ms**, and almost none of it
is the game:

| | |
|---|---|
| `GFX_init` — SDL video plus the EGL/GL context | ~620 ms |
| `dlopen` of the libretro core | ~170 ms |
| audio and settings | ~140 ms |
| **actually opening the ROM** | **~36 ms** |

Everything except the last line is the cost of *starting a process*. So PlayOS
does not start one.

### The resident emulator

`minarch.elf --resident NES=… PCE=… GBA=…` comes up during the boot animation
and stays up for the life of the session. It holds the GL context and all three
cores, and blocks on a fifo between games. The launcher hands it a game as one
line — `<tag>\t<core path>\t<rom path>` — and waits for `done`.

A launch is then the ~36ms of opening the ROM, plus the per-game setup that
cannot be shared, and lands at roughly **200 ms**.

**Three cores in one process** was a deliberate choice over the two
alternatives:

- *One resident process per core* — three EGL contexts on a device with one
  framebuffer. The failure mode is not a slow launch, it is no picture.
- *One resident that swaps cores* — `dlclose` + `dlopen` on every system
  change, ~170ms. Better than 1100, but paid every time you change machine.

Holding all three open costs a few MB of mapped, untouched pages and makes
switching from a NES game to a GBA game cost exactly what launching another NES
game costs. Cores are opened `RTLD_LOCAL`, so three libraries exporting the
same twenty `retro_*` symbols cannot see each other, and only one is ever
`retro_init`'d. `PLAYOS_PRELOAD=0` turns preloading off, so the two designs can
be measured against each other on hardware.

Nothing depends on the resident emulator. If its fifos are not there — in the
first second after boot, or if it has died — the launcher runs `minarch.elf`
the old way, one process per game.

### The boot animation runs *behind* startup

An animation that adds its own length to the boot is a delay with a picture on
it. PlayOS plays its 2.4s animation in the background while the launcher does
its entire startup — the card scan, GL init, font and card decode — and while
the resident emulator builds its context and opens three cores.

`ffmpeg` and the launcher both write to `/dev/fb0`, and it is last-writer-wins,
so they must never draw at the same time. A marker file is the handshake: the
launcher initialises freely, blocks on the marker, and presents its first frame
the moment the animation clears it. Bounded at 8 seconds, so a stuck decoder
cannot hang the boot.

The same idle seconds pull the emulator, the cores and their libraries into the
page cache. Cold reads of that set measure ~190ms against ~30ms warm.

Startup phases are logged rather than guessed at:

```
boot: scan               64 ms
boot: video+input       904 ms
boot: font+settings    1209 ms
boot: card assets      1499 ms
```

### The launcher never goes away

With a resident emulator there is nothing to tear down, so the launcher keeps
its own GL context through the whole game. Coming back from a game is a frame,
not a second and a half of re-initialising a display.

---

## What it looks like

A single row of cards in perspective, with reflections — Cover Flow, carried
over from an earlier project by the same author and retuned. The focused card
sits in a soft glow tinted with its system's colour, and the whole background
carries a wash of that colour that eases as you move between systems.

Card art comes from, in order:

1. box art you put in `Roms/<system>/.media/<name>.png`;
2. **the autosave preview** — the frame you were looking at when you stopped,
   which for a game in progress is a better card than any box;
3. a generated slab: the system's colour, the title, and the title's first
   letter enormous and barely there behind it.

A game with an autosave gets a dot in the system's colour beside its name.
Pressing A on it does not start it, it continues it.

---

## Saves

- **Autosave.** Every way out of a game — the Quit row, the power button, a
  signal from the launcher — leaves through one point in `minarch.c`, and that
  is where slot 9 is written. One funnel, so no exit can forget and none can
  save twice.
- **Auto-resume.** The launcher asks for slot 9 before every launch. If a state
  is there the game comes up exactly where it was left; if not it starts fresh.
- **Manual save and load**, eight slots, from the in-game menu (`MENU`). Silent
  — the device shows no in-game chrome.

Saves and states are keyed on the system's **tag** (`NES`, `PCE`, `GBA`), which
is stated outright in `systems.cfg` rather than guessed from punctuation in a
filename, so renaming a ROM folder cannot orphan a save.

---

## Controls

| | |
|---|---|
| **Left / Right** | move along the row |
| **L1 / R1** | jump a screenful (games) |
| **A** | open a system, or start a game |
| **B** | back to the systems row |
| **Volume rocker** | volume, everywhere, including in game |
| **F1 / F2** | brightness, everywhere, including in game |
| **MENU** (in game) | the in-game menu: Continue, Save, Load, Options, Reset, Quit |
| **POWER** | ends the game if one is running, otherwise powers off |

Volume and brightness draw the same thin line across the top of the screen in
the launcher, in a game, and in the in-game menu. One firmware, one piece of
feedback.

---

## Install

1. `make vendor` once, to pull the cores and runtime libraries.
2. `make && make minarch && make payload`
3. Copy the **contents** of `out/sd/` to the root of a FAT32 SD card
   (`make install-card CARD=/Volumes/YOURCARD` does it and ejects properly).
4. Put the card in a stock Brick and power on. The first boot installs the
   `runtrimui.sh` hook; every boot after that comes straight up in PlayOS.

Then put ROMs in:

```
Roms/NES/                     .nes .fds .unf .unif .zip
Roms/TurboGrafx-16/           .pce .sgx .cue .ccd .chd .toc .m3u .zip
Roms/Game Boy Advance/        .gba .agb .zip
Roms/<system>/.media/<name>.png     box art, optional
Bios/GBA/                     optional GBA BIOS
```

A folder inside a system folder that contains a disc image counts as one game,
launching the image inside it — that is how a multi-disc PC Engine CD set stays
a single entry.

### Undoing it

PlayOS replaces the boot splash and the loading splash on first boot, and backs
both up beside itself (`bootlogo.stock.bmp`, `splash.stock.png`) along with the
stock init script (`/etc/init.d/runtrimui.playos-bak`). Removing the card is
enough to boot stock again; `/usr/trimui/bin/runtrimui-original.sh` is the
original hook.

---

## Configuration

Two files, and they are the whole settings screen.

`PlayOS/playos.cfg`:

```
volume=40           # 0..100, applied at boot
brightness=8        # 0..10, applied at boot and to the animation before it
startup_system=NES  # only decides the very first boot; after that PlayOS
                    # comes back to wherever you were
wifi=1              # keeps WiFi up on a development unit, for ssh
```

`PlayOS/systems.cfg` — one line per system:

```
sys|display name|Roms/ folder|core|tag|card art|accent|extensions
```

The tag is three characters at most: `struct Core` declares `tag[8]` and a
longer one is silently truncated.

---

## Building

Everything cross-compiles in the tg5040 toolchain container, so the only host
requirements are Docker and (for regenerating art) Python with Pillow, plus
ffmpeg.

```sh
make            # the launcher            -> build/playos.elf
make minarch    # NextUI + PlayOS patches -> vendor/minarch.elf
make vendor     # cores + runtime libs    -> vendor/
make payload    # the installable card    -> out/sd/ and out/PlayOS-v1.0.zip
make native     # host build of the launcher, for working on how it looks
make boot       # regenerate the boot animation
make cards      # regenerate the system cards
```

`make minarch` is the one target that needs another project on disk: it builds
from a NextUI checkout with the overlays in `minarch/overrides/`, which is also
why the shipped image carries GPL-3.0 that PlayOS itself does not use.
[DIATOM-MIGRATION.md](DIATOM-MIGRATION.md) is a plan for removing both, by
replacing minarch with an MIT frontend written against the same device. Nothing
in it has been done yet.

The host build renders exactly what the handheld renders, and can be asked for
a single frame:

```sh
PLAYOS_ROOT=… PLAYOS_ROMS=… PLAYOS_FONT=res/fonts/menu.ttf \
  build-native/playos --shot /tmp/shelf.png --screen games
```

### On device

```sh
make adb          # push everything over USB
make adb-elf      # just the launcher
make adb-restart  # kill the launcher so launch.sh picks the new one up
make adb-log
```

**Never `kill` `launch.sh` itself.** The boot hook's failsafe powers the device
off when the launch loop exits.

---

## Layout

```
src/            the launcher (0BSD)
minarch/        NextUI overrides + the container build (GPL-3.0, see below)
mk/             cross build, payload, deployment
tools/          the boot-animation and card generators, and setbright
res/            the boot animation, the system cards, the font
config/         systems.cfg and playos.cfg as shipped
sd/             the boot hook and launch.sh as they land on the card
vendor/         fetched: cores, runtime libraries, built minarch.elf
```

## License

PlayOS's own code is **0BSD** (`LICENSE`).

`minarch.elf` is built from **NextUI** source and is **GPL-3.0** — a separate
program that PlayOS runs as a child process. The patches are in
`minarch/overrides/`, which has [its own README](minarch/overrides/README.md)
saying what each one changes and why. Full notices for everything
redistributed on a card are in `THIRD-PARTY-LICENSES.md`.
