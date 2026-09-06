# Turbo

**X and Y are turbo A and turbo B.** Hold one down and it presses the button
repeatedly for you. It applies to a whole system rather than to one game.

Nine of the eleven systems have it: NES, Master System, TurboGrafx-16, Game
Boy, Game Boy Color, Game Boy Advance, Game Gear, Neo Geo Pocket and Neo Geo
Pocket Color.

The settings database holds one entry per system, keyed `turbo.<tag>` in the
library scope, with a value like `x:a~3,y:b~3`. That reads *X acts as A, pressed
three frames and released three*, so about ten presses a second at 60 Hz. Lower
is faster; `1` is a press every other frame and almost certainly too fast for
anything. A system with no entry plays with X and Y doing nothing, which is what
they did everywhere before this existed.

The shipped values are compiled into the launcher and seed the database on
first run. `tortos.elf --dump` prints them.

## Why only nine

**Listed only where X and Y are spare.** Nine of the eleven consoles had two
face buttons, but a Genesis six-button pad and a SNES pad use X and Y for real,
so MD and SFC are absent on purpose rather than by oversight. Turbo there would
take away buttons games need.

## Why PC Engine is on the list anyway

The PC Engine core has a turbo of its own: a hotkey on buttons III and IV that
latches turbo for I and II. We do not use it.

One interaction across nine systems beats two, and Diatom's works without the
core's help. The PC Engine shipped a two-button pad and the core defaults to one
(`pce_fast_default_joypad_type_p1 = "2 Buttons"`), so III to VI do not exist and
X and Y go nowhere, exactly as on the rest of the list.

**The one thing that would break that**: setting that core option to 6 Buttons
turns X and Y into real buttons III and IV, and turbo would be taking them.

## Where the pulsing happens

Diatom does it, not the emulator core, which is why it behaves the same on all
nine rather than only on the one core that happens to implement turbo. TortOS
sends the map just after RUN, because RUN resets the map to identity and
anything sent before it would be discarded by the launch it was meant for.

`turbo_period` is half a cycle in frames - see Diatom's `src/env.c` and its
ADR-0028.
