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
boot hook powers the device off when the loop exits.

To replace the RESIDENT EMULATOR, kill diatom FIRST and the launcher second.
`launch.sh` only calls `start_resident` at the top of its supervisor loop, and
that loop is blocked inside `./playos.elf`, so killing diatom alone gets no
respawn until the launcher exits. Do it the other way round and
`start_resident`'s `pgrep` finds the old diatom still alive and skips.

The launcher stops a still-running game at boot before its first frame (the
two-presenter wedge guard), so a restart mid-game is safe - but it stops the
player's game, so still avoid restarting while one runs. Check first: see below.

The device is shared with the Diatom session. Say what you are about to do to it
before you do it, and check nothing is running - two sessions deployed the same
build within seconds of each other on 2026-08-27, and the second restart killed
the first one's freshly verified pids while a human was mid-test.

## Is a game actually running?

Sample CPU ticks, not process lists. `ps` shows diatom either way, and a mapped
core proves nothing - ADR-0006 never unloads them, so a core stays mapped long
after its game ended.

    D=$(ps | grep "[d]iatom --socket" | awk '{print $1}' | head -1)
    A=$(awk '{print $14+$15}' /proc/$D/stat); sleep 2
    B=$(awk '{print $14+$15}' /proc/$D/stat); echo $((B-A))

A resident idling at the shelf moves a few ticks. One running a game cannot -
the blit alone is half a frame. Single digits over two seconds is idle.

## Seeing the screen

/dev/fb0 holds what is displayed, INCLUDING the launcher's GL output, at
1024x768 BGRA. Capture it ON THE DEVICE and pull the file:

    adb shell 'dd if=/dev/fb0 of=/tmp/fb.raw bs=4096 count=768'
    adb pull /tmp/fb.raw && adb shell 'rm -f /tmp/fb.raw'

Do NOT pipe dd through `adb shell`: it translates LF to CRLF on the way out and
silently corrupts the image. The tell is the size - a good capture is exactly
3145728 bytes, and the mangled one came back 3241341 and decoded to coloured
noise with one readable band through the middle. `adb exec-out`, which would
avoid the translation, is not supported by this device's adbd.

Convert with PIL: `Image.frombytes("RGBA",(1024,768),data,"raw","BGRA")`. A
correct capture of a launcher frame is alpha 255 in all 786432 pixels; anything
else means the byte order is wrong or the read was short.

`count=768` reads page 0. The framebuffer is 1024x16384 virtual, about 21 pages,
so page 0 is only what is on glass when the pan offset says so. Read
`/sys/class/graphics/fb0/pan` first and capture the page it names - `skip` is in
4096-byte BLOCKS and a page is 768 of them, so page 1 is `skip=768` and page 2
is `skip=1536`.

Do NOT assume the launcher sits on page 0. It double buffers and alternates:
300 samples while the shelf was up came back 151 at `0,0` and 149 at `0,768`.
An earlier version of this file claimed `0,0` meant the launcher was presenting,
which is true about half the time, and half-true is worse than wrong - it reads
as a running game.

`--shot` renders the shelf headless, but only the shelf; SIGSTOP the live
launcher while a second --shot instance runs.

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
  after - and mute the right file. `.userdata/tg5040/levels.cfg` holds the
  level the player last chose and WINS over playos.cfg, so editing playos.cfg
  does nothing on a device that has ever had its volume touched. It is also
  all-or-nothing: a levels.cfg missing either key is ignored entirely, so
  write both.

      L=/mnt/SDCARD/.userdata/tg5040/levels.cfg
      cat $L                                    # keep these to restore
      printf 'volume=0\nbrightness=7\n' > $L

  Applies at launcher restart. playos.cfg is only the default for a device
  that has never had a level set on it.
- The device clock is wrong until network time syncs; file mtimes are honest
  but odd.
- FAT mtimes have two-second granularity and `ls -t` ordering lies.
- adb push + chmod + sync BEFORE any restart, and verify the /proc/exe hash
  after - a stale binary passing your test is this project's oldest failure.

## The escape hatch

`PlayOS.pre-diatom/` on the card is the complete pre-migration PlayOS
directory. The wedge (two things presenting at once) needs a power cycle -
adb usually survives it, `adb reboot` sometimes does not.
