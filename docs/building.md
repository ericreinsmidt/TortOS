# Building TortOS

Both devices run TortOS on [plastron](https://github.com/ericreinsmidt/plastron),
and plastron's build is what builds TortOS for them: it runs `mk/cross.mk`
with its own cross-compiler and libraries, and `mk/payload.sh` to lay out what
goes on the card, from this working tree (its Makefile's `TORTOS_SRC`). This
Makefile is for everything else. The host needs Docker (for plastron and the
mGBA core) and, for regenerating art, Python with Pillow, plus ffmpeg.

```sh
make check      # every check below, offline, in a second or two (also plain `make`)
make vendor     # five libretro cores     -> vendor/
mk/build-mgba-bridge.sh  # the sixth      -> vendor/            (see below)
make native     # host build of the launcher, for working on how it looks
make boot       # regenerate the boot animation
```

**mGBA is built rather than fetched, and is meant to stop being.** Every MBC2
Game Boy cartridge segfaults on the buildbot core - Kirby's Pinball Land, Wave
Race, Golf, X, both Final Fantasy Legends - a regression reported as
mgba-emu/mgba#3859 and fixed upstream the same day, but in no downloadable core
because libretro builds mGBA from a fork that has not synced.
`mk/build-mgba-bridge.sh` builds that same pinned tree with only upstream's own
fix applied, hardened to match the official build. Without it the payload
refuses the card rather than shipping one whose Game Boy, Game Boy Color and
Game Boy Advance shelves open onto nothing. Restore the fetch and delete the
script the day that fork syncs.

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
| `check-artrun` | whole Box Art runs against a fake network: Replace keeps a cover it cannot better, and a checksum names the ROM in a zip, not the readme beside it |
| `check-hare` | nothing on the file server is reachable without the PIN |
| `check-httpd` | request parsing, including the malformed ones |
| `check-xfer` | upload paths cannot escape the directory they were aimed at |
| `check-db` | a shipped default never overwrites a choice, and the two scopes stay apart |
| `check-stats` | play time is recorded, and a LAUNCH still writes nothing |
| `check-sort` | a shelf sorts the way it says it does, and the same shelf twice the same way |
| `check-bt` | a device name from the air is only ever data; an address is validated |

They are offline and need no device. A screen's rows are a pure function of
its state precisely so the first two can exist.

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

## On device

Build in plastron's checkout: `make brick BRICK_TARGET=tortos-rebuild` for the
Brick, or `make` for the Pixel 2's whole card image. A full `make brick` also
writes a card image.

A plastron development build has SSH on the USB cable, with the key in
plastron's `local/authorized_keys`: `ssh root@10.55.0.2` on the Brick, `ssh
root@10.42.0.1` on the Pixel 2. Files go over `scp -O` (there is no SFTP
server), to `/tmp` first: compare checksums on both ends before moving one
into place in `/mnt/SDCARD/TortOS/`. A release has no key and starts no SSH
server.

## Layout

```
src/            the launcher (GPL-3.0-or-later)
docs/           the guide, and building
mk/             the device build plastron runs, the payload, the cores
tools/          the boot-animation and card generators, the achievement
                fetcher, and the checks
res/            the boot animation, the system cards, the font, Over The
                Hare's page, and the marks and screenshots the README shows
config/         systems.cfg as shipped; the rest is compiled in
sd/             each device's launch.sh, and bt-alsa.sh, as they land on the card
vendor/         the libretro cores: five fetched and hash-pinned, mGBA
                built by mk/build-mgba-bridge.sh
```
