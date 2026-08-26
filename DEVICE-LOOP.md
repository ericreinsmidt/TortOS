# The device loop - what working on a live Brick actually takes

Practical knowledge that cost time to learn, written down so it is not
re-learned. The deploy pipeline is `make all` then `mk/adb-deploy.sh`; the
rest is the sharp edges.

## Restarting the launcher

`killall -q playos.elf` fails silently often enough to burn an hour. Kill by
pid and verify what came back:

    P=$(ps | grep "[.]/playos.elf" | awk '{print $1}' | head -1)
    kill -9 $P; sleep 5
    P2=$(ps | grep "[.]/playos.elf" | awk '{print $1}' | head -1)
    md5sum /proc/$P2/exe /mnt/SDCARD/PlayOS/playos.elf

The supervisor (`launch.sh`) restarts it; never kill launch.sh itself - the
boot hook powers the device off when the loop exits. The launcher now stops a
still-running game at boot before its first frame (the two-presenter wedge
guard), so a restart mid-game is safe - but it stops the player's game, so
still avoid restarting while one runs.

## Seeing the screen

`dd if=/dev/fb0 bs=4096 count=768` captures what is displayed, INCLUDING the
launcher's GL output - 1024x768 BGRA. Convert with PIL ('raw','BGRA'). This is
how you screenshot the shelf, the in-game menu, anything. `--shot` renders the
shelf headless, but only the shelf; SIGSTOP the live launcher while a second
--shot instance runs.

## Driving it with no hands

`keyinject` (in the Diatom repo, staged at /mnt/SDCARD/diatom/keyinject)
writes into /dev/input/event3 and every reader sees it:

    A=305  B=304  MENU=316  START=315  SELECT=314
    d-pad: `hat -1/1` (up/down), `hatx -1/1` (left/right)

Injected presses reach the game too, so a stray A is a stray jump.

## The traps

- A device-side script backgrounded from `adb shell` dies with the session -
  nohup included. Long sequences must stay client-driven, in SHORT adb calls;
  adb itself stalls intermittently, and one stalled long chain leaves the UI
  in an unknown state.
- Any headless game run makes SOUND at the shelf's volume. Mute first, restore
  after:  sed -i "s/^volume=40/volume=0/" /mnt/SDCARD/PlayOS/playos.cfg
  (applies at launcher restart; the launcher persists it into libmsettings).
- The device clock is wrong until network time syncs; file mtimes are honest
  but odd.
- FAT mtimes have two-second granularity and `ls -t` ordering lies.
- adb push + chmod + sync BEFORE any restart, and verify the /proc/exe hash
  after - a stale binary passing your test is this project's oldest failure.

## The escape hatch

`PlayOS.pre-diatom/` on the card is the complete pre-migration PlayOS
directory. The wedge (two things presenting at once) needs a power cycle -
adb usually survives it, `adb reboot` sometimes does not.
