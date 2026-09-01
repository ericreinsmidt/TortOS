<p align="center">
  <img src="res/readme/wordmark.png" alt="TortOS" height="72">
  &nbsp;&nbsp;&nbsp;&nbsp;
  <img src="res/readme/turtle.png" alt="" height="84">
</p>

A custom firmware for the **TrimUI Brick / Brick Hammer** that plays eleven 8-
and 16-bit consoles.

What is on screen is a row of cards, the name of the thing under the cursor,
and a rail saying where you are in the list. Behind that there is
RetroAchievements, box art the device fetches itself, and a small web server
for moving files on and off over Wi-Fi - each one row in one menu, and none of
it in the way of starting a game.

The design metric is **speed**. Every decision in here was made by measuring
first.

---

## Install

You need the Brick, an SD card, and nothing else. No soldering and no
unlocking.

**Format the card exFAT**, with a Master Boot Record partition scheme.

| | |
|---|---|
| **macOS** | Disk Utility, `View -> Show All Devices` so you get the whole card rather than just its volume, then `Erase`: format **ExFAT**, scheme **Master Boot Record**. |
| **Linux** | `mkfs.exfat` on an MBR-partitioned card. |
| **Windows** | Right-click the card, `Format`, **exFAT**. |

FAT32 works too if that is what the card already is - the kernel has both, and
118 GB of each has been booted and played from - but exFAT is what every
formatter offers at the size of card anyone actually uses, so it is the one
worth naming.

Copying from macOS leaves `._name` metadata files beside everything. They are
harmless - the launcher skips every name beginning with a dot, so they never
show up as games - and `dot_clean /Volumes/YOURCARD` removes them if they
bother you.

1. Download **`TortOS-v1.0.zip`** from
   [Releases](https://github.com/ericreinsmidt/TortOS/releases/latest).
2. Unzip it and copy everything inside it to the **root** of the card - six
   items: `TortOS/`, `.tmp_update/`, `trimui/`, `Roms/`, `Bios/` and `Saves/`.
   Not the zip, and not a folder containing them.
3. Eject the card properly, put it in the Brick, and power on.

The first boot is the one that installs: the stock firmware runs the card's
installer, which sets `/usr/trimui/bin/runtrimui.sh` aside as
`runtrimui-original.sh`, puts its own there, replaces the two splash images
and then comes up in TortOS like every boot after it.

That one script is the only thing TortOS changes on the device, and all it
does is look for the card - no card, or a card without TortOS on it, and it
hands straight back to the original. **Taking the card out is enough to get
the stock system back**; [Undoing it](#undoing-it) has the rest.

> **`.tmp_update` starts with a dot**, so Finder and most file managers hide
> it and a drag of "everything" leaves it behind. A card missing it gets
> through the installer and then powers straight off looking for the half that
> is not there, and boots stock from then on. In Finder, `Cmd-Shift-.` shows
> hidden files; from a terminal, `cp -R` the unzipped contents and it comes
> along on its own.

Building it yourself instead is [further down](#building).

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
Roms/Neo Geo Pocket/          .ngp .ngc .ngpc .npc .zip
Roms/Neo Geo Pocket Color/    .ngp .ngc .ngpc .npc .zip
Roms/Game Boy Advance/        .gba .agb .zip
Roms/<system>/.media/<name>.png     box art, optional
```

BIOS files go **loose in `Bios/`**, not in a folder per system - that directory
is handed to the core as its system directory, and a core asks for a filename
inside it:

```
Bios/syscard3.pce             TurboGrafx-16 CD games (.chd, .cue, .m3u ...)
```

That one is required: a PC Engine CD will not start without it, and TortOS says
so by name rather than letting the core refuse the disc. HuCards on the same
shelf need nothing and are unaffected.

Nothing else on the shelf needs a BIOS. mGBA has a built-in one, so Game Boy
Advance runs without the real thing; if you want the original it is
`Bios/gba_bios.bin`, and the difference is the boot animation.

The shelf is `systems.cfg`, so that list is the shipped one rather than a fixed
one: a line removed from the config takes its shelf with it, and a system whose
folder is empty is hidden until there is something in it.

A folder inside a system folder that contains a disc image counts as one game,
launching the image inside it - that is how a multi-disc PC Engine CD set stays
a single entry.

### Undoing it

TortOS replaces the boot splash and the loading splash on first boot, and backs
both up beside itself (`bootlogo.stock.bmp`, `splash.stock.png`) along with the
stock init script (`/etc/init.d/runtrimui.tortos-bak`). Removing the card is
enough to boot stock again; `/usr/trimui/bin/runtrimui-original.sh` is the
original hook.

What removing the card does **not** undo is anything written to the device
itself. The stock root filesystem is read-only and everything writable is an
overlay on the internal eMMC, so what TortOS puts there stays there:

```
/usr/trimui/bin/runtrimui.sh            the hook
/usr/trimui/bin/runtrimui-original.sh   the stock hook, moved aside
/usr/trimui/bin/setbright               brightness before the animation,
/usr/trimui/bin/tortos-bootbright.sh    re-copied by launch.sh every boot
/etc/init.d/runtrimui                   patched to call the line above
/etc/init.d/runtrimui.tortos-bak        the version before that patch
/etc/splash.png                         the pic2fb loading splash
/mnt/boot/bootlogo.bmp                  the u-boot splash, on mmcblk0p1
```

Eight files, none of them a driver and none of them replacing anything the
system needs to run. TortOS is a launcher that boots on the stock firmware
rather than a firmware of its own - which is also why **the Wi-Fi networks and
their passwords survive** a reformatted card. They live in
`/etc/wifi/wpa_supplicant.conf`, written by the stock `wpa_supplicant` that
TortOS drives rather than replaces, on the eMMC and never on the card. Forget
them from the Wi-Fi screen before the device goes to anyone else.

The two splashes are the awkward pair: they are replaced on the device, and the
originals are saved **to the card** as `bootlogo.stock.bmp` and
`splash.stock.png`. Keep a copy of those somewhere else if the stock boot logo
matters to you - a card is the one part of this that gets reformatted.

---

## Controls

| | |
|---|---|
| **Left / Right** | move along the row |
| **Up / Down** | jump to the previous / next initial (games) |
| **L1 / R1** | jump a screenful (games) |
| **A** | open a system, or start a game |
| **B** | back to the systems row |
| **X** | game info for the card under the cursor |
| **Y** | favorite it - Favorites is a shelf of its own |
| **Volume rocker** | volume, everywhere, including in game |
| **F1 / F2** | brightness, everywhere, including in game |
| **MENU** (on the systems row) | the TortOS menu - settings that are about the firmware |
| **MENU** (inside a system) | that system's menu, below |
| **MENU** (in game) | the in-game menu: Continue, Save, Load, Display, Cheevos, Reset, Quit |
| **POWER** | ends the game if one is running, otherwise powers off |

MENU means three different menus depending on where you are, and each one is
about the thing you are looking at: the firmware on the systems row, one
console inside it, the running game in a game.

The system menu opens on a shelf of games and applies to that system alone:

| | |
|---|---|
| **Games** | how many the shelf found |
| **Core** | which libretro core runs them |
| **Sort By**, **Show** | stated, not yet settings - `Name` and `All games` |
| **Display Mode** | left/right cycles it; saved the moment it changes |
| **Box Art** | fetch what this system is missing, and nothing else |
| **Rescan Folder** | read the card again, for ROMs that arrived since boot |

Display Mode is per-system, because a Game Boy and a Genesis do not want the
same answer. Set here it applies from the next launch; the same row in the
in-game menu changes the running game as you press it, which is the one to use
when you want to see the difference rather than guess at it. Either way it is
written the moment it changes - there is no confirm step to hang the save off.

Rescan Folder is what makes a ROM that arrived after boot appear without a
restart. Over The Hare already does it for you on the way out of the transfer
screen; this is the same thing by hand, for a card written some other way.

**In a game, X and Y are turbo A and turbo B** - hold one down and it presses
the button repeatedly for you instead of you mashing it. It applies to a whole
system rather than to one game.

Nine of the eleven have it: **NES, Master System, TurboGrafx-16, Game Boy, Game
Boy Color, Game Boy Advance, Game Gear, Neo Geo Pocket and Neo Geo Pocket
Color**. Those consoles had two face buttons,
so X and Y are spare and turbo can have them. Genesis and SNES are left out
because their pads use X and Y for real buttons. Which systems get it, and how
fast, is `turbo.cfg` below.

Diatom does the pulsing, not the emulator core, which is why it works the same
on all six rather than only on the one core that happens to implement turbo.

Volume and brightness draw the same thin line across the top of the screen in
the launcher, in a game, and in the in-game menu. One firmware, one piece of
feedback - tinted by which of the two it is, warm for brightness and cyan for
volume, so the line says what it is without a glyph or a number on it.

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
Quit act through one protocol line each. **Display** cycles the running game's
mode as you press it, which is the one to use when you want to see the
difference rather than guess at it, and **Cheevos** shows how much of this
game's set you have earned, or reads `none` and stays unselectable when there
is no set. Volume and brightness set in a game
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

1. box art in `Roms/<system>/.media/<name>.png`, whether you put it there or
   the Box Art row fetched it;
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

States are keyed on the system's **`Roms/` folder**, and battery `.srm` files
sit flat in `Saves/` named after the ROM. The **tag** in `systems.cfg` keys
something else: the per-system display mode and the favorites list. This
paragraph claimed for a while that saves and states hung off the tag; they do
not, checked in the code on 2026-09-01.

Cartridge battery saves are separate from all of that: a game with a battery
gets a `.srm` beside the state. **Neo Geo Pocket Color is the exception** - its
core reports no save memory at all, measured as zero bytes on two carts that do
save, so nothing writes a `.srm` and no battery file exists to copy off the card.
Progress there lives entirely in the autosave state, which is written on every
exit like every other system, so in normal play nothing is lost. It only matters
if you load an older slot, which rewinds the cartridge's own save with it.

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

**Unlocks go back to your account.** They are sent once the game is over and
the launcher has the screen back, never from inside the frame loop, and what
will not send stays queued and is tried again next time - an achievement
earned on a plane is still earned. On the next launch the account is read back
and merged, so anything already held is not offered again: the account wins on
what exists, the local store wins on what is still owed, and neither is thrown
away.

RA still serves a `Warning: Unknown Emulator` entry with every set, because
this client is not registered with them. It is dropped rather than shown - it
is not an achievement, and recording it would put a row in the store that can
never be displayed and might later be submitted as a duplicate of something
that was never real.

---

---

## Configuration

Four files, and they are the whole settings screen.

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

`TortOS/coreopts.cfg` - opinions handed to the core before a game loads:

```
mgba_sgb_borders=OFF        # no [SECTION] above it, so: every system

[NES]
fceumm_turbo_enable=Player 1   # X and Y become turbo A and turbo B

[GB]
mgba_gb_model=Game Boy         # not Super Game Boy, whatever the cartridge says
```

A `[TAG]` section applies to that system alone, keyed on the same tag
`systems.cfg` uses, and overrides a global of the same name. A core that does
not declare a key ignores it, so a key meant for one core is harmless
everywhere else. These are sent **before** the game loads, because a core reads
its `(Restart)` options during load and one set afterwards does nothing until
the next launch. The file carries the reasoning for every entry in it.

One trap it documents and worth repeating: **a resume state beats these.** A
save state carries the machine it was made on, so changing an option that
selects hardware will not appear to work on a game you have already played.
Test on a game that has never been launched, or delete its `.auto.state`.

`TortOS/turbo.cfg` - which systems get turbo, and how fast:

```
NES=x:a~3,y:b~3
GB=x:a~3,y:b~3
```

One line per system tag. `x:a~3` reads *X acts as A, pressed three frames and
released three*, so about ten presses a second at 60 Hz. Lower is faster. A
system with no line here plays with X and Y doing nothing, which is what they
did everywhere before this existed.

TortOS hands this to Diatom just after a game loads and Diatom does the rest, so
the rate is the same whatever core is running. Delete a line to turn a system
back into a plain pad.

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
res/            the boot animation, the system cards, the font, Over The
                Hare's page, and the two marks this README shows
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
