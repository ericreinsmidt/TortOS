# Third-party notices

PlayOS itself — everything under `src/`, `tools/`, `mk/`, and the configs,
scripts and generated art written for this project — is licensed **0BSD**; see
`LICENSE`.

A built PlayOS card (`out/sd/`) also redistributes third-party software that
keeps its own license. This file lists those components and their terms, and is
copied onto the card as `PlayOS/THIRD-PARTY-LICENSES.md` by `mk/payload.sh`.

---

## Diatom (the in-game libretro host)

- **Origin:** an independent frontend, built in its own repository and shipped
  as `PlayOS/diatom`. https://github.com/ericreinsmidt/diatom
- **License:** **MIT.** Its vendored `libretro.h` is MIT under the RetroArch
  team's own scoped notice.
- PlayOS's launcher runs it as a resident process and talks to it over a Unix
  socket; it does not link against it. Diatom ships no cores of its own.

## NextUI runtime libraries

Pulled by `mk/fetch-vendor.sh` from a NextUI release into `vendor/`:

- `vendor/lib/libmsettings.so`, `libgametimedb.so`, `libbatmondb.so` —
  NextUI/MinUI device libraries, used by the launcher for the device's volume,
  brightness and battery interfaces.
- These follow their NextUI upstream licensing.
  https://github.com/LoveRetro/NextUI

---

## libretro cores (`vendor/cores/`, shipped as `PlayOS/cores/`)

| Core | System | License |
|------|--------|---------|
| `fceumm_libretro.so` | NES | GPL-2.0-or-later |
| `mednafen_pce_fast_libretro.so` | TurboGrafx-16 / PC Engine | GPL-2.0-or-later (Mednafen-derived) |
| `mgba_libretro.so` | Game Boy, Game Boy Color, Game Boy Advance | MPL-2.0 |
| `snes9x2010_libretro.so` | SNES | **Non-commercial** |
| `genesis_plus_gx_libretro.so` | Genesis, Master System, Game Gear | **Non-commercial** |

**The last two carry a non-commercial restriction.** They are not open source
under either the OSI or FSF definition and they restrict commercial
redistribution outright, which constrains what a card carrying them may be
sold as - hobby redistribution is what every firmware shipping them relies
on. The reasoning is worked through in Diatom's ADR-0023. A card built
without SNES and the Sega systems carries no such restriction.

Core source: the libretro organization and each core's upstream repository
(https://github.com/libretro).

---

## Runtime libraries (`vendor/lib/`, shipped as `PlayOS/lib/`)

Pulled from the NextUI release; standard shared libraries linked by minarch and
the cores.

| Library | Project | License |
|---------|---------|---------|
| `libcrypto.so.1.1` | OpenSSL 1.1 | OpenSSL + SSLeay (dual) |
| `liblzma.so.5` | xz-utils | Public domain / BSD-0 |
| `libbz2.so.1.0` | bzip2 | bzip2 (BSD-style) |
| `libzstd.so.1` | Zstandard | BSD-3-Clause (or GPL-2.0) |
| `liblz4.so.1` | LZ4 | BSD-2-Clause |
| `libzip.so.5` | libzip | BSD-3-Clause |
| `libchdr.so.0` | libchdr | BSD-3-Clause |
| `libsamplerate.so.0` | libsamplerate | BSD-2-Clause |

---

## Fonts

- `res/fonts/menu.ttf` — **Josefin Sans**, SIL Open Font License 1.1. Full text
  in `res/fonts/OFL.txt`. It is PlayOS's UI face, the in-game menu's face, and
  the face the boot animation and the system cards are lettered in.

## Cover Flow

The perspective card row in `src/coverflow.c` is carried over from EROS, an
earlier project by the same author, and is 0BSD like the rest of PlayOS.
