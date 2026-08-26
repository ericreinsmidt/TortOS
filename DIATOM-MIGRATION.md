# Replacing minarch with Diatom

Written 2026-08-26. A plan, not a record: nothing here has been done yet.

## Why

`vendor/minarch.elf` is **GPL-3.0**, built from a NextUI checkout by
`minarch/build.sh`, which copies a pristine workspace and overlays the eight
files in `minarch/overrides/`. PlayOS's own code is 0BSD. So the shipped image
carries a licence PlayOS does not use, and the build carries a dependency on
another project's source tree being present and at the right tag.

[Diatom](https://github.com/ericreinsmidt/diatom) is an MIT libretro frontend written against the same
problems. It runs on this device today: nine systems across five cores, verified
on a Brick.

Three things this buys, in order of how much they matter:

1. **The NextUI dependency disappears.** No workspace to keep in sync, no
   overlay to re-apply, no tag to track. `minarch/` goes away entirely.
2. **GPL-3.0 leaves the image.** What remains is the cores, which are third
   party either way and which Diatom's ADR-0023 documents: two GPLv2, one MPL,
   and two **non-commercial**, the last being the real constraint on anything
   commercial and unrelated to which frontend is used.
3. **Capabilities that do not exist today.** Button remapping with core-supplied
   labels, per-system display modes, and volume and brightness that survive a
   game rather than being re-applied over the top.

## The integration surface is three functions

`src/platform.c` exposes `plat_resident_ready`, `plat_resident_send` and
`plat_resident_wait`. `src/main.c` calls them in one place. Everything else -
two fifos, `/tmp/playos_res.pid`, `/tmp/resume_slot.txt`, `SIGUSR1` - lives
behind that seam.

Diatom's ADR-0009 was written from exactly this arrangement and replaces all
five with one Unix socket: peer death becomes EOF, the slot becomes a field, the
signal becomes a `STOP` message, and the one-byte reply becomes
`EXIT reason=` and `ERROR code=`.

**`src/main.c` already falls back** when `plat_resident_wait` returns false: it
runs the game the slow way through a one-shot minarch. That fallback is what
makes this migration safe to attempt, and it is why minarch is deleted last
rather than first.

## Parity: seven of the eight overrides are already native

`minarch/overrides/README.md` lists what PlayOS had to patch into minarch.
Against Diatom:

| Patched into minarch | Diatom |
|---|---|
| `minarch.c` - resident mode, fifo loop, autosave funnel | ADR-0008, ADR-0009, ADR-0016 |
| `ma_core.c` - split `Core_open`, honour `PLAYOS_TAG` | ADR-0006 keeps every core mapped; `tag=` is a field of `RUN` |
| `ma_config.c` - config lifecycle survives repeated runs | no equivalent leak; options are rebuilt per game |
| `ma_input.c` - `Input_reset()`, keep L3/R3 out of the core | ADR-0020 resets the map on `RUN`; the port leaves `BTN_THUMBL`/`THUMBR` unmapped |
| `ma_frontend_opts.c` - trim options, hide L3/R3 | options are enumerated over the protocol and the launcher renders them |
| `api.c` - LEDs off, volume and brightness applied in game, indicator line | the port does both, with the same thin bar |
| `notification.c` - route the indicator | same bar |
| **`ma_menu.c` - in-game menu, and the autosave preview** | **the two gaps below** |

That is not a coincidence. Diatom was designed against the same device and the
same complaints; the overrides are largely a record of what minarch could not do
without being patched.

## The two gaps

### 1. The autosave preview

`src/main.c` reads `.minui/<folder>/<base>.9.bmp` and draws it on a game's card,
so the shelf shows the last frame the player saw. minarch writes it from
`ma_menu.c`.

`PREVIEW path=` is in ADR-0009's message table and **Diatom does not emit it**.
This is a hard parity requirement: without it every card falls back to box art
and the shelf visibly regresses.

Diatom work, not PlayOS work.

### 2. The in-game menu

`ma_menu.c` draws it inside minarch. Diatom's ADR-0016 hands the display to the
launcher instead: MENU produces `PAUSED`, and the launcher draws until it sends
`RESUME` or `STOP`.

That is the right split - the overrides for `ma_menu.c`, `api.c` and
`notification.c` exist mostly to force PlayOS's font, its soft pill and its thin
indicator line into someone else's menu, and drawing it here removes all of
that. But it is the one substantial piece of new PlayOS code in this plan.

## Phases

Each phase leaves a working device. The order is chosen so the risky part is
reversible and the irreversible part is last.

### Phase 1 - close the gaps in Diatom
No PlayOS changes. Emit `PREVIEW`, writing where PlayOS already looks, and
confirm the exit path gives slot-9 parity with minarch's autosave funnel.

*Exit criteria:* a Diatom session driven by `protodrive` leaves a `.9.bmp` where
PlayOS would find it, and a state PlayOS's resume path accepts.

### Phase 2 - socket transport behind the existing seam
Reimplement the three `plat_resident_*` bodies against the socket. Map
`"<tag>\t<core>\t<rom>\n"` onto `RUN core= rom= tag= slot=`, `SIGUSR1` onto
`STOP`, and the pid check onto a failed `connect()`. Select with an environment
variable, defaulting to the fifo path.

Both emulators work; flipping between them is one variable. **This is where
Diatom meets a real launcher for the first time**, and it will find things
`protodrive` structurally could not, which is the reason to do it while the old
path is one flag away.

*Exit criteria:* a game launches, runs and exits through Diatom with the
variable set, and through minarch without it, on the same build.

### Phase 3 - PlayOS draws the in-game menu
Handle `PAUSED`: draw, and answer with `RESUME`, `STOP`, `SAVE` or `LOAD`.

*Exit criteria:* every row minarch's menu offers is reachable, including Reset,
and the autosave still happens on every way out.

### Phase 4 - swap the resident
`start_resident()` in `sd/playos/launch.sh` runs `diatom` rather than
`minarch.elf --resident`. minarch stays installed as the one-shot fallback, so a
Diatom failure degrades to a slow launch instead of a dead device.

*Exit criteria:* a week of ordinary use with no fallback triggered.

### Phase 5 - delete minarch
Remove `minarch/`, `vendor/minarch.elf`, the NextUI dependency in
`mk/fetch-vendor.sh`, and the GPL-3.0 section of `THIRD-PARTY-LICENSES.md`.

Only after Phase 4 has held. This is the irreversible step and it buys nothing
that Phase 4 has not already delivered, so there is no reason to hurry it.

## Risks

**Diatom has never been driven by a real launcher.** Every protocol test to date
has used `protodrive`, a stand-in written by the same hand as the protocol,
which means it tests what the author expected rather than what a launcher does.
Phase 2 exists to expose that early and cheaply.

**Phase 3 is the long pole**, and it is PlayOS work. If it stalls, Phases 1 and
2 still stand on their own and the device still runs minarch.

**Licensing improves but does not finish.** Removing minarch removes GPL-3.0
from the image. It does not touch the two non-commercial cores, which restrict
commercial redistribution regardless of frontend.

**The fallback only helps while minarch exists.** Between Phase 4 and Phase 5
that is the entire safety margin, which is the argument for leaving a long gap
between them.
