# TortOS

A custom firmware for the **TrimUI Brick / Brick Hammer** (tg5040) that plays
nine 8- and 16-bit consoles, and does nothing else.

No store, no scraper, no achievements, no music player, no settings screen. No
WiFi and no Bluetooth - both radios are shut down at boot and never come back.
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
| `GFX_init` - SDL video plus the EGL/GL context | ~620 ms |
| `dlopen` of the libretro core | ~170 ms |
| audio and settings | ~140 ms |
| **actually opening the ROM** | **~36 ms** |

Everything except the last line is the cost of *starting a process*. So TortOS
does not start one.

### The resident emulator

[Diatom](https://github.com/ericreinsmidt/diatom) - an MIT libretro frontend
built for this device - comes up on a Unix socket during the boot animation
and stays up for the life of the session. The launcher hands it a game as a
`RUN` line carrying the core, the ROM, and the paths where the resume state
and the card preview live, and reads back what actually happened: `RUNNING`,
`EXIT reason=`, or an `ERROR code=` it can show.

A warm launch - the process up, the core already mapped - is **~15 ms** to
`RUNNING`. Cores are mapped the first time a game needs one and kept for the
life of the process (`RTLD_LOCAL`, so libraries exporting the same twenty
`retro_*` symbols cannot see each other), which makes switching systems cost
what launching another game on the same system costs, with no core list
configured anywhere.

The in-game menu is the launcher's own: MENU makes Diatom hand the display
over with a preview of the paused frame, and Continue, Save, Load, Reset and
Quit act through one protocol line each. Volume and brightness set in a game
come back to the launcher's settings when the game ends, because the two
sides share one levels channel instead of overwriting each other.

Nothing depends on the resident emulator. If the socket is not there - in the
first second after boot, or if it has died - the launcher runs the same
`diatom` binary standalone, one process for that game, and starts a fresh
resident once the display is back.

### The boot animation runs *behind* startup

An animation that adds its own length to the boot is a delay with a picture on
it. TortOS plays its 2.4s animation in the background while the launcher does
its entire startup - the card scan, GL init, font and card decode - and while
the resident emulator builds its context and maps the cores it needs.

`ffmpeg` and the launcher both write to `/dev/fb0`, and it is last-writer-wins,
so they must never draw at the same time. A marker file is the handshake: the
launcher initializes freely, blocks on the marker, and presents its first frame
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
not a second and a half of re-initializing a display.

---

## What it looks like

A single row of cards in perspective, with reflections - Cover Flow, carried
over from an earlier project by the same author and retuned. The focused card
sits in a soft glow tinted with its system's color, and the whole background
carries a wash of that color that eases as you move between systems.

Card art comes from, in order:

1. box art you put in `Roms/<system>/.media/<name>.png`;
2. **the autosave preview** - the frame you were looking at when you stopped,
   which for a game in progress is a better card than any box;
3. a generated slab: the system's color, the title, and the title's first
   letter enormous and barely there behind it.

A game with an autosave gets a dot in the system's color beside its name.
Pressing A on it does not start it, it continues it.

---

## Saves

- **Autosave.** Every way out of a game - the Quit row, the power button, a
  stop from the launcher - writes the state and the preview at the paths the
  launch handed over. One funnel, so no exit can forget and none can save
  twice.
- **Auto-resume.** The launcher asks for slot 9 before every launch. If a state
  is there the game comes up exactly where it was left; if not it starts fresh.
- **Manual save and load**, eight slots, from the in-game menu (`MENU`).
  Silent - the device shows no in-game chrome.

Saves and states are keyed on the system's **tag** (`NES`, `PCE`, `GBA`), which
is stated outright in `systems.cfg` rather than guessed from punctuation in a
filename, so renaming a ROM folder cannot orphan a save.

---

## Achievements

RetroAchievements, listed in the in-game menu, with what you have earned kept
across games and cards.

**The evaluation is Diatom's, and that is not a delegation of convenience.**
Conditions compare against the *previous frame* - `0xH06f0<d0xH06f0` is "this
byte is lower than it was last frame" - and the launcher only sees the socket
every 100ms against a core running at 60Hz. Six frames in seven would be
invisible to it, so unlocks would be missed silently. The launcher declares
which console the game is and hands over the set; Diatom watches every frame
(its ADR-0025 and ADR-0026).

**The device does the normal thing.** Sign in once under `MENU` ->
RetroAchievements, and the first time you launch a game TortOS hashes the ROM,
asks RetroAchievements which game it is, fetches the set and caches it at
`Roms/<System>/.cheevos/<name>.set`, beside the box art in `.media/`. After
that the launch is instant and works with Wi-Fi off.

That needs HTTPS, which the Brick turns out to have: `curl 7.54.1` against
`OpenSSL/1.1.0i`. What it does not have is anything to trust, so
`res/ssl/cacert.pem` ships with the launcher - see its README for the
measurement. Only the password is typed; RetroAchievements answers with a
token, and that is what is stored.

`tools/ra-sets.py` does the same fetch from a host, for seeding a whole library
at once or working offline. It writes the identical file, and is a convenience
rather than the mechanism. Measured over the 180-ROM test library: **166 games
have a set, 8,297 achievements, and every condition in all of them parses.**

Three things exist twice, once for the device and once for the host tool: the
per-console hash rules, the JSON to set-file conversion, and the set format
itself. `make check-rahash` and `make check-raset` run both implementations
over the same real data and require them to agree, because the failure they
guard against is silent - a wrong hash looks exactly like a game
RetroAchievements does not know.

**Submitting unlocks back to your account is not implemented**, and is blocked
on registering this client with RetroAchievements rather than on code. Until
then RA injects a `Warning: Unknown Emulator` entry into every set it serves,
which is dropped rather than shown as a fake achievement that unlocks itself
five seconds into every game.

---

## Controls

| | |
|---|---|
| **Left / Right** | move along the row |
| **Up / Down** | jump to the previous / next initial (games) |
| **L1 / R1** | jump a screenful (games) |
| **A** | open a system, or start a game |
| **B** | back to the systems row |
| **Volume rocker** | volume, everywhere, including in game |
| **F1 / F2** | brightness, everywhere, including in game |
| **MENU** (on the shelf) | the TortOS menu - settings that are about the firmware |
| **MENU** (in game) | the in-game menu: Continue, Save, Load, Options, Reset, Quit |
| **POWER** | ends the game if one is running, otherwise powers off |

Volume and brightness draw the same thin line across the top of the screen in
the launcher, in a game, and in the in-game menu. One firmware, one piece of
feedback - tinted by which of the two it is, warm for brightness and cyan for
volume, so the line says what it is without a glyph or a number on it.

---

## Install

1. `make toolchain` and `mk/fetch-sysroot.sh` once (the latter needs the device
   on adb), then `make vendor` to pull the cores.
2. Build Diatom in its own repository (`tools/brick-make.sh` there), or point
   `DIATOM_ELF` at a built binary.
3. `make && make payload`
4. Copy the **contents** of `out/sd/` to the root of a FAT32 SD card
   (`make install-card CARD=/Volumes/YOURCARD` does it and ejects properly).
5. Put the card in a stock Brick and power on. The first boot installs the
   `runtrimui.sh` hook; every boot after that comes straight up in TortOS.

Then put ROMs in:

```
Roms/NES/                     .nes .fds .unf .unif .zip
Roms/Master System/           .sms .zip
Roms/Genesis/                 .md .gen .bin .smd .zip
Roms/Game Boy/                .gb .dmg .zip
Roms/TurboGrafx-16/           .pce .sgx .cue .ccd .chd .toc .m3u .zip
Roms/Game Gear/               .gg .zip
Roms/SNES/                    .sfc .smc .zip
Roms/Game Boy Color/          .gbc .cgb .zip
Roms/Game Boy Advance/        .gba .agb .zip
Roms/<system>/.media/<name>.png     box art, optional
Bios/GBA/                     optional GBA BIOS
```

The shelf is `systems.cfg`, so that list is the shipped one rather than a fixed
one: a system with no ROMs in its folder still gets a card, and a line removed
from the config takes its shelf with it.

A folder inside a system folder that contains a disc image counts as one game,
launching the image inside it - that is how a multi-disc PC Engine CD set stays
a single entry.

### Undoing it

TortOS replaces the boot splash and the loading splash on first boot, and backs
both up beside itself (`bootlogo.stock.bmp`, `splash.stock.png`) along with the
stock init script (`/etc/init.d/runtrimui.tortos-bak`). Removing the card is
enough to boot stock again; `/usr/trimui/bin/runtrimui-original.sh` is the
original hook.

---

## Configuration

Two files, and they are the whole settings screen.

`TortOS/tortos.cfg`:

```
volume=40           # 0..100, the default before one has ever been set
brightness=7        # 0..11, twelve geometric rungs; 0 is the panel's floor
startup_system=NES  # only decides the very first boot; after that TortOS
                    # comes back to wherever you were
font_scale=1.0      # 0.75..1.50, multiplies the whole type scale at once
wifi=1              # keeps WiFi up on a development unit, for ssh
```

The two levels are **defaults, not settings**. Once the volume rocker or F1/F2
has been touched, the level lives in `.userdata/<platform>/levels.cfg` and that
is what every boot restores, because it is the level someone actually chose.
`launch.sh` reads the same file to light the panel for the boot animation, and
the brightness rungs are shared verbatim with Diatom, so a level set inside a
game and a level set on the shelf mean the same thing on both sides.

**Display mode** is per system, set from that system's own menu and kept in
`.userdata/<platform>/display.cfg` keyed on the system's tag. The seven modes
are Diatom's, named as it names them, and the launcher hands the chosen one
over with every launch - the emulator's mode is global and outlives a game, so
a system that has never been set would otherwise inherit whatever the last one
chose. An unrecognized name in that file falls back to `aspect`.

`font_scale` moves every size together. The sizes themselves are one base and
a multiplier per role - title, menu row, heading, the quiet line of counts and
timestamps - in `src/ui.c`, so the proportions between them are stated in one
table rather than as numbers spread across the call sites.

`TortOS/systems.cfg` - one line per system:

```
sys|display name|Roms/ folder|core|tag|card art|accent|extensions
```

The tag is three characters at most: `struct Core` declares `tag[8]` and a
longer one is silently truncated.

---

## Building

Everything cross-compiles in TortOS's own toolchain image - a stock Debian
cross-compiler pinned by digest, built by `mk/toolchain.Dockerfile`, linking
against the device's own SDL2 in `sysroot/`. So the only host requirements are
Docker and (for regenerating art) Python with Pillow, plus ffmpeg.

```sh
make toolchain  # the cross-compiler      -> tortos-toolchain   (once)
mk/fetch-sysroot.sh  # the device's SDL2  -> sysroot/           (once, needs adb)
make            # the launcher            -> build/tortos.elf
make vendor     # the libretro cores      -> vendor/
make payload    # the installable card    -> out/sd/ and out/TortOS-v1.0.zip
make native     # host build of the launcher, for working on how it looks
make boot       # regenerate the boot animation
make check-cheevos  # the achievement half, on the host: parsing and filtering
make check-rahash   # the C and Python ROM hashers agree, over the whole library
make check-raset    # the C and Python set converters agree (needs RA_USER/RA_PASS)
```

`make payload` needs a built Diatom binary (`DIATOM_ELF`, defaulting to a
sibling checkout).

The host build renders exactly what the handheld renders, and can be asked for
a single frame:

```sh
TORTOS_ROOT=… TORTOS_ROMS=… TORTOS_FONT=res/fonts/menu.ttf \
  build-native/tortos --shot /tmp/shelf.png --screen games
```

`--menu [row]` draws the TortOS menu over that shelf, and `--slots <n>
[aspect]` draws one frame of the save/load carousel over synthetic game
frames - the two screens that otherwise need a game running on a device before
they can be looked at. `--jump <n>` applies n letter-jumps first (negative for
up), so where the d-pad lands on a real library can be checked without a hand
on the device. Every shot names the screen and the focused item on stderr, so
a sequence of them reads back as a list of what was actually drawn.

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
mk/             cross build, payload, deployment
tools/          the boot-animation and card generators, setbright, the
                achievement fetcher and its check
res/            the boot animation, the system cards, the font
config/         systems.cfg and tortos.cfg as shipped
sd/             the boot hook and launch.sh as they land on the card
sysroot/        fetched: the device's own SDL2, for linking (mk/fetch-sysroot.sh)
vendor/         fetched: the libretro cores, hash-pinned
```

## License

TortOS's own code is **0BSD** (`LICENSE`).

The emulator is [Diatom](https://github.com/ericreinsmidt/diatom), **MIT**, a
separate program the launcher runs and talks to over a socket. The cores keep
their own licenses - two of them non-commercial, which is what actually
constrains a card - and full notices for everything redistributed are in
`THIRD-PARTY-LICENSES.md`.
