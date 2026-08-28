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

**1. Repo, mechanical. Done 2026-08-28.** Identifiers, strings, filenames,
`sd/playos/` -> `sd/tortos/`, `config/playos.cfg` -> `tortos.cfg`,
`playos.elf` -> `tortos.elf`.

"Safe by construction, the compiler catches a missed identifier" was too
confident. Three live names carry no type for it to check: the host binary
`build-native/playos`, which has no `.elf` in it to match on; the `pgrep -f
"PlayOS/diatom --socket"` guard inside `launch.sh`; and `P=$OUT/PlayOS` in
`payload.sh`. Both builds went green with all three still wrong. Running
`make payload` and resolving each `/mnt/SDCARD` path the chain names against
the tree it produced is the check that actually found them.

It also absorbed phase 3, not by choice. The pass that renamed
`/mnt/SDCARD/PlayOS` reached `tg5040.sh` and `launch.sh` in the same sweep, so
the boot chain was already half-moved -- `tg5040.sh` looking for
`/mnt/SDCARD/TortOS/launch.sh` while `payload.sh` still assembled
`out/sd/PlayOS/`. That half state is the one arrangement that certainly does
not boot, and it is not worth committing and then sleeping on, so the
remaining live names were finished here.

**2. Repo, prose.** README, licenses, DEVICE-LOOP, VOLUME-CURVE, the menu
heading and About row. No behavior change.

**3. The boot chain. Done 2026-08-28, folded into phase 1** for the reason
given there. `.tmp_update/tg5040.sh`, `launch.sh` internals, the payload and
zip names, all repo-only; the card itself is still phase 4.

**4. Device.** A fresh card install rather than an in-place rename: the
in-place version means renaming the directory a running `launch.sh` is
executing from, and there is no good moment to do that. Format, `make
install-card`, restore saves from `backups/`.

Before this, and it must be before: `playos-bootbright.sh` is installed into
`/usr/trimui/bin` and patched into `/etc/init.d/runtrimui`, which is firmware,
not card. A fresh card does not clean that up. The decision is to unpatch, for
consistency of naming.

The order is not free. `launch.sh` now greps `/etc/init.d/runtrimui` for
`tortos-bootbright` and patches when it does not find it, and the device's
`runtrimui` currently names `playos-bootbright`. So booting the new card
against the un-unpatched firmware would take a backup, with
`cp /etc/init.d/runtrimui /etc/init.d/runtrimui.tortos-bak`, of a file that is
already patched -- and the stock original survives only in
`runtrimui.playos-bak`, which nothing writes any more. Delete that or overwrite
it and the clean copy is gone for good; the result also calls both bootbright
scripts, one of which will not exist.

So, while the old card still boots: restore `/etc/init.d/runtrimui` from
`/etc/init.d/runtrimui.playos-bak`, delete `/usr/trimui/bin/playos-bootbright.sh`,
and confirm `grep bootbright /etc/init.d/runtrimui` is empty. Only then
format.

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
