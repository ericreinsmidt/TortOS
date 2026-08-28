# PlayOS -> TortOS

Plan, written 2026-08-28. Delete this file when the last phase is done; a
migration document that outlives its migration becomes a description of
something that is no longer true.

## The one thing that can brick the boot

`sd/.tmp_update/tg5040.sh` hardcodes the entry point:

    LAUNCH=/mnt/SDCARD/PlayOS/launch.sh

The stock firmware runs that script. Rename the directory without updating this
file and the device boots to stock firmware or to nothing. Everything else here
is reversible from a working device; this is the step that can take the device
away from you, so it moves last and gets verified first.

Mitigation: the working card is backed up (`backups/`, 120MB, all saves and
states), and nothing has shipped, so there are no other cards in the world.

## Scope, measured

31 tracked files, roughly 250 references. Not all are equal:

| kind | where | notes |
|---|---|---|
| identifiers | `PLAYOS_*` x 21 distinct | mechanical, compiler catches misses |
| on-disk paths | `/mnt/SDCARD/PlayOS`, `.playos/`, `playos.elf`, `playos.cfg` | breaks a live device |
| boot chain | `.tmp_update/tg5040.sh`, `launch.sh` | the brick risk |
| firmware-installed | `/usr/trimui/bin/playos-bootbright.sh`, patched into `/etc/init.d/runtrimui` | lives outside the card |
| docs | README, THIRD-PARTY-LICENSES, DEVICE-LOOP, VOLUME-CURVE | prose |
| the UI itself | menu heading, "About PlayOS" | user-visible |
| Diatom's docs | the sibling repo names PlayOS throughout | separate repo |
| repo + remotes | GitHub `ericreinsmidt/PlayOS`, the mini path | last |

## Phases

**1. Repo, mechanical.** Identifiers, strings, filenames, `sd/playos/` ->
`sd/tortos/`, `config/playos.cfg` -> `tortos.cfg`, `playos.elf` -> `tortos.elf`.
Both builds green is the exit criterion; the compiler catches a missed
identifier, so this phase is safe by construction.

**2. Repo, prose.** README, licences, DEVICE-LOOP, VOLUME-CURVE, the menu
heading and About row. No behaviour change.

**3. The boot chain.** `.tmp_update/tg5040.sh`, `launch.sh` internals, the
payload and zip names. Nothing deployed yet - this is still repo-only.

**4. Device.** A fresh card install rather than an in-place rename: the
in-place version means renaming the directory a running `launch.sh` is
executing from, and there is no good moment to do that. Format, `make
install-card`, restore saves from `backups/`.

Before this: `playos-bootbright.sh` is installed into `/usr/trimui/bin` and
patched into `/etc/init.d/runtrimui`, which is firmware, not card. A fresh card
does not clean that up. Either leave the old name installed and keep
`launch.sh` writing it, or unpatch the init script first. Decide before
formatting.

Also note `.bootlogo_applied` and `.splash_applied` guard one-time installs; a
fresh card has neither, so both get reapplied, which is what we want.

**5. Diatom.** Its docs name PlayOS as the consumer. Separate repo, separate
commit, no code change - Diatom has never depended on the launcher's name.

**6. Remotes.** Rename the GitHub repo and the mini path last, once everything
else is proven, so a rollback never has to fight a moved remote.

## Verification at each phase

`make native` and `make all` after 1 and 3. A boot, a game launch, a save, a
resume and a power-off after 4. The `boot: anim wait` line staying absent from
`playos.log` (which will be `tortos.log`) after 4, since that is the check that
the boot animation still finishes before the launcher.
