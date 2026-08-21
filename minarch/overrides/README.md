# minarch overrides — NextUI-derived (GPL-3.0)

Every source file under `minarch/overrides/` is a copy of a file from
[NextUI](https://github.com/LoveRetro/NextUI) (© the NextUI / MinUI authors),
patched for PlayOS and overlaid onto a pristine NextUI checkout by
`minarch/build.sh`. NextUI is **GPL-3.0**, so these files — and the
`minarch.elf` built from them (shipped as `vendor/minarch.elf`, installed as
`PlayOS/minarch.elf`) — are **GPL-3.0, not PlayOS's 0BSD**. Each file carries
an `SPDX-License-Identifier: GPL-3.0-only` header saying so.

PlayOS's own code (`src/`, `tools/`, `mk/`, the configs and scripts) is 0BSD
and is a separate program: it talks to `minarch.elf` by running it and by
writing lines into a fifo.

## What is patched, and why

| File | Change |
|---|---|
| `all/minarch/minarch.c` | Resident mode. Holds the GL context and all three cores between games; runs one game after another off a fifo. This is where the launch cost went from ~1100ms to ~200ms. Also funnels the autosave: every way out of a game saves slot 9 exactly once. |
| `all/minarch/ma_core.c` | Splits `Core_open` into `Core_openLib` (dlopen, ~170ms, once per core ever) and `Core_bind` (callbacks and paths, microseconds, once per game). Honours `PLAYOS_TAG` instead of guessing the tag from punctuation in a filename. |
| `all/minarch/ma_config.c` | Makes the config lifecycle survive running more than once: `Config_quit` clears what it frees so `Config_init` runs again for the next core, and the per-game file lists are released rather than leaked. |
| `all/minarch/ma_input.c` | `Input_reset()`, so the per-core button map is rebuilt for each game rather than inherited from the last one. Keeps L3/R3 out of the core — the Brick's front F1/F2 keys report as those, and PlayOS owns them for brightness. |
| `all/minarch/ma_menu.c` | The in-game menu: dimmed frozen frame, a soft pill on the selected row, no toasts, no chrome. Adds a Reset row. Writes a preview image for the autosave, which is what the launcher shows on a card. Creates the `.minui` parent directory, without which every preview write failed silently. |
| `all/minarch/ma_frontend_opts.c` | Trims the options menu, and hides L3/R3 from the controls list to match `ma_input.c`. |
| `all/common/api.c` | The LEDs stay off. Volume and brightness are applied in game, because with no keymon running nothing else does. The settings indicator is PlayOS's thin top line. The menu uses PlayOS's font. |
| `all/common/notification.c` | Routes the in-game volume/brightness indicator to that same line. |

## Source availability (GPL §6)

The unmodified upstream is the NextUI tag pinned in `mk/fetch-vendor.sh`
(currently `v6.11.2`). The PlayOS modifications are exactly the diffs in this
directory against that tag:

```sh
diff -u ~/Projects/NextUI/workspace/all/common/api.c minarch/overrides/all/common/api.c
```

See `THIRD-PARTY-LICENSES.md` at the repo root for the full notice.
