# The device loop - what working on a live Brick actually takes

Practical knowledge that cost time to learn, written down so it is not
re-learned. The deploy pipeline is `make all` then `mk/adb-deploy.sh`; the
rest is the sharp edges.

## Restarting the launcher

`killall -q tortos.elf` fails silently often enough to burn an hour. Kill by
pid and verify what came back:

    P=$(ps | grep "[.]/tortos.elf" | awk '{print $1}' | head -1)
    kill -9 $P; sleep 5
    P2=$(ps | grep "[.]/tortos.elf" | awk '{print $1}' | head -1)
    md5sum /proc/$P2/exe /mnt/SDCARD/TortOS/tortos.elf

The supervisor (`launch.sh`) restarts it; never kill launch.sh itself - the
boot hook powers the device off when the loop exits.

To replace the RESIDENT EMULATOR, kill diatom FIRST and the launcher second.
`launch.sh` only calls `start_resident` at the top of its supervisor loop, and
that loop is blocked inside `./tortos.elf`, so killing diatom alone gets no
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
3145728 bytes, and the mangled one came back 3241341 and decoded to colored
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

    A=305  B=304  Y=307  X=308  L1=310  R1=311
    SELECT=314  START=315  MENU=316
    d-pad: `hat -1/1` (up/down), `hatx -1/1` (left/right)

Note A/B and X/Y are both crossed relative to the evdev names: A is BTN_EAST
and B is BTN_SOUTH, X is BTN_WEST and Y is BTN_NORTH. Guessing from the
BTN_ name gets it wrong every time. The codes above are the buttons as
PRINTED on the shell, which is what a test is trying to press.

Derive this rather than probing for it. src/platform.c maps SDL joystick
INDICES (JOY_B=0, JOY_A=1, JOY_Y=2, JOY_X=3, JOY_L1=4, JOY_R1=5), and SDL
assigns those indices in ascending evdev-code order, so the table is whatever
the device declares:

    adb shell 'sed -n "/event3/,/^$/p" /proc/bus/input/devices | grep "B: KEY"'

decoded as a bitmap, gives 304 305 307 308 310 311 314 315 316 317 318 - and
the Nth of those is SDL index N. That yields the indices. Which physical
button carries which index cannot be derived at all, only observed, and on
2026-08-29 an attempt to reason it out from the evdev names got it backwards
and briefly "fixed" a mapping that was already right. Ask the person holding
the device; one press settles what an afternoon of inference will not.

Injected presses reach the game too, so a stray A is a stray jump.

## The traps

- A device-side script backgrounded from `adb shell` dies with the session -
  nohup included. Long sequences must stay client-driven, in SHORT adb calls;
  adb itself stalls intermittently, and one stalled long chain leaves the UI
  in an unknown state.
- Any headless game run makes SOUND at the shelf's volume. Mute first, restore
  after - and mute the right file. `.userdata/tg5040/levels.cfg` holds the
  level the player last chose and WINS over tortos.cfg, so editing tortos.cfg
  does nothing on a device that has ever had its volume touched. It is also
  all-or-nothing: a levels.cfg missing either key is ignored entirely, so
  write both.

      L=/mnt/SDCARD/.userdata/tg5040/levels.cfg
      cat $L                                    # keep these to restore
      printf 'volume=0\nbrightness=7\n' > $L

  Applies at launcher restart. tortos.cfg is only the default for a device
  that has never had a level set on it.
- The device clock is wrong until network time syncs; file mtimes are honest
  but odd.
- FAT mtimes have two-second granularity and `ls -t` ordering lies.
- adb push + chmod + sync BEFORE any restart, and verify the /proc/exe hash
  after - a stale binary passing your test is this project's oldest failure.

## The escape hatch

There is no longer one. This said `PlayOS.pre-diatom/` on the card was the
complete pre-migration launcher directory, and on 2026-08-28, checking before
the TortOS card install, it was not on the card at all. It had been gone long
enough that nobody noticed, which is the point worth keeping: a documented
fallback nobody has tried is a claim, not a fallback.

What is actually held: `backups/device-saves/<stamp>/`, verified by comparing
every file's MD5 against the device before the card was formatted, and the ROM
library, which lives in `TortOS-Test-Set/`. The launcher itself is rebuildable
from this repository, so it never needed a card copy.

The wedge (two things presenting at once) still needs a power cycle - adb
usually survives it, `adb reboot` sometimes does not.

## Never SIGKILL the launcher

`kill -9` on `tortos.elf` wedges the display until a power cycle.

It renders through GL, and killing it mid-render leaves a stalled command
buffer the PowerVR driver cannot reclaim:

    CheckForStalledCCB (force): CCCB has not progressed for "3D-P...-tortos.elf"
    Failed to free resource (_CleanupThreadPurgeConnectionData). Retry limit reached

After that, every `fb_open` blocks in `D` state - uninterruptible, so no
signal reaches it, including SIGKILL. Both the launcher and the emulator end
up stuck and the only way out is power.

Measured 2026-08-29, and caused by escalating on a bad measurement.

**Both tools worked the whole time.** `killall -q tortos.elf` restarts the
launcher in about a second, and SIGTERM stops an idle `diatom` in one. What
made them look broken:

  - `ps | grep tortos.elf` matches THE SHELL RUNNING THAT COMMAND, because the
    pattern is in its own command line. The launcher had already restarted
    under a new pid and the "still running" line was my own grep. Match on
    /proc/*/cmdline, or accept that a changed pid means it worked.
  - The one SIGTERM that genuinely did nothing was sent to a `diatom --core`
    already stuck in `D` on the display. That is not a process ignoring a
    signal, it is a process the kernel will not deliver one to - and by then
    the damage was already done.

So the rule is not "the gentle tools are unreliable". It is that a process
which looks unkillable is telling you something, and `kill -9` answers the
wrong question.

**Use `make adb-restart`** (SIGTERM) and give it a few seconds. If a process
will not go, that is information about the process, not a reason for a bigger
hammer.

The same applies to `diatom` while it holds the display. SIGTERM, wait, and
only consider SIGKILL once it has stopped presenting.
