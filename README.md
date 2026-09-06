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

The design metric is **speed**, and one decision carries most of it: **TortOS
never starts a process to run a game.**

A cold start costs about 1100 ms on this hardware and almost none of it is the
game - it is SDL, an EGL context, audio and settings. So the emulator comes up
once during the boot animation and stays up for the whole session, and a game
arrives as a single line on a socket.

| | |
|---|---|
| Starting a game | **~15 ms** |
| Switching to a different console | **~15 ms** |
| Every core held in memory at once | **15 MB** of 975 |

The middle row is the one nothing else here does. Each core is mapped the first
time a game needs it and is never unloaded, so going from a Game Boy game to a
SNES game costs exactly what starting another Game Boy game costs. Nothing is
preloaded, and there is no core list to configure anywhere.

Every number in this file was measured on the device.

---

## Install

You need the Brick, an SD card, and nothing else.

**Format the card exFAT**, with a Master Boot Record partition scheme.

| | |
|---|---|
| **macOS** | Disk Utility, `View -> Show All Devices` so you get the whole card rather than just its volume, then `Erase`: format **ExFAT**, scheme **Master Boot Record**. |
| **Linux** | `mkfs.exfat` on an MBR-partitioned card. |
| **Windows** | Right-click the card, `Format`, **exFAT**. |

FAT32 works too if that is what the card already is - the kernel has both - but
exFAT is what every formatter offers at the size of card anyone actually uses,
so it is the one worth naming.

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

The TortOS menu is the firmware's own, and opens from the systems row:

| | |
|---|---|
| **Wi-Fi** | the network's name when connected, or why it is not |
| **Bluetooth** | stated, not yet a setting - pairing is still done over a shell |
| **Audio Output** | `Auto` or `Speaker`, and where Auto landed - see **Audio** below |
| **Over The Hare** | the file server; needs Wi-Fi and says so when there is none |
| **Cheevos** | the RetroAchievements account, or `sign in` |
| **Box Art** | fetch what the whole library is missing |
| **Text Size** | left/right; reopens every font, so the whole UI is rebuilt |
| **Auto Off** | how long without a button before the device powers itself down |
| **About TortOS** | version, address, battery, uptime |

Over The Hare and Box Art need a network, and go quiet without one rather than
disappearing - a row that vanishes teaches nobody why. Cheevos stays reachable
either way, because signing in is the thing you go there to do.

On the Wi-Fi screen, **Y** rescans and **X** forgets the network under the
cursor, behind a confirm. Forgetting the one you are connected through is
allowed - refusing would leave a row that is visibly saved and visibly
un-forgettable, which is worse to explain than the consequence.

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
fast, is in [docs/turbo.md](docs/turbo.md).

Diatom does the pulsing, not the emulator core, which is why it works the same
on all nine rather than only on the one core that happens to implement turbo.

Volume and brightness draw the same thin line across the top of the screen in
the launcher, in a game, and in the in-game menu. One firmware, one piece of
feedback - tinted by which of the two it is, warm for brightness and cyan for
volume, so the line says what it is without a glyph or a number on it.

---

## Audio

Sound can come out of three places, and TortOS picks in a fixed order:

**wired headphones, then Bluetooth, then the speaker.**

A cable wins outright, in every setting. Someone who physically plugged
something in has said what they want more plainly than any menu can, and a
headset that merely happens to be connected has not said anything at all.

The **Audio Output** row has two positions rather than three. `Auto` follows
the rule above; `Speaker` refuses Bluetooth and nothing else - a cable still
works through it. There is no third "Headset" position because it would do
nothing `Auto` does not already do: `Auto` takes a headset whenever one is
connected, and neither setting can route to one that is not there. The row
shows where the sound actually went - `auto (wired)` - because `Auto` on its
own names a rule, not a place you can hear.

The wired jack has its own volume range, not the speaker's. The two are about
9 dB apart, and the level is re-mapped the moment a cable goes in or out, so
plugging in mid-game does not arrive at nine decibels louder than you left it.

### Bluetooth

A paired headset reconnects by itself at boot and mid-session, and game audio
follows it without relaunching anything. The bond survives a reboot.

**Pairing is not in the UI yet.** It is done once over `bluetoothctl` on the
device, and the one thing that matters there is the agent: headsets pair
"Just Works" and need `agent NoInputNoOutput`. With the default agent every
attempt fails with an authentication error that looks like a broken key, a
broken chip, or broken headphones, and is none of them.

Two things behave differently on a Bluetooth sink and are not bugs:

- **the volume keys do nothing.** Volume lives on the headset, so the device's
  own control is not in the path. Reachable, and not done yet.
- **there is roughly 100-150 ms of latency**, from SBC, the radio and the
  headset's own buffer. That is what Bluetooth audio costs on any device and
  nothing here can tune it away.

If a headset is switched off or walks out of range mid-game, sound falls back
to the speaker within a second or two and the game keeps running. It never ends
a game to report an audio problem. That fallback is the emulator's own - it
does not wait for the launcher to notice the headset is gone.

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
`RUNNING`. A core is mapped the first time a game needs it and never unloaded:
six mapped plus one running measured **15.0 MB** against the device's 975, which
is what makes holding all of them affordable rather than reckless. They are
opened `RTLD_LOCAL`, so libraries exporting the same twenty `retro_*` symbols
cannot see each other.

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

**Settings live in a database, not in files.** Two of them, and the split is
deliberate:

| | holds | why it is separate |
|---|---|---|
| `.userdata/<platform>/tortos.db` | volume, brightness, text size, Auto Off, audio output, Wi-Fi, Bluetooth, display mode per system | per handheld. A card moved to another device should not carry the first one's screen and speaker settings, or its account token |
| `.userdata/shared/.tortos/library.db` | timezone, startup system, turbo maps, core options | per card. It travels with the library, the same way favorites and earned achievements do |

The shipped defaults are **compiled into the launcher** and seed whichever
database is missing them, so there is no config file to ship, none to drift from
the code that reads it, and a deleted database comes back working.

### Reading it

Nothing on the device can open a database - there is no `sqlite3` binary - so
the launcher prints it:

```
tortos.elf --dump
```

That is deliberately read-only. Settings are not hand-edited any more; every one
of them is reachable from a menu.

### What the boot script reads

`launch.sh` needs five values before `tortos.elf` exists - the panel brightness
for before the boot animation, the timezone, and whether each radio should come
up. It is POSIX shell and cannot read a database, so the launcher exports
`.userdata/<platform>/boot.env` and the script sources it.

That file is **derived, never authoritative**. Delete it and the next boot runs
at the shipped defaults, then the next settings change rewrites it. It replaced
six `sed` invocations across four files, each one a fork, so the boot path got
shorter rather than longer.

### `systems.cfg`, which is still a file

`TortOS/systems.cfg` is the one that did not move, because it is **build input
rather than a setting**. `mk/payload.sh` reads it twice on the host: once to
refuse a card whose `systems.cfg` names cores that `vendor/` does not have -
which is how a card was nearly built with four of nine systems dead - and once
to create the ROM folders. Neither can wait for a database that only exists on
the device.

```
sys | display name | Roms/ folder | core | tag | card art | accent | extensions | disc bios
```

The **tag** keys the per-system display mode and the favorites list. It is not
what saves and states hang off - states are keyed on the folder and `.srm` files
sit flat in `Saves/` named after the ROM. `system_cfg` declares `tag[8]`, so up
to seven characters, and changing a tag orphans that system's display mode and
favorites.

### Core options and turbo

Both are entries in the library database rather than files, seeded from the
values compiled into the launcher.

**Core options** are keyed `coreopt.<tag>.<option>`, with an empty tag for a
global - so `coreopt..mgba_sgb_borders` applies everywhere and
`coreopt.GB.mgba_gb_model` to Game Boy alone. A tagged entry overrides a global
of the same name. A core that does not declare a key ignores it, so a key meant
for one core is harmless everywhere else. They are sent **before** the game
loads, because a core reads its `(Restart)` options during load and one set
afterwards does nothing until the next launch.

One trap worth repeating: **a resume state beats these.** A save state carries
the machine it was made on, so changing an option that selects hardware will not
appear to work on a game you have already played. Test on a game that has never
been launched, or delete its `.auto.state`.

**Turbo** is keyed `turbo.<tag>`, and [docs/turbo.md](docs/turbo.md) carries the
reasoning: which nine systems get it, why MD and SFC do not, and why PC Engine
is on the list despite its core having a turbo of its own.

### What the launcher still writes as files

Three things are not in a database, each for a reason:

| | |
|---|---|
| `cheevos-active.set` | Diatom reads it, handed over as a path on RUN under its ADR-0026. Moving it would mean Diatom linking sqlite and learning the schema |
| `favorites.cfg`, `cheevos.cfg` | player records rather than settings; they move next |
| `ra.cfg` | the RetroAchievements account, per device - it holds a session token, so a card moved to another handheld does not carry one with it. It is `0600`, which is why it has not moved yet |

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
make check      # every check below, offline, in a second or two
```

`make payload` needs a built Diatom binary (`DIATOM_ELF`, defaulting to a
sibling checkout).

`make check` runs before every commit. Each part can be run alone, and each
exists because something once broke in a way nothing noticed:

| | |
|---|---|
| `check-menus` | what each menu CONTAINS in a given state, with no renderer and no device |
| `check-audioout` | where sound goes given a cable, a headset and a setting - all eight combinations |
| `check-idle` | the Auto Off clock, including the charger case and the counter wrapping |
| `check-cheevos` | the achievement half: parsing and filtering |
| `check-rahash` | the C and Python ROM hashers agree, over the whole library |
| `check-raset` | the C and Python set converters agree (needs `RA_USER`/`RA_PASS`) |
| `check-artscrape` | two name normalizers agree on every candidate |
| `check-hare` | nothing on the file server is reachable without the PIN |
| `check-httpd` | request parsing, including the malformed ones |
| `check-xfer` | upload paths cannot escape the directory they were aimed at |

They are offline and need no device. A screen's rows are a pure function of
its state precisely so the first two can exist - see `docs/decisions/`.

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
src/            the launcher (MIT)
docs/           why the code is shaped the way it is: the architecture
                decisions in docs/decisions/, and the rules a menu follows
                in docs/menus.md. Not user documentation - read before
                changing a screen, not before using one.
mk/             cross build, payload, deployment
tools/          the boot-animation and card generators, setbright, the
                achievement fetcher, and the checks
res/            the boot animation, the system cards, the font, Over The
                Hare's page, and the two marks this README shows
config/         systems.cfg as shipped; the rest is compiled in
sd/             the boot hook and launch.sh as they land on the card
sysroot/        fetched: the device's own SDL2, for linking (mk/fetch-sysroot.sh)
vendor/         fetched: the libretro cores, hash-pinned
```

## License

TortOS's own code is **MIT** (`LICENSE`).

The emulator is [Diatom](https://github.com/ericreinsmidt/diatom), **MIT**, a
separate program the launcher runs and talks to over a socket. The cores keep
their own licenses - two of them non-commercial, which is what actually
constrains a card - and full notices for everything redistributed are in
`THIRD-PARTY-LICENSES.md`.
