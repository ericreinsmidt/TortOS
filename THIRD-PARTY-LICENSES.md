# Third-party notices

TortOS itself - everything under `src/`, `tools/`, `mk/`, `sd/`, and the
configs, scripts and art written for this project - is licensed under the
**GNU General Public License, version 3 or later**; see `LICENSE`. Releases up
to v1.6.0 were MIT.

A TortOS card also carries software and files that aren't TortOS's, which keep
their own licenses. This file lists them, with where each comes from, and goes
on the card as `Licenses/TortOS.md` (`mk/payload.sh`), beside the notices of
Diatom and of plastron, the system TortOS runs on.

---

## Diatom (the emulator games run in)

- **Origin:** its own project, built in its own repository and shipped as
  `TortOS/diatom`. https://github.com/ericreinsmidt/diatom
- **License:** **Mozilla Public License 2.0** (MIT up to 2026-10-25). Its
  vendored `libretro.h` is MIT under the RetroArch team's own scoped notice,
  and its vendored part of RetroAchievements' rcheevos is MIT.
- TortOS runs it as a separate process and talks to it over a Unix socket; it
  does not link against it.

---

## libretro cores (`vendor/cores/`, shipped as `TortOS/cores/`)

Each core is its authors' work, built by the libretro project's buildbot
(https://buildbot.libretro.com) unless noted, pinned by sha256 in
`mk/fetch-vendor.sh`. The commit is the one the binary names as its source,
or, for Beetle PCE Fast, the one it was rebuilt from identically.

| Core | Systems | License | Source |
|------|---------|---------|--------|
| `fceumm_libretro.so` | NES | GPL-2.0-or-later | https://github.com/libretro/libretro-fceumm at 236ccdf |
| `mednafen_pce_fast_libretro.so` | TurboGrafx-16 and CD | GPL-2.0 (Mednafen-derived) | https://github.com/libretro/beetle-pce-fast-libretro at bebe2b1 (the binary names no commit; rebuilt from it byte for byte, see `mk/fetch-vendor.sh`) |
| `mednafen_ngp_libretro.so` | Neo Geo Pocket and Color | GPL-2.0 (Mednafen-derived) | https://github.com/libretro/beetle-ngp-libretro at a50d5ac |
| `mgba_libretro.so` | Game Boy, Color and Advance | MPL-2.0 | https://github.com/libretro/mgba at e31759b, with mGBA's own fix 543a197 from https://github.com/mgba-emu/mgba, built by `mk/build-mgba-bridge.sh` |
| `snes9x2010_libretro.so` | SNES | **Non-commercial** | https://github.com/libretro/snes9x2010 at 7db129b |
| `genesis_plus_gx_libretro.so` | Genesis, Master System, Game Gear | **Non-commercial** | https://github.com/libretro/Genesis-Plus-GX at b7e79b3 |

**The last two carry a non-commercial restriction.** They are not open source
under either the OSI or FSF definition and they forbid commercial
redistribution outright: a card carrying them can't be sold, or shipped with a
device that is. Hobby redistribution is what every firmware shipping them
relies on. A card without SNES and the Sega systems carries no such
restriction.

---

## Runtime libraries

None are on the card. TortOS links against libraries plastron's system
carries: SDL2, SDL2_image and SDL2_ttf (with FreeType, libpng and
libjpeg-turbo beneath them), SQLite, libcurl with OpenSSL (RetroAchievements
and box art), FFmpeg (Muse plays music and audiobooks through it), ALSA and
glibc. Their versions and licenses are in plastron's package list,
`Licenses/plastron-packages.md` on the card.

---

## Files TortOS carries

- `res/fonts/menu.ttf` and `res/fonts/wordmark.ttf` - **Josefin Sans** (regular,
  and thin italic for the wordmark), by Santiago Orozco, SIL Open Font License
  1.1; full text in `res/fonts/OFL.txt`. TortOS's UI face, the in-game menu's,
  and the one the boot animation and the system cards are lettered in.
- `res/ssl/cacert.pem`, shipped as `TortOS/cacert.pem` - **Mozilla's root
  certificate list**, as the curl project publishes it
  (https://curl.se/ca/cacert.pem), Mozilla Public License 2.0. It lets TortOS
  check the servers it talks to over HTTPS; `res/ssl/README.md` says when it
  was fetched.

---

## Services TortOS talks to

Not software on the card, but TortOS wouldn't be what it is without them:

- **RetroAchievements** (https://retroachievements.org): the achievements, and
  the rcheevos library Diatom checks them with.
- **ScreenScraper** (https://www.screenscraper.fr): box art and game names.
