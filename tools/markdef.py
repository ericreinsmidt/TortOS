"""The TortOS mark: one definition, everything else derives from it.

The shell's colors and cell positions were duplicated across the boot
animation, the shutdown animation in src/main.c, and the exported SVG and PNG.
On 2026-08-28 the cell beside the head changed color and that meant editing
the same table in four places - the identical failure the system accents had,
where six of nine cards had drifted from config/systems.cfg.

So: this module is the source. tools/genboot.py and tools/genmark.py import it.
src/main.c cannot, being C, and carries the only hand-kept copy - draw_shell()
names this file so the two can be compared when either changes.

CYAN is canonical for the whole system as of 2026-08-28, and is the same value
as MENU_ACCENT in src/main.c and UI_CYAN_* in src/ui.h. The mark used to carry
its own blue, (74,158,255), while the launcher's chrome was this cyan; two
comments claimed they were one color and neither was checked against the other.
They are one color now, so the boot animation, the shutdown animation, the
wordmark and the menu chrome are the same accent rather than a near miss.

That direction was the safe one. Unifying on the mark's old blue instead would
have put the launcher's chrome about eight degrees of hue from the Genesis card
accent in config/systems.cfg, close enough to read as a mistake; cyan is well
clear of all nine.

The ring alternates light, mid, light, mid, light, mid, which gives the shell
three-fold symmetry rather than a light pair on one side. The head is the same
radius and the same lattice step as every other cell: it is a scute that
happens to be dark, not an appendage.
"""

BG      = (17, 19, 16)
OFFWHT  = (233, 236, 227)
CYAN    = (61, 214, 255)      # the center, the wordmark's OS, a speed line;
                              # also the launcher's menu chrome and volume OSD
GREEN   = (94, 138, 86)
DKGREEN = (61, 89, 67)        # the head, and what dims the center at shutdown
MIDGRN  = (104, 138, 96)
LTGRN   = (128, 176, 118)

# (lattice i, lattice j, color) - i in units of sqrt(3)*r, j in units of 1.5*r
CELLS = [
    (-0.5, -1.0, LTGRN),   # upper left
    ( 0.5, -1.0, MIDGRN),  # upper right
    ( 1.0,  0.0, LTGRN),   # right, beside the head
    ( 0.5,  1.0, MIDGRN),  # lower right
    (-0.5,  1.0, LTGRN),   # lower left
    (-1.0,  0.0, MIDGRN),  # left
]
HEAD = (2.0, 0.0, DKGREEN)    # one step beyond the right cell
CENTER = CYAN

def hexf(rgb):
    return "#%02X%02X%02X" % rgb
