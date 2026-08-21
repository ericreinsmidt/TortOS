# Third-party notices

PlayOS itself — everything under `src/`, `tools/`, `mk/`, and the configs,
scripts and generated art written for this project — is licensed **0BSD**; see
`LICENSE`.

A built PlayOS card (`out/sd/`) also redistributes third-party software that
keeps its own license. This file lists those components and their terms, and is
copied onto the card as `PlayOS/THIRD-PARTY-LICENSES.md` by `mk/payload.sh`.

None of the three cores PlayOS ships carries a non-commercial restriction, so
unlike some larger core sets a PlayOS card is redistributable under the terms
below.

---

## minarch (the in-game libretro host)

- **Origin:** built from **NextUI** source (LoveRetro/NextUI), which PlayOS
  patches and rebuilds — see `minarch/` and `minarch/build.sh`.
- **License:** **GPL-3.0**. https://github.com/LoveRetro/NextUI
- The eight files under `minarch/overrides/all/` are copied-and-patched NextUI
  GPL-3.0 source and remain GPL-3.0. They are listed, with what each changes,
  in `minarch/overrides/README.md`.
- PlayOS's launcher is a separate 0BSD program. It runs `minarch.elf` as a
  child process and talks to it over a fifo; it does not link against it.
- **Source availability (GPL §6):** the corresponding source for the shipped
  `minarch.elf`, the cores, and the NextUI-derived runtime libraries is the
  upstream repositories listed here at the release tag pulled by
  `mk/fetch-vendor.sh` (currently NextUI `v6.11.2`), plus the PlayOS patches in
  `minarch/overrides/`.

## NextUI runtime libraries and assets

Pulled by `mk/fetch-vendor.sh` from a NextUI release into `vendor/`:

- `vendor/lib/libmsettings.so`, `libgametimedb.so`, `libbatmondb.so` —
  NextUI/MinUI device libraries.
- `vendor/system/res/` — NextUI UI image assets, which minarch loads from its
  hardcoded `/.system/res` path: the in-game menu glyph sheet and the
  scanline/grid overlays. PlayOS does not use the NextUI fonts — the in-game
  menu is forced to PlayOS's own `menu.ttf` — nor the MinUI launcher assets,
  since PlayOS ships its own launcher.
- `vendor/bin/governor.sh` — called by name by minarch.
- These follow their NextUI upstream licensing (GPL-3.0 / MinUI MIT, plus the
  individual asset and font licenses). https://github.com/LoveRetro/NextUI

---

## libretro cores (`vendor/cores/`, shipped as `PlayOS/cores/`)

| Core | System | License |
|------|--------|---------|
| `fceumm_libretro.so` | NES | GPL-2.0-or-later |
| `mednafen_pce_fast_libretro.so` | TurboGrafx-16 / PC Engine | GPL-2.0-or-later (Mednafen-derived) |
| `mgba_libretro.so` | Game Boy Advance | MPL-2.0 |

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
